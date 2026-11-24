// Per-shard spinlock of the queue (queue.cpp) and the bounded back-off of its
// crash paths. Internal; in a header so the tests can exercise it directly.
//
// The lock is held only for one memcpy of a line, so the uncontended and the
// briefly contended cases are what matter for speed. What matters for
// correctness is the holder being preempted while a waiter spins:
//   * lock() / lock_unless() (full_policy = block) spin, then yield, then
//     SLEEP in short steps; lock_unless() also stops once the queue closed. Sleeping matters for real-time threads (SCHED_FIFO / SCHED_RR):
//     sched_yield only yields to threads of the same priority, so a waiter
//     that only yielded could spin forever while a lower-priority holder on
//     the same CPU never runs again. A sleeping waiter lets it run.
//   * try_lock_bounded() (full_policy = drop) gives up after a short, bounded
//     spin: the caller drops the line instead of waiting for a descheduled
//     holder.
//   * try_lock(Backoff &) (crash handler) gives up when its budget is used up.
#pragma once

#include <atomic>
#include <mutex>  // std::lock_guard, std::adopt_lock (users)
#include <chrono>
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
    // An enum, not static constexpr members: C++11/14 would need out-of-line
    // definitions wherever one is bound to a reference (std::chrono ctors).
    enum : unsigned {
        kPauses = 256,        // pause probes before yielding
        kYields = 256,        // lock(): yields before sleeping
        kBoundedYields = 64,  // try_lock_bounded(): yields before giving up
        kSleepUs = 50,        // lock(): sleep step once yielding did not help
    };

    // Test-and-test-and-set: spin on a plain load (no cache-line ping-pong),
    // pause between probes, then yield, then sleep (see the top of the file).
    void lock() noexcept { (void)lock_unless_impl(nullptr); }

    // As lock(), but returns false (lock not taken) once `stop` is seen set
    // in the sleep phase: the queue's closed flag, so a holder that died
    // inside its critical section (a crash) cannot hang the waiter forever.
    bool lock_unless(const std::atomic<bool> &stop) noexcept { return lock_unless_impl(&stop); }

    // full_policy = drop: never waits for a descheduled holder. False after
    // kPauses pauses and kBoundedYields yields without getting the lock.
    bool try_lock_bounded() noexcept {
        for (unsigned spins = 0;; ++spins) {
            if (!locked_.load(std::memory_order_relaxed) &&
                !flag_.test_and_set(std::memory_order_acquire)) {
                locked_.store(true, std::memory_order_relaxed);
                return true;
            }
            if (spins < kPauses) TBLOG_CPU_RELAX();
            else if (spins < kPauses + kBoundedYields) std::this_thread::yield();
            else return false;
        }
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
    bool lock_unless_impl(const std::atomic<bool> *stop) noexcept {
        unsigned spins = 0;
        while (flag_.test_and_set(std::memory_order_acquire)) {
            while (locked_.load(std::memory_order_relaxed)) {
                if (spins < kPauses) {
                    TBLOG_CPU_RELAX();
                } else if (spins < kPauses + kYields) {
                    std::this_thread::yield();
                } else {
                    if (stop && stop->load(std::memory_order_relaxed)) return false;
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(kSleepUs)));
                    continue;  // no counter overflow while sleeping
                }
                ++spins;
            }
        }
        locked_.store(true, std::memory_order_relaxed);
        return true;
    }

    // std::atomic_flag has no load() before C++20, so mirror its state.
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> locked_{false};
};
// SpinLock is BasicLockable: guard it with std::lock_guard<SpinLock> (with
// std::adopt_lock after try_lock_bounded() / lock_unless()).

}  // namespace tblog
