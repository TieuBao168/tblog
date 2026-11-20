// Backends — src/backend/*.cpp
#pragma once

#include "tblog/log.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace tblog {

class Sink;

enum class FullPolicy { Drop, Block };

// file.rotate: Size = app.log -> app.1.log ... (index shifting). Daily/Hourly =
// rotate at each local-time period boundary into app.2026-11-13.log /
// app.2026-11-13T14.log (a size rotation inside one period adds .1, .2, ...);
// max_size still caps every file. Boundaries are detected on the worker side.
enum class RotateMode { Size, Daily, Hourly };
// file.compress: gzip rotated files on a background thread (src/backend/archiver.*).
enum class CompressMode { None, Gzip };

struct FileOptions {
    std::string path = "./logs/app.log";
    uint64_t max_size = 256ull << 20;          // rotate when exceeded
    int max_files = 5;                         // app.log + up to N-1 newest rotated files
    tblog_level urgent_level = TBLOG_LV_ERROR; // append at >= this wakes worker + marks urgent
    RotateMode rotate = RotateMode::Size;
    CompressMode compress = CompressMode::None;
    // Test hook: wall clock in seconds since the epoch used for period
    // decisions (nullptr = the real wall clock). Called on the worker only.
    int64_t (*now_fn)() = nullptr;
};

class Backend {
public:
    virtual ~Backend() = default;

    // ---- caller threads (hot path). Thread-safe. No syscalls in the common
    // case. Returns false if the line was dropped.
    // `data` is one complete line ending in '\n'.
    virtual bool append(const char *data, uint32_t len, tblog_level lv) noexcept = 0;

    // ---- worker thread only
    // Block until append() signalled work (buffer half full / urgent) or timeout.
    virtual void wait_for_work(std::chrono::milliseconds timeout) = 0;
    // Move pending data towards the kernel (write()).
    virtual void flush() = 0;
    // fdatasync.
    virtual void sync() = 0;
    // True once since the last call if an urgent (>= urgent_level) line arrived.
    virtual bool consume_urgent() noexcept = 0;
    // Wake a thread blocked in wait_for_work (used for shutdown / tblog_flush).
    virtual void wake() noexcept = 0;

    // ---- any thread, once: final flush + sync, release files. Idempotent.
    // After shutdown, append() returns false.
    virtual void shutdown() = 0;

    // ---- signal handler: async-signal-safe ONLY (no locks, no malloc).
    // Push whatever is buffered to the file, best effort.
    virtual void on_crash() noexcept = 0;
    // Append raw bytes (e.g. "*** caught signal 11 ***\n") to the log file.
    virtual void crash_write(const char *data, size_t len) noexcept = 0;

    // Secondary sinks (non-owning). Call before the worker starts. Backends
    // that cannot feed sinks ignore them.
    virtual void add_sink(Sink *) {}

    virtual uint64_t dropped() const noexcept = 0;
};

// Sharded double buffers. src/backend/double_buffer.cpp
// make_double_buffer_backend() returns nullptr (and prints to stderr) if the
// file cannot be opened.
struct BufferOptions {
    FileOptions file;
    size_t buffer_size = 4u << 20;  // per buffer (A and B)
    FullPolicy policy = FullPolicy::Drop;
};
std::unique_ptr<Backend> make_double_buffer_backend(const BufferOptions &opt);

}  // namespace tblog
