// tblog platform layer: every OS-specific call the library makes goes through
// this header (namespace tblog::pf). The rest of src/ must not include OS
// headers or call OS APIs directly (enforced by
// packaging/check-platform-boundary.sh).
//
// Implementations:
//   src/platform/posix/*.cpp   Linux (and later macOS)
//   src/platform/win32/*.cpp   Windows (roadmap step 5; not written yet)
// plus one header of inline functions per platform (posix/inline.h,
// win32/inline.h) for the few calls on the logging hot path.
//
// Conventions:
//   * Functions never throw unless documented. Failures return false /
//     an invalid handle / -1 and leave an errno value in `errno` (the win32
//     implementation maps GetLastError() to the closest errno value), so
//     callers can report std::strerror(errno).
//   * Paths are UTF-8 std::strings. On Windows both '/' and '\\' separate
//     path components (is_path_separator / last_path_separator); the win32
//     implementation converts to UTF-16 and calls the W APIs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace tblog {
namespace pf {

// ============================================================================
// Files
// ============================================================================
#if defined(_WIN32)
typedef void *NativeFile;  // HANDLE; nullptr = none (INVALID_HANDLE_VALUE is mapped to nullptr)
#else
typedef int NativeFile;    // file descriptor; -1 = none
#endif

// A raw OS file handle. Trivially copyable (fits in a lock-free std::atomic,
// so a signal handler can read it); it does not own the handle, call
// file_close(). Use no_file() for "none" and valid() to test.
struct FileHandle {
    NativeFile native;
    bool valid() const noexcept;
    bool operator==(const FileHandle &o) const noexcept { return native == o.native; }
    bool operator!=(const FileHandle &o) const noexcept { return native != o.native; }
};
FileHandle no_file() noexcept;  // inline, in <os>/inline.h

enum class OpenMode {
    Append,    // write-only, create (0644), every write appends (O_APPEND /
               // FILE_APPEND_DATA). Log files.
    Read,      // read-only, must exist.
    Truncate,  // write-only, create (0644) or truncate to 0.
};

// Opens `path`. Handles are close-on-exec / not inherited by child processes.
// Windows: every handle is opened with FILE_SHARE_READ | FILE_SHARE_WRITE |
// FILE_SHARE_DELETE, so the file can still be renamed or deleted while open
// (rotation renames the live log file before closing it, and the archiver may
// shift a rotated file while compressing it).
FileHandle file_open(const std::string &path, OpenMode mode) noexcept;
void file_close(FileHandle h) noexcept;                 // invalid handle: no-op
FileHandle file_dup(FileHandle h) noexcept;             // independent handle to the same open file

// Writes all `len` bytes (partial writes and EINTR retried). Returns false on
// error (errno set; a 0-byte write counts as EIO). *written (optional)
// receives the number of bytes written before the error, or len.
bool file_write_all(FileHandle h, const void *data, size_t len, size_t *written = nullptr) noexcept;

// Async-signal-safe variant for the crash path: no locks, no allocation; the
// result is only the return value. Same loop as file_write_all.
bool file_write_crash(FileHandle h, const void *data, size_t len) noexcept;

// Reads up to `len` bytes (EINTR retried). Returns bytes read, 0 at end of
// file, -1 on error.
int64_t file_read(FileHandle h, void *buf, size_t len) noexcept;

bool file_sync(FileHandle h) noexcept;      // data + metadata (fsync / FlushFileBuffers)
bool file_datasync(FileHandle h) noexcept;  // data only (fdatasync; fsync on macOS; FlushFileBuffers)
bool file_truncate(FileHandle h, uint64_t size) noexcept;

// Identity of a file (dev + inode on POSIX; volume serial + file index on
// Windows). Two names / handles refer to the same file iff the ids are equal.
struct FileId {
    uint64_t dev;
    uint64_t ino;
    bool operator==(const FileId &o) const noexcept { return dev == o.dev && ino == o.ino; }
    bool operator!=(const FileId &o) const noexcept { return !(*this == o); }
};

struct FileInfo {
    uint64_t size;     // bytes
    int64_t mtime_ns;  // last modification, ns since the Unix epoch
    FileId id;
};
bool file_info(FileHandle h, FileInfo *out) noexcept;           // fstat
bool path_info(const std::string &path, FileInfo *out) noexcept; // stat (follows symlinks)

// ============================================================================
// Paths & directories
// ============================================================================
// Separator test ('/' on POSIX; '/' or '\\' on Windows). Inline: used by the
// formatter (%f basename) on the hot path.
bool is_path_separator(char c) noexcept;
// Index of the last separator in `path`, or std::string::npos.
size_t last_path_separator(const std::string &path) noexcept;

bool path_exists(const std::string &path) noexcept;  // anything at path (lstat: no symlink follow)
// mkdir -p for the directory part of `path` (everything before the last
// separator); true if it exists as a directory afterwards (or there is none).
bool make_parent_dirs(const std::string &path) noexcept;
// Atomic rename, replacing `to` if it exists (rename / MoveFileExW with
// MOVEFILE_REPLACE_EXISTING). Must work while `from` is open (see file_open).
bool rename_replace(const std::string &from, const std::string &to) noexcept;
bool remove_file(const std::string &path) noexcept;  // unlink / DeleteFileW
// Names (not paths) of the entries of `dir`, without "." and "..". false if
// the directory cannot be opened. May throw std::bad_alloc.
bool list_dir(const std::string &dir, std::vector<std::string> *names);
// Makes renames / creations in `dir` durable (fsync of the directory).
// No-op where that is meaningless (Windows).
void sync_dir(const std::string &dir) noexcept;

// ============================================================================
// Time
// ============================================================================
// Wall clock (CLOCK_REALTIME / GetSystemTimePreciseAsFileTime) in ns since
// the Unix epoch. Inline: called once per log line.
uint64_t realtime_ns() noexcept;
// Local-time breakdown of `t` (seconds since the epoch): localtime_r /
// localtime_s. false on failure (*out unspecified).
bool local_time(int64_t t, std::tm *out) noexcept;

// ============================================================================
// Threads
// ============================================================================
// OS id of the calling thread (gettid on Linux, pthread_threadid_np on macOS,
// GetCurrentThreadId on Windows), truncated to 32 bits, never 0. Not inline:
// callers cache it per thread (ThreadContext).
uint32_t current_thread_id() noexcept;

// ============================================================================
// Memory
// ============================================================================
// `align` must be a power of two >= sizeof(void*). nullptr on failure.
// posix_memalign/free; _aligned_malloc/_aligned_free on Windows.
void *alloc_aligned(size_t align, size_t size) noexcept;
void free_aligned(void *p) noexcept;

// ============================================================================
// Console (stderr)
// ============================================================================
FileHandle stderr_handle() noexcept;
bool stderr_is_terminal() noexcept;
// Prepares stderr for ANSI colour sequences; false if they cannot be shown.
// POSIX: no-op, true. Windows: ENABLE_VIRTUAL_TERMINAL_PROCESSING.
bool enable_ansi_colors() noexcept;
// Best-effort write of all bytes to stderr. Async-signal-safe.
void stderr_write(const char *data, size_t len) noexcept;

// ============================================================================
// Crash handling (src/platform/<os>/crash.cpp)
// ============================================================================
// Called on the crashing thread from the fatal-signal / exception handler.
// It must be async-signal-safe (no locks, no malloc, no stdio). `code` is the
// signal number (POSIX) or exception code (Windows); `name` e.g. "SIGSEGV".
typedef void (*CrashReporter)(int code, const char *name);

// Installs handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL (POSIX, on an
// alternate stack set up for the calling thread) or an unhandled-exception
// filter + SIGABRT handler (Windows). On a crash: the first crashing thread
// calls `reporter`, then a backtrace goes to stderr, then the previous
// handler / default action runs so a core dump is still produced. A fault
// inside the reporter, or a crash on a second thread, skips the reporter.
// Not thread-safe: the caller serialises install / uninstall.
void crash_install(CrashReporter reporter) noexcept;
void crash_uninstall() noexcept;  // restore previous handlers (and alt stack)
// Gives the calling thread its own alternate signal stack (freed at thread
// exit) so a stack overflow on it is still reported. Idempotent. Windows:
// SetThreadStackGuarantee.
bool crash_thread_init() noexcept;

// ============================================================================
// System log (src/platform/<os>/system_log.cpp)
// ============================================================================
enum class SyslogSeverity { Debug, Info, Warning, Error, Critical };
// false where there is no system log the library supports (then the
// functions below are no-ops).
bool syslog_available() noexcept;
// Reference-counted process-wide connection: the first open() connects with
// `ident` (nullptr/"" = program name), the last close() disconnects.
void syslog_open(const char *ident) noexcept;
void syslog_close() noexcept;
void syslog_write(SyslogSeverity sev, const char *msg, size_t len) noexcept;  // one message

}  // namespace pf
}  // namespace tblog

#if defined(_WIN32)
#include "platform/win32/inline.h"
#else
#include "platform/posix/inline.h"
#endif
