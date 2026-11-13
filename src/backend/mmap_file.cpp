// v2 backend: callers memcpy log lines straight into a MAP_SHARED mapping of
// the log file. Once a memcpy returns, the bytes are in the kernel page cache,
// so they survive any death of the process (SIGKILL, OOM kill, segfault) with
// no recovery step at restart. Only power loss / kernel crash needs sync().
//
// Layout. The file is cut into consecutive regions of `region_size` bytes
// (page multiple). Each region is posix_fallocate'd, mmap'd and pre-faulted
// writable by the worker BEFORE it is published, so the hot path never takes
// a page fault that has to allocate blocks and never touches an unallocated
// page (which could SIGBUS on ENOSPC).
//
// Lookahead. The worker keeps up to kLookahead (K = 2) prepared regions queued
// behind the current one, so a burst can consume K whole regions while the
// worker is still preparing (or is descheduled). Regions get a sequence
// number `seq` (0, 1, 2, ... in creation order, which is also file order) and
// region seq s sits in ring_[s % kRing], kRing = K + 1. published_ is the
// number of regions published (= next seq to create). There is no separate
// head index: the head of the queue is current_->seq + 1, so the writer that
// wins the CAS on current_ is exactly the one that "pops" the queue.
// Prepared (queued) count = published_ - 1 - current_->seq, in [0, K].
//
// Hot path (append):
//   1. r = current_ (acquire); CAS r->pos from p to p+len (a line never
//      straddles two regions).
//   2. memcpy into r->map + p; r->committed.fetch_add(len, release).
//   If the line does not fit, the thread that wins the CAS of pos -> size pads
//   the remainder with '\n' (blank lines) and commits it; then any thread
//   switches current_ to region r->seq + 1 if it is published (CAS on
//   current_) and retries. No next region (queue ran dry) -> ~8 us pause
//   spin, then sched_yield() for up to 200 us (1 ms while the worker is inside
//   flush(), i.e. about to publish), then Drop (count) or Block (condvar,
//   5 ms re-check) per policy.
//   The worker is woken (throttled by the pending_ flag, so the hot path does
//   not syscall per line): once per region when it passes 50%, once per
//   switch (the queue just dropped below K; only the CAS winner wakes), once
//   per worker cycle for urgent lines, and when a writer starts spinning.
//
// Worker (flush): processes a rotation the writers switched into, munmaps
// retired regions (committed == size, i.e. every reserved byte was copied),
// finalizes rotated-out files (ftruncate to exact used length, fdatasync,
// close) and tops the queue up to K regions, publishing each one as soon as
// it is ready (topping up runs before AND after retiring: munmap can stall).
//
// Memory-ordering argument:
//  * Exclusivity: pos is only changed by RMW (CAS); the modification order of
//    a single atomic is total, so every byte range [p, p+len) is handed to
//    exactly one thread. Relaxed is enough for that.
//  * Publication: the worker fully initialises Region seq s (map, seq, pos,
//    committed, pages pre-faulted), stores it to ring_[s % kRing] (release),
//    then stores published_ = s + 1 (release). A switching writer loads
//    published_ (acquire) > s, then the slot (acquire): the slot store
//    happens-before its load, so it reads Region s or a later write, and
//    everything the worker wrote before that store is visible. It then
//    publishes the region via the acq_rel CAS on current_; every writer loads
//    current_ with acquire, so by transitivity r->map etc. are visible.
//  * Slot reuse: the worker creates seq t only if t - current_->seq <= K
//    (current_ loaded with acquire), so the slot it overwrites held seq
//    t - kRing <= current_->seq - 1: a region already behind current_. A
//    writer switching from r reads slot (r->seq + 1) % kRing; if that slot was
//    already recycled (seq mismatch, checked), the worker had observed a
//    current_ at or beyond r->seq + 1, and by read-read/write-read coherence
//    the writer's subsequent CAS on current_ cannot read r (r is never stored
//    again: Region structs are never reused), so the CAS fails or is skipped
//    and the writer just reloads current_. A switch therefore always moves
//    current_ from seq s to exactly seq s + 1: regions are used strictly in
//    order and never skipped.
//  * Unmap safety: a writer touches r->map ONLY between a successful
//    reservation CAS and its release fetch_add on committed. committed starts
//    at the region's initial pos, so committed == size implies pos == size
//    (no further reservation can ever succeed) and that every reserver has
//    executed its fetch_add. The worker's acquire load that reads `size` reads
//    the last RMW of a release sequence that contains every writer's
//    fetch_add, so every memcpy/memset happens-before munmap. pad_start is
//    stored relaxed before the padder's release fetch_add, hence visible too.
//    Only regions with seq < current_->seq are retired; queued regions
//    (committed == 0 < size) and current_ never are.
//  * Region structs are never freed or reused before the backend is
//    destroyed, so a writer holding a stale pointer only ever reads pos/seq of
//    a full region (CAS fails / sees full) and never dereferences map.
//  * Shutdown "seals" every live region, current AND every queued one (CAS
//    pos -> size, recording the old pos as the used end); afterwards no
//    reservation can succeed anywhere (a writer may still switch into a
//    sealed queued region, but it is full), so waiting for committed == seal
//    point drains all in-flight writers. Queued regions seal at pos 0 and so
//    contribute nothing to the file length (exact length after shutdown).
//
// sync(): fdatasync(fd). On Linux the page cache is unified: a store through a
// MAP_SHARED writable mapping marks the page dirty (write-protect fault /
// page_mkwrite after every writeback), and fdatasync writes back all dirty
// pages of the inode, mapped or not. msync(MS_SYNC) on Linux is itself just
// vfs_fsync_range on the backing file, so it would add nothing.
//
// Preparation cost (16 MB region, ext4 on NVMe, kernel 7.0, measured with a
// standalone probe; "store" = latency of the first store per page afterwards):
//   posix_fallocate                         ~10 us   (batched: ~4 us/region)
//   mmap + write-touch loop (v2 before)     ~5.3 ms  store p99 0.03 us
//   mmap + MADV_POPULATE_WRITE (now)        ~2.5 ms  store p99 0.02 us
//   mmap(MAP_POPULATE)                      ~2.0 ms  store p50 0.7 us, p99 6 us
//                                           (read-faults only: every first store
//                                           still takes a page_mkwrite fault)
//   mmap + MADV_WILLNEED                    ~20 us   store p99 20 us (no PTEs)
// So regions are pre-faulted with MADV_POPULATE_WRITE (Linux >= 5.14), falling
// back to the write-touch loop if the kernel rejects it. All regions created
// in one flush() in the same file are fallocate'd with a single call.
//
// Rotation happens only at region boundaries. When the next region would push
// the file past max_size, the worker creates it in a fresh file `<path>.next`
// (and keeps queueing further regions in that pending file). After writers
// switched into it, the worker renames app.log -> app.1.log ... and
// `<path>.next` -> `<path>`. The old file keeps being written through its
// still-mapped regions (mmap follows the inode, not the name) and is
// ftruncated to its exact length once its last region is retired. (The old
// file cannot be finalised BEFORE preparing the new region: its last region
// only fills up once writers can switch away from it.) Since there is only
// one `<path>.next`, lookahead stops at a second file boundary: while a
// pending file exists, no region of the file after it is prepared until
// writers switched into the pending file and it was renamed. Because the
// queue is in file order and a file is finalized only when none of its
// regions is mapped (queued regions are mapped), a region queued in a file is
// never used after that file was finalized.
// If the process dies between the switch and the rename, the newest lines are
// in `<path>.next`; the next open finishes that rotation.
// The rename of app.log into the rotated set (app.1.log shifting, or
// app.<period>[.N].log in time modes, plus retention and gzip) is done by the
// shared Archiver (archiver.h). A rotated file is handed to the compressor
// only once it is finalized (ftruncated + fdatasync'd + closed).
//
// Time rotation (file.rotate = daily|hourly). Callers never look at the clock
// for this; the worker checks it once per flush() (after process_rotation).
// When the period ended, it:
//   1. creates `<path>.next` and its first region (unpublished; this is the
//      slow part: fallocate + mmap + populate) unless a size rotation already
//      queued regions in `<path>.next`;
//   2. seals every live region of the active file, current and queued: CAS
//      pos -> size exactly like the padding path, recording pad_start = the
//      old pos and committing the remainder (no memset: the file is
//      ftruncated to its used length when finalized, so the claimed bytes are
//      cut off). Queued regions seal at 0 and add nothing to the length;
//   3. walks current_ through the sealed regions itself (try_switch, the same
//      CAS a writer does), so the new region can be published without
//      violating the slot-reuse bound, publishes it and switches into it;
//   4. runs process_rotation(): app.log -> app.<old period>.log,
//      `<path>.next` -> app.log. The old file is finalized (and compressed)
//      once its in-flight writers committed and its regions are retired.
//   A line therefore lands in the old file iff its reservation precedes the
//   seal. Worst-case lateness of the switch = the worker's wakeup period
//   (flush.interval_ms, default 200 ms) + one region preparation (~2.5 ms for
//   16 MB) + scheduling delay; lines appended in that window after the
//   boundary go to the previous period's file. Between seal and publish (no
//   syscalls) writers briefly see no region and spin. If `<path>.next` cannot
//   be created, the rotation is retried at the next flush().
//   If a size rotation is already pending at the boundary, the pending file
//   becomes the new period's file (it may hold the lines that writers put
//   there just before the seal).
//
// Startup: trailing '\0' bytes (preallocation left by a crash) are trimmed
// from app.log (and app.1.log), and appending continues at the real end of
// data: the first region is mapped from that offset rounded down to a page,
// with pos = the in-page offset.
//
// Known properties / limitations (documented on purpose):
//  * While running (and after a crash) the file has up to ~(K + 1) regions of
//    trailing '\0' (preallocation). Readers should ignore '\0'.
//  * Memory: up to K + 1 regions (plus not-yet-retired full ones) are mapped
//    and resident in the page cache; the K queued ones are dirty zero pages,
//    which fdatasync also writes out (and write-protects again: after a sync
//    the first store to such a page takes a minor page_mkwrite fault).
//  * Lookahead only absorbs K * region_size bytes while the worker is busy
//    elsewhere (e.g. inside a long fdatasync in sync()).
//  * At a file boundary the effective lookahead can be smaller (see above);
//    with max_size < 2 regions it is 1 region per file.
//  * A line whose space was reserved but whose memcpy never ran (process died
//    in between) leaves a '\0' hole of that length in the middle of the file.
//  * Region padding shows up as blank lines ('\n' runs) inside the file.
//  * Lines longer than region_size are dropped.
//  * add_sink() is ignored: there is no worker-side copy of the data to feed
//    secondary sinks from (the data never passes through the worker).
//  * One small Region header (~200 B) per region ever used is kept until the
//    backend is destroyed (safe memory reclamation without hot-path cost).
//  * If a writer stalls for > 2 s inside its memcpy during shutdown, that
//    region is left mapped (leaked) rather than unmapped under its feet.
//  * Block policy blocks forever if the disk stays full.
//  * Drop policy: a line is dropped only after the writer waited up to 200 us
//    (1 ms while the worker is in flush()) for a region; was ~60 us of pure
//    spinning before lookahead. With ENOSPC (alloc failure) it drops after
//    the short pause spin.
#include "backend/archiver.h"
#include "internal.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Kernel headers older than 5.14 (common in vendor BSPs: 4.19, 5.4, 5.10) do
// not define it. Same value as <linux/mman.h>; an older running kernel answers
// EINVAL and we fall back to touching each page.
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

namespace tblog {
namespace {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// ---- small file helpers (rotated-file naming / retention: archiver.h)

void mkdirs_for(const std::string &path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return;
    std::string dir = path.substr(0, slash);
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            std::string sub = dir.substr(0, i);
            ::mkdir(sub.c_str(), 0755);  // EEXIST is fine
        }
    }
}

// Remove trailing '\0' bytes; returns the new length (or -1 on error).
int64_t trim_trailing_zeros(int fd) {
    struct stat st;
    if (::fstat(fd, &st) != 0) return -1;
    int64_t end = st.st_size;
    char buf[64 * 1024];
    while (end > 0) {
        int64_t chunk = end < (int64_t)sizeof(buf) ? end : (int64_t)sizeof(buf);
        ssize_t n = ::pread(fd, buf, (size_t)chunk, end - chunk);
        if (n != chunk) return -1;
        int64_t i = chunk;
        while (i > 0 && buf[i - 1] == '\0') --i;
        if (i > 0) {
            end = end - chunk + i;
            break;
        }
        end -= chunk;
    }
    if (end != st.st_size && ::ftruncate(fd, end) != 0) return -1;
    return end;
}

int64_t trim_file(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int64_t r = trim_trailing_zeros(fd);
    ::close(fd);
    return r;
}

// ---- data structures

struct File {
    int fd = -1;
    int64_t period = 0;     // time modes: a time inside this file's period
    uint64_t arch = 0;      // Archiver handle once rotated out (compressed when finalized)
    bool pending = false;   // created as <path>.next, not yet renamed to <path>
    uint64_t used_len = 0;  // max end of data over retired regions
    uint64_t alloc_end = 0; // posix_fallocate'd up to here (by this backend)
    int live = 0;           // regions of this file still mapped
};

struct Region : CacheAlignedNew {
    // immutable after publication
    char *map = nullptr;
    uint64_t file_off = 0;
    uint64_t size = 0;
    File *file = nullptr;
    uint64_t seq = 0;  // creation order; lives in ring_[seq % kRing]
    // hot, written by callers (separate cache lines)
    alignas(64) std::atomic<uint64_t> pos{0};
    alignas(64) std::atomic<uint64_t> committed{0};
    std::atomic<uint64_t> pad_start{0};  // where padding starts (== size if none)
    // worker-only
    bool mapped = false;
    bool shut_sealed = false;
    uint64_t seal_pos = 0;
    bool stuck = false;
};

enum ReserveResult { kReserved, kFull };

class MmapFileBackend final : public Backend {
public:
    explicit MmapFileBackend(const MmapOptions &opt)
        : path_(opt.file.path),
          next_path_(opt.file.path + ".next"),
          max_size_(opt.file.max_size),
          urgent_level_(opt.file.urgent_level),
          policy_(opt.policy) {
        long ps = ::sysconf(_SC_PAGESIZE);
        page_ = ps > 0 ? (uint64_t)ps : 4096;
        uint64_t rs = opt.region_size < page_ ? page_ : opt.region_size;
        rs_ = (rs + page_ - 1) / page_ * page_;
        arch_.configure(opt.file);
    }

    ~MmapFileBackend() override {
        shutdown();
        for (File *f : files_) {  // only non-empty if init() failed
            if (f->fd >= 0) ::close(f->fd);
            delete f;
        }
        for (Region *r : all_) delete r;
    }

    bool init() {
        mkdirs_for(path_);
        arch_.recover();  // interrupted compressions
        const int64_t now = arch_.now();
        // mtime (before trimming touches it) = period of an existing file's data.
        struct stat st;
        int64_t live_mtime = ::stat(path_.c_str(), &st) == 0 ? (int64_t)st.st_mtime : now;
        // Finish a rotation that a crash interrupted after writers switched
        // into <path>.next but before it was renamed.
        if (::stat(next_path_.c_str(), &st) == 0) {
            const int64_t next_mtime = (int64_t)st.st_mtime;
            if (trim_file(next_path_) > 0) {
                trim_file(path_);
                arch_.archive(path_, live_mtime, true);
                if (::rename(next_path_.c_str(), path_.c_str()) != 0)
                    std::fprintf(stderr, "tblog: mmap backend: rename %s: %s\n",
                                 next_path_.c_str(), std::strerror(errno));
                live_mtime = next_mtime;
            } else {
                ::unlink(next_path_.c_str());
            }
        }
        // Newest rotated file, possibly not finalised by a crash.
        std::string newest = arch_.newest_plain_archive();
        if (!newest.empty()) trim_file(newest);

        int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            std::fprintf(stderr, "tblog: mmap backend: cannot open %s: %s\n",
                         path_.c_str(), std::strerror(errno));
            return false;
        }
        int64_t end = trim_trailing_zeros(fd);
        if (end < 0) {
            std::fprintf(stderr, "tblog: mmap backend: cannot scan %s: %s\n",
                         path_.c_str(), std::strerror(errno));
            ::close(fd);
            return false;
        }
        bool rotate_now = end > 0 && (uint64_t)end >= max_size_;
        // Time modes: a non-empty file from an earlier period is rotated out.
        if (end > 0 && arch_.time_mode() &&
            arch_.period_start(live_mtime) != arch_.period_start(now))
            rotate_now = true;
        if (rotate_now) {
            ::close(fd);
            arch_.archive(path_, live_mtime, true);
            fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                std::fprintf(stderr, "tblog: mmap backend: cannot open %s: %s\n",
                             path_.c_str(), std::strerror(errno));
                return false;
            }
            end = 0;
        }
        File *f = new File;
        f->fd = fd;
        f->period = now;
        files_.push_back(f);
        active_ = f;
        next_boundary_ = arch_.next_boundary(now);
        arch_.start();  // retention + compress leftovers
        uint64_t aligned = (uint64_t)end / page_ * page_;
        Region *r = create_region(f, aligned, (uint64_t)end - aligned, 1);
        if (!r) {
            if (::ftruncate(fd, end) != 0) {
            }
            ::close(fd);
            f->fd = -1;
            done_ = true;
            closed_.store(true);
            return false;
        }
        tail_ = r;
        r->seq = 0;
        ring_[0].store(r, std::memory_order_relaxed);
        published_.store(1, std::memory_order_release);
        current_.store(r, std::memory_order_release);
        return true;
    }

    // ------------------------------------------------------------ hot path
    bool append(const char *data, uint32_t len, tblog_level lv) noexcept override {
        if (closed_.load(std::memory_order_acquire)) return false;
        if (len == 0) return true;
        if (len > rs_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        unsigned spins = 0;
        std::chrono::steady_clock::time_point wait_start{};
        for (;;) {
            Region *r = current_.load(std::memory_order_acquire);
            uint64_t p;
            if (try_reserve(r, len, &p) == kReserved) {
                std::memcpy(r->map + p, data, len);
                r->committed.fetch_add(len, std::memory_order_release);
                const uint64_t half = r->size / 2;
                if (p < half && p + len >= half) wake_worker();
                if (lv >= urgent_level_ && !urgent_.load(std::memory_order_relaxed) &&
                    !urgent_.exchange(true, std::memory_order_acq_rel))
                    wake_worker();
                return true;
            }
            // r is full.
            if (try_switch(r, true)) {
                spins = 0;
                continue;
            }
            if (closed_.load(std::memory_order_acquire)) return false;
            // No region ready (slow path, only when the lookahead queue ran
            // dry). Short pause spin, then sched_yield() until a time budget
            // runs out: yielding lets a worker that was woken onto this CPU
            // run (pure pause-spinning was measured to delay it by up to a
            // scheduler slice, ~0.8 ms). Budgets were 1 ms / 5 ms while fdatasync
            // ran on the worker; with sync on its own thread 200 us / 1 ms gave
            // 0 drops in 15 stress runs (64 KB regions, 8 threads). The budget is longer while the worker
            // is inside flush(): a region is about to be published (munmap /
            // populate were measured to stall for up to ~350 us under load).
            if (spins == 0) wake_worker();
            if (++spins <= kPauseSpins) {
                cpu_relax();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (spins == kPauseSpins + 1) wait_start = now;
            const auto budget = worker_busy_.load(std::memory_order_relaxed) ? kWaitBusy
                                                                           : kWaitIdle;
            if (!alloc_failed_.load(std::memory_order_relaxed) && now - wait_start < budget) {
                ::sched_yield();
                continue;
            }
            if (policy_ == FullPolicy::Drop) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            block_until_region(r);
            spins = 0;
        }
    }

    // ------------------------------------------------------------ worker
    void wait_for_work(std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lk(wm_);
        wcv_.wait_for(lk, timeout, [&] {
            return pending_.load(std::memory_order_acquire) || needs_region();
        });
        pending_.store(false, std::memory_order_release);
    }

    void flush() override {
        std::lock_guard<std::mutex> lk(wmu_);
        if (done_) return;
        worker_busy_.store(true, std::memory_order_relaxed);
        process_rotation();
        check_time();
        // Top the queue up first: munmap in retire() was measured to take up
        // to ~350 us under load, writers may be waiting for a region.
        prepare_ahead();
        retire();
        prepare_ahead();  // no-op unless writers switched meanwhile
        worker_busy_.store(false, std::memory_order_relaxed);
    }

    // fdatasync can take ~100 ms under load: never hold wmu_ (which region
    // preparation needs) while it runs. dup() keeps each file alive even if
    // rotation closes the original fd meanwhile.
    void sync() override {
        int fds[16];
        int n = 0;
        {
            std::lock_guard<std::mutex> lk(wmu_);
            if (done_) return;
            for (File *f : files_)
                if (f->fd >= 0 && n < 16) {
                    int d = ::dup(f->fd);
                    if (d >= 0) fds[n++] = d;
                }
        }
        for (int i = 0; i < n; ++i) {
            ::fdatasync(fds[i]);
            ::close(fds[i]);
        }
    }

    bool consume_urgent() noexcept override {
        if (!urgent_.load(std::memory_order_relaxed)) return false;
        return urgent_.exchange(false, std::memory_order_acq_rel);
    }

    void wake() noexcept override { wake_worker(); }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(wmu_);
        if (done_) return;
        done_ = true;
        closed_.store(true, std::memory_order_seq_cst);
        notify_blocked();

        // Seal every live region: from now on no reservation can succeed.
        for (Region *r : live_) {
            uint64_t p = r->pos.load(std::memory_order_acquire);
            while (p < r->size) {
                if (r->pos.compare_exchange_weak(p, r->size, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    r->shut_sealed = true;
                    r->seal_pos = p;
                    break;
                }
            }
        }
        // Wait (bounded) for writers that reserved before the seal.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (Region *r : live_) {
            uint64_t target = r->shut_sealed ? r->seal_pos : r->size;
            while (r->committed.load(std::memory_order_acquire) != target) {
                if (std::chrono::steady_clock::now() > deadline) {
                    std::fprintf(stderr, "tblog: mmap backend: writer stuck at shutdown; "
                                         "leaving region mapped\n");
                    r->stuck = true;
                    break;
                }
                std::this_thread::yield();
            }
        }
        for (Region *r : live_) {
            uint64_t used = r->shut_sealed ? r->seal_pos
                                           : r->pad_start.load(std::memory_order_relaxed);
            unmap_region(r, used);
        }
        live_.clear();
        // Data-driven: if anything landed in the pending file, finish rotation.
        for (File *f : files_) {
            if (f->pending && f->used_len > 0) {
                if (active_) active_->arch = arch_.archive(path_, active_->period, false);
                ::rename(next_path_.c_str(), path_.c_str());
                f->pending = false;
            }
        }
        for (File *f : files_) {
            if (f->fd < 0) continue;
            if (f->pending) {
                ::close(f->fd);
                ::unlink(next_path_.c_str());
            } else {
                if (::ftruncate(f->fd, (off_t)f->used_len) != 0)
                    std::fprintf(stderr, "tblog: mmap backend: ftruncate: %s\n",
                                 std::strerror(errno));
                ::fdatasync(f->fd);
                ::close(f->fd);
                arch_.mark_ready(f->arch);
            }
            f->fd = -1;
        }
        for (File *f : files_) delete f;
        files_.clear();
        active_ = nullptr;
        arch_.finish();  // let queued compressions complete (bounded)
        wake_worker();
    }

    // ------------------------------------------------------------ crash path
    // Data is already in the page cache: nothing to drain.
    void on_crash() noexcept override {}

    // Async-signal-safe: lock-free CAS reservation, no malloc, no locks, no
    // syscalls. If it does not fit (and no next region is ready) it is dropped.
    void crash_write(const char *data, size_t len) noexcept override {
        if (closed_.load(std::memory_order_acquire) || len == 0 || len > rs_) return;
        for (int tries = 0; tries < 8; ++tries) {
            Region *r = current_.load(std::memory_order_acquire);
            uint64_t p;
            if (try_reserve(r, len, &p) == kReserved) {
                std::memcpy(r->map + p, data, len);
                r->committed.fetch_add(len, std::memory_order_release);
                return;
            }
            if (!try_switch(r, false)) return;
        }
    }

    uint64_t dropped() const noexcept override {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    // Writer wait when no region is ready (see append): pause spins, then
    // sched_yield() for up to kWaitIdle, or kWaitBusy while the worker is
    // inside flush(); then Drop / Block. Only reached when the queue is empty.
    static constexpr unsigned kPauseSpins = 256;  // ~8 us
    static constexpr std::chrono::microseconds kWaitIdle{200};
    static constexpr std::chrono::microseconds kWaitBusy{1000};
    // Lookahead depth K: prepared regions kept queued behind current_. 2 lets a
    // burst eat two whole regions while the worker prepares (2.5-10 ms per
    // 16 MB region) or is descheduled; more only costs page cache.
    static constexpr uint64_t kLookahead = 2;
    static constexpr uint64_t kRing = kLookahead + 1;

    // Reserve len bytes in r. On kFull the region is full (this thread may
    // have padded the remainder).
    static ReserveResult try_reserve(Region *r, uint64_t len, uint64_t *out) noexcept {
        uint64_t cur = r->pos.load(std::memory_order_relaxed);
        for (;;) {
            if (cur + len <= r->size) {
                if (r->pos.compare_exchange_weak(cur, cur + len, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    *out = cur;
                    return kReserved;
                }
                continue;
            }
            if (cur >= r->size) return kFull;
            // Claim the remainder and pad it with '\n' so committed reaches size.
            if (r->pos.compare_exchange_weak(cur, r->size, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
                r->pad_start.store(cur, std::memory_order_relaxed);
                std::memset(r->map + cur, '\n', r->size - cur);
                r->committed.fetch_add(r->size - cur, std::memory_order_release);
                return kFull;
            }
        }
    }

    // r is full. Returns true if the caller should retry (current_ moved on,
    // either by us or by someone else), false if no next region is ready.
    // Lock-free and syscall-free unless `wake` (async-signal-safe with !wake).
    bool try_switch(Region *r, bool wake) noexcept {
        if (current_.load(std::memory_order_acquire) != r) return true;
        const uint64_t s = r->seq + 1;
        if (published_.load(std::memory_order_acquire) <= s) return false;
        Region *n = ring_[s % kRing].load(std::memory_order_acquire);
        // Slot already recycled for a later seq: current_ has moved past r.
        if (n->seq != s) return true;
        Region *expected = r;
        if (current_.compare_exchange_strong(expected, n, std::memory_order_acq_rel,
                                             std::memory_order_acquire) &&
            wake)
            wake_worker();  // the queue just dropped below K
        return true;
    }

    void wake_worker() noexcept {
        if (pending_.load(std::memory_order_relaxed)) return;
        if (pending_.exchange(true, std::memory_order_acq_rel)) return;
        { std::lock_guard<std::mutex> lk(wm_); }
        wcv_.notify_one();
    }

    void block_until_region(Region *r) noexcept {
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        wake_worker();
        {
            std::unique_lock<std::mutex> lk(bmu_);
            bcv_.wait_for(lk, std::chrono::milliseconds(5), [&] {
                return published_.load(std::memory_order_seq_cst) > r->seq + 1 ||
                       current_.load(std::memory_order_seq_cst) != r ||
                       closed_.load(std::memory_order_seq_cst);
            });
        }
        waiters_.fetch_sub(1, std::memory_order_seq_cst);
    }

    void notify_blocked() {
        if (waiters_.load(std::memory_order_seq_cst) == 0) return;
        { std::lock_guard<std::mutex> lk(bmu_); }
        bcv_.notify_all();
    }

    bool needs_region() const noexcept {
        if (closed_.load(std::memory_order_acquire) ||
            alloc_failed_.load(std::memory_order_relaxed))
            return false;
        Region *cur = current_.load(std::memory_order_acquire);
        if (cur->seq == stall_seq_.load(std::memory_order_relaxed)) return false;
        return published_.load(std::memory_order_acquire) - 1 - cur->seq < kLookahead;
    }

    // worker-only (under wmu_) -------------------------------------------
    // Creates (does not publish) a region at off. If [off, off + rs_) is not
    // preallocated yet, fallocates `batch` regions' worth in one call.
    Region *create_region(File *f, uint64_t off, uint64_t start, uint64_t batch) {
        int e = 0;
        if (off + rs_ > f->alloc_end) {
            e = ::posix_fallocate(f->fd, (off_t)off, (off_t)(rs_ * batch));
            if (e != 0 && batch > 1) e = ::posix_fallocate(f->fd, (off_t)off, (off_t)rs_);
            else if (e == 0) f->alloc_end = off + rs_ * batch;
        }
        if (e != 0) {
            // Never map unallocated blocks: a store to them could SIGBUS.
            if (!alloc_failed_.exchange(true))
                std::fprintf(stderr,
                             "tblog: mmap backend: cannot preallocate %llu bytes at %llu: %s;"
                             " lines will be dropped until space is available\n",
                             (unsigned long long)rs_, (unsigned long long)off, std::strerror(e));
            return nullptr;
        }
        void *m = ::mmap(nullptr, rs_, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, (off_t)off);
        if (m == MAP_FAILED) {
            if (!alloc_failed_.exchange(true))
                std::fprintf(stderr, "tblog: mmap backend: mmap failed: %s\n",
                             std::strerror(errno));
            return nullptr;
        }
        alloc_failed_.store(false, std::memory_order_relaxed);
        // Pre-fault every page for write (MAP_POPULATE only read-faults shared
        // mappings, the first store would still page_mkwrite-fault; see the
        // measurements at the top). MADV_POPULATE_WRITE does the write faults
        // without touching data. Fallback: the region is unpublished, so
        // rewriting each byte with itself is safe (the first page may hold
        // existing data when reopening).
        char *base = static_cast<char *>(m);
        if (!populate_write_ || ::madvise(base, rs_, MADV_POPULATE_WRITE) != 0) {
            if (populate_write_ && errno == EINVAL) populate_write_ = false;
            for (uint64_t i = 0; i < rs_; i += page_) {
                volatile char *q = base + i;
                *q = *q;
            }
        }
        Region *r = new Region;
        r->map = base;
        r->file_off = off;
        r->size = rs_;
        r->file = f;
        r->pos.store(start, std::memory_order_relaxed);
        r->committed.store(start, std::memory_order_relaxed);
        r->pad_start.store(rs_, std::memory_order_relaxed);
        r->mapped = true;
        f->live++;
        all_.push_back(r);
        live_.push_back(r);
        return r;
    }

    void unmap_region(Region *r, uint64_t used_end) {
        if (r->mapped && !r->stuck) {
            ::munmap(r->map, r->size);
            r->mapped = false;
        }
        File *f = r->file;
        // An empty region (e.g. the pre-mapped next one) holds no data: it
        // must not extend the used length to its start offset.
        uint64_t end = r->file_off + used_end;
        if (used_end > 0 && end > f->used_len) f->used_len = end;
        f->live--;
    }

    void process_rotation() {
        Region *cur = current_.load(std::memory_order_acquire);
        File *f = cur->file;
        if (f == active_ || !f->pending) return;
        active_->arch = arch_.archive(path_, active_->period, false);
        if (::rename(next_path_.c_str(), path_.c_str()) != 0)
            std::fprintf(stderr, "tblog: mmap backend: rename %s -> %s: %s\n",
                         next_path_.c_str(), path_.c_str(), std::strerror(errno));
        f->pending = false;
        f->period = arch_.now();
        active_ = f;
    }

    // Time rotation check, once per flush (see the header comment).
    void check_time() {
        if (!arch_.time_mode()) return;
        const int64_t now = arch_.now();
        if (now < next_boundary_) return;
        if (time_rotate(now)) next_boundary_ = arch_.next_boundary(now);
    }

    // Claims the unreserved remainder of r (as the padding path does, without
    // writing the padding: it lies beyond the file's final length).
    static void seal_early(Region *r) noexcept {
        uint64_t p = r->pos.load(std::memory_order_acquire);
        while (p < r->size) {
            if (r->pos.compare_exchange_weak(p, r->size, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                r->pad_start.store(p, std::memory_order_relaxed);
                r->committed.fetch_add(r->size - p, std::memory_order_release);
                return;
            }
        }
    }

    // Returns false if it must be retried (cannot create <path>.next).
    bool time_rotate(int64_t now) {
        process_rotation();
        Region *cur = current_.load(std::memory_order_acquire);
        // Nothing written to the live file yet: just start the new period.
        if (cur->file == active_ && cur->file_off == 0 &&
            cur->pos.load(std::memory_order_acquire) == 0) {
            active_->period = now;
            return true;
        }
        Region *nr = nullptr;
        if (tail_->file == active_) {
            int fd = ::open(next_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                std::fprintf(stderr, "tblog: mmap backend: cannot create %s: %s\n",
                             next_path_.c_str(), std::strerror(errno));
                return false;
            }
            File *nf = new File;
            nf->fd = fd;
            nf->pending = true;
            nr = create_region(nf, 0, 0, std::min<uint64_t>(kLookahead, regions_fitting(0)));
            if (!nr) {
                ::close(fd);
                ::unlink(next_path_.c_str());
                delete nf;
                return false;
            }
            files_.push_back(nf);
        }
        // From here on, no syscall until the new region is published.
        for (Region *r : live_)
            if (r->file == active_) seal_early(r);
        // Walk current_ to the last region of the active file (or into the
        // already pending file), exactly like switching writers do.
        for (;;) {
            Region *r = current_.load(std::memory_order_acquire);
            if (r->file != active_ || !try_switch(r, false)) break;
        }
        if (nr) {
            // current_ is the old tail_ now (nothing later was published), so
            // seq - current_->seq == 1 <= kLookahead: the slot is free.
            const uint64_t seq = published_.load(std::memory_order_relaxed);
            nr->seq = seq;
            tail_ = nr;
            ring_[seq % kRing].store(nr, std::memory_order_release);
            published_.store(seq + 1, std::memory_order_release);
            notify_blocked();
            Region *r = current_.load(std::memory_order_acquire);
            if (r->file == active_) try_switch(r, false);
        }
        process_rotation();  // app.log -> app.<old period>.log, .next -> app.log
        return true;
    }

    void retire() {
        Region *cur = current_.load(std::memory_order_acquire);
        for (size_t i = 0; i < live_.size();) {
            Region *r = live_[i];
            if (r->seq < cur->seq &&
                r->committed.load(std::memory_order_acquire) == r->size) {
                unmap_region(r, r->pad_start.load(std::memory_order_relaxed));
                live_.erase(live_.begin() + (long)i);
            } else {
                ++i;
            }
        }
        // Finalize rotated-out files whose last region is retired.
        for (size_t i = 0; i < files_.size();) {
            File *f = files_[i];
            if (f != active_ && !f->pending && f->live == 0) {
                if (::ftruncate(f->fd, (off_t)f->used_len) != 0)
                    std::fprintf(stderr, "tblog: mmap backend: ftruncate: %s\n",
                                 std::strerror(errno));
                ::fdatasync(f->fd);
                ::close(f->fd);
                arch_.mark_ready(f->arch);  // complete now: may be compressed
                delete f;
                files_.erase(files_.begin() + (long)i);
            } else {
                ++i;
            }
        }
    }

    // Top the queue up to kLookahead regions, publishing each as soon as it
    // is ready. Regions are created strictly in file order after tail_.
    void prepare_ahead() {
        stall_seq_.store(kNoStall, std::memory_order_relaxed);
        for (;;) {
            if (closed_.load(std::memory_order_acquire)) return;
            Region *cur = current_.load(std::memory_order_acquire);
            const uint64_t seq = published_.load(std::memory_order_relaxed);
            const uint64_t queued = seq - 1 - cur->seq;
            if (queued >= kLookahead) return;
            const uint64_t want = kLookahead - queued;
            uint64_t off = tail_->file_off + tail_->size;
            Region *nr = nullptr;
            if (off + rs_ > max_size_) {
                // Only one <path>.next can exist: never start a second new
                // file while the previous one has not been switched into.
                if (tail_->file->pending) {
                    if (cur->file != tail_->file) {
                        stall_seq_.store(cur->seq, std::memory_order_relaxed);
                        return;
                    }
                    process_rotation();  // writers switched in since flush() began
                }
                int fd = ::open(next_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (fd < 0) {
                    if (!alloc_failed_.exchange(true))
                        std::fprintf(stderr, "tblog: mmap backend: cannot create %s: %s\n",
                                     next_path_.c_str(), std::strerror(errno));
                    return;
                }
                File *nf = new File;
                nf->fd = fd;
                nf->pending = true;
                nr = create_region(nf, 0, 0, std::min<uint64_t>(want, regions_fitting(0)));
                if (!nr) {
                    ::close(fd);
                    ::unlink(next_path_.c_str());
                    delete nf;
                    return;
                }
                files_.push_back(nf);
            } else {
                nr = create_region(tail_->file, off, 0,
                                   std::min<uint64_t>(want, regions_fitting(off)));
                if (!nr) return;
            }
            nr->seq = seq;
            tail_ = nr;
            // Slot seq % kRing held seq - kRing <= cur->seq - 1 (behind current_).
            ring_[seq % kRing].store(nr, std::memory_order_release);
            published_.store(seq + 1, std::memory_order_release);
            notify_blocked();
        }
    }

    // Number of whole regions that fit in a file from `off` (at least 1).
    uint64_t regions_fitting(uint64_t off) const noexcept {
        if (off + rs_ > max_size_) return 1;
        return (max_size_ - off) / rs_;
    }

    // config
    Archiver arch_;                       // rotated files: names, retention, gzip
    int64_t next_boundary_ = INT64_MAX;   // worker-only: next period boundary
    const std::string path_;
    const std::string next_path_;
    const uint64_t max_size_;
    const tblog_level urgent_level_;
    const FullPolicy policy_;
    uint64_t page_ = 4096;
    uint64_t rs_ = 0;

    // shared with callers
    static constexpr uint64_t kNoStall = ~0ull;
    std::atomic<Region *> current_{nullptr};
    std::atomic<Region *> ring_[kRing] = {};      // prepared regions, by seq % kRing
    std::atomic<uint64_t> published_{0};         // regions published (next seq)
    std::atomic<uint64_t> stall_seq_{kNoStall};  // queue blocked at a file boundary
                                                 // while current_->seq == this
    std::atomic<bool> closed_{false};
    std::atomic<bool> urgent_{false};
    std::atomic<bool> pending_{false};
    std::atomic<bool> alloc_failed_{false};
    std::atomic<bool> worker_busy_{false};  // worker is inside flush() (hint only)
    std::atomic<int> waiters_{0};
    std::atomic<uint64_t> dropped_{0};

    std::mutex wm_;  // worker wakeup
    std::condition_variable wcv_;
    std::mutex bmu_;  // blocked writers (Block policy)
    std::condition_variable bcv_;

    // worker-only, guarded by wmu_
    std::mutex wmu_;
    bool done_ = false;
    std::vector<Region *> all_;   // every region ever created (freed in dtor)
    std::vector<Region *> live_;  // still mapped
    std::vector<File *> files_;
    File *active_ = nullptr;      // the file currently named path_
    Region *tail_ = nullptr;      // last created region
    bool populate_write_ = true;  // MADV_POPULATE_WRITE supported (cleared on EINVAL)
};

#if __cplusplus < 201703L
// Before C++17 static constexpr members are not implicitly inline: odr-used
// ones (bound to references, e.g. by std::chrono / std::min) need a definition.
constexpr unsigned MmapFileBackend::kPauseSpins;
constexpr std::chrono::microseconds MmapFileBackend::kWaitIdle;
constexpr std::chrono::microseconds MmapFileBackend::kWaitBusy;
constexpr uint64_t MmapFileBackend::kLookahead;
constexpr uint64_t MmapFileBackend::kRing;
constexpr uint64_t MmapFileBackend::kNoStall;
#endif

}  // namespace

std::unique_ptr<Backend> make_mmap_file_backend(const MmapOptions &opt) {
    std::unique_ptr<MmapFileBackend> b(new MmapFileBackend(opt));
    if (!b->init()) return nullptr;
    return std::unique_ptr<Backend>(std::move(b));
}

}  // namespace tblog
