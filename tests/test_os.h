// Portable OS helpers for the unit tests (POSIX and Windows): scratch
// directories, files, environment, time, threads with a given stack size,
// stderr redirection, child processes and memory shared with them.
//
// Child processes re-execute the test binary itself on a TINY_CHILD scenario
// (tiny_test.h) instead of fork()ing a copy, so the same test code runs on
// both OSes (run_child(), or ChildProcess to kill a running child hard). The child gets `arg` in the TINY_CHILD_ARG environment variable
// (tos::child_arg()), its stderr in `err_path` (optional), and runs
// tos::child_setup() first: crash handlers reset to the defaults, no core
// dumps / error dialogs, a 30 s self-destruct timer on POSIX.
//
// Not part of the library: tests may use OS headers freely (the platform
// boundary applies to src/ only). Everything here is ASCII-path only.
#pragma once

#include "tiny_test.h"

#include <algorithm>
#include <cerrno>
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
#include <csignal>
#include <process.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <climits>  // PATH_MAX
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif
#endif

// printf format checking for test helpers (gnu_printf on MinGW, as log.h).
#if defined(__MINGW32__) && !defined(__clang__)
#define TOS_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#elif defined(__GNUC__) || defined(__clang__)
#define TOS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define TOS_PRINTF(f, a)
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define TOS_NOINLINE __declspec(noinline)
#else
#define TOS_NOINLINE __attribute__((noinline))
#endif

// A deliberate fault must reach tblog's crash handler: under
// -fsanitize=undefined with -fno-sanitize-recover (the CI sanitizer jobs)
// UBSan's null check would abort the process first. Functions that fault on
// purpose turn UBSan off for themselves.
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define TBLOG_TEST_NO_UBSAN __attribute__((no_sanitize("undefined")))
#else
#define TBLOG_TEST_NO_UBSAN
#endif

namespace tos {

// Store through a null pointer (SIGSEGV / access violation), invisible to
// the optimizer (both volatiles) and to UBSan.
TBLOG_TEST_NO_UBSAN TOS_NOINLINE inline void fault_null_store() {
    volatile int *volatile p = nullptr;
    *p = 1;
}

#if defined(_WIN32)
inline std::wstring wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
inline bool is_sep(char c) { return c == '/' || c == '\\'; }
#else
inline bool is_sep(char c) { return c == '/'; }
#endif

// ------------------------------------------------------------------ process
inline int pid() {
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)::getpid();
#endif
}

inline void set_env(const char *name, const char *value) {
#if defined(_WIN32)
    // CRT copy (getenv, the TZ the CRT reads) and the process environment
    // block (inherited by CreateProcess children).
    std::string kv = std::string(name) + "=" + value;
    _putenv(kv.c_str());
    SetEnvironmentVariableA(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

// Local time = UTC for the rest of the process.
inline void set_tz_utc() {
#if defined(_WIN32)
    set_env("TZ", "UTC0");
    _tzset();
#else
    ::setenv("TZ", "UTC", 1);
    ::tzset();
#endif
}

inline std::tm local_tm(time_t t) {
    std::tm tm;
    std::memset(&tm, 0, sizeof tm);
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    ::localtime_r(&t, &tm);
#endif
    return tm;
}

inline time_t timegm_utc(std::tm *t) {
#if defined(_WIN32)
    return _mkgmtime(t);
#else
    return ::timegm(t);
#endif
}

inline uint64_t mono_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ------------------------------------------------------------------ files
inline bool exists(const std::string &p) {
#if defined(_WIN32)
    return GetFileAttributesW(wide(p).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::lstat(p.c_str(), &st) == 0;
#endif
}

inline bool is_dir(const std::string &p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// ~0ull if missing.
inline unsigned long long file_size(const std::string &p) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(p).c_str(), GetFileExInfoStandard, &d)) return ~0ull;
    return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
#else
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 ? (unsigned long long)st.st_size : ~0ull;
#endif
}

// mkdir -p
inline void make_dirs(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && !is_sep(dir[i])) continue;
        const std::string part = dir.substr(0, i);
#if defined(_WIN32)
        if (part.size() == 2 && part[1] == ':') continue;  // "C:"
        CreateDirectoryW(wide(part).c_str(), nullptr);
#else
        ::mkdir(part.c_str(), 0755);
#endif
    }
}

// Entry names (no "." / ".."), sorted.
inline std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = narrow(fd.cFileName);
            if (n != "." && n != "..") out.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    if (DIR *d = ::opendir(dir.c_str())) {
        while (dirent *e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        ::closedir(d);
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
}

inline bool remove_file(const std::string &p) {
#if defined(_WIN32)
    return DeleteFileW(wide(p).c_str()) != FALSE;
#else
    return ::unlink(p.c_str()) == 0;
#endif
}

// rm -rf
inline void remove_tree(const std::string &p) {
    if (is_dir(p)) {
        for (const std::string &n : list_dir(p)) remove_tree(p + "/" + n);
#if defined(_WIN32)
        RemoveDirectoryW(wide(p).c_str());
#else
        ::rmdir(p.c_str());
#endif
    } else if (exists(p)) {
        remove_file(p);
    }
}

// Removes `p` only if it is an empty directory.
inline void remove_empty_dir(const std::string &p) {
#if defined(_WIN32)
    RemoveDirectoryW(wide(p).c_str());
#else
    ::rmdir(p.c_str());
#endif
}

// rename(2): replaces an existing target.
inline bool rename_replace(const std::string &from, const std::string &to) {
#if defined(_WIN32)
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
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
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

// Sets both access and modification time to `sec` (seconds since the epoch).
inline bool set_mtime(const std::string &p, int64_t sec) {
#if defined(_WIN32)
    HANDLE h = CreateFileW(wide(p).c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint64_t t = (uint64_t)(sec * 10000000LL) + 116444736000000000ull;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    BOOL ok = SetFileTime(h, nullptr, &ft, &ft);
    CloseHandle(h);
    return ok != FALSE;
#else
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)sec;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    return ::utimensat(AT_FDCWD, p.c_str(), ts, 0) == 0;
#endif
}

// $TBLOG_TEST_DIR (set per test by CTest) or build/tmp; created.
inline std::string test_dir() {
    const char *d = std::getenv("TBLOG_TEST_DIR");
    std::string dir = (d && *d) ? d : "build/tmp";
    make_dirs(dir);
    return dir;
}

// An append-only raw OS file for async-signal-safe writes from crash
// handlers (write(2) / WriteFile; no stdio, no allocation).
struct RawFile {
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
    bool valid() const { return h != INVALID_HANDLE_VALUE; }
    static RawFile open_append(const std::string &path) {
        RawFile f;
        f.h = CreateFileW(wide(path).c_str(), FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        return f;
    }
    void write(const char *s, size_t n) const noexcept {
        while (valid() && n > 0) {
            DWORD w = 0;
            if (!WriteFile(h, s, (DWORD)n, &w, nullptr) || w == 0) return;
            s += w;
            n -= w;
        }
    }
#else
    int fd = -1;
    bool valid() const { return fd >= 0; }
    static RawFile open_append(const std::string &path) {
        RawFile f;
        f.fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        return f;
    }
    void write(const char *s, size_t n) const noexcept {
        while (valid() && n > 0) {
            ssize_t w = ::write(fd, s, n);
            if (w < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (w == 0) return;
            s += w;
            n -= (size_t)w;
        }
    }
#endif
    void write(const char *s) const noexcept { write(s, std::strlen(s)); }
};

// A small file mapped shared (MapViewOfFile / mmap MAP_SHARED) between a
// parent and its child: progress counters the child stores with plain atomic
// stores and the parent reads after the child died. The parent opens it with
// create = true (sized and zeroed), the child with create = false.
class SharedFile {
public:
    SharedFile() = default;
    SharedFile(const SharedFile &) = delete;
    SharedFile &operator=(const SharedFile &) = delete;
    bool open(const std::string &path, size_t size, bool create) {
        size_ = size;
#if defined(_WIN32)
        file_ = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        map_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, (DWORD)size, nullptr);
        if (!map_) return false;
        p_ = MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, size);
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (create ? O_CREAT | O_TRUNC : 0), 0644);
        if (fd_ < 0) return false;
        if (create && ::ftruncate(fd_, (off_t)size) != 0) return false;
        void *p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        p_ = p == MAP_FAILED ? nullptr : p;
#endif
        if (p_ && create) std::memset(p_, 0, size);
        return p_ != nullptr;
    }
    void *data() const { return p_; }
    ~SharedFile() {
#if defined(_WIN32)
        if (p_) UnmapViewOfFile(p_);
        if (map_) CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (p_) ::munmap(p_, size_);
        if (fd_ >= 0) ::close(fd_);
#endif
    }

private:
    void *p_ = nullptr;
    size_t size_ = 0;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// Redirects the process's stderr (fd 2 / STD_ERROR_HANDLE) into `path` for
// the lifetime of the object.
class StderrToFile {
public:
    explicit StderrToFile(const std::string &path) {
        std::fflush(stderr);
#if defined(_WIN32)
        saved_ = GetStdHandle(STD_ERROR_HANDLE);
        file_ = CreateFileW(wide(path).c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok_ = file_ != INVALID_HANDLE_VALUE && SetStdHandle(STD_ERROR_HANDLE, file_);
#else
        int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
        saved_ = ::dup(STDERR_FILENO);
        ok_ = fd >= 0 && saved_ >= 0 && ::dup2(fd, STDERR_FILENO) >= 0;
        if (fd >= 0) ::close(fd);
#endif
    }
    ~StderrToFile() {
        std::fflush(stderr);
#if defined(_WIN32)
        SetStdHandle(STD_ERROR_HANDLE, saved_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (saved_ >= 0) {
            ::dup2(saved_, STDERR_FILENO);
            ::close(saved_);
        }
#endif
    }
    bool ok() const { return ok_; }

private:
#if defined(_WIN32)
    HANDLE saved_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int saved_ = -1;
#endif
    bool ok_ = false;
};

// Runs fn() on a new thread with a `stack_bytes` stack and waits for it.
inline void run_thread_with_stack(size_t stack_bytes, void (*fn)()) {
    struct Arg {
        void (*fn)();
    } arg = {fn};
#if defined(_WIN32)
    struct Tramp {
        static DWORD WINAPI start(LPVOID p) {
            static_cast<Arg *>(p)->fn();
            return 0;
        }
    };
    HANDLE t = CreateThread(nullptr, stack_bytes, &Tramp::start, &arg, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            nullptr);
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
#else
    struct Tramp {
        static void *start(void *p) {
            static_cast<Arg *>(p)->fn();
            return nullptr;
        }
    };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_t t;
    if (pthread_create(&t, &attr, &Tramp::start, &arg) == 0) pthread_join(t, nullptr);
    pthread_attr_destroy(&attr);
#endif
}

// ------------------------------------------------------------------ children
// How a child ended. POSIX: exit status or terminating signal. Windows: the
// exit code; an unhandled exception ends a process with its exception code
// (e.g. 0xC0000005), abort() with 3.
struct ChildResult {
    bool started = false;
    bool timed_out = false;
    bool exited = false;     // normal exit (POSIX) / any end (Windows)
    int exit_code = -1;      // POSIX: WEXITSTATUS; Windows: (int)GetExitCodeProcess
    int signal = 0;          // POSIX: WTERMSIG if killed by a signal
    std::string err;         // the child's stderr when err_path was given
};

// Ways a child is expected to die, mapped to the OS's outcome.
enum class Crash { Segv, Abort, Fpe, Ill, Bus, StackOverflow };

inline bool died_with(const ChildResult &r, Crash c) {
#if defined(_WIN32)
    unsigned want = 0;
    switch (c) {
        case Crash::Segv: want = 0xC0000005u; break;           // EXCEPTION_ACCESS_VIOLATION
        case Crash::Abort: want = 3u; break;                   // abort() / SIGABRT default action
        case Crash::Fpe: want = 0xC0000094u; break;            // EXCEPTION_INT_DIVIDE_BY_ZERO
        case Crash::Ill: want = 0xC000001Du; break;            // EXCEPTION_ILLEGAL_INSTRUCTION
        case Crash::Bus: return false;
        case Crash::StackOverflow: want = 0xC00000FDu; break;  // EXCEPTION_STACK_OVERFLOW
    }
    return !r.timed_out && r.exited && (unsigned)r.exit_code == want;
#else
    int want = 0;
    switch (c) {
        case Crash::Segv: want = SIGSEGV; break;
        case Crash::Abort: want = SIGABRT; break;
        case Crash::Fpe: want = SIGFPE; break;
        case Crash::Ill: want = SIGILL; break;
        case Crash::Bus: want = SIGBUS; break;
        case Crash::StackOverflow: want = SIGSEGV; break;
    }
#if defined(__APPLE__)
    // macOS: overflowing a secondary thread's stack hits its guard page,
    // which XNU reports as SIGBUS (SIGSEGV only for the main thread).
    if (c == Crash::StackOverflow && r.signal == SIGBUS) want = SIGBUS;
#endif
    return !r.timed_out && !r.exited && r.signal == want;
#endif
}

inline bool exited_with(const ChildResult &r, int code) {
    return !r.timed_out && r.exited && r.exit_code == code;
}

inline const char *child_arg() {
    const char *a = std::getenv("TINY_CHILD_ARG");
    return a ? a : "";
}

inline std::string self_exe() {
#if defined(_WIN32)
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return n ? narrow(buf) : std::string(tiny::argv0());
#elif defined(__APPLE__)
    // No /proc on macOS: dyld's path of the executable, made canonical.
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return tiny::argv0();
    char real[PATH_MAX];
    return ::realpath(buf, real) ? std::string(real) : std::string(buf);
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return tiny::argv0();
    buf[n] = '\0';
    return buf;
#endif
}

// What every child does before its scenario (installed as tiny's hook).
inline void child_setup() {
#if defined(_WIN32)
    // No WER / "program stopped working" dialog: the process just ends with
    // the exception code. Debug CRT: no abort()/assert message boxes.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#if defined(_MSC_VER) && defined(_DEBUG)  // the _Crt* calls are no-op macros otherwise
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    std::signal(SIGABRT, SIG_DFL);
#else
    // Make the child independent of whatever a sanitizer runtime installed.
    static const int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (int s : kCrashSignals) sigaction(s, &sa, nullptr);
    // 1 (not 0): with a piped core_pattern (apport, systemd-coredump) the
    // kernel still invokes the helper for 0; 1 makes it skip the dump.
    struct rlimit rl = {1, 1};
    setrlimit(RLIMIT_CORE, &rl);
    alarm(30);  // safety net against hangs
#endif
}

namespace detail {
struct ChildSetupReg {
    ChildSetupReg() { tiny::child_setup() = &tos::child_setup; }
};
static ChildSetupReg g_child_setup_reg;
}  // namespace detail

// A child process running TINY_CHILD(`scenario`) of this binary, started
// without waiting for it: start(), then optionally kill_hard() (SIGKILL /
// TerminateProcess: no code of the child runs any more, like kill -9), then
// wait(). run_child() below is start() + wait().
class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ~ChildProcess() {
        if (running_) {
            kill_hard();
            wait(10);
        }
    }

    bool start(const char *scenario, const std::string &arg = std::string(),
               const std::string &err_path = std::string()) {
        err_path_ = err_path;
        set_env("TINY_CHILD_ARG", arg.c_str());
        const std::string exe = self_exe();
        const std::string flag = std::string("--tiny-child=") + scenario;
        std::fflush(stdout);
        std::fflush(stderr);
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle = TRUE;
        HANDLE self = GetCurrentProcess();
        HANDLE out = nullptr, err = nullptr;
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
        if (so && so != INVALID_HANDLE_VALUE) DuplicateHandle(self, so, self, &out, 0, TRUE, DUPLICATE_SAME_ACCESS);
        if (!err_path.empty()) {
            err = CreateFileW(wide(err_path).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (err == INVALID_HANDLE_VALUE) err = nullptr;
        } else {
            HANDLE se = GetStdHandle(STD_ERROR_HANDLE);
            if (se && se != INVALID_HANDLE_VALUE) DuplicateHandle(self, se, self, &err, 0, TRUE, DUPLICATE_SAME_ACCESS);
        }
        std::wstring cmd = L"\"" + wide(exe) + L"\" " + wide(flag);
        STARTUPINFOW si;
        std::memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = out;
        si.hStdError = err;
        std::memset(&pi_, 0, sizeof pi_);
        running_ = CreateProcessW(wide(exe).c_str(), &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                                  &pi_) != FALSE;
        if (out) CloseHandle(out);
        if (err) CloseHandle(err);
#else
        pid_ = ::fork();
        if (pid_ == 0) {
            if (!err_path.empty()) {
                int fd = ::open(err_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
                if (fd >= 0) {
                    ::dup2(fd, STDERR_FILENO);
                    ::close(fd);
                }
            }
            ::execl(exe.c_str(), exe.c_str(), flag.c_str(), (char *)nullptr);
            _exit(127);
        }
        running_ = pid_ > 0;
#endif
        return running_;
    }

    // SIGKILL / TerminateProcess (exit code 137 on Windows).
    void kill_hard() {
        if (!running_) return;
#if defined(_WIN32)
        TerminateProcess(pi_.hProcess, 137u);
#else
        ::kill(pid_, SIGKILL);
#endif
    }

    // Waits at most `timeout_s` (then kills the child: timed_out).
    ChildResult wait(int timeout_s = 60) {
        ChildResult r;
        if (!running_) return r;
        running_ = false;
        r.started = true;
#if defined(_WIN32)
        if (WaitForSingleObject(pi_.hProcess, (DWORD)timeout_s * 1000u) == WAIT_TIMEOUT) {
            r.timed_out = true;
            TerminateProcess(pi_.hProcess, 0xDEADu);
            WaitForSingleObject(pi_.hProcess, INFINITE);
        }
        DWORD code = 0;
        GetExitCodeProcess(pi_.hProcess, &code);
        r.exited = true;
        r.exit_code = (int)code;
        CloseHandle(pi_.hThread);
        CloseHandle(pi_.hProcess);
#else
        int st = 0;
        const uint64_t deadline = mono_ns() + (uint64_t)timeout_s * 1000000000ull;
        for (;;) {
            const pid_t w = ::waitpid(pid_, &st, WNOHANG);
            if (w == pid_) break;
            if (w < 0 && errno != EINTR) break;
            if (mono_ns() > deadline) {  // the child's alarm() should have fired first
                r.timed_out = true;
                ::kill(pid_, SIGKILL);
                while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {
                }
                break;
            }
            sleep_ms(1);
        }
        if (WIFEXITED(st)) {
            r.exited = true;
            r.exit_code = WEXITSTATUS(st);
        } else if (WIFSIGNALED(st)) {
            r.signal = WTERMSIG(st);
            r.timed_out = r.timed_out || r.signal == SIGALRM;
        }
#endif
        if (!err_path_.empty()) {
            r.err = read_file(err_path_);
            remove_file(err_path_);
        }
        return r;
    }

private:
    bool running_ = false;
    std::string err_path_;
#if defined(_WIN32)
    PROCESS_INFORMATION pi_;
#else
    pid_t pid_ = -1;
#endif
};

// Runs TINY_CHILD(`scenario`) of this binary in a new process and waits for
// it (at most `timeout_s`; then it is killed and timed_out is set).
inline ChildResult run_child(const char *scenario, const std::string &arg = std::string(),
                             const std::string &err_path = std::string(), int timeout_s = 60) {
    ChildProcess c;
    if (!c.start(scenario, arg, err_path)) return ChildResult();
    return c.wait(timeout_s);
}

// True if the child was ended by ChildProcess::kill_hard().
inline bool killed_hard(const ChildResult &r) {
#if defined(_WIN32)
    return !r.timed_out && r.exited && r.exit_code == 137;
#else
    return !r.timed_out && !r.exited && r.signal == SIGKILL;
#endif
}

// Printable outcome, for failure messages.
inline std::string describe(const ChildResult &r) {
    char b[96];
    if (!r.started) return "not started";
    if (r.timed_out) return "timed out";
    if (r.exited)
        std::snprintf(b, sizeof b, "exit code %d (0x%08X)", r.exit_code, (unsigned)r.exit_code);
    else
        std::snprintf(b, sizeof b, "killed by signal %d", r.signal);
    return b;
}

}  // namespace tos
