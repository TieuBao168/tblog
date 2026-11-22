// tblog vs spdlog (async) vs Quill: throughput and per-call latency, the same
// line through each library. Built with -DTBLOG_BENCH_COMPARE=ON (CMake
// downloads spdlog 1.17.0 and Quill 13.1.0). Results as JSON, rendered by
// bench/report.py; methodology in docs/benchmarks.md.
//
//   bench_vs_libs [--quick] [--runs 5] [--threads 1,2,4,8,16] [--out-dir DIR | --out FILE]
//
// One process per measured run: the parent re-runs this program with --cell
// for every (scenario, variant, threads, run), because Quill's backend can be
// started only once per process. The variants run in rotating order within a
// round, after --warmup rounds that are discarded (as in bench_vs_printf).
//
// Variants (each library's defaults, file sink only, one file):
//   tblog_block   TBLOG_INFO, the tblog config of bench_vs_printf (full_policy = block)
//   spdlog_async  async_logger, thread pool queue 8192 / 1 thread, overflow block
//   quill         default FrontendOptions (UnboundedBlocking queue) and
//                 BackendOptions, except error_notifier = {} (no "queue
//                 reallocated" notices on stderr)
// Line: "<date> <time>.<us> [INFO] [<tid>] <file>:<line> - iteration <i> value=<v> name=t<k>".
// e2e ends once every line is written and the file closed and fsynced:
// tblog_shutdown (it fsyncs); spdlog / Quill: flush + shutdown, then the file
// is reopened and fsynced (neither fsyncs by default).
//
// Scenarios: flood (every thread logs flood-lines/threads lines as fast as it
// can) and paced (open loop, --rate lines/s per thread, latency from the
// scheduled send time), as in bench_vs_printf.
#include "bench_common.h"

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/spdlog.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"

#include <cinttypes>

#if defined(_WIN32)
#define bench_popen _popen
#define bench_pclose _pclose
#else
#define bench_popen popen
#define bench_pclose pclose
#endif

namespace {

using namespace bench;

enum Lib { kTblog = 0, kSpdlog = 1, kQuill = 2 };

struct Variant {
    const char *name;
    Lib lib;
    const char *desc;
};

const Variant kVariants[] = {
    {"tblog_block", kTblog, "TBLOG_INFO, default config except file only (console=false, reload.watch=false, file.max_size=4GB), full_policy=block"},
    {"spdlog_async", kSpdlog, "spdlog 1.17.0 async_logger: thread pool queue 8192, 1 thread, overflow block (defaults); basic_file_sink"},
    {"quill", kQuill, "Quill 13.1.0: default FrontendOptions (UnboundedBlocking, 128 KiB initial) and BackendOptions except error_notifier = {}; FileSink"},
};

const Variant *find_variant(const std::string &n) {
    for (const Variant &v : kVariants)
        if (n == v.name) return &v;
    return nullptr;
}

// ------------------------------------------------------------------ child: one run
std::shared_ptr<spdlog::logger> g_spd;
quill::Logger *g_quill = nullptr;

bool open_lib(Lib lib, const std::string &dir, const std::string &log) {
    if (lib == kTblog) {
        const std::string conf = dir + "/log.conf";
        return write_file(conf, tblog_config(log, "block")) && tblog_init(conf.c_str()) == 0;
    }
    if (lib == kSpdlog) {
        spdlog::init_thread_pool(8192, 1);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log, true);
        g_spd = std::make_shared<spdlog::async_logger>("bench", sink, spdlog::thread_pool(),
                                                       spdlog::async_overflow_policy::block);
        g_spd->set_pattern("%Y-%m-%d %H:%M:%S.%f [INFO] [%t] %s:%# - %v");  // only INFO is logged
        return true;
    }
    quill::BackendOptions bo;
    bo.error_notifier = {};
    quill::Backend::start(bo);
    auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>(log, [] {
        quill::FileSinkConfig cfg;
        cfg.set_open_mode('w');
        return cfg;
    }());
    quill::PatternFormatterOptions pfo;
    pfo.format_pattern = "%(time) [%(log_level)] [%(thread_id)] %(file_name):%(line_number) - %(message)";
    pfo.timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qus";
    pfo.timestamp_timezone = quill::Timezone::LocalTime;
    g_quill = quill::Frontend::create_or_get_logger("bench", std::move(sink), pfo);
    return g_quill != nullptr;
}

void close_lib(Lib lib, const std::string &log) {
    if (lib == kTblog) {
        tblog_shutdown();
        return;
    }
    if (lib == kSpdlog) {
        g_spd->flush();
        g_spd.reset();
        spdlog::shutdown();  // joins the pool once its queue is drained
    } else {
        g_quill->flush_log();  // blocks until the backend wrote everything
        quill::Backend::stop();
    }
    FILE *f = std::fopen(log.c_str(), "ab");
    if (f) {
        fsync_file(f);
        std::fclose(f);
    }
}

template <int L>
inline void emit(long i, const char *name) {
    if (L == kTblog) BENCH_TBLOG_LINE(i, name);
    else if (L == kSpdlog) SPDLOG_LOGGER_INFO(g_spd, "iteration {} value={} name={}", i, (int)(i * 7 % 1000), name);
    else LOG_INFO(g_quill, "iteration {} value={} name={}", i, (int)(i * 7 % 1000), name);
}

inline uint32_t clamp32(uint64_t d) { return d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d; }
inline double pns(const std::vector<uint32_t> &sorted, double p) { return ticks_to_ns((uint64_t)pct(sorted, p)); }

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

using Metrics = std::vector<std::pair<std::string, double>>;

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

template <int L>
bool flood(int threads, long total, const std::string &log, Metrics *m, std::string *json) {
    const long per = std::max(1L, total / threads);
    std::vector<std::vector<uint32_t>> lat((size_t)threads, std::vector<uint32_t>((size_t)per));
    std::vector<uint64_t> t_end((size_t)threads);
    std::vector<std::string> names;
    for (int t = 0; t < threads; ++t) names.push_back("t" + std::to_string(t));
    const uint64_t t_go = run_threads(threads, [&](int t) {
        uint32_t *l = lat[(size_t)t].data();
        const char *name = names[(size_t)t].c_str();
        for (long i = 0; i < per; ++i) {
            const uint64_t a = ticks();
            emit<L>(i, name);
            l[i] = clamp32(ticks() - a);
        }
        t_end[(size_t)t] = now_ns();
    });
    uint64_t last = 0;
    for (uint64_t e : t_end) last = std::max(last, e);
    const double loop_s = double(last - t_go) / 1e9;
    close_lib((Lib)L, log);
    const double e2e_s = double(now_ns() - t_go) / 1e9;
    const uint64_t attempted = (uint64_t)per * (uint64_t)threads;
    const uint64_t in_file = count_lines(log).lines;
    std::vector<uint32_t> all;
    all.reserve((size_t)attempted);
    for (auto &x : lat) all.insert(all.end(), x.begin(), x.end());
    const std::string lj = latency_json(all);
    *m = {{"e2e_s", e2e_s},
          {"e2e_lines_per_s", double(attempted) / e2e_s},
          {"loop_s", loop_s},
          {"loop_lines_per_s", double(attempted) / loop_s},
          {"lat_p50_ns", pns(all, 0.50)},
          {"lat_p90_ns", pns(all, 0.90)},
          {"lat_p99_ns", pns(all, 0.99)},
          {"lat_p999_ns", pns(all, 0.999)},
          {"lat_max_ns", ticks_to_ns(all.back())}};
    *json = Obj()
                .num("lines_attempted", double(attempted))
                .num("lines_in_file", double(in_file))
                .boolean("file_complete", in_file == attempted)
                .num("loop_s", loop_s)
                .num("e2e_s", e2e_s)
                .num("e2e_lines_per_s", double(attempted) / e2e_s)
                .num("loop_lines_per_s", double(attempted) / loop_s)
                .raw("latency_ns", lj)
                .done();
    return in_file == attempted;
}

template <int L>
bool paced(int threads, long per, uint64_t rate, const std::string &log, Metrics *m, std::string *json) {
    const uint64_t period = ns_to_ticks(1000000000ull / std::max<uint64_t>(1, rate));
    std::vector<std::vector<uint32_t>> sched((size_t)threads, std::vector<uint32_t>((size_t)per));
    std::vector<std::vector<uint32_t>> call((size_t)threads, std::vector<uint32_t>((size_t)per));
    std::vector<uint64_t> t_end((size_t)threads);
    std::vector<std::string> names;
    for (int t = 0; t < threads; ++t) names.push_back("t" + std::to_string(t));
    std::atomic<uint64_t> t0{0};
    run_threads(threads, [&](int t) {
        uint64_t start = 0;
        t0.compare_exchange_strong(start, ticks() + ns_to_ticks(1000000));
        start = t0.load() + period * (uint64_t)t / (uint64_t)threads;
        uint32_t *ls = sched[(size_t)t].data(), *lc = call[(size_t)t].data();
        const char *name = names[(size_t)t].c_str();
        for (long i = 0; i < per; ++i) {
            const uint64_t due = start + period * (uint64_t)i;
            uint64_t a = ticks();
            while (a < due) a = ticks();  // busy-wait, as bench_vs_printf
            emit<L>(i, name);
            const uint64_t e = ticks();
            ls[i] = clamp32(e - due);
            lc[i] = clamp32(e - a);
        }
        t_end[(size_t)t] = ticks();
    });
    uint64_t last = 0;
    for (uint64_t e : t_end) last = std::max(last, e);
    close_lib((Lib)L, log);
    const uint64_t in_file = count_lines(log).lines;
    std::vector<uint32_t> as, ac;
    for (auto &x : sched) as.insert(as.end(), x.begin(), x.end());
    for (auto &x : call) ac.insert(ac.end(), x.begin(), x.end());
    const std::string sj = latency_json(as), cj = latency_json(ac);
    const double secs = ticks_to_ns(last - t0.load()) / 1e9;
    const double achieved = double(per) * threads / secs;
    *m = {{"lat_p50_ns", pns(as, 0.50)},
          {"lat_p90_ns", pns(as, 0.90)},
          {"lat_p99_ns", pns(as, 0.99)},
          {"lat_p999_ns", pns(as, 0.999)},
          {"lat_max_ns", ticks_to_ns(as.back())},
          {"call_p50_ns", pns(ac, 0.50)},
          {"call_p99_ns", pns(ac, 0.99)},
          {"call_p999_ns", pns(ac, 0.999)},
          {"achieved_lines_per_s", achieved}};
    *json = Obj()
                .num("lines", double(per) * threads)
                .num("lines_in_file", double(in_file))
                .num("target_lines_per_s", double(rate) * threads)
                .num("achieved_lines_per_s", achieved)
                .raw("latency_from_schedule_ns", sj)
                .raw("latency_call_ns", cj)
                .done();
    return in_file == (uint64_t)per * (uint64_t)threads;
}

template <int L>
bool run_cell(const std::string &scn, int threads, const Args &a, const std::string &log, Metrics *m,
              std::string *json) {
    if (scn == "paced")
        return paced<L>(threads, (long)a.num("--paced-lines", 50000), (uint64_t)a.num("--rate", 50000), log, m, json);
    return flood<L>(threads, (long)a.num("--flood-lines", 1000000), log, m, json);
}

// Child protocol on stdout: "M <name> <value>" per metric, "R <run json>", "OK <0|1>".
int child_main(const Args &a) {
    const Variant *v = find_variant(a.get("--variant", ""));
    const std::string scn = a.get("--cell", ""), dir = a.get("--dir", "");
    const int threads = (int)a.num("--threads", 1);
    if (!v || dir.empty() || (scn != "flood" && scn != "paced")) return 90;
    calibrate_ticks();
    make_dirs(dir);
    const std::string log = dir + "/app.log";
    if (!open_lib(v->lib, dir, log)) return 91;
    Metrics m;
    std::string json;
    const bool ok = v->lib == kTblog    ? run_cell<kTblog>(scn, threads, a, log, &m, &json)
                    : v->lib == kSpdlog ? run_cell<kSpdlog>(scn, threads, a, log, &m, &json)
                                        : run_cell<kQuill>(scn, threads, a, log, &m, &json);
    for (const auto &x : m) std::printf("M %s %.17g\n", x.first.c_str(), x.second);
    std::printf("R %s\nOK %d\n", json.c_str(), ok ? 1 : 0);
    std::fflush(stdout);
    return 0;
}

// ------------------------------------------------------------------ parent
struct ChildOut {
    bool ok = false;
    Metrics m;
    std::string json;
};

std::string quote(const std::string &s) { return "\"" + s + "\""; }

ChildOut run_child(const std::string &exe, const std::vector<std::string> &args) {
    std::string cmd = quote(exe);
    for (const auto &x : args) cmd += " " + quote(x);
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";  // cmd.exe /c strips one outer pair of quotes
#endif
    ChildOut o;
    FILE *p = bench_popen(cmd.c_str(), "r");
    if (!p) return o;
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    const int rc = bench_pclose(p);
    size_t b = 0;
    while (b < out.size()) {
        size_t e = out.find('\n', b);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(b, e - b);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 2, "M ") == 0) {
            const size_t sp = line.find(' ', 2);
            if (sp != std::string::npos)
                o.m.push_back({line.substr(2, sp - 2), std::strtod(line.c_str() + sp + 1, nullptr)});
        } else if (line.compare(0, 2, "R ") == 0) {
            o.json = line.substr(2);
        } else if (line == "OK 1") {
            o.ok = true;
        }
        b = e + 1;
    }
    if (rc != 0) o.ok = false;
    return o;
}

struct Cell {
    std::string scenario, variant;
    int threads;
    std::vector<std::string> runs;
    std::vector<Metrics> metrics;
    bool all_ok = true;
};

std::string cell_json(const Cell &c) {
    Obj sum;
    if (!c.metrics.empty())
        for (size_t i = 0; i < c.metrics[0].size(); ++i) {
            std::vector<double> v;
            for (const auto &run : c.metrics)
                if (i < run.size()) v.push_back(run[i].second);
            sum.raw(c.metrics[0][i].first, spread(v));
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
        "usage: bench_vs_libs [options]\n"
        "  --quick                 short smoke run (1 run, 1 and 4 threads, few lines)\n"
        "  --runs N                measured runs per cell (default 5), after --warmup N (default 1)\n"
        "  --scenarios LIST        flood,paced (default both)\n"
        "  --variants LIST         tblog_block,spdlog_async,quill (default all)\n"
        "  --threads LIST          flood thread counts (default 1,2,4,8,16)\n"
        "  --paced-threads LIST    paced thread counts (default 1,4,8)\n"
        "  --flood-lines N         lines per flood run, split over the threads (default 1000000)\n"
        "  --paced-lines N         lines per thread per paced run (default 50000)\n"
        "  --rate N                paced lines/s per thread (default 50000)\n"
        "  --dir DIR               scratch directory (default: <temp>/tblog-libs-<pid>)\n"
        "  --out FILE | --out-dir DIR   result JSON (default name <os>-<compiler>-<cpu>-<date>-libs.json)\n"
        "  --label S --build-info S --commit SHA   recorded in the JSON\n");
}

}  // namespace

int main(int argc, char **argv) {
    const Args args(argc, argv);
    if (args.flag("--help") || args.flag("-h")) {
        usage();
        return 0;
    }
    if (!args.get("--cell", "").empty()) return child_main(args);

    const bool quick = args.flag("--quick");
    const int runs = (int)args.num("--runs", quick ? 1 : 5);
    const int warmup = (int)args.num("--warmup", quick ? 0 : 1);
    const std::vector<std::string> scenarios = split(args.get("--scenarios", "flood,paced"));
    const std::vector<std::string> vnames = split(args.get("--variants", "tblog_block,spdlog_async,quill"));
    const std::vector<int> flood_threads = int_list(args.get("--threads", quick ? "1,4" : "1,2,4,8,16"));
    const std::vector<int> paced_threads = int_list(args.get("--paced-threads", quick ? "1,4" : "1,4,8"));
    const long flood_lines = (long)args.num("--flood-lines", quick ? 20000 : 1000000);
    const long paced_lines = (long)args.num("--paced-lines", quick ? 2000 : 50000);
    const uint64_t rate = (uint64_t)args.num("--rate", 50000);
    for (const auto &v : vnames)
        if (!find_variant(v)) {
            std::fprintf(stderr, "unknown variant %s\n", v.c_str());
            return 2;
        }
    calibrate_ticks();
    const TimerOverhead tov = timer_overhead();
    const std::string base = args.get("--dir", temp_root() + "/tblog-libs-" + std::to_string(pid()));
    make_dirs(base);
#if defined(_WIN32)
    char self[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, self, MAX_PATH);
    const std::string exe = len > 0 && len < MAX_PATH ? std::string(self, len) : std::string(argv[0]);
#else
    const std::string exe = argv[0];
#endif
    const uint64_t t_start = now_ns();
    std::vector<Cell> cells;
    bool all_ok = true;
    int seq = 0;
    for (const auto &scn : scenarios) {
        const std::vector<int> &tl = scn == "paced" ? paced_threads : flood_threads;
        for (int t : tl) {
            std::vector<Cell> row;
            for (const auto &v : vnames) row.push_back(Cell{scn, v, t, {}, {}, true});
            for (int r = 0; r < warmup + runs; ++r) {
                for (size_t k = 0; k < vnames.size(); ++k) {
                    Cell &c = row[(k + (size_t)r) % vnames.size()];  // rotating order
                    const std::string dir = base + "/run" + std::to_string(seq++);
                    std::vector<std::string> a = {"--cell", scn, "--variant", c.variant, "--threads",
                                                  std::to_string(t), "--dir", dir, "--flood-lines",
                                                  std::to_string(flood_lines), "--paced-lines",
                                                  std::to_string(paced_lines), "--rate", std::to_string(rate)};
                    ChildOut o = run_child(exe, a);
                    remove_tree(dir);
                    if (r < warmup) continue;
                    if (!o.ok) {
                        c.all_ok = all_ok = false;
                        std::fprintf(stderr, "  !! %s %s %d thr: run failed or incomplete log file\n", scn.c_str(),
                                     c.variant.c_str(), t);
                    }
                    if (!o.json.empty()) c.runs.push_back(o.json);
                    if (!o.m.empty()) c.metrics.push_back(o.m);
                }
            }
            for (Cell &c : row) {
                std::printf("%-6s %-13s %2d thr: %zu runs\n", scn.c_str(), c.variant.c_str(), t, c.metrics.size());
                cells.push_back(std::move(c));
            }
            std::fflush(stdout);
        }
    }
    remove_tree(base);
    const double wall = double(now_ns() - t_start) / 1e9;

    Obj vd;
    for (const Variant &v : kVariants) vd.str(v.name, v.desc);
    std::vector<std::string> cj, sl, vl, ft, pt;
    for (const Cell &c : cells) cj.push_back(cell_json(c));
    for (const auto &s : scenarios) sl.push_back(jstr(s));
    for (const auto &v : vnames) vl.push_back(jstr(v));
    for (int t : flood_threads) ft.push_back(jnum(t));
    for (int t : paced_threads) pt.push_back(jnum(t));
    const std::string config =
        Obj()
            .str("program", "bench_vs_libs")
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
            .str("per_call_timer", tick_source())
            .num("ns_per_tick", ns_per_tick())
            .raw("timer_overhead_ns", Obj().num("min", tov.min_ns).num("median", tov.median_ns).done())
            .num("steady_clock_step_ns", tov.steady_resolution_ns)
            .str("libraries", "spdlog 1.17.0, Quill 13.1.0 (header-only, downloaded by CMake)")
            .str("process_per_run", "every measured run is a fresh process (Quill's backend starts once per process)")
            .str("e2e_definition",
                 "start until every line is written and the file closed and fsynced: tblog_shutdown; spdlog / Quill: "
                 "flush + shutdown, then reopen + fsync")
            .num("wall_s", wall)
            .done();
    const std::string json = Obj()
                                 .str("schema", "tblog-bench-vs-libs/1")
                                 .raw("env", env_json(args, base))
                                 .raw("config", config)
                                 .raw("variants", vd.done())
                                 .raw("results", "[\n    " + [&]() -> std::string {
                                     std::string s;
                                     for (size_t i = 0; i < cj.size(); ++i) s += (i ? ",\n    " : "") + cj[i];
                                     return s;
                                 }() + "\n  ]")
                                 .done();
    const std::string path = save_json(args, "libs", json);
    std::printf("wall %.1f s; results: %s%s\n", wall, path.c_str(), all_ok ? "" : " (SOME RUNS FAILED)");
    return path.empty() || !all_ok ? 1 : 0;
}
