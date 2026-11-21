// How many log lines does a program lose when it dies abnormally: tblog vs
// plain fprintf. Methodology and expectations: docs/benchmarks.md.
//
//   bench_crash_loss [--quick] [--trials 20] [--rate 20000] [--out-dir DIR | --out FILE]
//
// The parent runs this same binary as a child once per (scenario, variant,
// trial). The child logs lines 1, 2, 3, ... from one or more threads at
// --rate lines/s per thread; right after each logging call RETURNS it stores
// the line's index in a per-thread counter in shared memory (a mapped file,
// MapViewOfFile / mmap MAP_SHARED), so the parent knows exactly which lines
// the program believed were logged ("acknowledged"). Then the child dies the
// scenario's way. The parent reads the log file and computes per thread:
//   found       lines present (each one checked whole: prefix, fields, value)
//   lost        acknowledged - the longest run 1..k found (0 if all there)
//   missing     acknowledged lines absent (holes included)
//   duplicates  lines found more than once; corrupt: unparseable lines;
//   torn        a last line without '\n'; beyond_ack: lines whose call had
//               not returned yet (written, but not acknowledged)
// plus the process outcome (exit code / signal) and whether tblog wrote its
// "*** tblog: caught ... ***" marker.
//
// Scenarios: segv_main, segv_other_thread, abort, assert, uncaught_exception,
// stack_overflow, stack_overflow_thread, kill_hard, exit, _Exit (see
// kScenarios). Variants: fprintf_buffered, fprintf_flush, tblog (default
// config except file only; full_policy = block, the default),
// tblog_flush_error (tblog + flush.on_level = ERROR; every 100th line of a
// thread is an ERROR line), and on request tblog_drop, fprintf_unbuffered.
// For variants with ERROR lines the parent also counts the INFO and the
// ERROR lines lost, and the lines lost up to each thread's last acknowledged
// ERROR line (flush.on_level promises 0 there, even for kill -9 / _Exit).
#undef NDEBUG  // the `assert` scenario needs a live assert(), whatever the build type
#include <cassert>

#include "bench_common.h"

#include <cerrno>
#include <csignal>
#include <stdexcept>

#if defined(_WIN32)
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#else
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif
#endif

#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define BENCH_NO_UBSAN __attribute__((no_sanitize("undefined")))
#else
#define BENCH_NO_UBSAN
#endif

namespace {

using namespace bench;

// Built with ThreadSanitizer (GCC defines __SANITIZE_THREAD__, Clang has
// __has_feature(thread_sanitizer)).
#if defined(__SANITIZE_THREAD__)
#define TBLOG_BENCH_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TBLOG_BENCH_TSAN 1
#endif
#endif
#ifndef TBLOG_BENCH_TSAN
#define TBLOG_BENCH_TSAN 0
#endif

// ------------------------------------------------------------------ scenarios
struct Scenario {
    const char *name;
    int threads;       // logging threads
    bool param_is_ms;  // param: a delay in ms (else: lines before the trigger)
    bool crash;        // a crash tblog's handler is meant to catch (marker expected)
    const char *what;
};

const Scenario kScenarios[] = {
    {"segv_main", 1, false, true, "null-pointer store on the logging thread after N lines"},
    {"segv_other_thread", 4, true, true,
     "4 threads log continuously; a 5th thread does a null-pointer store after a random delay"},
    {"abort", 1, false, true, "abort() on the logging thread after N lines"},
    {"assert", 1, false, true, "failed assert() (NDEBUG undefined) on the logging thread after N lines"},
    {"uncaught_exception", 1, false, true, "throw with no handler (std::terminate) after N lines"},
    {"stack_overflow", 1, false, true, "infinite recursion on the thread that called tblog_init, after N lines"},
    {"stack_overflow_thread", 1, false, true,
     "infinite recursion on a second thread (not the tblog_init thread), after it logged N lines"},
    {"kill_hard", 4, true, false,
     "4 threads log; the parent kills the child after a random delay (TerminateProcess / SIGKILL)"},
    {"exit", 1, false, false, "exit(0) after N lines, without tblog_shutdown / fclose (= return from main)"},
    {"_Exit", 1, false, false, "std::_Exit(0) after N lines: no atexit handlers, no stdio flush"},
};

const Scenario *find_scenario(const std::string &n) {
    for (const Scenario &s : kScenarios)
        if (n == s.name) return &s;
    return nullptr;
}

enum Kind { kStdio, kStdioFlush, kTblog };

struct Variant {
    const char *name;
    Kind kind;
    const char *policy;
    bool unbuffered;
    const char *extra_config;  // tblog: more log.conf lines
    long error_every;          // tblog: every Nth line of a thread is an ERROR line (0: none)
    const char *desc;
};

// Every 100th line an ERROR line: at 20 000 lines/s per thread, 200 ERROR
// lines/s per thread, i.e. one every 5 ms.
constexpr long kErrorEvery = 100;

const Variant kVariants[] = {
    {"fprintf_buffered", kStdio, "", false, "", 0, "fprintf to one shared FILE* opened \"wb\", default stdio buffering"},
    {"fprintf_flush", kStdioFlush, "", false, "", 0, "fprintf + fflush after every line"},
    {"fprintf_unbuffered", kStdio, "", true, "", 0, "fprintf with setvbuf(_IONBF)"},
    {"tblog", kTblog, "block", false, "", 0,
     "TBLOG_INFO, default config except file only (console=false, reload.watch=false, file.max_size=4GB); full_policy=block (default)"},
    {"tblog_flush_error", kTblog, "block", false, "flush.on_level = ERROR\n", kErrorEvery,
     "as tblog + flush.on_level=ERROR; every 100th line of each thread is TBLOG_ERROR (same text)"},
    {"tblog_drop", kTblog, "drop", false, "", 0, "as tblog, full_policy=drop"},
};

const Variant *find_variant(const std::string &n) {
    for (const Variant &v : kVariants)
        if (n == v.name) return &v;
    return nullptr;
}

// ------------------------------------------------------------------ shared memory
constexpr int kMaxThreads = 16;
constexpr uint32_t kMagic = 0x7B10C0DEu;

struct alignas(64) Slot {
    std::atomic<int64_t> v;
};

struct Shared {
    uint32_t magic;
    uint32_t threads;
    std::atomic<uint32_t> state;     // 1: logging started
    std::atomic<uint64_t> dropped;   // tblog_dropped_count() at the last acknowledgement
    std::atomic<uint64_t> trigger_ns;  // steady_clock when the fatal action started (kill_hard: the parent)
    Slot acked[kMaxThreads];         // last index whose logging call returned
    Slot last_error[kMaxThreads];    // last ERROR index whose logging call returned
};

class Mapping {
public:
    // create: the parent (sizes and zeroes the file); else: the child.
    bool open(const std::string &path, bool create) {
#if defined(_WIN32)
        file_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        map_ = CreateFileMappingA(file_, nullptr, PAGE_READWRITE, 0, (DWORD)kSize, nullptr);
        if (!map_) return false;
        p_ = MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, kSize);
#else
        fd_ = ::open(path.c_str(), O_RDWR | (create ? O_CREAT | O_TRUNC : 0), 0644);
        if (fd_ < 0) return false;
        if (create && ::ftruncate(fd_, (off_t)kSize) != 0) return false;
        void *p = ::mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        p_ = p == MAP_FAILED ? nullptr : p;
#endif
        if (p_ && create) std::memset(p_, 0, kSize);
        return p_ != nullptr;
    }
    Shared *get() const { return static_cast<Shared *>(p_); }
    ~Mapping() {
#if defined(_WIN32)
        if (p_) UnmapViewOfFile(p_);
        if (map_) CloseHandle(map_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (p_) ::munmap(p_, kSize);
        if (fd_ >= 0) ::close(fd_);
#endif
    }

private:
    static constexpr size_t kSize = 4096;
    void *p_ = nullptr;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_ = nullptr;
#else
    int fd_ = -1;
#endif
};
static_assert(sizeof(Shared) <= 4096, "Shared must fit the mapping");

// ------------------------------------------------------------------ child
Shared *g_sh = nullptr;
FILE *g_file = nullptr;
Kind g_kind = kStdio;
long g_error_every = 0;
uint64_t g_rate = 0;
std::atomic<int> g_zero{0};

// Store through a null pointer, invisible to the optimizer and to UBSan.
BENCH_NO_UBSAN BENCH_NOINLINE void fault_null_store() {
    volatile int *volatile p = nullptr;
    *p = 1;
}

// Unbounded recursion the compiler can neither prove infinite nor turn into
// a loop (the frame is used after the call).
BENCH_NOINLINE int recurse(int depth) {
    volatile char pad[512];
    pad[0] = (char)depth;
    pad[511] = (char)(depth >> 8);
    if (depth == g_zero.load() - 1) return 0;  // never: depth >= 0
    return recurse(depth + 1) + pad[0] + pad[511];
}

void child_setup() {
#if defined(_WIN32)
    // No WER / "stopped working" dialog for this process (not system-wide):
    // the process just ends with its exception code.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
    std::signal(SIGABRT, SIG_DFL);
#else
    static const int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (int s : kSignals) sigaction(s, &sa, nullptr);
    struct rlimit rl = {1, 1};  // no core dumps (1, not 0: piped core_pattern helpers)
    setrlimit(RLIMIT_CORE, &rl);
    alarm(60);  // safety net
#endif
}

const char *const kNames[kMaxThreads] = {"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                         "t8", "t9", "t10", "t11", "t12", "t13", "t14", "t15"};

inline void log_line(int t, long i) {
    if (g_kind == kTblog) {
        const bool err = g_error_every && i % g_error_every == 0;
        if (err) BENCH_TBLOG_ERROR_LINE(i, kNames[t]);
        else BENCH_TBLOG_LINE(i, kNames[t]);
        g_sh->dropped.store(tblog_dropped_count(), std::memory_order_relaxed);
        g_sh->acked[t].v.store(i, std::memory_order_release);  // the call returned
        if (err) g_sh->last_error[t].v.store(i, std::memory_order_release);
        return;
    }
    BENCH_FPRINTF_LINE(g_file, i, kNames[t], g_kind == kStdioFlush);
    g_sh->acked[t].v.store(i, std::memory_order_release);  // the call returned
}

// Logs lines 1..n (n < 0: until the cap) at g_rate lines/s; returns false if
// the cap was hit (the scenario's end never came).
bool log_lines(int t, long n) {
    const long cap = (long)std::max<uint64_t>(g_rate, 1000) * 30;
    const uint64_t period = g_rate ? 1000000000ull / g_rate : 0;
    const uint64_t start = now_ns();
    for (long i = 1; n < 0 ? i <= cap : i <= n; ++i) {
        if (period) spin_until(start + period * (uint64_t)(i - 1));
        log_line(t, i);
    }
    return n >= 0;
}

int child_main(const Args &a) {
    child_setup();
    const Scenario *scn = find_scenario(a.get("--child", ""));
    const Variant *var = find_variant(a.get("--variant", ""));
    const std::string dir = a.get("--dir", "");
    const long param = (long)a.num("--param", 1000);
    g_rate = (uint64_t)a.num("--rate", 20000);
    if (!scn || !var || dir.empty()) return 90;
    Mapping m;
    if (!m.open(dir + "/progress.bin", false)) return 91;
    g_sh = m.get();
    if (g_sh->magic != kMagic || (int)g_sh->threads != scn->threads) return 94;
    g_kind = var->kind;
    g_error_every = var->error_every;
    if (g_kind == kTblog) {
        if (tblog_init((dir + "/log.conf").c_str()) != 0) return 92;
    } else {
        g_file = std::fopen((dir + "/app.log").c_str(), "wb");
        if (!g_file) return 93;
        if (var->unbuffered) std::setvbuf(g_file, nullptr, _IONBF, 0);
    }
    g_sh->state.store(1, std::memory_order_release);
    const std::string s = scn->name;

    if (s == "segv_other_thread" || s == "kill_hard") {
        std::vector<std::thread> ts;
        for (int t = 0; t < scn->threads; ++t) ts.emplace_back([t] { log_lines(t, -1); });
        if (s == "segv_other_thread") {
            std::thread crasher([param] {
                sleep_ms((int)param);
                g_sh->trigger_ns.store(now_ns());
                fault_null_store();
            });
            crasher.join();
        }
        for (auto &t : ts) t.join();
        return 99;  // the crash / kill never came
    }
    if (s == "stack_overflow_thread") {
        std::thread th([param] {
            log_lines(0, param);
            g_sh->trigger_ns.store(now_ns());
            recurse(0);
        });
        th.join();
        return 99;
    }
    log_lines(0, param);
    g_sh->trigger_ns.store(now_ns());
    if (s == "segv_main") fault_null_store();
    else if (s == "abort") std::abort();
    else if (s == "assert") assert(g_zero.load() == 1 && "bench_crash_loss: deliberate assert failure");
    else if (s == "uncaught_exception") throw std::runtime_error("bench_crash_loss: deliberate uncaught exception");
    else if (s == "stack_overflow") recurse(0);
    else if (s == "exit") std::exit(0);
    else if (s == "_Exit") std::_Exit(0);
    return 99;
}

// ------------------------------------------------------------------ parent: processes
struct Outcome {
    bool started = false, timed_out = false, killed_by_parent = false;
    bool exited = false;  // POSIX: normal exit; Windows: always (exit code)
    long long code = -1;  // exit code (Windows: also the exception code)
    int signal = 0;       // POSIX: terminating signal
};

#if defined(_WIN32)
constexpr int kKillCode = 137;  // TerminateProcess exit code (128 + SIGKILL)
const char *signal_name(int) { return "?"; }
#else
const char *signal_name(int s) {
    switch (s) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGKILL: return "SIGKILL";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGALRM: return "SIGALRM";
        default: return "?";
    }
}
#endif

std::string describe(const Outcome &o) {
    char b[64];
    if (!o.started) return "not started";
    if (o.timed_out) return "timed out";
    if (o.exited && (o.code > 255 || o.code < 0))
        std::snprintf(b, sizeof b, "exit 0x%08llX", (unsigned long long)(o.code & 0xFFFFFFFFll));
    else if (o.exited)
        std::snprintf(b, sizeof b, "exit %lld", o.code);
    else
        std::snprintf(b, sizeof b, "signal %d (%s)", o.signal, signal_name(o.signal));
    return b;
}

// The outcome the scenario should produce when nothing interferes.
bool outcome_expected(const std::string &s, const Outcome &o) {
    if (o.timed_out || !o.started) return false;
#if defined(_WIN32)
    const long long c = o.code & 0xFFFFFFFFll;
    if (s == "segv_main" || s == "segv_other_thread") return c == 0xC0000005ll;
    if (s == "abort" || s == "assert" || s == "uncaught_exception") return c == 3;
    if (s == "stack_overflow" || s == "stack_overflow_thread") return c == 0xC00000FDll;
    if (s == "kill_hard") return c == kKillCode;
    return c == 0;
#else
    if (s == "segv_main" || s == "segv_other_thread") return !o.exited && o.signal == SIGSEGV;
    if (s == "abort" || s == "assert" || s == "uncaught_exception") return !o.exited && o.signal == SIGABRT;
    // macOS reports a secondary thread's guard-page hit as SIGBUS.
    if (s == "stack_overflow" || s == "stack_overflow_thread")
        return !o.exited && (o.signal == SIGSEGV || o.signal == SIGBUS);
    if (s == "kill_hard") return !o.exited && o.signal == SIGKILL;
    return o.exited && o.code == 0;
#endif
}

std::string expected_text(const std::string &s) {
#if defined(_WIN32)
    if (s == "segv_main" || s == "segv_other_thread") return "exit 0xC0000005 (access violation)";
    if (s == "abort" || s == "assert" || s == "uncaught_exception") return "exit 3 (abort)";
    if (s == "stack_overflow" || s == "stack_overflow_thread") return "exit 0xC00000FD (stack overflow)";
    if (s == "kill_hard") return "exit 137 (TerminateProcess)";
    return "exit 0";
#else
    if (s == "segv_main" || s == "segv_other_thread") return "signal SIGSEGV";
    if (s == "abort" || s == "assert" || s == "uncaught_exception") return "signal SIGABRT";
    if (s == "stack_overflow" || s == "stack_overflow_thread") return "signal SIGSEGV (macOS thread: SIGBUS)";
    if (s == "kill_hard") return "signal SIGKILL";
    return "exit 0";
#endif
}

std::string self_exe(const char *argv0) {
#if defined(_WIN32)
    char buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return n > 0 && n < sizeof buf ? std::string(buf, n) : std::string(argv0);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return argv0;
    char real[PATH_MAX];
    return ::realpath(buf, real) ? std::string(real) : std::string(buf);
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return argv0;
    buf[n] = '\0';
    return buf;
#endif
}

class Child {
public:
    bool start(const std::string &exe, const std::vector<std::string> &args, const std::string &out_path) {
#if defined(_WIN32)
        std::string cmd = "\"" + exe + "\"";
        for (const auto &x : args) cmd += " \"" + x + "\"";
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle = TRUE;
        HANDLE out = CreateFileA(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOA si;
        std::memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = out;
        si.hStdError = out;
        std::memset(&pi_, 0, sizeof pi_);
        std::vector<char> line(cmd.begin(), cmd.end());
        line.push_back('\0');
        const BOOL ok = CreateProcessA(exe.c_str(), line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi_);
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        return ok != FALSE;
#else
        std::fflush(stdout);
        std::fflush(stderr);
        pid_ = ::fork();
        if (pid_ == 0) {
            const int fd = ::open(out_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (fd >= 0) {
                ::dup2(fd, STDOUT_FILENO);
                ::dup2(fd, STDERR_FILENO);
                ::close(fd);
            }
            std::vector<char *> av;
            av.push_back(const_cast<char *>(exe.c_str()));
            for (const auto &x : args) av.push_back(const_cast<char *>(x.c_str()));
            av.push_back(nullptr);
            ::execv(exe.c_str(), av.data());
            _exit(127);
        }
        return pid_ > 0;
#endif
    }

    void kill_hard() {
#if defined(_WIN32)
        TerminateProcess(pi_.hProcess, kKillCode);
#else
        ::kill(pid_, SIGKILL);
#endif
    }

    // Waits up to timeout_ms; then kills it (timed_out).
    Outcome wait(int timeout_ms) {
        Outcome o;
        o.started = true;
#if defined(_WIN32)
        if (WaitForSingleObject(pi_.hProcess, (DWORD)timeout_ms) == WAIT_TIMEOUT) {
            o.timed_out = true;
            TerminateProcess(pi_.hProcess, 0xDEAD);
            WaitForSingleObject(pi_.hProcess, INFINITE);
        }
        DWORD code = 0;
        GetExitCodeProcess(pi_.hProcess, &code);
        o.exited = true;
        o.code = (long long)code;
        CloseHandle(pi_.hThread);
        CloseHandle(pi_.hProcess);
#else
        const uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
        int st = 0;
        for (;;) {
            const pid_t r = ::waitpid(pid_, &st, WNOHANG);
            if (r == pid_) break;
            if (r < 0 && errno != EINTR) break;
            if (now_ns() > deadline) {
                o.timed_out = true;
                ::kill(pid_, SIGKILL);
                while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {
                }
                break;
            }
            sleep_ms(1);
        }
        if (WIFEXITED(st)) {
            o.exited = true;
            o.code = WEXITSTATUS(st);
        } else if (WIFSIGNALED(st)) {
            o.signal = WTERMSIG(st);
        }
#endif
        return o;
    }

private:
#if defined(_WIN32)
    PROCESS_INFORMATION pi_;
#else
    pid_t pid_ = -1;
#endif
};

// ------------------------------------------------------------------ parent: log check
// Matches one line of the layout in bench_common.h:
//   YYYY-MM-DD HH:MM:SS.uuuuuu [INFO] [<tid>] <file>:<line> - iteration <i> value=<v> name=t<k>
// ([ERROR] instead of [INFO] for the ERROR lines of tblog_flush_error).
struct Cursor {
    const char *p, *e;
    bool lit(const char *s) {
        const size_t n = std::strlen(s);
        if ((size_t)(e - p) < n || std::memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }
    bool digits(int count) {  // exactly `count` digits
        for (int i = 0; i < count; ++i, ++p)
            if (p >= e || *p < '0' || *p > '9') return false;
        return true;
    }
    bool number(long long *v) {
        const char *s = p;
        long long x = 0;
        while (p < e && *p >= '0' && *p <= '9' && p - s < 18) x = x * 10 + (*p++ - '0');
        if (p == s) return false;
        *v = x;
        return true;
    }
    bool until(char c) {  // a non-empty run without spaces, up to c
        const char *s = p;
        while (p < e && *p != c && *p != ' ') ++p;
        return p > s && p < e && *p == c;
    }
};

bool parse_line(const char *b, const char *e, int *thread, long long *index, bool *is_error) {
    Cursor c{b, e};
    long long tid, line, i, v, t;
    if (!(c.digits(4) && c.lit("-") && c.digits(2) && c.lit("-") && c.digits(2) && c.lit(" ") && c.digits(2) &&
          c.lit(":") && c.digits(2) && c.lit(":") && c.digits(2) && c.lit(".") && c.digits(6)))
        return false;
    *is_error = c.lit(" [ERROR] [");
    if (!(*is_error || c.lit(" [INFO] [")))
        return false;
    if (!(c.number(&tid) && c.lit("] ") && c.until(':') && c.lit(":") && c.number(&line) &&
          c.lit(" - iteration ") && c.number(&i) && c.lit(" value=") && c.number(&v) && c.lit(" name=t") &&
          c.number(&t)))
        return false;
    if (c.p != c.e || v != i * 7 % 1000 || t < 0 || t >= kMaxThreads) return false;
    *thread = (int)t;
    *index = i;
    return true;
}

struct Check {
    long long acked = 0, found = 0, lost = 0, missing = 0, duplicates = 0, corrupt = 0, torn = 0,
              beyond_ack = 0, markers = 0;
    // error_every > 0 only: acknowledged INFO / ERROR lines missing, lines
    // missing among 1..last acknowledged ERROR line (per thread, summed).
    long long info_missing = 0, error_missing = 0, missing_to_last_error = 0, last_error = 0;
    std::vector<long long> lost_per_thread;
};

Check check_log(const std::string &text, const Shared &sh, int threads, long error_every) {
    Check r;
    std::vector<std::vector<uint8_t>> seen((size_t)threads);
    std::vector<long long> acked((size_t)threads), last_err((size_t)threads);
    for (int t = 0; t < threads; ++t) {
        acked[(size_t)t] = sh.acked[t].v.load();
        last_err[(size_t)t] = std::min<long long>(sh.last_error[t].v.load(), acked[(size_t)t]);
        seen[(size_t)t].assign((size_t)acked[(size_t)t] + 2, 0);
        r.acked += acked[(size_t)t];
        r.last_error += last_err[(size_t)t];
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        const bool whole = nl != std::string::npos;
        if (!whole) nl = text.size();
        const char *b = text.data() + pos, *e = text.data() + nl;
        if (e > b && e[-1] == '\r') --e;
        pos = nl + 1;
        if (e == b) continue;
        const std::string line(b, e);
        if (line.find("*** tblog:") != std::string::npos) {  // tblog's crash marker line
            if (line.find("caught") != std::string::npos) ++r.markers;
            continue;
        }
        if (!whole) {
            ++r.torn;
            continue;
        }
        int t = 0;
        long long i = 0;
        bool is_error = false;
        if (!parse_line(b, e, &t, &i, &is_error) || t >= threads || i < 1 ||
            is_error != (error_every > 0 && i % error_every == 0)) {
            ++r.corrupt;
            continue;
        }
        if (i > acked[(size_t)t]) {
            ++r.beyond_ack;
            continue;
        }
        uint8_t &s = seen[(size_t)t][(size_t)i];
        if (s) ++r.duplicates;
        else ++r.found;
        if (s < 255) ++s;
    }
    for (int t = 0; t < threads; ++t) {
        long long k = 0;
        while (k < acked[(size_t)t] && seen[(size_t)t][(size_t)k + 1]) ++k;
        long long miss = 0;
        for (long long i = 1; i <= acked[(size_t)t]; ++i) {
            if (seen[(size_t)t][(size_t)i]) continue;
            ++miss;
            if (error_every > 0 && i % error_every == 0) ++r.error_missing;
            else ++r.info_missing;
            if (i <= last_err[(size_t)t]) ++r.missing_to_last_error;
        }
        r.lost += acked[(size_t)t] - k;
        r.missing += miss;
        r.lost_per_thread.push_back(acked[(size_t)t] - k);
    }
    return r;
}

// ------------------------------------------------------------------ parent: driver
uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

struct Trial {
    long param = 0;
    Outcome out;
    bool outcome_ok = false;
    Check chk;
    uint64_t dropped = 0;
    double trigger_to_exit_ms = -1;  // fatal action -> the parent saw the process end
    std::string child_output;  // kept when the outcome was unexpected
};

std::string trial_json(const Trial &t) {
    std::vector<std::string> lpt;
    for (long long x : t.chk.lost_per_thread) lpt.push_back(jnum(double(x)));
    Obj o;
    o.num("param", t.param)
        .str("outcome", describe(t.out))
        .boolean("outcome_expected", t.outcome_ok)
        .num("acked", double(t.chk.acked))
        .num("found", double(t.chk.found))
        .num("lost", double(t.chk.lost))
        .raw("lost_per_thread", jarr(lpt))
        .num("missing", double(t.chk.missing))
        .num("duplicates", double(t.chk.duplicates))
        .num("corrupt", double(t.chk.corrupt))
        .num("torn", double(t.chk.torn))
        .num("beyond_ack", double(t.chk.beyond_ack))
        .num("markers", double(t.chk.markers))
        .num("info_missing", double(t.chk.info_missing))
        .num("error_missing", double(t.chk.error_missing))
        .num("missing_to_last_error", double(t.chk.missing_to_last_error))
        .num("last_error_acked", double(t.chk.last_error))
        .num("tblog_dropped", double(t.dropped))
        .num("trigger_to_exit_ms", t.trigger_to_exit_ms);
    if (!t.child_output.empty()) o.str("child_output", t.child_output);
    return o.done();
}

std::string summary_json(const std::vector<Trial> &ts) {
    double sum_lost = 0, max_lost = 0, sum_acked = 0, sum_missing = 0;
    int with_loss = 0, ok = 0, markers = 0, timeouts = 0;
    long long dups = 0, corrupt = 0, torn = 0, beyond = 0, dropped = 0;
    std::vector<double> lost;
    double sum_tte = 0, max_tte = 0;
    int n_tte = 0;
    double sum_im = 0, max_im = 0, sum_em = 0, max_em = 0, sum_ml = 0, max_ml = 0;
    for (const Trial &t : ts) {
        sum_im += double(t.chk.info_missing);
        max_im = std::max(max_im, double(t.chk.info_missing));
        sum_em += double(t.chk.error_missing);
        max_em = std::max(max_em, double(t.chk.error_missing));
        sum_ml += double(t.chk.missing_to_last_error);
        max_ml = std::max(max_ml, double(t.chk.missing_to_last_error));
        if (t.trigger_to_exit_ms >= 0) {
            sum_tte += t.trigger_to_exit_ms;
            max_tte = std::max(max_tte, t.trigger_to_exit_ms);
            ++n_tte;
        }
        sum_lost += double(t.chk.lost);
        max_lost = std::max(max_lost, double(t.chk.lost));
        sum_acked += double(t.chk.acked);
        sum_missing += double(t.chk.missing);
        with_loss += t.chk.missing > 0 || t.chk.lost > 0;
        ok += t.outcome_ok;
        markers += t.chk.markers > 0;
        timeouts += t.out.timed_out;
        dups += t.chk.duplicates;
        corrupt += t.chk.corrupt;
        torn += t.chk.torn;
        beyond += t.chk.beyond_ack;
        dropped += (long long)t.dropped;
        lost.push_back(double(t.chk.lost));
    }
    const double n = ts.empty() ? 1 : double(ts.size());
    std::sort(lost.begin(), lost.end());
    return Obj()
        .num("trials", double(ts.size()))
        .num("mean_lost", sum_lost / n)
        .num("median_lost", lost.empty() ? 0 : lost[lost.size() / 2])
        .num("max_lost", max_lost)
        .num("pct_trials_with_loss", 100.0 * with_loss / n)
        .num("mean_acked", sum_acked / n)
        .num("mean_missing", sum_missing / n)
        .num("trials_outcome_expected", ok)
        .num("trials_with_marker", markers)
        .num("timeouts", timeouts)
        .num("duplicates", double(dups))
        .num("corrupt", double(corrupt))
        .num("torn", double(torn))
        .num("beyond_ack", double(beyond))
        .num("tblog_dropped", double(dropped))
        .num("mean_info_missing", sum_im / n)
        .num("max_info_missing", max_im)
        .num("mean_error_missing", sum_em / n)
        .num("max_error_missing", max_em)
        .num("mean_missing_to_last_error", sum_ml / n)
        .num("max_missing_to_last_error", max_ml)
        .num("mean_trigger_to_exit_ms", n_tte ? sum_tte / n_tte : -1)
        .num("max_trigger_to_exit_ms", n_tte ? max_tte : -1)
        .done();
}

void usage() {
    std::printf(
        "usage: bench_crash_loss [options]\n"
        "  --quick               1 trial per cell (smoke run)\n"
        "  --trials N            trials per (scenario, variant) (default 20)\n"
        "  --scenarios LIST      default: all (segv_main,segv_other_thread,abort,assert,uncaught_exception,\n"
        "                        stack_overflow,stack_overflow_thread,kill_hard,exit,_Exit)\n"
        "  --variants LIST       fprintf_buffered,fprintf_flush,tblog,tblog_flush_error (default);\n"
        "                        also tblog_drop, fprintf_unbuffered\n"
        "  --rate N              lines/s per logging thread in the child (default 20000; 0 = flood)\n"
        "  --min-lines N --max-lines N   N lines before the trigger (default rate/20 .. rate/4)\n"
        "  --min-ms N --max-ms N         random delay of segv_other_thread / kill_hard (default 50 .. 250)\n"
        "  --seed N              (default 1)   --timeout-ms N  per child (default 30000)\n"
        "  --dir DIR             scratch directory (default <temp>/tblog-crash-<pid>); --keep: keep it\n"
        "  --out FILE | --out-dir DIR   result JSON (default name <os>-<compiler>-<cpu>-<date>-crash.json)\n"
        "  --label S --build-info S --commit SHA   recorded in the JSON\n");
}

int parent_main(const Args &args, const char *argv0) {
    const bool quick = args.flag("--quick");
    const int trials = (int)args.num("--trials", quick ? 1 : 20);
    const uint64_t rate = (uint64_t)args.num("--rate", 20000);
    const long rate_ref = (long)std::max<uint64_t>(rate, 20000);
    const long min_lines = (long)args.num("--min-lines", rate_ref / 20);
    const long max_lines = (long)args.num("--max-lines", rate_ref / 4);
    const long min_ms = (long)args.num("--min-ms", 50), max_ms = (long)args.num("--max-ms", 250);
    const uint64_t seed = (uint64_t)args.num("--seed", 1);
    const int timeout_ms = (int)args.num("--timeout-ms", 30000);
    const bool keep = args.flag("--keep");
    std::vector<std::string> snames;
    if (args.get("--scenarios", "").empty())
        for (const Scenario &s : kScenarios) snames.push_back(s.name);
    else
        snames = split(args.get("--scenarios", ""));
    const std::vector<std::string> vnames = split(args.get("--variants", "fprintf_buffered,fprintf_flush,tblog,tblog_flush_error"));
    for (const auto &s : snames)
        if (!find_scenario(s)) {
            std::fprintf(stderr, "unknown scenario %s\n", s.c_str());
            return 2;
        }
    for (const auto &v : vnames)
        if (!find_variant(v)) {
            std::fprintf(stderr, "unknown variant %s\n", v.c_str());
            return 2;
        }
#if TBLOG_BENCH_TSAN
    // ThreadSanitizer intercepts signals and runs the application's handler
    // later, on the faulting thread's stack; after a stack overflow there is
    // no stack left, so the child hangs or dies with SIGILL (seen on macOS CI)
    // instead of reaching tblog's handler. A limit of the TSan runtime, not of
    // tblog (the Release and ASan builds run these scenarios).
    {
        std::vector<std::string> kept;
        for (const auto &s : snames) {
            if (s == "stack_overflow" || s == "stack_overflow_thread")
                std::printf("skipped %s: not meaningful under ThreadSanitizer\n", s.c_str());
            else
                kept.push_back(s);
        }
        snames.swap(kept);
    }
#endif
    const std::string base = args.get("--dir", temp_root() + "/tblog-crash-" + std::to_string(pid()));
    const std::string exe = self_exe(argv0);
    remove_tree(base);
    make_dirs(base);
    std::printf("bench_crash_loss: %d trial(s) per cell, %llu lines/s per thread, scratch %s\n", trials,
                (unsigned long long)rate, base.c_str());
    std::fflush(stdout);
    const uint64_t t_start = now_ns();
    std::vector<std::string> cells;
    bool all_ok = true;
    int counter = 0;

    for (size_t si = 0; si < snames.size(); ++si) {
        const Scenario &scn = *find_scenario(snames[si]);
        std::vector<std::vector<Trial>> per_variant(vnames.size());
        for (int k = 0; k < trials; ++k) {
            const uint64_t r = splitmix(seed * 1000003ull + si * 7919ull + (uint64_t)k);
            const long lo = scn.param_is_ms ? min_ms : min_lines, hi = scn.param_is_ms ? max_ms : max_lines;
            const long param = lo + (long)(r % (uint64_t)std::max(1L, hi - lo + 1));
            for (size_t j0 = 0; j0 < vnames.size(); ++j0) {
                const size_t j = (j0 + (size_t)k) % vnames.size();  // rotating order
                const Variant &var = *find_variant(vnames[j]);
                const std::string dir = base + "/c" + std::to_string(counter++);
                remove_tree(dir);
                make_dirs(dir);
                if (var.kind == kTblog)
                    write_file(dir + "/log.conf", tblog_config(dir + "/app.log", var.policy, var.extra_config));
                Trial t;
                t.param = param;
                Mapping m;
                if (!m.open(dir + "/progress.bin", true)) {
                    std::fprintf(stderr, "cannot map %s/progress.bin\n", dir.c_str());
                    return 1;
                }
                m.get()->magic = kMagic;
                m.get()->threads = (uint32_t)scn.threads;
                Child c;
                const long child_param = scn.name == std::string("kill_hard") ? 0 : param;
                const std::vector<std::string> cargs = {"--child", scn.name, "--variant", var.name, "--dir", dir,
                                                        "--param", std::to_string(child_param), "--rate",
                                                        std::to_string(rate)};
                if (!c.start(exe, cargs, dir + "/child.txt")) {
                    std::fprintf(stderr, "cannot start %s\n", exe.c_str());
                    return 1;
                }
                if (scn.name == std::string("kill_hard")) {
                    const uint64_t until = now_ns() + 10000000000ull;
                    while (m.get()->state.load() == 0 && now_ns() < until) sleep_ms(1);
                    sleep_ms((int)param);
                    m.get()->trigger_ns.store(now_ns());
                    c.kill_hard();
                }
                t.out = c.wait(timeout_ms);
                const uint64_t trig = m.get()->trigger_ns.load(), end = now_ns();
                t.trigger_to_exit_ms = trig && end > trig ? double(end - trig) / 1e6 : -1;
                t.outcome_ok = outcome_expected(scn.name, t.out);
                t.chk = check_log(read_file(dir + "/app.log"), *m.get(), scn.threads, var.error_every);
                t.dropped = m.get()->dropped.load();
                if (!t.outcome_ok) {
                    t.child_output = read_file(dir + "/child.txt").substr(0, 400);
                    std::printf("  ! %s/%s trial %d: %s (expected %s)\n", scn.name, var.name, k,
                                describe(t.out).c_str(), expected_text(scn.name).c_str());
                }
                if (t.out.timed_out) all_ok = false;
                per_variant[j].push_back(t);
                if (!keep) remove_tree(dir);
            }
        }
        for (size_t j = 0; j < vnames.size(); ++j) {
            const std::vector<Trial> &ts = per_variant[j];
            double sl = 0, ml = 0;
            int ok = 0, mk = 0;
            for (const Trial &t : ts) {
                sl += double(t.chk.lost);
                ml = std::max(ml, double(t.chk.lost));
                ok += t.outcome_ok;
                mk += t.chk.markers > 0;
            }
            std::printf("%-22s %-17s lost mean %8.1f max %6.0f | outcome ok %d/%zu | marker %d\n", scn.name,
                        vnames[j].c_str(), sl / double(std::max<size_t>(1, ts.size())), ml, ok, ts.size(), mk);
            std::fflush(stdout);
            std::vector<std::string> tj;
            for (const Trial &t : ts) tj.push_back(trial_json(t));
            cells.push_back(Obj()
                                .str("scenario", scn.name)
                                .str("variant", vnames[j])
                                .num("threads", scn.threads)
                                .num("error_every", double(find_variant(vnames[j])->error_every))
                                .str("expected_outcome", expected_text(scn.name))
                                .boolean("crash_handler_expected", scn.crash)
                                .raw("summary", summary_json(ts))
                                .raw("trials", jarr(tj, ",\n        "))
                                .done());
        }
    }
    if (!keep) remove_tree(base);
    const double wall = double(now_ns() - t_start) / 1e9;

    Obj sd, vd;
    for (const auto &s : snames) sd.str(s, find_scenario(s)->what);
    for (const auto &v : vnames) vd.str(v, find_variant(v)->desc);
    std::string cj;
    for (size_t i = 0; i < cells.size(); ++i) cj += (i ? ",\n    " : "") + cells[i];
    const std::string config = Obj()
                                   .boolean("quick", quick)
                                   .num("trials", trials)
                                   .num("rate_per_thread", double(rate))
                                   .raw("lines_before_trigger", Obj().num("min", min_lines).num("max", max_lines).done())
                                   .raw("delay_ms", Obj().num("min", min_ms).num("max", max_ms).done())
                                   .num("seed", double(seed))
                                   .num("timeout_ms", timeout_ms)
                                   .str("lost_definition",
                                        "acknowledged (last index whose logging call returned, per thread) minus the "
                                        "longest prefix 1..k of that thread found whole in the log file")
                                   .num("wall_s", wall)
                                   .done();
    const std::string json = Obj()
                                 .str("schema", "tblog-bench-crash-loss/1")
                                 .raw("env", env_json(args, base))
                                 .raw("config", config)
                                 .raw("scenarios", sd.done())
                                 .raw("variants", vd.done())
                                 .raw("results", "[\n    " + cj + "\n  ]")
                                 .done();
    const std::string path = save_json(args, "crash", json);
    std::printf("wall %.1f s; results: %s%s\n", wall, path.c_str(), all_ok ? "" : " (SOME CHILDREN TIMED OUT)");
    return path.empty() || !all_ok ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    const Args args(argc, argv);
    if (!args.get("--child", "").empty()) return child_main(args);
    if (args.flag("--help") || args.flag("-h")) {
        usage();
        return 0;
    }
    return parent_main(args, argv[0]);
}
