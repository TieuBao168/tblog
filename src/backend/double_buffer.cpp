// File backend: sharded double buffers. Callers memcpy into the active buffer
// of their own shard under a short per-shard spinlock; the worker swaps every
// shard's buffers and write()s the full ones outside the locks.
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
//   own lines stay in order. flush() swaps all non-empty shards, then writes
//   them shard by shard: lines of DIFFERENT threads in one flush may appear
//   out of global time order by at most one flush interval.
//   Full / notify are per shard: the worker is woken when one shard reaches
//   75% or on an urgent line (only the first notifier since the last flush
//   pays for the syscall); Drop / Block apply to the caller's shard.
//
// Crash safety (on_crash, async-signal-safe):
//   Each buffer's committed length `len` is an atomic, published with a
//   release store only AFTER the line's memcpy is complete, so [0, len) always
//   holds whole lines. The buffer the worker is writing is published per shard
//   in `pending` with `pending_done` = bytes already handed to write().
//   on_crash() takes NO lock (the crashing thread may own a spinlock) and
//   writes, for every shard in order, straight to the fd:
//     1. pending buffer [pending_done, len)   (older data first)
//     2. active  buffer [0, len)
//   What can be lost: the line being memcpy'd at the moment of the crash
//   (its len is not yet published), lines appended by other threads after the
//   handler read `len`. What can be duplicated: a chunk whose write() was in
//   progress in the worker (pending_done is only advanced after write()
//   returns), or data racing with a concurrent worker swap. After on_crash()
//   the backend is "dead": append() returns false and flush() writes nothing
//   more (it checks before each shard), so data is not written a second time.
#include "backend/rotating_file.h"

#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define TBLOG_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__)
#define TBLOG_CPU_RELAX() __asm__ __volatile__("yield")
#else
#define TBLOG_CPU_RELAX() ((void)0)
#endif

namespace tblog {

namespace {

// on_crash() relies on these being lock-free (async-signal-safe).
TBLOG_ASSERT_LOCK_FREE(size_t);
TBLOG_ASSERT_LOCK_FREE(int);
TBLOG_ASSERT_LOCK_FREE(bool);

constexpr size_t kMaxShards = 16;
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

struct alignas(64) Shard : CacheAlignedNew {
    // Producer side (hot, mostly touched by the shard's own threads).
    SpinLock spin;
    Buffer bufs[2];
    std::atomic<int> active{0};
    std::atomic<uint64_t> swap_gen{0};
    // Crash-handler view of the buffer being written (worker-written).
    alignas(64) std::atomic<int> pending{-1};
    std::atomic<size_t> pending_done{0};
};

size_t shard_count() {
    unsigned hc = std::thread::hardware_concurrency();
    size_t n = 1;
    while (n < hc && n < kMaxShards) n <<= 1;
    return n;
}

// Process-wide round-robin thread id (shared by all backend instances).
std::atomic<unsigned> g_next_thread_id{0};

inline unsigned this_thread_id() noexcept {
    static thread_local unsigned id = 0;  // 0 = not assigned yet; else id + 1
    if (__builtin_expect(id == 0, 0))
        id = g_next_thread_id.fetch_add(1, std::memory_order_relaxed) + 1;
    return id - 1;
}

class DoubleBufferBackend final : public Backend, public CacheAlignedNew {
public:
    explicit DoubleBufferBackend(const BufferOptions &opt)
        : nshards_(shard_count()),
          mask_(nshards_ - 1),
          cap_(opt.buffer_size / nshards_ < kMinShardSize ? kMinShardSize
                                                          : opt.buffer_size / nshards_),
          threshold_(cap_ - cap_ / 4),
          policy_(opt.policy),
          urgent_level_(opt.file.urgent_level),
          shards_(new Shard[nshards_]) {}

    ~DoubleBufferBackend() override {
        shutdown();
        for (size_t i = 0; i < nshards_; ++i) {
            std::free(shards_[i].bufs[0].data);
            std::free(shards_[i].bufs[1].data);
        }
        delete[] shards_;
    }

    bool init(const BufferOptions &opt) {
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
        return file_.open(opt.file);
    }

    // ------------------------------------------------------------------ hot path
    bool append(const char *data, uint32_t len, tblog_level lv) noexcept override {
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
    void wait_for_work(std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lk(wait_mu_);
        wait_cv_.wait_for(lk, timeout, [&] {
            return wake_req_ || notified_.load(std::memory_order_relaxed);
        });
        wake_req_ = false;
    }

    void flush() override {
        std::lock_guard<std::mutex> lk(flush_mu_);
        if (crashed_.load(std::memory_order_acquire)) return;
        flush_locked();
    }

    // Don't hold flush_mu_ during fdatasync (it can take ~100 ms): dup the fd
    // so a concurrent rotation cannot close it under us.
    void sync() override {
        int d = -1;
        {
            std::lock_guard<std::mutex> lk(flush_mu_);
            int fd = file_.fd();
            if (fd >= 0) d = ::dup(fd);
        }
        if (d >= 0) {
            ::fdatasync(d);
            ::close(d);
        }
    }

    bool consume_urgent() noexcept override {
        return urgent_.exchange(false, std::memory_order_acq_rel);
    }

    void wake() noexcept override {
        try {
            std::lock_guard<std::mutex> lk(wait_mu_);
            wake_req_ = true;
        } catch (...) {
        }
        wait_cv_.notify_all();
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(flush_mu_);
        if (shut_) return;
        shut_ = true;
        closed_.store(true, std::memory_order_relaxed);
        // Barrier: appends that saw closed_ == false finish before we flush;
        // later ones see closed_ (the lock orders them after this store).
        for (size_t i = 0; i < nshards_; ++i) SpinGuard g(shards_[i].spin);
        if (!crashed_.load(std::memory_order_acquire)) {
            flush_locked();
            flush_locked();
        }
        for (Sink *s : sinks_) s->flush();
        file_.datasync();
        file_.close();
        wake_blocked();
        wake();
    }

    // ------------------------------------------------------------------ crash
    void on_crash() noexcept override {
        if (crashed_.exchange(true, std::memory_order_acq_rel)) return;
        closed_.store(true, std::memory_order_release);  // lock-free: no new appends
        int fd = file_.fd();
        if (fd < 0) return;
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
                if (done < end)
                    RotatingFile::raw_write_all(fd, sh.bufs[p].data + done, end - done);
            }
            if (a != p) {
                size_t end = sh.bufs[a].len.load(std::memory_order_acquire);
                if (end) RotatingFile::raw_write_all(fd, sh.bufs[a].data, end);
            }
        }
    }

    void crash_write(const char *data, size_t len) noexcept override {
        int fd = file_.fd();
        if (fd >= 0) RotatingFile::raw_write_all(fd, data, len);
    }

    void add_sink(Sink *s) override {
        std::lock_guard<std::mutex> lk(flush_mu_);
        if (s) sinks_.push_back(s);
    }

    uint64_t dropped() const noexcept override {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    void notify_worker() noexcept {
        // Only the first notifier since the last flush pays for the syscall.
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
    bool wait_for_swap(Shard &sh, uint64_t gen) noexcept {
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

    void notify_worker_force() noexcept {
        notified_.store(true, std::memory_order_release);
        try {
            std::lock_guard<std::mutex> lk(wait_mu_);
        } catch (...) {
        }
        wait_cv_.notify_one();
    }

    void wake_blocked() noexcept {
        if (block_waiters_.load(std::memory_order_seq_cst) == 0) return;
        try {
            std::lock_guard<std::mutex> lk(block_mu_);
        } catch (...) {
        }
        block_cv_.notify_all();
    }

    // Caller holds flush_mu_.
    void flush_locked() {
        // Reset before swapping: a notification raised after this point is
        // either covered by the swaps below or causes one extra (cheap) wakeup.
        notified_.store(false, std::memory_order_relaxed);

        // 1. Swap every non-empty shard (short critical sections).
        int full[kMaxShards];
        bool any = false;
        for (size_t i = 0; i < nshards_; ++i) {
            Shard &sh = shards_[i];
            SpinGuard g(sh.spin);
            int a = sh.active.load(std::memory_order_relaxed);
            if (sh.bufs[a].len.load(std::memory_order_relaxed) == 0) {
                full[i] = -1;
                continue;
            }
            // Publish the pending buffer before switching, so a crash
            // handler always sees each byte in at least one place.
            sh.pending_done.store(0, std::memory_order_relaxed);
            sh.pending.store(a, std::memory_order_release);
            sh.active.store(a ^ 1, std::memory_order_release);
            sh.swap_gen.fetch_add(1, std::memory_order_seq_cst);
            full[i] = a;
            any = true;
        }

        // Time rotation, decided here AFTER the swaps: every line taken above
        // was appended before this clock read, so it never lands in a file of
        // an earlier period than the one in effect when it was appended.
        if (!crashed_.load(std::memory_order_acquire)) file_.check_time();

        // 2. Write them out (RotatingFile has no writev: one write per shard).
        if (any) {
            wake_blocked();
            for (size_t i = 0; i < nshards_; ++i) {
                if (full[i] < 0) continue;
                Shard &sh = shards_[i];
                Buffer &b = sh.bufs[full[i]];
                // The crash handler already drained this shard: don't duplicate.
                if (!crashed_.load(std::memory_order_acquire)) {
                    size_t n = b.len.load(std::memory_order_acquire);
                    file_.write_all(b.data, n);
                    sh.pending_done.store(n, std::memory_order_release);
                    for (Sink *s : sinks_) s->write(b.data, n);
                }
                sh.pending.store(-1, std::memory_order_release);
                b.len.store(0, std::memory_order_release);
            }
        }

        uint64_t d = dropped_.load(std::memory_order_relaxed);
        if (d != reported_dropped_ && !crashed_.load(std::memory_order_acquire)) {
            char line[64];
            int m = std::snprintf(line, sizeof line, "tblog: %llu lines dropped\n",
                                  static_cast<unsigned long long>(d - reported_dropped_));
            reported_dropped_ = d;
            if (m > 0) {
                file_.write_all(line, static_cast<size_t>(m));
                for (Sink *s : sinks_) s->write(line, static_cast<size_t>(m));
            }
        }
        for (Sink *s : sinks_) s->flush();
    }

    const size_t nshards_;
    const size_t mask_;
    const size_t cap_;        // per shard buffer
    const size_t threshold_;  // 75% of cap_
    const FullPolicy policy_;
    const tblog_level urgent_level_;
    Shard *const shards_;

    // Read on every append, written once at shutdown/crash.
    alignas(64) std::atomic<bool> closed_{false};
    // Written rarely by producers (first notifier per flush / urgent lines).
    alignas(64) std::atomic<bool> notified_{false};
    std::atomic<bool> urgent_{false};
    alignas(64) std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> crashed_{false};

    // Worker side.
    alignas(64) std::mutex flush_mu_;
    RotatingFile file_;
    std::vector<Sink *> sinks_;
    uint64_t reported_dropped_ = 0;
    bool shut_ = false;

    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    bool wake_req_ = false;

    std::mutex block_mu_;
    std::condition_variable block_cv_;
    std::atomic<int> block_waiters_{0};
};

}  // namespace

std::unique_ptr<Backend> make_double_buffer_backend(const BufferOptions &opt) {
    std::unique_ptr<DoubleBufferBackend> b(new DoubleBufferBackend(opt));
    if (!b->init(opt)) return nullptr;
    return b;
}

}  // namespace tblog
