// RotatingFile — append-only log file with size-based rotation. Internal to
// the backends (not thread-safe: callers serialise write_all/rotate/open/close;
// fd() and raw_write_all() may be used concurrently, e.g. from a signal handler).
#pragma once

#include "internal.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tblog {

class RotatingFile {
public:
    RotatingFile() = default;
    ~RotatingFile();
    RotatingFile(const RotatingFile &) = delete;
    RotatingFile &operator=(const RotatingFile &) = delete;

    // mkdir -p the parent directory and open (O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC).
    // Prints a message to stderr and returns false on failure.
    bool open(const FileOptions &opt);

    // Writes everything (partial writes / EINTR handled). Rotates first when
    // the data would push the file over max_size (splitting at '\n' so lines
    // are never cut across files). Write errors (ENOSPC, ...) are counted in
    // lost_bytes() with a rate-limited warning on stderr; never throws.
    void write_all(const char *data, size_t len) noexcept;

    void rotate() noexcept;     // app.log -> app.1.log -> ... ; reopen app.log
    void datasync() noexcept;   // fdatasync
    void close() noexcept;

    // Current fd, or -1. Safe to read from a signal handler.
    int fd() const noexcept { return fd_.load(std::memory_order_acquire); }
    uint64_t size() const noexcept { return size_; }
    uint64_t lost_bytes() const noexcept { return lost_.load(std::memory_order_relaxed); }
    const std::string &path() const noexcept { return opt_.path; }

    // Name of rotated file `index` (0 = path itself): "dir/app.log", "dir/app.1.log".
    std::string rotated_name(int index) const;

    // Async-signal-safe: write() loop with EINTR handling. Returns false on error.
    static bool raw_write_all(int fd, const char *data, size_t len) noexcept;

private:
    bool open_fd() noexcept;            // (re)open opt_.path, update size_
    void check_reopen() noexcept;       // at most once/second: deleted or renamed?
    void write_chunk(const char *data, size_t len) noexcept;
    void report_loss(size_t n, int err) noexcept;

    FileOptions opt_;
    std::atomic<int> fd_{-1};
    uint64_t size_ = 0;
    std::atomic<uint64_t> lost_{0};
    std::chrono::steady_clock::time_point last_check_{};
    std::chrono::steady_clock::time_point last_warn_{};
    bool warned_ = false;
    uint64_t lost_since_warn_ = 0;
};

}  // namespace tblog
