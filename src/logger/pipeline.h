// Pipeline — src/logger/pipeline.cpp: one Queue and the ordered list of Sinks
// it feeds. The worker loop (logger.cpp) calls drain() once per cycle; the
// syncer calls sync(); tblog_shutdown calls shutdown(); the crash handler
// calls crash().
//
// drain(), one worker cycle:
//   take the queue -> every sink: begin_batch() -> for each chunk, in order:
//   write it to the crash-safe sinks, mark it written (the crash handler will
//   not write it again), write it to the other sinks -> publish the batch as
//   written (Queue::publish_written: callers waiting under flush.on_level
//   return) -> "tblog: N lines dropped" note if the drop counter moved ->
//   every sink: end_batch() ->
//   release the buffers -> flush() every sink if anything was written (or
//   force_flush). Every write to a crash-safe sink is bracketed by
//   Queue::begin_write() / end_write(), so a crash handler waits for the one
//   in flight and then drains exactly the rest. Once the queue crashed,
//   nothing is written here any more.
//
// crash() (async-signal-safe): Queue::crash_drain() into every crash-safe
// sink, in sink order, then the crash marker to the same sinks. Lines logged
// after that are written synchronously by the logging thread to the same
// sinks (after the marker), through crash_visit.
#pragma once

#include "queue/queue.h"
#include "sinks/sink.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace tblog {

class Pipeline {
public:
    explicit Pipeline(std::unique_ptr<Queue> q);
    ~Pipeline();  // shutdown()
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    // Setup, before the worker starts and before the crash handler is
    // installed. Null sinks are ignored. Sinks are written in this order.
    void add_sink(std::unique_ptr<LogSink> s);
    size_t sink_count() const noexcept { return sinks_.size(); }

    Queue &queue() noexcept { return *queue_; }

    // ---- worker: one cycle (see above). No-op after shutdown().
    void drain(bool force_flush = false);
    // ---- syncer (or worker): every sink's sync().
    void sync();
    // ---- any thread, once the worker has stopped: close the queue, final
    // drain + flush, LogSink::shutdown() on every sink. Idempotent.
    void shutdown();

    // ---- signal handler: async-signal-safe. marker may be null.
    // others_running = false (process exit, the other threads are gone): do
    // not wait for the worker's write in flight / appends in progress.
    void crash(const char *marker, size_t len, bool others_running = true) noexcept;
    // True if a crash-safe sink writes to stderr (it then gets the marker).
    bool crash_covers_stderr() const noexcept { return crash_stderr_; }

private:
    static void crash_visit(void *ctx, const char *data, size_t len) noexcept;

    std::unique_ptr<Queue> queue_;
    std::vector<std::unique_ptr<LogSink>> owned_;
    std::vector<LogSink *> sinks_;        // all, in order
    std::vector<LogSink *> crash_sinks_;  // crash_safe(), in order (read by crash())
    std::vector<LogSink *> other_sinks_;  // the rest, in order
    bool crash_stderr_ = false;

    std::mutex mu_;  // drain / shutdown
    uint64_t reported_dropped_ = 0;
    bool shut_ = false;
};

}  // namespace tblog
