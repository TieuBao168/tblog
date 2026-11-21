// Public C API + worker thread. Glues config, formatter, queue, sinks and
// the crash handler together:
//
//   caller -- tblog_write: level check -> format into the thread's scratch
//             buffer -> Queue::append (src/queue/queue.h)
//   worker -- worker_main: wait -> Pipeline::drain (take the queue, write the
//             chunks to every sink, release, flush) -> urgent line? request a
//             sync -> tblog_flush handshake (src/logger/pipeline.h)
//   syncer -- syncer_main: Pipeline::sync (fdatasync) every sync.interval_ms
//             and on urgent lines
//   crash  -- Pipeline::crash from the signal handler (src/crash/)
//   exit   -- tblog_shutdown from atexit while the process is being torn down
//             (Windows DLL unload): shutdown_at_exit, no joins (see below)
#include "config/config.h"
#include "core/compat.h"
#include "core/formatter.h"
#include "core/thread_context.h"
#include "crash/crash_handler.h"
#include "logger/pipeline.h"
#include "platform/platform.h"
#include "queue/queue.h"
#include "sinks/file_sink.h"
#include "sinks/sink.h"
#include "tblog/log.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace tblog {
namespace {

using Clock = std::chrono::steady_clock;

struct Runtime {
    Config cfg;
    std::string config_path;
    std::unique_ptr<Pipeline> pipeline;
    Queue *queue = nullptr;  // &pipeline->queue(), for the hot path
    size_t user_sinks = 0;   // tblog_add_sink() sinks in this runtime

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
// queue rejects appends, so a stale pointer is harmless.
std::atomic<Runtime *> g_rt{nullptr};
std::mutex g_life_mu;  // serialises init/shutdown
// Shut-down runtimes stay reachable here (so leak checkers stay quiet). Each
// init/shutdown cycle keeps its queue's buffers: apps should init once.
// Heap-allocated and never destroyed, so it outlives static destructors.
std::vector<Runtime *> *const g_retired = new std::vector<Runtime *>;  // guarded by g_life_mu
std::atomic<uint64_t> g_dropped_after_shutdown{0};

// tblog_add_sink() registrations: kept across init/shutdown cycles; each
// tblog_init wraps them in CallbackSinks. Guarded by g_life_mu.
struct UserSink {
    tblog_sink_write_fn write;
    tblog_sink_flush_fn flush;
    void *ctx;
};
std::vector<UserSink> *const g_user_sinks = new std::vector<UserSink>;  // never destroyed

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
    validate_config(next, &warnings, rt->user_sinks);
    print_warnings(warnings);

    apply_levels(next);
    if (next.pattern != rt->cfg.pattern) {
        rt->formatters.push_back(tblog::make_unique<Formatter>(next.pattern));
        rt->formatter.store(rt->formatters.back().get(), std::memory_order_release);
    }
    rt->flush_interval_ms.store(next.flush_interval_ms, std::memory_order_relaxed);
    rt->sync_interval_ms.store(next.sync_interval_ms, std::memory_order_relaxed);

    if (next.file_enabled != rt->cfg.file_enabled || next.file.path != rt->cfg.file.path ||
        next.buffer_size != rt->cfg.buffer_size || next.console != rt->cfg.console ||
        next.syslog != rt->cfg.syslog) {
        std::fprintf(stderr, "tblog: config: file/buffer/sink changes need a restart\n");
    }
    // Keep the restart-only fields as they are actually running.
    next.file_enabled = rt->cfg.file_enabled;
    next.file = rt->cfg.file;
    next.urgent_level = rt->cfg.urgent_level;
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
        rt->pipeline->sync();
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
            rt->queue->wait_for_work(
                std::chrono::milliseconds(rt->flush_interval_ms.load(std::memory_order_relaxed)));

        // take -> every sink: begin_batch, write the chunks, end_batch ->
        // release -> flush the sinks (always on tblog_flush, so a user sink's
        // flush callback runs even when nothing new was written).
        rt->pipeline->drain(flush_wanted);

        const auto now = Clock::now();
        if (rt->queue->consume_urgent()) request_sync(rt);
        // tblog_flush() promises durability on return: sync inline (rare).
        if (flush_wanted) rt->pipeline->sync();

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

// The queue and the sinks in their write order: file, console, syslog, user
// sinks. nullptr if the queue or the file cannot be set up (already reported).
std::unique_ptr<Pipeline> make_pipeline(const Config &c, const std::vector<UserSink> &user) {
    QueueOptions qo;
    qo.buffer_size = c.buffer_size;
    qo.policy = c.full_policy;
    qo.urgent_level = c.urgent_level;
    std::unique_ptr<Queue> q = Queue::create(qo);
    if (!q) return nullptr;
    std::unique_ptr<Pipeline> p = tblog::make_unique<Pipeline>(std::move(q));
    if (c.file_enabled) {
        std::unique_ptr<LogSink> f = make_file_sink(c.file);
        if (!f) return nullptr;
        p->add_sink(std::move(f));
    }
    if (c.console) p->add_sink(make_console_sink(c.console_color));
    if (c.syslog) p->add_sink(make_syslog_sink("tblog"));  // nullptr where unsupported
    for (const UserSink &u : user) p->add_sink(make_callback_sink(u.write, u.flush, u.ctx));
    if (p->sink_count() == 0) {  // file = false + syslog on a platform without one
        std::fprintf(stderr, "tblog: no usable sink, logging to the console\n");
        p->add_sink(make_console_sink(c.console_color));
    }
    return p;
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
    rt->user_sinks = g_user_sinks->size();
    validate_config(rt->cfg, &warnings, rt->user_sinks);
    print_warnings(warnings);

    rt->pipeline = make_pipeline(rt->cfg, *g_user_sinks);
    if (!rt->pipeline) return -1;
    rt->queue = &rt->pipeline->queue();

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
    install_crash_handler(raw->pipeline.get());
    g_rt.store(raw, std::memory_order_release);

    static bool atexit_done = false;
    if (!atexit_done) {
        atexit_done = true;
        std::atexit(tblog_shutdown);
    }
    return 0;
}

namespace tblog {
namespace {

// tblog_shutdown() while the process is going away (pf::process_exiting():
// Windows, from the atexit handler of tblog.dll -- or of a static tblog inside
// another DLL -- which runs under the loader lock after ExitProcess killed the
// other threads). Joining the worker/syncer would deadlock or wait on threads
// that are gone, and any lock they held stays held. So: no join, no wait, no
// allocation; drain the queue on this thread through the lock-free crash path
// into the crash-safe sinks (file, console). User sinks do not get these last
// lines (as after a crash). The file's handle is closed by the OS; what was
// written is in the OS cache and survives the process.
void shutdown_at_exit() noexcept {
    std::unique_lock<std::mutex> life(g_life_mu, std::try_to_lock);  // never block here
    Runtime *rt = g_rt.exchange(nullptr, std::memory_order_acq_rel);
    if (!rt) return;
    uninstall_crash_handler();
    rt->stop.store(true, std::memory_order_release);
    rt->pipeline->crash(nullptr, 0);  // closes the queue: later appends go to stderr
}

}  // namespace
}  // namespace tblog

extern "C" void tblog_shutdown(void) {
    if (pf::process_exiting()) {
        shutdown_at_exit();
        return;
    }
    std::lock_guard<std::mutex> life(g_life_mu);
    Runtime *rt = g_rt.exchange(nullptr, std::memory_order_acq_rel);
    if (!rt) return;
    uninstall_crash_handler();
    rt->stop.store(true, std::memory_order_release);
    rt->queue->wake();
    if (rt->worker.joinable()) rt->worker.join();
    {
        std::lock_guard<std::mutex> lk(rt->sync_mu);
        rt->sync_stop = true;
    }
    rt->sync_cv.notify_one();
    if (rt->syncer.joinable()) rt->syncer.join();
    // Close the queue, final drain + flush, LogSink::shutdown (fdatasync + close).
    rt->pipeline->shutdown();
    g_dropped_after_shutdown.fetch_add(rt->queue->dropped(), std::memory_order_relaxed);
    g_retired->push_back(rt);  // never deleted (see g_rt)
}

extern "C" void tblog_flush(void) {
    Runtime *rt = g_rt.load(std::memory_order_acquire);
    if (!rt) return;
    std::unique_lock<std::mutex> lk(rt->flush_mu);
    const uint64_t ticket = ++rt->flush_req;
    rt->queue->wake();
    rt->flush_cv.wait_for(lk, std::chrono::seconds(2), [&] { return rt->flush_done >= ticket; });
}

extern "C" uint64_t tblog_dropped_count(void) {
    uint64_t n = g_dropped_after_shutdown.load(std::memory_order_relaxed);
    if (Runtime *rt = g_rt.load(std::memory_order_acquire)) n += rt->queue->dropped();
    return n;
}

extern "C" int tblog_add_sink(tblog_sink_write_fn write, tblog_sink_flush_fn flush, void *ctx) {
    if (!write) return -1;
    std::lock_guard<std::mutex> life(g_life_mu);
    if (g_rt.load(std::memory_order_acquire)) return -1;  // running: add before tblog_init
    try {
        g_user_sinks->push_back(UserSink{write, flush, ctx});
    } catch (...) {
        return -1;
    }
    return 0;
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

    if (!rt || !rt->queue->append(tc.scratch, static_cast<uint32_t>(n), lv)) {
        // Not initialised / already shut down -> stderr. (A full buffer under
        // the Drop policy also lands here only when the queue is stopped.)
        if (!rt || rt->stop.load(std::memory_order_relaxed)) pf::stderr_write(tc.scratch, n);
    }

    if (lv == TBLOG_LV_FATAL) {
        tblog_flush();
        std::abort();
    }
}
