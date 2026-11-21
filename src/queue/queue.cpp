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
//   crash_drain() takes NO lock (the crashing thread may own a spinlock) and
//   visits, for every shard in order:
//     1. pending buffer [pending_done, len)   (older data first)
//     2. active  buffer [0, len)
//   What can be lost: the line being memcpy'd at the moment of the crash
//   (its len is not yet published), lines appended by other threads after the
//   handler read `len`. What can be duplicated: a chunk whose write to the
//   crash-safe sinks was in progress in the worker (pending_done is only
//   advanced after every crash-safe sink returned), or data racing with a
//   concurrent worker swap. After crash_drain() the queue is "dead": append()
//   returns false and the worker writes nothing more (Pipeline checks
//   crashed() before each chunk), so data is not written a second time.
//   The handler may still be walking the shards while the worker finishes
//   its cycle, so once crashed: the worker never take()s again (no buffer is
//   handed back to producers under the handler's feet), and release() keeps
//   every chunk that was not mark_written() published as `pending` (the old
//   backend reset them, and a handler that reached such a shard after the
//   worker lost its oldest lines).
#include "queue/queue.h"

#include "core/compat.h"

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

namespace tblog {

#if TBLOG_CPP_LEVEL < 201703L
constexpr size_t Queue::kMaxShards;  // C++11/14: out-of-line definition
#endif

namespace {

// crash_drain() relies on these being lock-free (async-signal-safe).
TBLOG_ASSERT_LOCK_FREE(size_t);
TBLOG_ASSERT_LOCK_FREE(int);
TBLOG_ASSERT_LOCK_FREE(bool);

constexpr size_t kMinShardSize = 64u << 10;

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
    Shard &sh = shards_[this_thread_id() & mask_];
    for (;;) {
        size_t used = 0;
        uint64_t gen = 0;
        bool ok = false;
        {
            SpinGuard g(sh.spin);
            if (closed_.load(std::memory_order_relaxed)) return false;
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
        if (!wait_for_swap(sh, gen)) return false;  // closed
    }
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
bool Queue::crash_drain(void (*visit)(void *, const char *, size_t), void *ctx) noexcept {
    if (crashed_.exchange(true, std::memory_order_acq_rel)) return false;
    closed_.store(true, std::memory_order_release);  // lock-free: no new appends
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
    return true;
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
