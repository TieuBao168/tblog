// File sink — src/sinks/file_sink.cpp: the log file, on top of RotatingFile
// (rotating_file.*: size/time rotation) and Archiver (archiver.*: naming,
// retention, gzip of rotated files).
#pragma once

#include "sinks/sink.h"

#include <cstdint>
#include <memory>
#include <string>

namespace tblog {

// file.rotate: Size = app.log -> app.1.log ... (index shifting). Daily/Hourly =
// rotate at each local-time period boundary into app.2026-11-13.log /
// app.2026-11-13T14.log (a size rotation inside one period adds .1, .2, ...);
// max_size still caps every file. Boundaries are detected on the worker side
// (FileSink::begin_batch, i.e. after the queue was taken).
enum class RotateMode { Size, Daily, Hourly };
// file.compress: gzip rotated files on a background thread (src/sinks/archiver.*).
enum class CompressMode { None, Gzip };

struct FileOptions {
    std::string path = "./logs/app.log";
    uint64_t max_size = 256ull << 20;          // rotate when exceeded
    int max_files = 5;                         // app.log + up to N-1 newest rotated files
    RotateMode rotate = RotateMode::Size;
    CompressMode compress = CompressMode::None;
    // Test hook: wall clock in seconds since the epoch used for period
    // decisions (nullptr = the real wall clock). Called on the worker only.
    int64_t (*now_fn)() = nullptr;
};

// Opens (creating parent directories) the log file. Returns nullptr, after a
// message on stderr, if it cannot be opened.
//   write        RotatingFile::write_all (rotation by size, never cuts a line)
//   begin_batch  time rotation check (file.rotate = daily|hourly): runs after
//                the queue was taken, so a line never lands in a file of an
//                earlier period than the one in effect when it was appended
//   sync         fdatasync on a dup of the handle (safe against a concurrent
//                rotation; the file lock is not held during the sync)
//   shutdown     fdatasync + close (waits for queued compressions, bounded)
//   crash_safe   true: crash_write() write()s to the current handle
std::unique_ptr<Sink> make_file_sink(const FileOptions &opt);

}  // namespace tblog
