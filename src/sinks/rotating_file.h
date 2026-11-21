// RotatingFile — append-only log file with size- and time-based rotation.
// Internal to FileSink (not thread-safe: callers serialise
// write_all/rotate/check_time/open/close; handle() may be used concurrently,
// e.g. from a signal handler, with pf::file_write_crash()). Naming, retention and gzip of the
// rotated files live in Archiver (archiver.h).
#pragma once

#include "sinks/archiver.h"
#include "sinks/file_sink.h"
#include "platform/platform.h"

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

    // mkdir -p the parent directory and open for append (pf::OpenMode::Append).
    // Prints a message to stderr and returns false on failure.
    bool open(const FileOptions &opt);

    // Writes everything (partial writes / EINTR handled). Rotates first when
    // the data would push the file over max_size (splitting at '\n' so lines
    // are never cut across files). Write errors (ENOSPC, ...) are counted in
    // lost_bytes() with a rate-limited warning on stderr; never throws.
    void write_all(const char *data, size_t len) noexcept;

    void rotate() noexcept;     // app.log -> app.1.log / app.<period>[.N].log; reopen app.log

    // Time rotation (file.rotate = daily|hourly): if the clock passed the
    // current period's end, rotate (a non-empty file) and start the new
    // period. Worker side only: callers call it once per flush, AFTER taking
    // the data to write (so a line never lands in a file of an earlier period
    // than the one in effect when it was appended). Cheap: one clock read.
    void check_time() noexcept;
    void datasync() noexcept;   // pf::file_datasync
    void close() noexcept;

    // Current handle (invalid when closed). Safe to read from a signal handler.
    pf::FileHandle handle() const noexcept { return fd_.load(std::memory_order_acquire); }
    bool is_open() const noexcept { return handle().valid(); }
    uint64_t size() const noexcept { return size_; }
    uint64_t lost_bytes() const noexcept { return lost_.load(std::memory_order_relaxed); }
    const std::string &path() const noexcept { return opt_.path; }

    // Name of rotated file `index` (0 = path itself): "dir/app.log", "dir/app.1.log".
    std::string rotated_name(int index) const;

private:
    bool open_fd() noexcept;            // (re)open opt_.path, update size_ (errno on failure)
    void check_reopen() noexcept;       // at most once/second: deleted or renamed?
    void write_chunk(const char *data, size_t len) noexcept;
    void report_loss(size_t n, int err) noexcept;

    void start_period(int64_t now) noexcept;

    FileOptions opt_;
    Archiver arch_;
    int64_t period_ = 0;                 // any time inside the current file's period
    int64_t next_boundary_ = INT64_MAX;  // rotate when now >= this (time modes)
    std::atomic<pf::FileHandle> fd_{pf::no_file()};
    uint64_t size_ = 0;
    std::atomic<uint64_t> lost_{0};
    std::chrono::steady_clock::time_point last_check_{};
    std::chrono::steady_clock::time_point last_warn_{};
    bool warned_ = false;
    uint64_t lost_since_warn_ = 0;
};

}  // namespace tblog
