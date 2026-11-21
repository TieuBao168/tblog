// Shared helpers of bench_vs_printf.cpp and bench_crash_loss.cpp: clocks, the
// printf-side log line (same layout as tblog's default pattern), scratch
// directories, command-line options, the machine description and a minimal
// JSON writer. Not part of the library: OS headers are fine here (the
// platform boundary applies to src/ only). ASCII paths only.
#pragma once

#include "tblog/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")  // RegGetValueA (machine description)
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define BENCH_HAVE_TSC 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif
#if defined(__MINGW32__) && !defined(__clang__)
#define BENCH_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#elif defined(__GNUC__) || defined(__clang__)
#define BENCH_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define BENCH_PRINTF(f, a)
#endif

namespace bench {

// ------------------------------------------------------------------ clocks
inline uint64_t now_ns() {  // steady_clock: every interval and latency
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Wall clock for the printf variants' timestamps: the same source tblog uses
// (src/platform/*: GetSystemTimePreciseAsFileTime / clock_gettime(REALTIME)).
inline uint64_t wall_ns() {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Busy-waits until steady time `t` (pacing: sleeping would add scheduler jitter).
inline void spin_until(uint64_t t) {
    while (now_ns() < t) {
    }
}

// Per-call timing: the CPU's time-stamp counter on x86 (steady_clock ticks
// only every 100 ns on Windows), calibrated against steady_clock; steady_clock
// itself elsewhere (ARM: macOS ticks at ~42 ns, Linux at 1 ns).
inline double &ns_per_tick() {
    static double v = 1.0;
    return v;
}
inline const char *&tick_source() {
    static const char *s = "steady_clock";
    return s;
}
inline uint64_t ticks() {
#if defined(BENCH_HAVE_TSC)
    return __rdtsc();
#else
    return now_ns();
#endif
}
inline double ticks_to_ns(uint64_t d) { return double(d) * ns_per_tick(); }
inline uint64_t ns_to_ticks(uint64_t ns) { return (uint64_t)(double(ns) / ns_per_tick()); }
// Measures the tick rate over ~200 ms (call once, before any measurement).
inline void calibrate_ticks() {
#if defined(BENCH_HAVE_TSC)
    const uint64_t n0 = now_ns(), c0 = ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t n1 = now_ns(), c1 = ticks();
    if (c1 > c0 && n1 > n0) {
        ns_per_tick() = double(n1 - n0) / double(c1 - c0);
        tick_source() = "rdtsc (calibrated against steady_clock)";
    }
#endif
}

struct TimerOverhead {
    double min_ns, median_ns;  // one ticks() pair
    double steady_resolution_ns;  // smallest non-zero steady_clock step
};
// Cost of one ticks() pair (what every per-call latency below includes).
inline TimerOverhead timer_overhead() {
    std::vector<uint64_t> v(20000);
    for (auto &x : v) {
        const uint64_t a = ticks();
        x = ticks() - a;
    }
    std::sort(v.begin(), v.end());
    uint64_t res = ~0ull;
    for (int i = 0; i < 2000; ++i) {
        const uint64_t a = now_ns();
        uint64_t b = now_ns();
        while (b == a) b = now_ns();
        res = std::min(res, b - a);
    }
    return {ticks_to_ns(v.front()), ticks_to_ns(v[v.size() / 2]), double(res)};
}

inline unsigned long os_tid() {
    static thread_local unsigned long tid = 0;
    if (tid == 0) {
#if defined(_WIN32)
        tid = (unsigned long)GetCurrentThreadId();
#elif defined(__APPLE__)
        uint64_t t = 0;
        pthread_threadid_np(nullptr, &t);
        tid = (unsigned long)(uint32_t)t;
#elif defined(__linux__)
        tid = (unsigned long)syscall(SYS_gettid);
#else
        tid = (unsigned long)getpid();
#endif
    }
    return tid;
}

// ------------------------------------------------------------------ printf line
// The printf variants write exactly what tblog's default pattern
// "%D %T.%us [%L] [%t] %f:%l - %m" produces, with one fprintf call per line:
//   2026-11-22 10:11:12.123456 [INFO] [1234] bench_vs_printf.cpp:123 - iteration 7 value=49 name=t0
// Date and time come from a per-thread cache refreshed once per second
// (localtime once a second, like tblog's formatter); the microseconds, the
// thread id (cached per thread, like tblog) and the file basename (scanned on
// every call, like tblog) are filled in on each call.
struct Stamp {
    char date[11];
    char time[9];
    unsigned us;
};

inline void stamp_now(Stamp *out) {
    struct Cache {
        int64_t sec = -1;
        char date[11];
        char time[9];
    };
    static thread_local Cache c;
    const uint64_t ns = wall_ns();
    const int64_t sec = (int64_t)(ns / 1000000000ull);
    if (sec != c.sec) {
        const time_t tt = (time_t)sec;
        std::tm tm;
        std::memset(&tm, 0, sizeof tm);
#if defined(_WIN32)
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        // Unsigned and bounded, so the compiler can prove the fields fit.
        std::snprintf(c.date, sizeof c.date, "%04u-%02u-%02u", (unsigned)(tm.tm_year + 1900) % 10000u,
                      (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u);
        std::snprintf(c.time, sizeof c.time, "%02u:%02u:%02u", (unsigned)tm.tm_hour % 100u,
                      (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u);
        c.sec = sec;
    }
    std::memcpy(out->date, c.date, sizeof c.date);
    std::memcpy(out->time, c.time, sizeof c.time);
    out->us = (unsigned)(ns % 1000000000ull / 1000u);
}

inline const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

// One log line through stdio (the printf variants). `flush`: fflush after it.
#define BENCH_FPRINTF_LINE(f, i, name, flush)                                                     \
    do {                                                                                          \
        ::bench::Stamp bench_st_;                                                                 \
        ::bench::stamp_now(&bench_st_);                                                           \
        std::fprintf((f), "%s %s.%06u [INFO] [%lu] %s:%d - iteration %ld value=%d name=%s\n",    \
                     bench_st_.date, bench_st_.time, bench_st_.us, ::bench::os_tid(),             \
                     ::bench::base_name(__FILE__), __LINE__, (long)(i), (int)((i) * 7 % 1000),    \
                     (name));                                                                     \
        if (flush) std::fflush(f);                                                                \
    } while (0)

// The same line formatted with snprintf into a stack buffer, then one fwrite
// (what tblog does with vsnprintf, minus the queue): separates the cost of
// formatting from the C library's stream path.
#define BENCH_SNPRINTF_FWRITE_LINE(f, i, name)                                                          \
    do {                                                                                                \
        ::bench::Stamp bench_st_;                                                                       \
        ::bench::stamp_now(&bench_st_);                                                                 \
        char bench_buf_[1024];                                                                          \
        int bench_n_ = std::snprintf(bench_buf_, sizeof bench_buf_,                                     \
                                     "%s %s.%06u [INFO] [%lu] %s:%d - iteration %ld value=%d name=%s\n", \
                                     bench_st_.date, bench_st_.time, bench_st_.us, ::bench::os_tid(),   \
                                     ::bench::base_name(__FILE__), __LINE__, (long)(i),                 \
                                     (int)((i) * 7 % 1000), (name));                                    \
        if (bench_n_ > 0)                                                                               \
            std::fwrite(bench_buf_, 1,                                                                  \
                        (size_t)bench_n_ < sizeof bench_buf_ ? (size_t)bench_n_ : sizeof bench_buf_ - 1, \
                        (f));                                                                           \
    } while (0)

// The same line through tblog (the prefix comes from tblog's default pattern).
#define BENCH_TBLOG_LINE(i, name) \
    TBLOG_INFO("iteration %ld value=%d name=%s", (long)(i), (int)((i) * 7 % 1000), (name))

// The data reached the OS (fflush) -> also the disk (like tblog_shutdown's fsync).
inline void fsync_file(FILE *f) {
#if defined(_WIN32)
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
}

// ------------------------------------------------------------------ files
inline bool is_sep(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

inline bool is_dir(const std::string &p) {
#if defined(_WIN32)
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

inline void make_dirs(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && !is_sep(dir[i])) continue;
        const std::string part = dir.substr(0, i);
#if defined(_WIN32)
        if (part.size() == 2 && part[1] == ':') continue;
        CreateDirectoryA(part.c_str(), nullptr);
#else
        ::mkdir(part.c_str(), 0755);
#endif
    }
}

inline void remove_tree(const std::string &p) {
#if defined(_WIN32)
    const DWORD a = GetFileAttributesA(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((p + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::string n = fd.cFileName;
                if (n != "." && n != "..") remove_tree(p + "\\" + n);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(p.c_str());
    } else {
        DeleteFileA(p.c_str());
    }
#else
    struct stat st;
    if (::lstat(p.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = ::opendir(p.c_str())) {
            while (dirent *e = ::readdir(d)) {
                const std::string n = e->d_name;
                if (n != "." && n != "..") remove_tree(p + "/" + n);
            }
            ::closedir(d);
        }
        ::rmdir(p.c_str());
    } else {
        ::unlink(p.c_str());
    }
#endif
}

inline int pid() {
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)::getpid();
#endif
}

// The system's temporary directory.
inline std::string temp_root() {
#if defined(_WIN32)
    char buf[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof buf, buf);
    std::string s = (n > 0 && n < sizeof buf) ? std::string(buf, n) : std::string(".");
    while (s.size() > 3 && is_sep(s.back())) s.pop_back();
    return s;
#else
    const char *t = std::getenv("TMPDIR");
    std::string s = (t && *t) ? t : "/tmp";
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
#endif
}

inline std::string read_file(const std::string &path) {
    std::string s;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

inline bool write_file(const std::string &path, const std::string &data) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

// Lines of a log file, without loading it whole: `lines` counts every
// '\n'-terminated line, `notes` those that start with "tblog: " (tblog's own
// notes, e.g. "tblog: 123 lines dropped" after the drop counter moved).
struct LineCount {
    uint64_t lines = 0, notes = 0;
};
inline LineCount count_lines(const std::string &path) {
    LineCount c;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return c;
    static char buf[1 << 16];
    static const char kNote[] = "tblog: ";
    size_t col = 0;     // position in the current line
    bool note = true;   // the current line still matches kNote so far
    size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < r; ++i) {
            if (buf[i] == '\n') {
                ++c.lines;
                c.notes += note && col >= sizeof kNote - 1;
                col = 0;
                note = true;
                continue;
            }
            if (col < sizeof kNote - 1 && buf[i] != kNote[col]) note = false;
            ++col;
        }
    }
    std::fclose(f);
    return c;
}

// tblog config used by every tblog variant: the defaults, except file only
// (console = false is the default too), no config watcher, and a file size
// limit large enough that a run never rotates.
inline std::string tblog_config(const std::string &log_path, const char *policy) {
    return "file.path = " + log_path + "\nconsole = false\nreload.watch = false\nfile.max_size = 4GB\n" +
           "full_policy = " + policy + "\n";
}

// ------------------------------------------------------------------ options
struct Args {
    std::vector<std::string> v;
    Args(int argc, char **argv) : v(argv + 1, argv + argc) {}
    bool flag(const char *name) const {
        for (const auto &a : v)
            if (a == name) return true;
        return false;
    }
    // --name value or --name=value; `def` if absent.
    std::string get(const char *name, const std::string &def) const {
        const std::string n = name, eq = n + "=";
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == n && i + 1 < v.size()) return v[i + 1];
            if (v[i].compare(0, eq.size(), eq) == 0) return v[i].substr(eq.size());
        }
        return def;
    }
    long long num(const char *name, long long def) const {
        const std::string s = get(name, "");
        return s.empty() ? def : std::strtoll(s.c_str(), nullptr, 10);
    }
};

inline std::vector<std::string> split(const std::string &s, char sep = ',') {
    std::vector<std::string> out;
    size_t b = 0;
    while (b <= s.size()) {
        size_t e = s.find(sep, b);
        if (e == std::string::npos) e = s.size();
        if (e > b) out.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

inline std::vector<int> int_list(const std::string &s) {
    std::vector<int> out;
    for (const auto &x : split(s)) out.push_back(std::atoi(x.c_str()));
    return out;
}

// ------------------------------------------------------------------ JSON
inline std::string jstr(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o + "\"";
}

inline std::string jnum(double d) {
    if (!(d == d) || d > 1e300 || d < -1e300) return "null";
    char b[64];
    if (d == (double)(long long)d && d < 9e15 && d > -9e15)
        std::snprintf(b, sizeof b, "%lld", (long long)d);
    else
        std::snprintf(b, sizeof b, "%.6g", d);
    return b;
}

// Builds {"k": v, ...} in insertion order; values are already JSON text.
class Obj {
public:
    Obj &raw(const std::string &k, const std::string &json) {
        s_ += (s_.empty() ? "{" : ", ") + jstr(k) + ": " + json;
        return *this;
    }
    Obj &str(const std::string &k, const std::string &v) { return raw(k, jstr(v)); }
    Obj &num(const std::string &k, double v) { return raw(k, jnum(v)); }
    Obj &boolean(const std::string &k, bool v) { return raw(k, v ? "true" : "false"); }
    std::string done() const { return s_.empty() ? "{}" : s_ + "}"; }

private:
    std::string s_;
};

inline std::string jarr(const std::vector<std::string> &items, const char *sep = ", ") {
    std::string o = "[";
    for (size_t i = 0; i < items.size(); ++i) o += (i ? sep : "") + items[i];
    return o + "]";
}

// ------------------------------------------------------------------ statistics
// Percentile of a sorted vector (nearest rank on index p*(n-1)).
template <class T>
double pct(const std::vector<T> &sorted, double p) {
    if (sorted.empty()) return 0;
    size_t i = (size_t)(p * double(sorted.size() - 1) + 0.5);
    if (i >= sorted.size()) i = sorted.size() - 1;
    return double(sorted[i]);
}

// {"median": m, "min": a, "max": b} of one metric across runs.
inline std::string spread(std::vector<double> v) {
    if (v.empty()) return "null";
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    const double med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    return Obj().num("median", med).num("min", v.front()).num("max", v.back()).done();
}

// ------------------------------------------------------------------ machine
inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t'))
        s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

#if defined(_WIN32)
inline std::string reg_string(const char *key, const char *value) {
    char buf[512];
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return std::string();
    return trim(buf);
}
#endif

inline std::string cpu_model() {
#if defined(_WIN32)
    std::string s = reg_string("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    return s.empty() ? "unknown" : s;
#elif defined(__APPLE__)
    char buf[256];
    size_t n = sizeof buf;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0) return trim(buf);
    return "unknown";
#else
    // x86: "model name"; ARM: "Model" / "Hardware" (when present at all).
    const std::string info = "\n" + read_file("/proc/cpuinfo");
    for (const char *key : {"\nmodel name", "\nModel", "\nHardware"}) {
        const size_t p = info.find(key);
        if (p == std::string::npos) continue;
        const size_t c = info.find(':', p), e = info.find('\n', p + 1);
        if (c != std::string::npos && c < e) return trim(info.substr(c + 1, e - c - 1));
    }
    return "unknown";
#endif
}

inline double ram_gib() {
#if defined(_WIN32)
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? double(m.ullTotalPhys) / (1024.0 * 1024 * 1024) : 0;
#elif defined(__APPLE__)
    uint64_t b = 0;
    size_t n = sizeof b;
    return sysctlbyname("hw.memsize", &b, &n, nullptr, 0) == 0 ? double(b) / (1024.0 * 1024 * 1024) : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? double(pages) * double(size) / (1024.0 * 1024 * 1024) : 0;
#endif
}

inline std::string os_name() {
#if defined(_WIN32)
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi;
    std::memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (HMODULE nt = GetModuleHandleA("ntdll.dll")) {
        FARPROC p = GetProcAddress(nt, "RtlGetVersion");
        RtlGetVersionFn fn = nullptr;
        if (p) std::memcpy(&fn, &p, sizeof fn);
        if (fn) fn(&vi);
    }
    const char *nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = reg_string(nt, "ProductName"), disp = reg_string(nt, "DisplayVersion");
    // ProductName still says "Windows 10" on Windows 11; the build number tells.
    if (vi.dwBuildNumber >= 22000 && product.compare(0, 10, "Windows 10") == 0)
        product = "Windows 11" + product.substr(10);
    char b[256];
    std::snprintf(b, sizeof b, "%s %s (%lu.%lu.%lu)", product.empty() ? "Windows" : product.c_str(),
                  disp.c_str(), (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion,
                  (unsigned long)vi.dwBuildNumber);
    return b;
#else
    struct utsname u;
    std::string base = ::uname(&u) == 0 ? std::string(u.sysname) + " " + u.release + " " + u.machine : "unknown";
#if defined(__APPLE__)
    char buf[64];
    size_t n = sizeof buf;
    if (sysctlbyname("kern.osproductversion", buf, &n, nullptr, 0) == 0)
        return "macOS " + std::string(buf) + " (" + base + ")";
    return base;
#else
    const std::string rel = read_file("/etc/os-release");
    const size_t p = rel.find("PRETTY_NAME=");
    if (p != std::string::npos) {
        std::string v = rel.substr(p + 12, rel.find('\n', p) - p - 12);
        v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
        return v + " (" + base + ")";
    }
    return base;
#endif
#endif
}

inline std::string os_short() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unix";
#endif
}

inline std::string compiler() {
    char b[160];
#if defined(__clang__)
    std::snprintf(b, sizeof b, "clang %s", __clang_version__);
#elif defined(__GNUC__)
#if defined(__MINGW64__)
    std::snprintf(b, sizeof b, "MinGW-w64 g++ %s", __VERSION__);
#else
    std::snprintf(b, sizeof b, "g++ %s", __VERSION__);
#endif
#elif defined(_MSC_VER)
    std::snprintf(b, sizeof b, "MSVC %d", (int)_MSC_FULL_VER);
#else
    std::snprintf(b, sizeof b, "unknown");
#endif
    return trim(b);
}

inline std::string compiler_short() {
#if defined(__clang__)
    return "clang" + std::to_string(__clang_major__);
#elif defined(__MINGW64__)
    return "mingw-gcc" + std::to_string(__GNUC__);
#elif defined(__GNUC__)
    return "gcc" + std::to_string(__GNUC__);
#elif defined(_MSC_VER)
    return "msvc" + std::to_string(_MSC_VER);
#else
    return "cc";
#endif
}

// Compile-time facts about the build (the flags themselves: --build-info).
inline std::string build_facts() {
    std::string s;
#if defined(__OPTIMIZE__)
    s += "optimized";
#elif defined(_MSC_VER) && !defined(_DEBUG)
    s += "msvc-release-crt";
#elif defined(_DEBUG)
    s += "debug-crt";
#else
    s += "not-optimized";
#endif
#if defined(NDEBUG)
    s += ", NDEBUG";
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    s += ", sanitizer";
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    s += ", sanitizer";
#endif
#endif
#if defined(__MINGW32__)
#if defined(_UCRT)
    s += ", stdio: mingw-w64 on UCRT";
#else
    s += ", stdio: mingw-w64 on msvcrt";
#endif
#if defined(__USE_MINGW_ANSI_STDIO) && __USE_MINGW_ANSI_STDIO
    s += " (__USE_MINGW_ANSI_STDIO=1: mingw's own C99 printf)";
#endif
#elif defined(_MSC_VER)
    s += ", stdio: UCRT";
#elif defined(__APPLE__)
    s += ", stdio: macOS libc";
#elif defined(__GLIBC__)
    s += ", stdio: glibc " + std::to_string(__GLIBC__) + "." + std::to_string(__GLIBC_MINOR__);
#endif
    s += ", tblog: static library";
    return s;
}

inline std::string utc_now_iso() {
    const time_t t = std::time(nullptr);
    std::tm tm;
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

inline std::string slug(const std::string &s) {
    std::string o;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
        else if (c >= 'A' && c <= 'Z') o += (char)(c - 'A' + 'a');
        else if (!o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o;
}

// Short CPU name for file names: "AMD Ryzen 7 8845HS w/ Radeon..." -> "ryzen-7-8845hs".
inline std::string cpu_short() {
    std::string m = cpu_model();
    for (const char *cut : {" w/", " with ", " CPU", " @", "(R)", "(TM)"}) {
        size_t p;
        while ((p = m.find(cut)) != std::string::npos) {
            if (std::strcmp(cut, "(R)") == 0 || std::strcmp(cut, "(TM)") == 0) m.erase(p, std::strlen(cut));
            else m.erase(p);
        }
    }
    std::string s = slug(m);
    for (const char *pre : {"amd-", "intel-core-", "intel-", "apple-"})
        if (s.compare(0, std::strlen(pre), pre) == 0 && s.size() > std::strlen(pre)) s = s.substr(std::strlen(pre));
    return s.empty() ? "cpu" : s;
}

// <os>-<compiler>-<cpu>-<date>-<kind>.json
inline std::string default_result_name(const char *kind) {
    return os_short() + "-" + compiler_short() + "-" + cpu_short() + "-" + utc_now_iso().substr(0, 10) + "-" +
           kind + ".json";
}

inline std::string env_json(const Args &args, const std::string &work_dir) {
    std::string commit = args.get("--commit", "");
    if (commit.empty()) {
        const char *g = std::getenv("GITHUB_SHA");
        commit = g && *g ? g : "unknown";
    }
    return Obj()
        .str("date_utc", utc_now_iso())
        .str("label", args.get("--label", ""))
        .str("cpu", cpu_model())
        .num("logical_cpus", std::thread::hardware_concurrency())
        .num("ram_gib", double((long long)(ram_gib() * 10 + 0.5)) / 10)
        .str("os", os_name())
        .str("compiler", compiler())
        .str("build", args.get("--build-info", "unknown (pass --build-info)"))
        .str("build_facts", build_facts())
        .str("tblog_commit", commit)
        .str("work_dir", work_dir)
        .done();
}

// Writes `json` to --out FILE, or to --out-dir DIR/<default name>; returns the path.
inline std::string save_json(const Args &args, const char *kind, const std::string &json) {
    std::string out = args.get("--out", "");
    if (out.empty()) {
        const std::string dir = args.get("--out-dir", ".");
        make_dirs(dir);
        out = dir + "/" + default_result_name(kind);
    }
    if (!write_file(out, json + "\n")) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return std::string();
    }
    return out;
}

}  // namespace bench
