// tblog internal contracts. Every module implements exactly what is declared
// here for it; do not change a signature without telling the coordinator.
#pragma once

#include "tblog/log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if __cplusplus < 201103L
#error "tblog needs C++11 or newer to build (its public header log.h is plain C)"
#endif

namespace tblog {

// ============================================================================
// C++11 compatibility. The library builds as C++11, 14 and 17.
// ============================================================================
// std::make_unique is C++14.
template <class T, class... Args>
std::unique_ptr<T> make_unique(Args &&...args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// std::atomic<T>::is_always_lock_free is C++17; the GCC/Clang builtin gives
// the same compile-time answer on C++11/14. Signal handlers rely on it.
#if __cplusplus >= 201703L
#define TBLOG_ASSERT_LOCK_FREE(T) \
    static_assert(std::atomic<T>::is_always_lock_free, "std::atomic<" #T "> must be lock-free")
#else
#define TBLOG_ASSERT_LOCK_FREE(T) \
    static_assert(__atomic_always_lock_free(sizeof(T), 0), "std::atomic<" #T "> must be lock-free")
#endif

// Before C++17, `new` ignores alignas() above 16 bytes, so cache-line-aligned
// types (alignas(64) to avoid false sharing) could land misaligned. Types that
// use alignas(64) inherit this to get 64-byte-aligned heap allocations on every
// standard. Works through a base-class pointer as long as the destructor is
// virtual (delete then looks up operator delete in the dynamic type).
struct CacheAlignedNew {
    static void *operator new(std::size_t n) { return alloc(n); }
    static void *operator new[](std::size_t n) { return alloc(n); }
    static void operator delete(void *p) noexcept { std::free(p); }
    static void operator delete[](void *p) noexcept { std::free(p); }

private:
    static void *alloc(std::size_t n) {
        void *p = nullptr;
        if (posix_memalign(&p, 64, n ? n : 1) != 0) throw std::bad_alloc();
        return p;
    }
};

// ============================================================================
// Levels & modules — src/core/level_filter.cpp
// Also implements the C functions tblog_module_get, tblog_set_level,
// tblog_get_level from log.h.
// ============================================================================
const char *level_name(tblog_level lv);              // "TRACE".."FATAL","OFF"
bool parse_level(const char *s, tblog_level *out);   // case-insensitive
void reset_levels_for_test();                        // root=INFO, clear explicit

// ============================================================================
// Thread context & clock — src/core/thread_context.cpp
// ============================================================================
constexpr size_t kScratchSize = 1024;  // max bytes of one formatted line

struct ThreadContext {
    uint32_t tid;                // gettid(), cached
    char scratch[kScratchSize];  // per-thread formatting buffer
};
ThreadContext &thread_ctx();     // thread_local, lazily initialised
uint64_t now_ns();               // CLOCK_REALTIME in nanoseconds

// ============================================================================
// Formatter — src/core/formatter.cpp
// Pattern tokens:
//   %D date YYYY-MM-DD   %T time HH:MM:SS (local time)
//   %us microseconds (6 digits)   %ms milliseconds (3 digits)
//   %L level name   %t thread id   %n module name ("" for root -> "-")
//   %f file basename   %l line   %F function   %m message   %% literal '%'
// Unknown tokens are copied verbatim.
// ============================================================================
struct Record {
    uint64_t ts_ns;
    uint32_t tid;
    tblog_level level;
    const char *module;  // never null; "" = root
    const char *file;
    int line;
    const char *func;
};

class Formatter {
public:
    explicit Formatter(const std::string &pattern);
    ~Formatter();
    Formatter(const Formatter &) = delete;
    Formatter &operator=(const Formatter &) = delete;

    // Writes ONE complete line ending in '\n' into out (cap bytes, cap >= 64).
    // Returns bytes written. Too long -> message truncated, line ends "...\n".
    // Must not allocate. Thread-safe (const, may use thread_local caches).
    size_t format(char *out, size_t cap, const Record &rec,
                  const char *fmt, va_list ap) const;

    // Same line as format() when the message expands to `len` bytes, of which
    // msg holds at least min(len, cap). If utc_offset_s is non-null the date
    // and time are computed from it (no localtime_r, no thread_local): the
    // call is then async-signal-safe. Used by deferred formatting.
    size_t format_message(char *out, size_t cap, const Record &rec, const char *msg,
                          size_t len, const long *utc_offset_s = nullptr) const noexcept;

private:
    struct Impl;
    Impl *impl_;
};

// ============================================================================
// Sinks — src/sinks/console.cpp, src/sinks/syslog.cpp
// Sinks receive raw bytes (one or more complete lines) from the worker thread.
// ============================================================================
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const char *data, size_t len) = 0;
    virtual void flush() {}
};
std::unique_ptr<Sink> make_console_sink(bool color);       // stderr
std::unique_ptr<Sink> make_syslog_sink(const char *ident); // openlog/syslog

// ============================================================================
// Backends — src/backend/*.cpp
// ============================================================================
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
    // decisions (nullptr = CLOCK_REALTIME). Called on the worker only.
    int64_t (*now_fn)() = nullptr;
};

class Backend {
public:
    virtual ~Backend() = default;

    // ---- caller threads (hot path). Thread-safe. No syscalls in the common
    // case (except the sync backend). Returns false if the line was dropped.
    // `data` is one complete line ending in '\n'.
    virtual bool append(const char *data, uint32_t len, tblog_level lv) noexcept = 0;
    // Deferred formatting: `data` is a binary record from deferred_encode().
    // Only backends built with a deferred formatter accept it.
    virtual bool append_record(const char *, uint32_t, tblog_level) noexcept { return false; }

    // ---- worker thread only
    // Block until append() signalled work (buffer half full / urgent) or timeout.
    virtual void wait_for_work(std::chrono::milliseconds timeout) = 0;
    // Move pending data towards the kernel (write(), prepare next mmap region...).
    virtual void flush() = 0;
    // fdatasync / msync.
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

// Baseline: write() under a mutex on every append. src/backend/sync_file.cpp
std::unique_ptr<Backend> make_sync_file_backend(const FileOptions &opt);

// v1. src/backend/double_buffer.cpp
struct BufferOptions {
    FileOptions file;
    size_t buffer_size = 4u << 20;  // per buffer (A and B)
    FullPolicy policy = FullPolicy::Drop;
    // Non-null: format.mode = deferred. Buffers hold framed entries (text
    // lines and binary records); the worker renders records with *formatter.
    const std::atomic<const Formatter *> *formatter = nullptr;
};
std::unique_ptr<Backend> make_double_buffer_backend(const BufferOptions &opt);

// v2. src/backend/mmap_file.cpp
struct MmapOptions {
    FileOptions file;
    size_t region_size = 16u << 20;  // multiple of page size
    FullPolicy policy = FullPolicy::Drop;
};
std::unique_ptr<Backend> make_mmap_file_backend(const MmapOptions &opt);

// All factories return nullptr (and print to stderr) if the file cannot be opened.

// ============================================================================
// Config — src/config/config_parser.cpp, src/config/config_watcher.cpp
// ============================================================================
enum class BackendKind { Sync, Buffer, Mmap };
enum class FormatMode { Eager, Deferred };

struct Config {
    tblog_level level = TBLOG_LV_INFO;
    std::string pattern = "%D %T.%us [%L] [%t] %f:%l - %m";
    BackendKind backend = BackendKind::Buffer;
    FileOptions file;
    size_t buffer_size = 4u << 20;
    size_t mmap_region_size = 16u << 20;
    FullPolicy full_policy = FullPolicy::Drop;
    FormatMode format_mode = FormatMode::Eager;  // deferred: buffer backend only
    uint32_t flush_interval_ms = 200;
    uint32_t sync_interval_ms = 1000;
    bool console = false;
    bool console_color = true;
    bool syslog = false;
    std::vector<std::pair<std::string, tblog_level>> module_levels;
    bool reload_watch = true;
};

// Keys: level, pattern, backend (sync|buffer|mmap), file.path, file.max_size,
// file.max_files, file.rotate (size|daily|hourly), file.compress (none|gzip),
// buffer.size, mmap.region_size, full_policy (drop|block),
// format.mode (eager|deferred),
// flush.interval_ms, sync.interval_ms, sync.on_level, console, console.color,
// syslog, module.<name>, reload.watch.
// Sizes accept B/KB/MB/GB suffixes. Bools: true/false/1/0/yes/no/on/off.
// '#' starts a comment (outside the value of `pattern`, see parser docs).
// Unknown key / bad value -> a warning string, field keeps its previous value.
bool parse_config_string(const std::string &text, Config &out,
                         std::vector<std::string> *warnings);
// false if the file cannot be read (out untouched).
bool load_config(const char *path, Config &out, std::vector<std::string> *warnings);

class ConfigWatcher {
public:
    explicit ConfigWatcher(std::string path);
    // true if the file's mtime/size/inode changed since construction or the
    // previous true result. Cheap (one stat()).
    bool changed();

private:
    std::string path_;
    int64_t mtime_ns_ = -1;
    int64_t size_ = -1;
    uint64_t ino_ = 0;
};

// ============================================================================
// Deferred formatting — src/core/deferred.cpp (format.mode = deferred, v1).
// The caller encodes a binary record (header + raw printf arguments, %s copied
// by value) instead of formatting; the worker renders it into the same text.
// ============================================================================
constexpr size_t kRecordMax = 4096;  // max encoded record (else eager fallback)

// Caller side. Returns the record size, or 0 if this call must be formatted
// eagerly (fmt is not site->fmt, unsupported conversion, too large...).
// Consumes ap. Allocates once per call site (the cached argument plan).
size_t deferred_encode(char *out, size_t cap, uint64_t ts_ns, uint32_t tid, tblog_level lv,
                       const tblog_module *m, tblog_site *site, const char *fmt,
                       va_list ap) noexcept;
// Worker side. out must hold kScratchSize bytes. Output is byte-identical to
// Formatter::format() of the original call. Returns bytes written.
size_t deferred_render(char *out, const Formatter &f, const char *rec, size_t len);
// Signal handler side: async-signal-safe (no malloc/locks/stdio/locale).
// Exact for integer, char and string conversions; %p and floating point are
// approximate; date/time uses the UTC offset last noted below.
size_t deferred_render_crash(char *out, const Formatter &f, const char *rec,
                             size_t len) noexcept;
// Remember the local UTC offset for the crash renderer (normal context only).
void deferred_note_utc_offset(uint64_t ts_ns);
// Tests: drop the plan cached in a site (sites are normally static).
void deferred_site_reset(tblog_site *site);

// ============================================================================
// Crash handling — src/crash/crash_handler.cpp
// Installs SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL handlers on an alternate
// stack. On signal: backend->on_crash() (drain buffered lines first), then
// backend->crash_write("*** tblog: caught signal N (NAME) ***\n"), backtrace to stderr, then restore default handler and
// re-raise so a core dump is still produced. backend may be replaced by calling
// install again; nullptr disables the backend part.
// ============================================================================
void install_crash_handler(Backend *backend);
void uninstall_crash_handler();
// Gives the calling thread its own alternate signal stack (mmap'd, freed at
// thread exit) so a stack overflow on that thread is still reported.
bool crash_altstack_for_this_thread();

}  // namespace tblog
