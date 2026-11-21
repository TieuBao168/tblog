// Archiver — the set of rotated files of one log path: naming, retention and
// background gzip compression (zlib builds only). Used by RotatingFile.
// Internal.
//
// Names (path "dir/app.log" -> stem "dir/app", ext ".log"):
//   size mode    dir/app.1.log (newest) .. dir/app.(N-1).log, shifted on rotation
//   daily        dir/app.2026-11-13.log, then dir/app.2026-11-13.1.log, .2, ...
//   hourly       dir/app.2026-11-13T14.log, dir/app.2026-11-13T14.1.log, ...
//   compressed   <name>.gz ; in-flight compression writes <name>.gz.tmp
// In time modes a name is never reused or shifted: a size rotation inside a
// period takes the first free index (bare name first, then .1, .2, ...), so
// (period, index) order is chronological. Retention keeps the newest
// max_files - 1 rotated files (the live file is the N-th), compressed or not.
//
// Compression protocol (crash-safe): open name, write name.gz.tmp, fsync it,
// rename -> name.gz, fsync the directory, then unlink name. At startup
// recover() deletes every *.gz.tmp (the original is still there) and deletes
// `name` if `name.gz` exists (the .gz is only ever created complete). A crash
// thus leaves either the original or a complete .gz (possibly both, briefly).
//
// All file system access goes through the platform layer (pf::); zlib is
// used through its stream API (deflate), so no OS file descriptor reaches it.
//
// Threading: archive()/mark_ready() run on the worker (FileSink) and only take
// mu_ for a few rename/unlink/stat syscalls; the compressor thread holds mu_
// only around open/rename/unlink, never while compressing. Callers' hot path
// never touches this class.
#pragma once

#include "sinks/file_sink.h"
#include "platform/platform.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tblog {

class Archiver {
public:
    Archiver() = default;
    ~Archiver();
    Archiver(const Archiver &) = delete;
    Archiver &operator=(const Archiver &) = delete;

    // Before anything else (and again only after finish()). Without zlib
    // (TBLOG_HAVE_ZLIB unset) CompressMode::Gzip silently falls back to None
    // (validate_config() warns about it).
    void configure(const FileOptions &opt);

    bool time_mode() const noexcept { return rotate_ != RotateMode::Size; }
    bool compress() const noexcept { return compress_ != CompressMode::None; }

    // ---- clock / periods (local time). All in seconds since the epoch.
    int64_t now() const noexcept;
    int64_t period_start(int64_t t) const noexcept;   // start of t's period
    int64_t next_boundary(int64_t t) const noexcept;  // first boundary > t
    std::string period_label(int64_t t) const;        // "2026-11-13" / "2026-11-13T14"

    // ---- names
    std::string size_name(int index) const;  // index 0 = path itself
    std::string time_name(const std::string &label, int index) const;

    // ---- startup, in this order: recover(); start(). recover() cleans
    // interrupted compressions; start() queues every uncompressed rotated file
    // for compression (if enabled) and applies retention (time modes).
    void recover() noexcept;
    void start() noexcept;

    // Renames `live` into the rotated set (period = any time inside the
    // period its content belongs to; unused in size mode) and applies
    // retention. max_files <= 1: `live` is unlinked. Returns a handle for
    // mark_ready() (0 = nothing to compress). With ready == false the file is
    // compressed only after mark_ready(handle) (e.g. once its fd is closed).
    uint64_t archive(const std::string &live, int64_t period, bool ready) noexcept;
    void mark_ready(uint64_t handle) noexcept;

    // Waits (at most `budget`) for queued compressions, then aborts the rest
    // (their originals stay and are picked up by the next start()) and stops
    // the thread. Idempotent.
    void finish(std::chrono::milliseconds budget = std::chrono::seconds(10)) noexcept;

private:
    struct Job {
        uint64_t id;
        std::string src;  // current name of the uncompressed file
        bool ready;
        bool active = false;
        bool cancelled = false;
    };
    struct Entry {
        std::string name;  // full path of the uncompressed name
        std::string key;   // sortable: time: label + index; size: index (zero-padded)
        bool is_time = false;
        bool plain = false, gz = false, tmp = false;
    };

    std::vector<Entry> scan() const;  // rotated files on disk (both naming kinds)
    void retention_locked();
    void cancel_locked(const std::string &src);
    void add_job_locked(uint64_t id, const std::string &src, bool ready);
    void compressor_main();
    bool compress_file(pf::FileHandle in, pf::FileHandle out) noexcept;
    void ensure_thread_locked();

    std::string path_, dir_, stem_, ext_, base_prefix_;
    char sep_ = '/';  // separator used to join dir_ and a name (the one in path_)
    int max_files_ = 5;
    RotateMode rotate_ = RotateMode::Size;
    CompressMode compress_ = CompressMode::None;
    int64_t (*now_fn_)() = nullptr;

    std::mutex mu_;
    std::condition_variable cv_;       // compressor wakeups
    std::condition_variable idle_cv_;  // finish() waits for an empty queue
    std::list<Job> jobs_;
    uint64_t next_id_ = 1;
    bool stopping_ = false;
    std::atomic<bool> abort_{false};  // read by compress_file() without mu_
    bool thread_started_ = false;
    std::thread thread_;
};

}  // namespace tblog
