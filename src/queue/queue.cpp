// Queue: sharded double buffers (see queue.h for the interface). Callers
// memcpy into the active buffer of their own shard under a short per-shard
// spinlock; the worker swaps every shard's buffers (take()) and hands the
// full ones to the sinks outside the locks, then gives them back (release()).
//
// Sharding:
//   N = min(16, next_pow2(hardware_concurrency())), at least 1. Each shard is
//   the old single-buffer structure (cache-line-aligned spinlock, two buffers,
//   committed lengths, active index). Per-shard buffer size is
//   max(64 KB, buffer_size / N), so total memory = 2 x N x shard size
//   (e.g. 4 MB default -> 16 x 2 x 256 KB = 8 MB, same as before; tiny
//   buffer_size values are rounded up to 64 KB per shard).
//   A caller thread picks its shard once (thread_local id handed out
//   round-robin from an atomic counter) and always uses it, so each thread's
//   own lines stay in order. take() swaps all non-empty shards and returns
//   them shard by shard: lines of DIFFERENT threads in one batch may appear
//   out of global time order by at most one flush interval.
//   Full / notify are per shard: the worker is woken when one shard reaches
//   75% or on an urgent line (only the first notifier since the last take
//   pays for the syscall); Drop / Block apply to the caller's shard.
//
// Crash safety (crash_drain, async-signal-safe):
//   Each buffer's committed length `len` is an atomic, published with a
//   release store only AFTER the line's memcpy is complete, so [0, len) always
//   holds whole lines. The buffer the worker is writing is published per shard
//   in `pending` with `pending_done` = bytes already handed to every
//   crash-safe sink (mark_written()).
//
//   Guarantee: a line whose append() returned true before the process died
//   is in every crash-safe sink, whole, exactly once, except in the bounded
//   fallbacks listed at the end.
//
//   crash_drain() (the handler; it never blocks unboundedly, takes no mutex):
//     1. crashed_ = true (seq_cst), the visitor is published, closed_ = true.
//     2. Claims writer_ (CAS 0 -> own thread id), waiting up to ~2 s for the
//        worker to finish the write it is in. The worker writes the
//        crash-safe sinks only while it holds writer_ (begin_write ..
//        end_write) and calls mark_written() before releasing it, so once the
//        claim succeeds pending_done is exact: the chunk in flight is neither
//        lost nor written twice. begin_write() re-checks crashed_ after its
//        CAS, so once the handler is waiting the worker starts no new write.
//     3. Barrier: try-locks every shard spinlock (bounded: ~50 ms in total,
//        shared by all shards) and releases it at once. Appends that saw
//        closed_ == false inside their critical section have then published
//        their `len`; every later one sees closed_ (closed_ is stored before
//        the lock is taken, the lock orders them). take() checks crashed_
//        under the same lock and swaps nothing once the handler passed, so
//        `active` / `pending` are stable from here on.
//     4. Visits, for every shard in order:
//          pending buffer [pending_done, len)   (older data first)
//          active  buffer [0, len)
//        then the tail (the crash marker), sets crash_done_ and releases
//        writer_.
//   append() after the crash: closed_ is checked before the shard lock is
//   taken (the one extra load of the hot path; closed_ is read-only in normal
//   operation, so the line stays shared in every cache) and again under it.
//   Either check routes to write_after_crash(), outside every shard lock:
//   wait for crash_done_ (the drained lines and the marker come first), claim
//   writer_ (serializes post-crash lines: never interleaved), visit the line,
//   release writer_, return true. So the line is written before the call
//   returns; lines written this way come AFTER the marker. Never taking a
//   shard lock there matters: the crashing thread may have died holding one,
//   and may log again from a chained application handler.
//   Interleavings:
//     * worker mid-write when the crash hits: the handler waits in step 2,
//       then drains from the exact pending_done.
//     * worker between two chunks: its begin_write() fails (writer_ taken or
//       crashed_ seen); the chunks it did not write stay published (release()
//       keeps them) and the handler drains them.
//     * append inside its critical section: the barrier waits for it; it
//       returns true and its line is drained.
//     * append between the pre-lock check and the lock: sees closed_ under the
//       lock and goes to write_after_crash().
//     * append blocked by full_policy = block: wait_for_swap() returns on
//       closed_ (within its 50 ms poll) and the line goes to
//       write_after_crash().
//     * the crashing thread logs again (chained application handler, after
//       the drain): write_after_crash(); a thread that already holds writer_
//       (a signal handler interrupting a post-crash write) writes without
//       waiting for itself.
//     * a second crash on another thread: the platform handler parks it (it
//       never re-enters crash_drain); post-crash writers it blocked are
//       bounded (below).
//   Bounded fallbacks (documented residual windows):
//     * the worker holds writer_ longer than ~2 s (stuck sink, or blocked on
//       a lock the crashing thread holds) or the crash is ON the worker inside
//       a crash-safe write: the handler drains from pending_done anyway, so
//       part of that one chunk may appear twice (and a torn line).
//     * a shard lock held for the whole barrier budget (the crashing thread
//       itself crashed inside append, or a holder descheduled ~50 ms): a line
//       committed after the handler read that shard is lost.
//     * write_after_crash() waits at most ~5 s for the drain and the claim,
//       then writes anyway (could interleave with a stuck handler); it never
//       deadlocks. The process may die while it writes: that call never
//       returned, so its line was not acknowledged.
//   The handler may still be walking the shards while the worker finishes
//   its cycle, so once crashed: the worker never take()s again (no buffer is
//   handed back to producers under the handler's feet), and release() keeps
//   every chunk that was not mark_written() published as `pending`.
#include "queue/queue.h"

#include "core/compat.h"
#include "platform/platform.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define TBLOG_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__)
#define TBLOG_CPU_RELAX() __asm__ __volatile__("yield")
#elif defined(_M_ARM64) || defined(_M_ARM64EC)
#include <intrin.h>
#define TBLOG_CPU_RELAX() __yield()
#else
#define TBLOG_CPU_RELAX() ((void)0)
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define TBLOG_COLD __declspec(noinline)
#else
#define TBLOG_COLD __attribute__((noinline, cold))
#endif

namespace tblog {

#if TBLOG_CPP_LEVEL < 201703L
constexpr size_t Queue::kMaxShards;  // C++11/14: out-of-line definition
#endif

namespace {

// crash_drain() relies on these being lock-free (async-signal-safe).
TBLOG_ASSERT_LOCK_FREE(size_t);
TBLOG_ASSERT_LOCK_FREE(int);
TBLOG_ASSERT_LOCK_FREE(bool);
TBLOG_ASSERT_LOCK_FREE(uint32_t);
TBLOG_ASSERT_LOCK_FREE(void *);

constexpr size_t kMinShardSize = 64u << 10;

// Wait budgets of the crash paths (see "Crash safety").
constexpr unsigned kWorkerWaitMs = 2000;    // handler: the worker's write in flight
constexpr unsigned kAppendWaitMs = 50;      // handler: appends inside a shard lock (all shards)
constexpr unsigned kAfterCrashWaitMs = 5000;  // post-crash append: drain + claim

// Bounded back-off for the crash paths: 256 pauses, 256 yields, then 1 ms
// sleeps until `budget_ms` of steady-clock time has passed. step() returns
// false once the budget is used up (budget 0: after the first probe).
// Async-signal-safe in practice: pause, sched_yield / SwitchToThread,
// nanosleep / Sleep and clock_gettime / QueryPerformanceCounter only.
class Backoff {
public:
    explicit Backoff(unsigned budget_ms) noexcept : budget_ms_(budget_ms) {}
    bool step() noexcept {
        if (budget_ms_ == 0) return false;
        if (n_ < 256) {
            ++n_;
            TBLOG_CPU_RELAX();
            return true;
        }
        if (n_ < 512) {
            ++n_;
            std::this_thread::yield();
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (n_ == 512) {
            ++n_;
            deadline_ = now + std::chrono::milliseconds(budget_ms_);
        } else if (now >= deadline_) {
            budget_ms_ = 0;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }

private:
    unsigned budget_ms_;
    unsigned n_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
};

class SpinLock {
public:
    // Test-and-test-and-set: spin on a plain load (no cache-line ping-pong),
    // pause between probes, yield after a while (holder may be descheduled).
    void lock() noexcept {
        unsigned spins = 0;
        while (flag_.test_and_set(std::memory_order_acquire)) {
            while (locked_.load(std::memory_order_relaxed)) {
                if (++spins < 256) TBLOG_CPU_RELAX();
                else std::this_thread::yield();
            }
        }
        locked_.store(true, std::memory_order_relaxed);
    }
    void unlock() noexcept {
        locked_.store(false, std::memory_order_relaxed);
        flag_.clear(std::memory_order_release);
    }
    // Crash handler: give up when `bo` is used up (the holder may be the
    // crashing thread itself).
    bool try_lock(Backoff &bo) noexcept {
        for (;;) {
            if (!locked_.load(std::memory_order_relaxed) &&
                !flag_.test_and_set(std::memory_order_acquire)) {
                locked_.store(true, std::memory_order_relaxed);
                return true;
            }
            if (!bo.step()) return false;
        }
    }

private:
    // std::atomic_flag has no load() before C++20, so mirror its state.
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> locked_{false};
};

struct SpinGuard {
    explicit SpinGuard(SpinLock &l) : l_(l) { l_.lock(); }
    ~SpinGuard() { l_.unlock(); }
    SpinLock &l_;
};

struct Buffer {
    char *data = nullptr;
    std::atomic<size_t> len{0};  // committed bytes (whole lines only)
};

size_t shard_count() {
    unsigned hc = std::thread::hardware_concurrency();
    size_t n = 1;
    while (n < hc && n < Queue::kMaxShards) n <<= 1;
    return n;
}

// Process-wide round-robin thread id (shared by all queue instances).
std::atomic<unsigned> g_next_thread_id{0};

inline unsigned this_thread_id() noexcept {
    static thread_local unsigned id = 0;  // 0 = not assigned yet; else id + 1
    if (TBLOG_UNLIKELY(id == 0))
        id = g_next_thread_id.fetch_add(1, std::memory_order_relaxed) + 1;
    return id - 1;
}

}  // namespace

struct alignas(64) Queue::Shard : CacheAlignedNew {
    // Producer side (hot, mostly touched by the shard's own threads).
    SpinLock spin;
    Buffer bufs[2];
    std::atomic<int> active{0};
    std::atomic<uint64_t> swap_gen{0};
    // Crash-handler view of the buffer being written (worker-written).
    alignas(64) std::atomic<int> pending{-1};
    std::atomic<size_t> pending_done{0};
};

Queue::Queue(const QueueOptions &opt)
    : nshards_(shard_count()),
      mask_(nshards_ - 1),
      cap_(opt.buffer_size / nshards_ < kMinShardSize ? kMinShardSize
                                                      : opt.buffer_size / nshards_),
      threshold_(cap_ - cap_ / 4),
      policy_(opt.policy),
      urgent_level_(opt.urgent_level),
      shards_(new Shard[nshards_]) {}

Queue::~Queue() {
    close();
    for (size_t i = 0; i < nshards_; ++i) {
        std::free(shards_[i].bufs[0].data);
        std::free(shards_[i].bufs[1].data);
    }
    delete[] shards_;
}

std::unique_ptr<Queue> Queue::create(const QueueOptions &opt) {
    std::unique_ptr<Queue> q(new Queue(opt));
    if (!q->init()) return nullptr;
    return q;
}

bool Queue::init() {
    for (size_t i = 0; i < nshards_; ++i) {
        for (Buffer &b : shards_[i].bufs) {
            b.data = static_cast<char *>(std::malloc(cap_));
            if (!b.data) {
                std::fprintf(stderr, "tblog: cannot allocate 2 x %zu x %zu byte buffers\n",
                             nshards_, cap_);
                return false;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------------ hot path
bool Queue::append(const char *data, uint32_t len, tblog_level lv) noexcept {
    if (len == 0) return true;
    if (len > cap_) {  // can never fit
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Closed / crashed: never touch the shard lock (a crashed thread may hold it).
    if (TBLOG_UNLIKELY(closed_.load(std::memory_order_relaxed))) return append_closed(data, len);
    Shard &sh = shards_[this_thread_id() & mask_];
    for (;;) {
        size_t used = 0;
        uint64_t gen = 0;
        bool ok = false;
        {
            SpinGuard g(sh.spin);
            // Re-checked under the lock: close() / crash_drain() barriers.
            if (TBLOG_UNLIKELY(closed_.load(std::memory_order_relaxed))) goto closed;
            Buffer &b = sh.bufs[sh.active.load(std::memory_order_relaxed)];
            used = b.len.load(std::memory_order_relaxed);
            if (used + len <= cap_) {
                std::memcpy(b.data + used, data, len);
                used += len;
                b.len.store(used, std::memory_order_release);
                ok = true;
            } else {
                gen = sh.swap_gen.load(std::memory_order_relaxed);
            }
        }
        if (ok) {
            bool urgent = lv >= urgent_level_;
            if (urgent) urgent_.store(true, std::memory_order_relaxed);
            if (used >= threshold_ || urgent) notify_worker();
            return true;
        }
        // Shard full.
        notify_worker();
        if (policy_ == FullPolicy::Drop) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!wait_for_swap(sh, gen)) break;  // closed
    }
closed:  // outside the shard lock (the guard's scope was left)
    return append_closed(data, len);
}

// Cold: the queue is closed (shutdown: drop) or crashed (write it now).
TBLOG_COLD bool Queue::append_closed(const char *data, size_t len) noexcept {
    // Pairs with crash_drain()'s seq_cst store of closed_ (after crashed_ and
    // the visitor): a closed_ seen by a relaxed load implies both.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!crashed_.load(std::memory_order_acquire)) return false;
    return write_after_crash(data, len);
}

// ------------------------------------------------------------------ worker
void Queue::wait_for_work(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(wait_mu_);
    wait_cv_.wait_for(lk, timeout, [&] {
        return wake_req_ || notified_.load(std::memory_order_relaxed);
    });
    wake_req_ = false;
}

bool Queue::consume_urgent() noexcept {
    return urgent_.exchange(false, std::memory_order_acq_rel);
}

size_t Queue::take(Batch &out) noexcept {
    out.size = 0;
    // Reset before swapping: a notification raised after this point is
    // either covered by the swaps below or causes one extra (cheap) wakeup.
    notified_.store(false, std::memory_order_relaxed);

    // Swap every non-empty shard (short critical sections).
    for (size_t i = 0; i < nshards_; ++i) {
        Shard &sh = shards_[i];
        SpinGuard g(sh.spin);
        // A crash handler passed (or is about to pass) its barrier: leave
        // every shard as it found it. The lock orders this load after the
        // handler's store when the handler took this lock first.
        if (crashed_.load(std::memory_order_relaxed)) break;
        int a = sh.active.load(std::memory_order_relaxed);
        size_t n = sh.bufs[a].len.load(std::memory_order_relaxed);
        if (n == 0) continue;
        // Publish the pending buffer before switching, so a crash
        // handler always sees each byte in at least one place.
        sh.pending_done.store(0, std::memory_order_relaxed);
        sh.pending.store(a, std::memory_order_release);
        sh.active.store(a ^ 1, std::memory_order_release);
        sh.swap_gen.fetch_add(1, std::memory_order_seq_cst);
        // No producer writes this buffer any more: n is final.
        out.chunks[out.size].data = sh.bufs[a].data;
        out.chunks[out.size].len = n;
        out.shard[out.size] = static_cast<unsigned char>(i);
        ++out.size;
    }
    if (out.size) wake_blocked();
    return out.size;
}

bool Queue::begin_write() noexcept {
    uint32_t cur = 0;
    // Before a crash only the worker claims writer_, so the CAS cannot fail;
    // after one, the handler or a post-crash append may hold it.
    if (!writer_.compare_exchange_strong(cur, pf::current_thread_id(), std::memory_order_seq_cst,
                                         std::memory_order_relaxed))
        return false;
    // The handler sets crashed_ before it tries to claim writer_: seen here,
    // write nothing (it is waiting for us); not seen, the handler finds
    // writer_ taken and waits until end_write().
    if (crashed_.load(std::memory_order_seq_cst)) {
        writer_.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

void Queue::end_write() noexcept { writer_.store(0, std::memory_order_release); }

void Queue::mark_written(const Batch &b, size_t i) noexcept {
    shards_[b.shard[i]].pending_done.store(b.chunks[i].len, std::memory_order_release);
}

void Queue::release(Batch &b) noexcept {
    for (size_t i = 0; i < b.size; ++i) {
        Shard &sh = shards_[b.shard[i]];
        // A chunk that never reached the crash-safe sinks (the worker stopped
        // at a crash) stays published: the crash handler, possibly still
        // walking the shards on another thread, must find it. There is no
        // take() after a crash, so the buffer is never needed again.
        if (sh.pending_done.load(std::memory_order_relaxed) < b.chunks[i].len &&
            crashed_.load(std::memory_order_acquire))
            continue;
        const int p = sh.pending.load(std::memory_order_relaxed);
        sh.pending.store(-1, std::memory_order_release);
        if (p >= 0) sh.bufs[p].len.store(0, std::memory_order_release);
    }
    b.size = 0;
}

// ------------------------------------------------------------------ any thread
void Queue::wake() noexcept {
    try {
        std::lock_guard<std::mutex> lk(wait_mu_);
        wake_req_ = true;
    } catch (...) {
    }
    wait_cv_.notify_all();
}

void Queue::close() noexcept {
    closed_.store(true, std::memory_order_relaxed);
    // Barrier: appends that saw closed_ == false finish before we return;
    // later ones see closed_ (the lock orders them after this store).
    for (size_t i = 0; i < nshards_; ++i) SpinGuard g(shards_[i].spin);
    wake_blocked();
    wake();
}

// ------------------------------------------------------------------ crash
namespace {
enum class Claim { Failed, Claimed, Nested };

// Claim `writer` for thread `self`, backing off with `bo`. Nested: `self`
// already holds it (a handler running on the thread that is writing).
Claim claim_writer(std::atomic<uint32_t> &writer, uint32_t self, Backoff &bo) noexcept {
    for (;;) {
        uint32_t cur = 0;
        if (writer.compare_exchange_strong(cur, self, std::memory_order_seq_cst,
                                           std::memory_order_relaxed))
            return Claim::Claimed;
        if (cur == self) return Claim::Nested;
        if (!bo.step()) return Claim::Failed;
    }
}
}  // namespace

bool Queue::crash_drain(void (*visit)(void *, const char *, size_t), void *ctx, const char *tail,
                        size_t tail_len, bool others_running) noexcept {
    if (crashed_.exchange(true, std::memory_order_seq_cst)) {
        // Not the first: just the tail, after the first drain (post-crash way).
        if (tail && tail_len) write_after_crash(tail, tail_len);
        return false;
    }
    crash_ctx_.store(ctx, std::memory_order_relaxed);
    crash_visit_.store(visit, std::memory_order_release);
    closed_.store(true, std::memory_order_seq_cst);  // no new appends into the buffers

    // 1. The worker's write in flight (if any) completes; then the sinks are ours.
    const uint32_t self = pf::current_thread_id();
    Backoff wait_worker(others_running ? kWorkerWaitMs : 0);
    const Claim claim = claim_writer(writer_, self, wait_worker);
    // 2. Barrier: appends inside their critical section publish their line.
    Backoff wait_appends(others_running ? kAppendWaitMs : 0);
    for (size_t i = 0; i < nshards_; ++i)
        if (shards_[i].spin.try_lock(wait_appends)) shards_[i].spin.unlock();

    // 3. Whatever has not reached the crash-safe sinks, oldest first.
    for (size_t i = 0; i < nshards_; ++i) {
        Shard &sh = shards_[i];
        // Read `active` BEFORE `pending`: the worker publishes pending
        // before switching active, so if we see the new active buffer we
        // also see the old one as pending (or already fully written).
        int a = sh.active.load(std::memory_order_acquire);
        int p = sh.pending.load(std::memory_order_acquire);
        if (p >= 0) {
            size_t done = sh.pending_done.load(std::memory_order_acquire);
            size_t end = sh.bufs[p].len.load(std::memory_order_acquire);
            if (done < end) visit(ctx, sh.bufs[p].data + done, end - done);
        }
        if (a != p) {
            size_t end = sh.bufs[a].len.load(std::memory_order_acquire);
            if (end) visit(ctx, sh.bufs[a].data, end);
        }
    }
    // 4. The marker, then let the post-crash appends in (in claim order).
    if (tail && tail_len) visit(ctx, tail, tail_len);
    crash_done_.store(true, std::memory_order_release);
    // Nested: the crash is on the thread that was writing (the worker); that
    // write never resumes, so the claim is released as well. Likewise when
    // the other threads are gone (process exit): a holder died with it.
    if (claim != Claim::Failed || !others_running) writer_.store(0, std::memory_order_release);
    return true;
}

// A line appended after the crash (any thread, normal context; also the
// crashing thread itself, e.g. from a chained application signal handler).
// Lock-free: it must not take any lock the crashing thread might hold.
bool Queue::write_after_crash(const char *data, size_t len) noexcept {
    Backoff bo(kAfterCrashWaitMs);
    // The drained lines and the marker first (bounded: never deadlock on a
    // handler that is stuck or gone).
    while (!crash_done_.load(std::memory_order_acquire) && bo.step()) {
    }
    // One writer at a time, so lines are never interleaved. Nested: this
    // thread already writes (a signal handler interrupted it): don't wait
    // for ourselves.
    const Claim claim = claim_writer(writer_, pf::current_thread_id(), bo);
    void (*visit)(void *, const char *, size_t) = crash_visit_.load(std::memory_order_acquire);
    if (visit) visit(crash_ctx_.load(std::memory_order_relaxed), data, len);
    if (claim == Claim::Claimed) writer_.store(0, std::memory_order_release);
    return visit != nullptr;
}

// ------------------------------------------------------------------ private
void Queue::notify_worker() noexcept {
    // Only the first notifier since the last take pays for the syscall.
    if (notified_.load(std::memory_order_relaxed) ||
        notified_.exchange(true, std::memory_order_acq_rel))
        return;
    try {
        std::lock_guard<std::mutex> lk(wait_mu_);
    } catch (...) {
    }
    wait_cv_.notify_one();
}

// Block policy: wait until the worker swapped this shard past `gen` (or closed).
bool Queue::wait_for_swap(Shard &sh, uint64_t gen) noexcept {
    try {
        std::unique_lock<std::mutex> lk(block_mu_);
        block_waiters_.fetch_add(1, std::memory_order_seq_cst);
        while (sh.swap_gen.load(std::memory_order_seq_cst) == gen &&
               !closed_.load(std::memory_order_relaxed)) {
            block_cv_.wait_for(lk, std::chrono::milliseconds(50));
            if (sh.swap_gen.load(std::memory_order_seq_cst) == gen) notify_worker_force();
        }
        block_waiters_.fetch_sub(1, std::memory_order_seq_cst);
    } catch (...) {
        std::this_thread::yield();
    }
    return !closed_.load(std::memory_order_relaxed);
}

void Queue::notify_worker_force() noexcept {
    notified_.store(true, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> lk(wait_mu_);
    } catch (...) {
    }
    wait_cv_.notify_one();
}

void Queue::wake_blocked() noexcept {
    if (block_waiters_.load(std::memory_order_seq_cst) == 0) return;
    try {
        std::lock_guard<std::mutex> lk(block_mu_);
    } catch (...) {
    }
    block_cv_.notify_all();
}

}  // namespace tblog
