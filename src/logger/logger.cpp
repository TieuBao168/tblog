// Public C API + worker thread. Glues config, formatter, backend, sinks and
// the crash handler together.
#include "backend/backend.h"
#include "config/config.h"
#include "core/compat.h"
#include "core/formatter.h"
#include "core/thread_context.h"
#include "crash/crash_handler.h"
#include "platform/platform.h"
#include "sinks/sink.h"
#include "tblog/log.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace tblog {
namespace {

using Clock = std::chrono::steady_clock;

struct Runtime {
    Config cfg;
    std::string config_path;
    std::unique_ptr<Backend> backend;
    std::vector<std::unique_ptr<Sink>> sinks;

    // Formatter can be swapped by hot-reload. Old ones are retired, never
    // freed while the runtime lives, because a caller thread may still use one.
    std::atomic<const Formatter *> formatter{nullptr};
    std::vector<std::unique_ptr<Formatter>> formatters;  // owned (worker/init only)

    std::atomic<uint32_t> flush_interval_ms{200};
    std::atomic<uint32_t> sync_interval_ms{1000};

    std::unique_ptr<ConfigWatcher> watcher;
    std::thread worker;
    std::atomic<bool> stop{false};

    // fdatasync runs on its own thread so a slow disk never stalls the worker
    // (which must keep swapping and writing buffers).
    std::thread syncer;
    std::mutex sync_mu;
    std::condition_variable sync_cv;
    bool sync_req = false;   // guarded by sync_mu
    bool sync_stop = false;  // guarded by sync_mu

    // tblog_flush() handshake.
    std::mutex flush_mu;
    std::condition_variable flush_cv;
    uint64_t flush_req = 0;   // guarded by flush_mu
    uint64_t flush_done = 0;  // guarded by flush_mu
};

// The runtime is published here. It is intentionally never deleted: a caller
// thread may have loaded the pointer just before shutdown. After shutdown the
// backend rejects appends, so a stale pointer is harmless.
std::atomic<Runtime *> g_rt{nullptr};
std::mutex g_life_mu;  // serialises init/shutdown
// Shut-down runtimes stay reachable here (so leak checkers stay quiet). Each
// init/shutdown cycle keeps its backend's buffers: apps should init once.
// Heap-allocated and never destroyed, so it outlives static destructors.
std::vector<Runtime *> *const g_retired = new std::vector<Runtime *>;  // guarded by g_life_mu
std::atomic<uint64_t> g_dropped_after_shutdown{0};

const Formatter &default_formatter() {
    static Formatter f(Config().pattern);
    return f;
}

void apply_levels(const Config &c) {
    tblog_set_level(nullptr, c.level);
    for (const auto &ml : c.module_levels) tblog_set_level(ml.first.c_str(), ml.second);
}

void print_warnings(const std::vector<std::string> &w) {
    for (const auto &s : w) std::fprintf(stderr, "tblog: config: %s\n", s.c_str());
}

// Hot-reload: only settings that are safe to change at runtime.
void reload(Runtime *rt) {
    Config next = rt->cfg;
    std::vector<std::string> warnings;
    if (!load_config(rt->config_path.c_str(), next, &warnings)) return;  // keep old
    validate_config(next, &warnings);
    print_warnings(warnings);

    apply_levels(next);
    if (next.pattern != rt->cfg.pattern) {
        rt->formatters.push_back(tblog::make_unique<Formatter>(next.pattern));
        rt->formatter.store(rt->formatters.back().get(), std::memory_order_release);
    }
    rt->flush_interval_ms.store(next.flush_interval_ms, std::memory_order_relaxed);
    rt->sync_interval_ms.store(next.sync_interval_ms, std::memory_order_relaxed);

    if (next.file.path != rt->cfg.file.path || next.buffer_size != rt->cfg.buffer_size ||
        next.console != rt->cfg.console || next.syslog != rt->cfg.syslog) {
        std::fprintf(stderr, "tblog: config: file/buffer/sink changes need a restart\n");
    }
    // Keep the restart-only fields as they are actually running.
    next.file = rt->cfg.file;
    next.buffer_size = rt->cfg.buffer_size;
    next.full_policy = rt->cfg.full_policy;
    next.console = rt->cfg.console;
    next.syslog = rt->cfg.syslog;
    rt->cfg = std::move(next);
}

void request_sync(Runtime *rt) {
    {
        std::lock_guard<std::mutex> lk(rt->sync_mu);
        rt->sync_req = true;
    }
    rt->sync_cv.notify_one();
}

// Periodic fdatasync (sync.interval_ms) plus on-demand syncs for urgent lines.
void syncer_main(Runtime *rt) {
    crash_altstack_for_this_thread();
    std::unique_lock<std::mutex> lk(rt->sync_mu);
    for (;;) {
        const uint32_t ms = rt->sync_interval_ms.load(std::memory_order_relaxed);
        auto ready = [&] { return rt->sync_req || rt->sync_stop; };
        if (ms > 0)
            rt->sync_cv.wait_for(lk, std::chrono::milliseconds(ms), ready);
        else
            rt->sync_cv.wait(lk, ready);
        if (rt->sync_stop) return;
        rt->sync_req = false;
        lk.unlock();
        rt->backend->sync();
        lk.lock();
    }
}

void worker_main(Runtime *rt) {
    crash_altstack_for_this_thread();
    auto last_watch = Clock::now();
    for (;;) {
        const bool stopping = rt->stop.load(std::memory_order_acquire);
        uint64_t req;
        {
            std::lock_guard<std::mutex> lk(rt->flush_mu);
            req = rt->flush_req;
        }
        const bool flush_wanted = [&] {
            std::lock_guard<std::mutex> lk(rt->flush_mu);
            return rt->flush_done < req;
        }();

        if (!stopping && !flush_wanted)
            rt->backend->wait_for_work(
                std::chrono::milliseconds(rt->flush_interval_ms.load(std::memory_order_relaxed)));

        rt->backend->flush();
        for (auto &s : rt->sinks) s->flush();

        const auto now = Clock::now();
        if (rt->backend->consume_urgent()) request_sync(rt);
        // tblog_flush() promises durability on return: sync inline (rare).
        if (flush_wanted) rt->backend->sync();

        if (req) {
            std::lock_guard<std::mutex> lk(rt->flush_mu);
            if (rt->flush_done < req) {
                rt->flush_done = req;
                rt->flush_cv.notify_all();
            }
        }

        if (stopping) break;

        if (rt->watcher && now - last_watch >= std::chrono::seconds(1)) {
            last_watch = now;
            if (rt->watcher->changed()) reload(rt);
        }
    }
}

std::unique_ptr<Backend> make_backend(const Config &c) {
    BufferOptions o;
    o.file = c.file;
    o.buffer_size = c.buffer_size;
    o.policy = c.full_policy;
    return make_double_buffer_backend(o);
}

}  // namespace
}  // namespace tblog

using namespace tblog;

extern "C" int tblog_init(const char *config_path) {
    std::lock_guard<std::mutex> life(g_life_mu);
    if (g_rt.load(std::memory_order_acquire)) return 0;  // already running

    auto rt = tblog::make_unique<Runtime>();
    std::vector<std::string> warnings;
    if (config_path && *config_path) {
        rt->config_path = config_path;
        if (!load_config(config_path, rt->cfg, &warnings))
            std::fprintf(stderr, "tblog: cannot read %s, using defaults\n", config_path);
    }
    validate_config(rt->cfg, &warnings);
    print_warnings(warnings);

    rt->backend = make_backend(rt->cfg);
    if (!rt->backend) return -1;

    if (rt->cfg.console) rt->sinks.push_back(make_console_sink(rt->cfg.console_color));
    if (rt->cfg.syslog) {
        if (std::unique_ptr<Sink> s = make_syslog_sink("tblog")) rt->sinks.push_back(std::move(s));
    }
    for (auto &s : rt->sinks) rt->backend->add_sink(s.get());

    rt->formatters.push_back(tblog::make_unique<Formatter>(rt->cfg.pattern));
    rt->formatter.store(rt->formatters.back().get(), std::memory_order_release);
    rt->flush_interval_ms.store(rt->cfg.flush_interval_ms);
    rt->sync_interval_ms.store(rt->cfg.sync_interval_ms);
    if (rt->cfg.reload_watch && !rt->config_path.empty())
        rt->watcher = tblog::make_unique<ConfigWatcher>(rt->config_path);

    apply_levels(rt->cfg);

    Runtime *raw = rt.release();
    raw->worker = std::thread(worker_main, raw);
    raw->syncer = std::thread(syncer_main, raw);
    install_crash_handler(raw->backend.get());
    g_rt.store(raw, std::memory_order_release);

    static bool atexit_done = false;
    if (!atexit_done) {
        atexit_done = true;
        std::atexit(tblog_shutdown);
    }
    return 0;
}

extern "C" void tblog_shutdown(void) {
    std::lock_guard<std::mutex> life(g_life_mu);
    Runtime *rt = g_rt.exchange(nullptr, std::memory_order_acq_rel);
    if (!rt) return;
    uninstall_crash_handler();
    rt->stop.store(true, std::memory_order_release);
    rt->backend->wake();
    if (rt->worker.joinable()) rt->worker.join();
    {
        std::lock_guard<std::mutex> lk(rt->sync_mu);
        rt->sync_stop = true;
    }
    rt->sync_cv.notify_one();
    if (rt->syncer.joinable()) rt->syncer.join();
    rt->backend->shutdown();  // final flush + fdatasync
    for (auto &s : rt->sinks) s->flush();
    g_dropped_after_shutdown.fetch_add(rt->backend->dropped(), std::memory_order_relaxed);
    g_retired->push_back(rt);  // never deleted (see g_rt)
}

extern "C" void tblog_flush(void) {
    Runtime *rt = g_rt.load(std::memory_order_acquire);
    if (!rt) return;
    std::unique_lock<std::mutex> lk(rt->flush_mu);
    const uint64_t ticket = ++rt->flush_req;
    rt->backend->wake();
    rt->flush_cv.wait_for(lk, std::chrono::seconds(2), [&] { return rt->flush_done >= ticket; });
}

extern "C" uint64_t tblog_dropped_count(void) {
    uint64_t n = g_dropped_after_shutdown.load(std::memory_order_relaxed);
    if (Runtime *rt = g_rt.load(std::memory_order_acquire)) n += rt->backend->dropped();
    return n;
}

extern "C" void tblog_write(tblog_module *m, tblog_level lv, const char *file, int line,
                            const char *func, const char *fmt, ...) {
    ThreadContext &tc = thread_ctx();
    Record rec{now_ns(), tc.tid, lv, m ? m->name : "", file, line, func};

    Runtime *rt = g_rt.load(std::memory_order_acquire);
    const Formatter *f = rt ? rt->formatter.load(std::memory_order_acquire) : &default_formatter();

    va_list ap;
    va_start(ap, fmt);
    const size_t n = f->format(tc.scratch, kScratchSize, rec, fmt, ap);
    va_end(ap);

    if (!rt || !rt->backend->append(tc.scratch, static_cast<uint32_t>(n), lv)) {
        // Not initialised / already shut down -> stderr. (A full buffer under
        // the Drop policy also lands here only when the backend is stopped.)
        if (!rt || rt->stop.load(std::memory_order_relaxed)) pf::stderr_write(tc.scratch, n);
    }

    if (lv == TBLOG_LV_FATAL) {
        tblog_flush();
        std::abort();
    }
}
