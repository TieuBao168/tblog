// Queue — src/queue/queue.cpp. The caller-side half of the logger: sharded
// double buffers that tblog_write() appends formatted lines to, and that the
// worker takes in batches and hands to the sinks (src/logger/pipeline.h).
//
// Threads:
//   append()                       any thread (hot path; no syscall in the
//                                  common case, no allocation, no virtual call)
//   wait_for_work/take/begin_write/mark_written/end_write/publish_written/
//   release/consume_urgent         the worker only (one thread at a time)
//   wake(), close(), dropped(), set_flush_level()
//                                  any thread
//   crash_drain(), crashed()       signal handler: async-signal-safe
//
// Batch protocol (worker):  take(b) -> for each chunk i, in order:
// begin_write() (false: the queue crashed, write nothing more), write
// b.chunks[i] to every crash-safe sink, mark_written(b, i), end_write(),
// write it to the other sinks -> (every chunk in every sink)
// publish_written(b) -> release(b). Every other write to a crash-safe sink
// (the time-rotation check of begin_batch, the "lines dropped" note) also
// happens between
// begin_write() and end_write(). Exactly one batch may be outstanding (take()
// again only after release()). Each chunk holds whole lines only; one chunk
// per shard.
//
// After a crash (crash_drain() started), append() no longer buffers: it
// waits until the drain and its marker are written, then writes the line
// itself, synchronously, through the crash_drain() visitor (the crash-safe
// sinks), one thread at a time. See queue.cpp, "Crash safety".
//
// flush.on_level (queue.cpp, "Waiting for the write"): a line at or above
// the flush level is appended as usual; then append() waits (bounded) until
// the worker has written the batch that holds it to the sinks
// (publish_written), so the line is in the OS (page cache) when the call
// returns and survives a kill of the process.
#pragma once

#include "core/compat.h"
#include "tblog/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace tblog {

// full_policy: what append() does when the caller's shard is full.
enum class FullPolicy { Drop, Block };

struct QueueOptions {
    size_t buffer_size = 4u << 20;               // buffer.size (per buffer, A and B)
    FullPolicy policy = FullPolicy::Block;       // full_policy
    tblog_level urgent_level = TBLOG_LV_ERROR;   // sync.on_level: wakes worker + marks urgent
    tblog_level flush_level = TBLOG_LV_OFF;      // flush.on_level: caller waits for the write
    unsigned flush_wait_ms = 2000;               // upper bound of that wait
};

class Queue : public CacheAlignedNew {
public:
    static constexpr size_t kMaxShards = 16;

    struct Chunk {
        const char *data;
        size_t len;
    };
    // The full buffers of one take(), oldest data of each shard first. Shards
    // are listed in shard order; lines of different threads may therefore
    // appear out of global time order by at most one flush interval.
    struct Batch {
        size_t size = 0;
        Chunk chunks[kMaxShards];
        unsigned char shard[kMaxShards];  // internal: shard of chunks[i]
        uint64_t gen[kMaxShards];         // internal: that shard's swap generation after the take
    };

    // nullptr (after a message on stderr) if the buffers cannot be allocated.
    static std::unique_ptr<Queue> create(const QueueOptions &opt);
    ~Queue();
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;

    // ---- caller threads (hot path). `data` is one complete line ending in
    // '\n'. Returns false if the line was dropped (full under Drop, longer
    // than a shard, or the queue is closed). Once the queue crashed the line
    // is written synchronously to the crash-safe sinks, after the crash
    // drain, and true is returned.
    // lv >= flush level and the line was buffered: returns only once the
    // worker published the batch holding it (publish_written), or the queue
    // closed / crashed, or flush_wait_ms passed. Never waits on the worker
    // thread itself (a sink that logs).
    bool append(const char *data, uint32_t len, tblog_level lv) noexcept;

    // ---- worker thread
    // Block until append() signalled work (a shard 75% full / urgent line),
    // wake() was called, or timeout.
    void wait_for_work(std::chrono::milliseconds timeout);
    // True once since the last call if an urgent (>= urgent_level) line arrived.
    bool consume_urgent() noexcept;
    // Swap every non-empty shard; out receives the full buffers. Returns
    // out.size. Wakes producers blocked by the Block policy.
    size_t take(Batch &out) noexcept;
    // Claim the crash-safe sinks for one write (lock-free, never waits).
    // false once the queue crashed: the worker writes nothing more there (the
    // crash handler drains what is left). Pair every true with end_write().
    bool begin_write() noexcept;
    void end_write() noexcept;
    // Chunk i reached every crash-safe sink: the crash handler must not
    // write it again. Called before end_write().
    void mark_written(const Batch &b, size_t i) noexcept;
    // Every chunk of `b` reached every sink (write(), not fsync): wake the
    // callers waiting for it (flush.on_level). Not called when the batch was
    // cut short by a crash (those callers see crashed() instead).
    void publish_written(const Batch &b) noexcept;
    // Hand the buffers of `b` back for reuse (b is emptied). Once the queue
    // crashed, chunks not marked written are kept for crash_drain().
    void release(Batch &b) noexcept;

    // ---- any thread
    // Wake a thread blocked in wait_for_work (shutdown / tblog_flush).
    void wake() noexcept;
    // Reject every later append (they return false) and release producers
    // blocked by the Block policy. Appends that were already inside their
    // critical section complete first, so a take() after close() returns
    // everything that was accepted. Idempotent.
    void close() noexcept;
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    // flush.on_level (hot-reload). TBLOG_LV_OFF: no caller ever waits.
    void set_flush_level(tblog_level lv) noexcept {
        flush_level_.store(static_cast<int>(lv), std::memory_order_relaxed);
    }
    tblog_level flush_level() const noexcept {
        return static_cast<tblog_level>(flush_level_.load(std::memory_order_relaxed));
    }

    // ---- signal handler: async-signal-safe ONLY (no locks, no malloc).
    // Closes the queue, waits (bounded) for the worker's current write to the
    // crash-safe sinks and for appends inside their critical section, then
    // calls visit(ctx, data, len) for every byte that has not reached the
    // crash-safe sinks yet, shard by shard, oldest first, and last for `tail`
    // (the crash marker; may be null). The first call returns true. A later
    // call returns false and only writes its `tail`, the way a post-crash
    // append() does. Afterwards the worker writes nothing more (begin_write()
    // fails) and append() writes through `visit`, so nothing is written twice.
    // others_running = false: the other threads are gone or must not be
    // waited for (process exit): no waiting at all.
    bool crash_drain(void (*visit)(void *ctx, const char *data, size_t len), void *ctx,
                     const char *tail = nullptr, size_t tail_len = 0,
                     bool others_running = true) noexcept;
    bool crashed() const noexcept { return crashed_.load(std::memory_order_acquire); }

    // Per-shard capacity in bytes (a line longer than this is always dropped).
    size_t shard_capacity() const noexcept { return cap_; }

private:
    struct Shard;
    explicit Queue(const QueueOptions &opt);
    bool init();
    void notify_worker() noexcept;
    void notify_worker_force() noexcept;
    bool append_closed(const char *data, size_t len) noexcept;
    bool write_after_crash(const char *data, size_t len) noexcept;
    bool wait_for_swap(Shard &sh, uint64_t gen) noexcept;
    void wait_written(Shard &sh, uint64_t gen) noexcept;
    void wake_blocked() noexcept;
    void wake_flush_waiters() noexcept;

    const size_t nshards_;
    const size_t mask_;
    const size_t cap_;        // per shard buffer
    const size_t threshold_;  // 75% of cap_
    const FullPolicy policy_;
    const tblog_level urgent_level_;
    const unsigned flush_wait_ms_;
    Shard *const shards_;

    // Read on every append; written once at close/crash (closed_) and on a
    // hot-reload (flush_level_).
    alignas(64) std::atomic<bool> closed_{false};
    std::atomic<int> flush_level_;
    // Written rarely by producers (first notifier per take / urgent lines).
    alignas(64) std::atomic<bool> notified_{false};
    std::atomic<bool> urgent_{false};
    alignas(64) std::atomic<uint64_t> dropped_{0};

    // Crash state (queue.cpp, "Crash safety"). writer_: id of the thread
    // writing to the crash-safe sinks (worker / crash handler / a post-crash
    // append), 0 = none.
    alignas(64) std::atomic<bool> crashed_{false};  // crash_drain() started
    std::atomic<bool> crash_done_{false};           // ... and finished (tail written)
    std::atomic<uint32_t> writer_{0};
    std::atomic<void (*)(void *, const char *, size_t)> crash_visit_{nullptr};
    std::atomic<void *> crash_ctx_{nullptr};

    alignas(64) std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    bool wake_req_ = false;

    std::mutex block_mu_;
    std::condition_variable block_cv_;
    std::atomic<int> block_waiters_{0};

    // flush.on_level waiters (wait_written / publish_written).
    std::mutex written_mu_;
    std::condition_variable written_cv_;
    std::atomic<int> written_waiters_{0};
    std::atomic<uint32_t> worker_id_{0};  // thread of the last take() (never waits on itself)
};

}  // namespace tblog
