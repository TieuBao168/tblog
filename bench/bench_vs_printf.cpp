// tblog vs plain fprintf: throughput, per-call latency and the cost of a
// filtered (disabled-level) call. One binary, results as JSON (render them
// with bench/report.py). Methodology: docs/benchmarks.md.
//
//   bench_vs_printf [--quick] [--runs 5] [--threads 1,2,4,8,16] [--out-dir DIR | --out FILE]
//
// Variants (every one writes the same line, the layout of tblog's default
// pattern, see BENCH_FPRINTF_LINE in bench_common.h):
//   fprintf_buffered    fprintf to one shared FILE* ("wb", default stdio buffer;
//                       the C library locks the stream for each call)
//   fprintf_flush       the same + fflush after every line (each line reaches
//                       the OS before the call returns)
//   fprintf_unbuffered  setvbuf(_IONBF) (not in the default set)
//   tblog_block         tblog, defaults except file only, full_policy = block
//                       (the default policy)
//   tblog_drop          tblog, defaults except file only, full_policy = drop
//
// Scenarios:
//   flood     every thread logs flood-lines/threads lines as fast as it can.
//             e2e = start until everything is written and fsynced: tblog until
//             tblog_shutdown() returns (it drains, fsyncs, closes); fprintf
//             until fflush + fsync + fclose return (e2e_nosync: until fflush).
//             loop = until the slowest thread left its loop. Per-call latency
//             is each call bracketed by steady_clock (timer cost included,
//             reported as timer_overhead_ns).
//   paced     open loop: each thread sends --rate lines/s on a fixed schedule
//             (threads phase-shifted); latency = end of the call - scheduled
//             send time, so a call that stalls also charges the calls queued
//             behind it (no coordinated omission). call latency = the call only.
//   disabled  a call below the level: TBLOG_DEBUG with level = INFO vs
//             `if (debug_enabled) fprintf(...)` with an atomic flag = false.
//   flush_level  tblog only, flush.on_level = ERROR (defaults otherwise, file
//             only): flood where 0%, 1%, 10% or 100% of the lines are
//             TBLOG_ERROR (every 100th / 10th / every line), the rest
//             TBLOG_INFO. Each ERROR call returns once the worker wrote it
//             to the file. Two sync settings: the default sync.on_level =
//             ERROR (every batch with an ERROR line is also fsynced, on the
//             sync thread) and sync.on_level = OFF ("nosync": periodic sync
//             only). Throughput as in flood; latency split into INFO calls
//             and ERROR calls. A run has flush-lines / threads lines per
//             thread, at most flush-max-errors ERROR lines per thread (fewer
//             lines at 10% / 100%: an ERROR call can take an fsync).
//
// Every measurement is repeated: --warmup rounds (discarded) then --runs
// rounds; within a round the variants run in rotating order. Each run gets a
// fresh scratch directory under --dir, removed afterwards.
#include "bench_common.h"

#include <cinttypes>

namespace {

using namespace bench;

enum Kind { kTblog = 0, kStdio = 1, kStdioFlush = 2, kSnprintfFwrite = 3 };

struct Variant {
    const char *name;
    Kind kind;
    const char *policy;  // tblog full_policy
    bool unbuffered;     // stdio: setvbuf(_IONBF)
    const char *desc;
};

const Variant kVariants[] = {
    {"fprintf_buffered", kStdio, "", false,
     "fprintf to one shared FILE* opened \"wb\", default stdio buffering (the C library locks the stream per call)"},
    {"fprintf_flush", kStdioFlush, "", false, "fprintf_buffered + fflush(f) after every line"},
    {"fprintf_unbuffered", kStdio, "", true, "fprintf with setvbuf(f, NULL, _IONBF, 0)"},
    {"snprintf_fwrite", kSnprintfFwrite, "", false,
     "snprintf of the same line into a stack buffer + one fwrite to the shared FILE* (default buffering)"},
    {"tblog_block", kTblog, "block", false,
     "TBLOG_INFO, default config except file only (console=false, reload.watch=false, file.max_size=4GB), full_policy=block (the default)"},
    {"tblog_drop", kTblog, "drop", false,
     "TBLOG_INFO, default config except file only (console=false, reload.watch=false, file.max_size=4GB), full_policy=drop"},
};

// flush_level scenario: tblog_block + flush.on_level = ERROR, `pct` % of the
// lines at ERROR (spread evenly: every 100 / pct-th line).
const int kErrorPcts[] = {0, 1, 10, 100};
std::string flush_variant_name(int pct, bool nosync) {
    return std::string(nosync ? "tblog_flush_error_nosync_" : "tblog_flush_error_") + std::to_string(pct) + "pct";
}
bool flush_variant_parse(const std::string &name, int *pct, bool *nosync) {
    for (int ns = 0; ns < 2; ++ns)
        for (int p : kErrorPcts)
            if (name == flush_variant_name(p, ns != 0)) {
                *pct = p;
                *nosync = ns != 0;
                return true;
            }
    return false;
}

const Variant *find_variant(const std::string &n) {
    for (const Variant &v : kVariants)
        if (n == v.name) return &v;
    return nullptr;
}

template <int K>
inline void emit(FILE *f, long i, const char *name) {
    if (K == kTblog) BENCH_TBLOG_LINE(i, name);
    else if (K == kSnprintfFwrite) BENCH_SNPRINTF_FWRITE_LINE(f, i, name);
    else BENCH_FPRINTF_LINE(f, i, name, K == kStdioFlush);
}

inline uint32_t clamp32(uint64_t d) { return d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d; }

// Percentile of sorted tick samples, in ns.
inline double pns(const std::vector<uint32_t> &sorted_ticks, double p) {
    return ticks_to_ns((uint64_t)pct(sorted_ticks, p));
}

// ------------------------------------------------------------------ one run
struct Scratch {
    std::string dir, log;
};

struct Opened {
    FILE *f = nullptr;
    uint64_t dropped_before = 0;
    bool ok = false;
};

Opened open_sink(const Variant &v, const Scratch &s) {
    Opened o;
    if (v.kind == kTblog) {
        const std::string conf = s.dir + "/log.conf";
        if (!write_file(conf, tblog_config(s.log, v.policy))) return o;
        o.dropped_before = tblog_dropped_count();
        o.ok = tblog_init(conf.c_str()) == 0;
    } else {
        o.f = std::fopen(s.log.c_str(), "wb");
        if (o.f && v.unbuffered) std::setvbuf(o.f, nullptr, _IONBF, 0);
        o.ok = o.f != nullptr;
    }
    return o;
}

struct Closed {
    double nosync_s = 0;  // stdio: after fflush (data in the OS)
    double done_s = 0;    // after fsync + close (tblog: tblog_shutdown returned)
    uint64_t dropped = 0;
};

Closed close_sink(const Variant &v, Opened &o, uint64_t t_go) {
    Closed c;
    if (v.kind == kTblog) {
        tblog_shutdown();
        c.done_s = c.nosync_s = double(now_ns() - t_go) / 1e9;
        c.dropped = tblog_dropped_count() - o.dropped_before;
    } else {
        std::fflush(o.f);
        c.nosync_s = double(now_ns() - t_go) / 1e9;
        fsync_file(o.f);
        std::fclose(o.f);
        c.done_s = double(now_ns() - t_go) / 1e9;
    }
    return c;
}

// Runs `body(t)` on `threads` threads released together; returns the go time.
template <class Body>
uint64_t run_threads(int threads, Body body) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            body(t);
        });
    while (ready.load() < threads) std::this_thread::yield();
    const uint64_t t_go = now_ns();
    go.store(true, std::memory_order_release);
    for (auto &t : ts) t.join();
    return t_go;
}

std::string thread_name(int t) { return "t" + std::to_string(t); }

// Sorts tick samples in place; percentiles in ns.
std::string latency_json(std::vector<uint32_t> &all) {
    std::sort(all.begin(), all.end());
    return Obj()
        .num("p50", pns(all, 0.50))
        .num("p90", pns(all, 0.90))
        .num("p99", pns(all, 0.99))
        .num("p999", pns(all, 0.999))
        .num("max", all.empty() ? 0 : ticks_to_ns(all.back()))
        .done();
}

struct RunOut {
    bool ok = false;
    std::string json;
    // metrics summarised across runs (name -> value)
    std::vector<std::pair<std::string, double>> metrics;
};

template <int K>
RunOut flood_run(const Variant &v, const Scratch &s, int threads, long total) {
    RunOut r;
    const long per = std::max(1L, total / threads);
    Opened o = open_sink(v, s);
    if (!o.ok) return r;
    std::vector<std::vector<uint32_t>> lat((size_t)threads, std::vector<uint32_t>((size_t)per));
    std::vector<uint64_t> t_end((size_t)threads);
    std::vector<std::string> names;
    for (int t = 0; t < threads; ++t) names.push_back(thread_name(t));
    const uint64_t t_go = run_threads(threads, [&](int t) {
        FILE *f = o.f;
        uint32_t *l = lat[(size_t)t].data();
        const char *name = names[(size_t)t].c_str();
        for (long i = 0; i < per; ++i) {
            const uint64_t a = ticks();
            emit<K>(f, i, name);
            l[i] = clamp32(ticks() - a);
        }
        t_end[(size_t)t] = now_ns();
    });
    uint64_t last = 0;
    double mean_loop = 0;
    for (uint64_t e : t_end) {
        last = std::max(last, e);
        mean_loop += double(e - t_go) / 1e9 / threads;
    }
    const double loop_s = double(last - t_go) / 1e9;
    const Closed c = close_sink(v, o, t_go);
    const uint64_t attempted = (uint64_t)per * (uint64_t)threads;
    const uint64_t written = attempted - c.dropped;
    const LineCount lc = count_lines(s.log);
    const uint64_t in_file = lc.lines - lc.notes;  // log lines (tblog notes such as "N lines dropped" apart)
    std::vector<uint32_t> all;
    all.reserve((size_t)attempted);
    for (auto &x : lat) all.insert(all.end(), x.begin(), x.end());
    const std::string lj = latency_json(all);
    r.metrics = {{"e2e_s", c.done_s},
                 {"e2e_lines_per_s", double(written) / c.done_s},
                 {"loop_s", loop_s},
                 {"loop_lines_per_s", double(attempted) / loop_s},
                 {"dropped", double(c.dropped)},
                 {"lat_p50_ns", pns(all, 0.50)},
                 {"lat_p90_ns", pns(all, 0.90)},
                 {"lat_p99_ns", pns(all, 0.99)},
                 {"lat_p999_ns", pns(all, 0.999)},
                 {"lat_max_ns", ticks_to_ns(all.back())}};
    if (v.kind != kTblog) r.metrics.push_back(std::make_pair(std::string("e2e_nosync_s"), c.nosync_s));
    r.json = Obj()
                 .num("lines_attempted", double(attempted))
                 .num("lines_dropped", double(c.dropped))
                 .num("lines_in_file", double(in_file))
                 .num("tblog_note_lines", double(lc.notes))
                 .boolean("file_complete", in_file == written)
                 .num("loop_s", loop_s)
                 .num("loop_mean_thread_s", mean_loop)
                 .num("e2e_nosync_s", c.nosync_s)
                 .num("e2e_s", c.done_s)
                 .num("e2e_lines_per_s", double(written) / c.done_s)
                 .num("loop_lines_per_s", double(attempted) / loop_s)
                 .raw("latency_ns", lj)
                 .done();
    r.ok = in_file == written;
    if (!r.ok)
        std::fprintf(stderr, "  !! %s: %" PRIu64 " lines in the file, expected %" PRIu64 "\n", v.name, in_file,
                     written);
    return r;
}

template <int K>
RunOut paced_run(const Variant &v, const Scratch &s, int threads, long per, uint64_t rate) {
    RunOut r;
    const uint64_t period = ns_to_ticks(1000000000ull / std::max<uint64_t>(1, rate));  // in ticks
    Opened o = open_sink(v, s);
    if (!o.ok) return r;
    std::vector<std::vector<uint32_t>> sched((size_t)threads, std::vector<uint32_t>((size_t)per));
    std::vector<std::vector<uint32_t>> call((size_t)threads, std::vector<uint32_t>((size_t)per));
    std::vector<uint64_t> t_end((size_t)threads);
    std::vector<std::string> names;
    for (int t = 0; t < threads; ++t) names.push_back(thread_name(t));
    std::atomic<uint64_t> t0{0};
    const uint64_t t_go = run_threads(threads, [&](int t) {
        // Common schedule origin 1 ms after the first thread got going; each
        // thread is phase-shifted so the threads do not send in lockstep.
        uint64_t start = 0;
        t0.compare_exchange_strong(start, ticks() + ns_to_ticks(1000000));
        start = t0.load() + period * (uint64_t)t / (uint64_t)threads;
        FILE *f = o.f;
        uint32_t *ls = sched[(size_t)t].data(), *lc = call[(size_t)t].data();
        const char *name = names[(size_t)t].c_str();
        for (long i = 0; i < per; ++i) {
            const uint64_t due = start + period * (uint64_t)i;
            uint64_t a = ticks();
            while (a < due) a = ticks();  // busy-wait: sleeping would add scheduler jitter
            emit<K>(f, i, name);
            const uint64_t e = ticks();
            ls[i] = clamp32(e - due);
            lc[i] = clamp32(e - a);
        }
        t_end[(size_t)t] = ticks();
    });
    uint64_t last = 0;
    for (uint64_t e : t_end) last = std::max(last, e);
    const Closed c = close_sink(v, o, t_go);
    std::vector<uint32_t> as, ac;
    for (auto &x : sched) as.insert(as.end(), x.begin(), x.end());
    for (auto &x : call) ac.insert(ac.end(), x.begin(), x.end());
    const std::string sj = latency_json(as), cj = latency_json(ac);
    const double secs = ticks_to_ns(last - t0.load()) / 1e9;
    const double achieved = double(per) * threads / secs;
    r.metrics = {{"lat_p50_ns", pns(as, 0.50)},
                 {"lat_p90_ns", pns(as, 0.90)},
                 {"lat_p99_ns", pns(as, 0.99)},
                 {"lat_p999_ns", pns(as, 0.999)},
                 {"lat_max_ns", ticks_to_ns(as.back())},
                 {"call_p50_ns", pns(ac, 0.50)},
                 {"call_p99_ns", pns(ac, 0.99)},
                 {"call_p999_ns", pns(ac, 0.999)},
                 {"achieved_lines_per_s", achieved},
                 {"dropped", double(c.dropped)}};
    r.json = Obj()
                 .num("lines", double(per) * threads)
                 .num("lines_dropped", double(c.dropped))
                 .num("target_lines_per_s", double(rate) * threads)
                 .num("achieved_lines_per_s", achieved)
                 .raw("latency_from_schedule_ns", sj)
                 .raw("latency_call_ns", cj)
                 .done();
    r.ok = true;
    return r;
}
// flush_level: one flood run of tblog with flush.on_level = ERROR, `pct` % of
// the lines at ERROR. Latency of the INFO calls and of the ERROR calls apart.
RunOut flush_level_run(int pct, bool nosync, const Scratch &s, int threads, long total, long max_errors) {
    RunOut r;
    long per = std::max(1L, total / threads);
    if (pct > 0) per = std::min(per, std::max(1L, max_errors * 100 / pct));
    const long every = pct > 0 ? 100 / pct : 0;
    const std::string conf = s.dir + "/log.conf";
    if (!write_file(conf, tblog_config(s.log, "block",
                                       std::string("flush.on_level = ERROR\n") +
                                           (nosync ? "sync.on_level = OFF\n" : ""))))
        return r;
    const uint64_t dropped_before = tblog_dropped_count();
    if (tblog_init(conf.c_str()) != 0) return r;
    std::vector<std::vector<uint32_t>> info((size_t)threads), err((size_t)threads);
    for (int t = 0; t < threads; ++t) {
        info[(size_t)t].reserve((size_t)per);
        if (every) err[(size_t)t].reserve((size_t)(per / every + 1));
    }
    std::vector<uint64_t> t_end((size_t)threads);
    std::vector<std::string> names;
    for (int t = 0; t < threads; ++t) names.push_back(thread_name(t));
    const uint64_t t_go = run_threads(threads, [&](int t) {
        std::vector<uint32_t> &li = info[(size_t)t], &le = err[(size_t)t];
        const char *name = names[(size_t)t].c_str();
        for (long i = 0; i < per; ++i) {
            const bool is_err = every && i % every == 0;
            const uint64_t a = ticks();
            if (is_err) BENCH_TBLOG_ERROR_LINE(i, name);
            else BENCH_TBLOG_LINE(i, name);
            const uint32_t d = clamp32(ticks() - a);
            (is_err ? le : li).push_back(d);
        }
        t_end[(size_t)t] = now_ns();
    });
    uint64_t last = 0;
    for (uint64_t e : t_end) last = std::max(last, e);
    const double loop_s = double(last - t_go) / 1e9;
    tblog_shutdown();
    const double e2e_s = double(now_ns() - t_go) / 1e9;
    const uint64_t dropped = tblog_dropped_count() - dropped_before;
    const uint64_t attempted = (uint64_t)per * (uint64_t)threads;
    const LineCount lc = count_lines(s.log);
    const uint64_t in_file = lc.lines - lc.notes;
    std::vector<uint32_t> ai, ae;
    for (auto &x : info) ai.insert(ai.end(), x.begin(), x.end());
    for (auto &x : err) ae.insert(ae.end(), x.begin(), x.end());
    const std::string ij = ai.empty() ? "null" : latency_json(ai);
    const std::string ej = ae.empty() ? "null" : latency_json(ae);
    r.metrics = {{"e2e_s", e2e_s},
                 {"e2e_lines_per_s", double(attempted - dropped) / e2e_s},
                 {"loop_s", loop_s},
                 {"loop_lines_per_s", double(attempted) / loop_s},
                 {"dropped", double(dropped)}};
    if (!ai.empty()) {
        r.metrics.push_back({"info_p50_ns", pns(ai, 0.50)});
        r.metrics.push_back({"info_p99_ns", pns(ai, 0.99)});
        r.metrics.push_back({"info_p999_ns", pns(ai, 0.999)});
        r.metrics.push_back({"info_max_ns", ticks_to_ns(ai.back())});
    }
    if (!ae.empty()) {
        r.metrics.push_back({"error_p50_ns", pns(ae, 0.50)});
        r.metrics.push_back({"error_p99_ns", pns(ae, 0.99)});
        r.metrics.push_back({"error_p999_ns", pns(ae, 0.999)});
        r.metrics.push_back({"error_max_ns", ticks_to_ns(ae.back())});
    }
    r.json = Obj()
                 .num("error_pct", pct)
                 .boolean("sync_on_error", !nosync)
                 .num("lines_per_thread", double(per))
                 .num("lines_attempted", double(attempted))
                 .num("error_lines", double(ae.size()))
                 .num("lines_dropped", double(dropped))
                 .num("lines_in_file", double(in_file))
                 .boolean("file_complete", in_file == attempted - dropped)
                 .num("loop_s", loop_s)
                 .num("e2e_s", e2e_s)
                 .num("e2e_lines_per_s", double(attempted - dropped) / e2e_s)
                 .num("loop_lines_per_s", double(attempted) / loop_s)
                 .raw("info_latency_ns", ij)
                 .raw("error_latency_ns", ej)
                 .done();
    r.ok = in_file == attempted - dropped;
    if (!r.ok)
        std::fprintf(stderr, "  !! flush_level %d%%: %" PRIu64 " lines in the file, expected %" PRIu64 "\n", pct,
                     in_file, attempted - dropped);
    return r;
}

std::atomic<bool> g_debug_enabled{false};  // the printf side's "level check"

RunOut disabled_run(bool tblog, const Scratch &s, long calls) {
    RunOut r;
    FILE *f = nullptr;
    if (tblog) {
        const std::string conf = s.dir + "/log.conf";
        write_file(conf, tblog_config(s.log, "drop") + "level = INFO\n");
        if (tblog_init(conf.c_str()) != 0) return r;
    } else {
        f = std::fopen(s.log.c_str(), "wb");
        if (!f) return r;
    }
    const uint64_t a = now_ns();
    if (tblog) {
        for (long i = 0; i < calls; ++i)
            TBLOG_DEBUG("iteration %ld value=%d name=%s", i, (int)(i * 7 % 1000), "t0");
    } else {
        for (long i = 0; i < calls; ++i)
            if (g_debug_enabled.load(std::memory_order_relaxed)) BENCH_FPRINTF_LINE(f, i, "t0", false);
    }
    const double ns = double(now_ns() - a) / double(calls);
    if (tblog) tblog_shutdown();
    else std::fclose(f);
    const uint64_t lines = count_lines(s.log).lines;
    r.metrics = {{"ns_per_call", ns}};
    r.json = Obj().num("calls", double(calls)).num("ns_per_call", ns).num("lines_in_file", double(lines)).done();
    r.ok = lines == 0;
    return r;
}

// ------------------------------------------------------------------ driver
struct Cell {
    std::string scenario, variant;
    int threads;
    std::vector<std::string> runs;
    std::vector<std::vector<std::pair<std::string, double>>> metrics;
    bool all_ok = true;
};

std::string cell_json(const Cell &c) {
    Obj sum;
    if (!c.metrics.empty()) {
        for (size_t m = 0; m < c.metrics[0].size(); ++m) {
            std::vector<double> v;
            for (const auto &run : c.metrics) v.push_back(run[m].second);
            sum.raw(c.metrics[0][m].first, spread(v));
        }
    }
    return Obj()
        .str("scenario", c.scenario)
        .str("variant", c.variant)
        .num("threads", c.threads)
        .boolean("all_runs_ok", c.all_ok)
        .raw("summary", sum.done())
        .raw("runs", jarr(c.runs, ",\n        "))
        .done();
}

void usage() {
    std::printf(
        "usage: bench_vs_printf [options]\n"
        "  --quick                 short smoke run (1 run, 1 and 4 threads, few lines)\n"
        "  --runs N                measured runs per cell (default 5), after --warmup N (default 1)\n"
        "  --scenarios LIST        flood,paced,disabled,flush_level (default all)\n"
        "  --variants LIST         fprintf_buffered,fprintf_flush,snprintf_fwrite,tblog_block,tblog_drop\n"
        "                          (default); also fprintf_unbuffered\n"
        "  --threads LIST          flood thread counts (default 1,2,4,8,16)\n"
        "  --paced-threads LIST    paced thread counts (default 1,4,8)\n"
        "  --flood-lines N         lines per flood run, split over the threads (default 1000000)\n"
        "  --paced-lines N         lines per thread per paced run (default 50000)\n"
        "  --rate N                paced lines/s per thread (default 50000)\n"
        "  --disabled-calls N      calls per disabled-level run (default 20000000)\n"
        "  --flush-threads LIST    flush_level thread counts (default 1,8)\n"
        "  --flush-lines N         lines per flush_level run, split over the threads (default 200000)\n"
        "  --flush-max-errors N    at most N ERROR lines per thread per flush_level run (default 1000)\n"
        "  --dir DIR               scratch directory (default: <temp>/tblog-bench-<pid>)\n"
        "  --out FILE | --out-dir DIR   result JSON (default name <os>-<compiler>-<cpu>-<date>-perf.json)\n"
        "  --label S --build-info S --commit SHA   recorded in the JSON\n");
}

}  // namespace

int main(int argc, char **argv) {
    const Args args(argc, argv);
    if (args.flag("--help") || args.flag("-h")) {
        usage();
        return 0;
    }
    const bool quick = args.flag("--quick");
    const int runs = (int)args.num("--runs", quick ? 1 : 5);
    const int warmup = (int)args.num("--warmup", 1);
    const std::vector<std::string> scenarios = split(args.get("--scenarios", "flood,paced,disabled,flush_level"));
    const std::vector<std::string> vnames =
        split(args.get("--variants", "fprintf_buffered,fprintf_flush,snprintf_fwrite,tblog_block,tblog_drop"));
    const std::vector<int> flood_threads = int_list(args.get("--threads", quick ? "1,4" : "1,2,4,8,16"));
    const std::vector<int> paced_threads = int_list(args.get("--paced-threads", quick ? "1,4" : "1,4,8"));
    const long flood_lines = (long)args.num("--flood-lines", quick ? 20000 : 1000000);
    const long paced_lines = (long)args.num("--paced-lines", quick ? 2000 : 50000);
    const uint64_t rate = (uint64_t)args.num("--rate", 50000);
    const long disabled_calls = (long)args.num("--disabled-calls", quick ? 1000000 : 20000000);
    const std::vector<int> flush_threads = int_list(args.get("--flush-threads", quick ? "1,4" : "1,8"));
    const long flush_lines = (long)args.num("--flush-lines", quick ? 4000 : 200000);
    const long flush_max_errors = (long)args.num("--flush-max-errors", quick ? 100 : 1000);
    const std::string base = args.get("--dir", temp_root() + "/tblog-bench-" + std::to_string(pid()));

    std::vector<const Variant *> variants;
    for (const auto &n : vnames) {
        const Variant *v = find_variant(n);
        if (!v) {
            std::fprintf(stderr, "unknown variant %s\n", n.c_str());
            return 2;
        }
        variants.push_back(v);
    }
    remove_tree(base);
    make_dirs(base);
    calibrate_ticks();
    const TimerOverhead tov = timer_overhead();
    std::printf("bench_vs_printf: %s, %u logical CPUs; per-call timer: %s, overhead min %.1f ns, median %.1f ns; "
                "steady_clock step %.0f ns\n",
                cpu_model().c_str(), std::thread::hardware_concurrency(), tick_source(), tov.min_ns, tov.median_ns,
                tov.steady_resolution_ns);
    std::printf("scratch: %s; runs %d (+%d warm-up)\n", base.c_str(), runs, warmup);
    std::fflush(stdout);

    int counter = 0;
    auto scratch = [&]() -> Scratch {
        Scratch s;
        s.dir = base + "/run-" + std::to_string(counter++);
        remove_tree(s.dir);
        make_dirs(s.dir);
        s.log = s.dir + "/app.log";
        return s;
    };
    std::vector<Cell> cells;
    bool all_ok = true;
    const uint64_t t_start = now_ns();

    auto run_matrix = [&](const std::string &scn, const std::vector<int> &tlist,
                          const std::vector<std::string> &vlist, RunOut (*fn)(const std::string &, const Scratch &, int)) {
        for (int threads : tlist) {
            std::vector<size_t> idx;
            for (const auto &vn : vlist) {
                Cell c;
                c.scenario = scn;
                c.variant = vn;
                c.threads = threads;
                idx.push_back(cells.size());
                cells.push_back(c);
            }
            for (int round = 0; round < warmup + runs; ++round) {
                for (size_t k = 0; k < vlist.size(); ++k) {
                    const size_t j = (k + (size_t)round) % vlist.size();  // rotating order
                    const Scratch s = scratch();
                    RunOut r = fn(vlist[j], s, threads);
                    remove_tree(s.dir);
                    Cell &c = cells[idx[j]];
                    const bool measured = round >= warmup;
                    std::printf("%-8s %-18s thr=%-2d %s", scn.c_str(), vlist[j].c_str(), threads,
                                measured ? "run " : "warm");
                    for (const auto &m : r.metrics)
                        if (m.first == "e2e_lines_per_s" || m.first == "lat_p50_ns" || m.first == "lat_p99_ns" ||
                            m.first == "ns_per_call" || m.first == "achieved_lines_per_s" ||
                            m.first == "error_p50_ns" || m.first == "error_p99_ns")
                            std::printf("  %s=%.4g", m.first.c_str(), m.second);
                    std::printf("%s\n", r.ok ? "" : "  FAILED");
                    std::fflush(stdout);
                    if (!r.ok) c.all_ok = all_ok = false;
                    if (measured && !r.json.empty()) {
                        c.runs.push_back(r.json);
                        c.metrics.push_back(r.metrics);
                    }
                }
            }
        }
    };

    static long s_flood_lines, s_paced_lines, s_disabled, s_flush_lines, s_flush_max_errors;
    static uint64_t s_rate;
    s_flood_lines = flood_lines;
    s_paced_lines = paced_lines;
    s_disabled = disabled_calls;
    s_flush_lines = flush_lines;
    s_flush_max_errors = flush_max_errors;
    s_rate = rate;
    for (const auto &scn : scenarios) {
        if (scn == "flood") {
            run_matrix(scn, flood_threads, vnames, [](const std::string &vn, const Scratch &s, int t) -> RunOut {
                const Variant &v = *find_variant(vn);
                if (v.kind == kTblog) return flood_run<kTblog>(v, s, t, s_flood_lines);
                if (v.kind == kSnprintfFwrite) return flood_run<kSnprintfFwrite>(v, s, t, s_flood_lines);
                if (v.kind == kStdioFlush) return flood_run<kStdioFlush>(v, s, t, s_flood_lines);
                return flood_run<kStdio>(v, s, t, s_flood_lines);
            });
        } else if (scn == "paced") {
            run_matrix(scn, paced_threads, vnames, [](const std::string &vn, const Scratch &s, int t) -> RunOut {
                const Variant &v = *find_variant(vn);
                if (v.kind == kTblog) return paced_run<kTblog>(v, s, t, s_paced_lines, s_rate);
                if (v.kind == kSnprintfFwrite) return paced_run<kSnprintfFwrite>(v, s, t, s_paced_lines, s_rate);
                if (v.kind == kStdioFlush) return paced_run<kStdioFlush>(v, s, t, s_paced_lines, s_rate);
                return paced_run<kStdio>(v, s, t, s_paced_lines, s_rate);
            });
        } else if (scn == "disabled") {
            run_matrix(scn, {1}, {"printf_if_guard", "tblog_below_level"},
                       [](const std::string &vn, const Scratch &s, int) -> RunOut {
                           return disabled_run(vn == "tblog_below_level", s, s_disabled);
                       });
        } else if (scn == "flush_level") {
            std::vector<std::string> fv;
            for (int ns = 0; ns < 2; ++ns)
                for (int p : kErrorPcts) fv.push_back(flush_variant_name(p, ns != 0));
            run_matrix(scn, flush_threads, fv, [](const std::string &vn, const Scratch &s, int t) -> RunOut {
                int pct = 0;
                bool nosync = false;
                flush_variant_parse(vn, &pct, &nosync);
                return flush_level_run(pct, nosync, s, t, s_flush_lines, s_flush_max_errors);
            });
        } else {
            std::fprintf(stderr, "unknown scenario %s\n", scn.c_str());
            return 2;
        }
    }
    remove_tree(base);
    const double wall = double(now_ns() - t_start) / 1e9;

    Obj vd;
    for (const Variant &v : kVariants) vd.str(v.name, v.desc);
    vd.str("printf_if_guard", "if (atomic<bool> debug_enabled /* false */) fprintf(...): the filtered printf call");
    vd.str("tblog_below_level", "TBLOG_DEBUG(...) with level = INFO: the filtered tblog call");
    for (int ns = 0; ns < 2; ++ns)
        for (int p : kErrorPcts)
            vd.str(flush_variant_name(p, ns != 0),
                   std::string("tblog_block + flush.on_level=ERROR") + (ns ? " + sync.on_level=OFF" : "") + "; " +
                       std::to_string(p) + "% of the lines are TBLOG_ERROR (evenly spread), the rest TBLOG_INFO");
    std::vector<std::string> cj;
    for (const Cell &c : cells) cj.push_back(cell_json(c));
    std::vector<std::string> sl, vl;
    for (const auto &s : scenarios) sl.push_back(jstr(s));
    for (const auto &v : vnames) vl.push_back(jstr(v));
    std::vector<std::string> ft, pt;
    for (int t : flood_threads) ft.push_back(jnum(t));
    for (int t : paced_threads) pt.push_back(jnum(t));
    std::vector<std::string> flt;
    for (int t : flush_threads) flt.push_back(jnum(t));
    const std::string config =
        Obj()
            .boolean("quick", quick)
            .num("runs", runs)
            .num("warmup_runs", warmup)
            .raw("scenarios", jarr(sl))
            .raw("variants", jarr(vl))
            .raw("flood_threads", jarr(ft))
            .raw("paced_threads", jarr(pt))
            .num("flood_lines_per_run", double(flood_lines))
            .num("paced_lines_per_thread", double(paced_lines))
            .num("paced_rate_per_thread", double(rate))
            .num("disabled_calls", double(disabled_calls))
            .raw("flush_threads", jarr(flt))
            .num("flush_lines_per_run", double(flush_lines))
            .num("flush_max_errors_per_thread", double(flush_max_errors))
            .str("per_call_timer", tick_source())
            .num("ns_per_tick", ns_per_tick())
            .raw("timer_overhead_ns", Obj().num("min", tov.min_ns).num("median", tov.median_ns).done())
            .num("steady_clock_step_ns", tov.steady_resolution_ns)
            .str("line_format",
                 "<YYYY-MM-DD HH:MM:SS.uuuuuu> [INFO] [<tid>] bench_vs_printf.cpp:<line> - iteration <i> value=<i*7%1000> name=t<thread>")
            .str("e2e_definition",
                 "tblog: start until tblog_shutdown() returns (drain + fsync + close); fprintf: start until fflush + fsync + fclose return")
            .num("wall_s", wall)
            .done();
    const std::string json = Obj()
                                 .str("schema", "tblog-bench-vs-printf/1")
                                 .raw("env", env_json(args, base))
                                 .raw("config", config)
                                 .raw("variants", vd.done())
                                 .raw("results", "[\n    " + [&]() -> std::string {
                                     std::string s;
                                     for (size_t i = 0; i < cj.size(); ++i) s += (i ? ",\n    " : "") + cj[i];
                                     return s;
                                 }() + "\n  ]")
                                 .done();
    const std::string path = save_json(args, "perf", json);
    std::printf("wall %.1f s; results: %s%s\n", wall, path.c_str(), all_ok ? "" : " (SOME RUNS FAILED)");
    return path.empty() || !all_ok ? 1 : 0;
}
