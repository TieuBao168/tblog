// Sinks — where the worker sends the lines it takes from the Queue.
//   file_sink.cpp  FileSink    (rotating log file; crash-safe)
//   console.cpp    ConsoleSink (stderr; crash-safe)
//   syslog.cpp     SyslogSink  (system log)
//   callback.cpp   CallbackSink (user sink registered with tblog_add_sink)
// The worker drives them through Pipeline (src/logger/pipeline.h).
//
// Threading contract:
//   * write / begin_batch / end_batch / flush / shutdown: the worker thread
//     only (or the thread calling Pipeline::shutdown() once the worker has
//     stopped) -- never concurrently with each other.
//   * sync(): the syncer thread (sync.interval_ms, urgent lines) and the
//     worker (tblog_flush), CONCURRENTLY with write(); a sink that implements
//     it must synchronise with its own write path.
//   * crash_safe() / crash_write(): from the fatal-signal handler, possibly
//     while the worker is inside write() on another thread (or on the
//     crashing thread itself). crash_write() must be async-signal-safe: no
//     locks, no allocation, no stdio. Only called when crash_safe() is true.
//
// Per worker cycle: begin_batch(); write(chunk) for each chunk (zero or more,
// each holds one or more complete lines, in queue order); end_batch(); then
// flush() if anything was written or tblog_flush() asked for it.
#pragma once

#include "platform/platform.h"

#include <cstddef>
#include <memory>

namespace tblog {

class Sink {
public:
    virtual ~Sink() = default;

    // One or more complete lines ('\n'-terminated).
    virtual void write(const char *data, size_t len) = 0;
    // Called once per worker cycle, even when the batch is empty, AFTER the
    // queue was taken (FileSink checks the time rotation here) / after the
    // last write of the cycle.
    virtual void begin_batch() {}
    virtual void end_batch() {}
    virtual void flush() {}
    // Make written data durable (fdatasync for files). Thread: see above.
    virtual void sync() {}
    // Final: make everything durable, release resources. Called once, after
    // the last write. Default: flush().
    virtual void shutdown() { flush(); }

    // True if crash_write() may be called from a signal handler.
    virtual bool crash_safe() const noexcept { return false; }
    // Async-signal-safe ONLY. Raw bytes (drained lines, the crash marker).
    virtual void crash_write(const char *, size_t) noexcept {}
    // True if this sink writes to the process's stderr (the crash handler
    // then prints its marker through the sink instead of directly).
    virtual bool writes_stderr() const noexcept { return false; }
};

std::unique_ptr<Sink> make_console_sink(bool color);  // stderr
// System log (openlog/syslog on POSIX). Returns nullptr, after one warning on
// stderr, where the platform has no supported system log.
std::unique_ptr<Sink> make_syslog_sink(const char *ident);

// User sink (tblog_add_sink): write(ctx, data, len) per chunk, flush(ctx)
// (may be null) on flush and at shutdown. Not crash-safe.
typedef void (*SinkWriteFn)(void *ctx, const char *data, size_t len);
typedef void (*SinkFlushFn)(void *ctx);
std::unique_ptr<Sink> make_callback_sink(SinkWriteFn write, SinkFlushFn flush, void *ctx);

// Test hook: write `data` to `h` exactly as the console sink would,
// colourised when `color` is true (no terminal check here).
void console_write_to(pf::FileHandle h, const char *data, size_t len, bool color);

}  // namespace tblog
