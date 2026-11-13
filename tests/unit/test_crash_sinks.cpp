// Tests for src/crash/crash_handler.cpp, src/sinks/console.cpp, src/sinks/syslog.cpp
#include "internal.h"
#include "tiny_test.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tblog {
// Internal hooks defined in the module .cpp files (not part of internal.h).
void console_write_fd(int fd, const char *data, size_t len, bool color);
bool crash_altstack_for_this_thread();
}  // namespace tblog

namespace {

// ------------------------------------------------------------------ helpers
std::string test_dir() {
    const char *d = std::getenv("TBLOG_TEST_DIR");
    std::string dir = (d && *d) ? d : "build/tmp";
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
        }
        if (i < dir.size()) cur += dir[i];
    }
    return dir;
}

std::string read_file(const std::string &path) {
    std::string out;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return out;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

void raw_write(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= static_cast<size_t>(w);
    }
}
void raw_write(int fd, const char *s) { raw_write(fd, s, std::strlen(s)); }

int g_report_fd = -1;  // child side of the pipe

void null_deref() {
    volatile int *volatile p = nullptr;
    *p = 42;
}

// Fake backend: async-signal-safe recording of crash calls, mirrored to a fd.
class FakeBackend final : public tblog::Backend {
public:
    explicit FakeBackend(int fd, const char *tag = "") : fd_(fd), tag_(tag) {}

    bool append(const char *, uint32_t, tblog_level) noexcept override { return true; }
    void wait_for_work(std::chrono::milliseconds) override {}
    void flush() override {}
    void sync() override {}
    bool consume_urgent() noexcept override { return false; }
    void wake() noexcept override {}
    void shutdown() override {}
    uint64_t dropped() const noexcept override { return 0; }

    void crash_write(const char *data, size_t len) noexcept override {
        size_t room = sizeof(buf) - buf_len;
        size_t n = len < room ? len : room;
        std::memcpy(buf + buf_len, data, n);
        buf_len += n;
        if (fd_ >= 0) {
            raw_write(fd_, tag_);
            raw_write(fd_, data, len);
        }
    }
    void on_crash() noexcept override {
        on_crash_count.fetch_add(1);
        if (fd_ >= 0) {
            raw_write(fd_, tag_);
            raw_write(fd_, "ON_CRASH\n");
        }
        if (segv_in_on_crash) null_deref();
    }

    char buf[4096];
    size_t buf_len = 0;
    std::atomic<int> on_crash_count{0};
    bool segv_in_on_crash = false;

private:
    int fd_;
    const char *tag_;
};

const int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};

void reset_crash_signals_to_default() {
    // Make the child independent of whatever a sanitizer runtime installed.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for (int s : kCrashSignals) sigaction(s, &sa, nullptr);
}

struct ChildResult {
    int status = 0;
    std::string report;  // what the child wrote to the pipe
    std::string err;     // child's stderr
};

int g_child_seq = 0;

// Run fn in a forked child (stderr -> file, report pipe in g_report_fd,
// no core dumps, 10 s alarm as a safety net against hangs).
template <class F>
ChildResult run_child(F fn) {
    ChildResult r;
    std::string err_path = test_dir() + "/crash_err_" + std::to_string(::getpid()) + "_" +
                           std::to_string(g_child_seq++) + ".txt";
    int fds[2];
    if (::pipe(fds) != 0) return r;
    std::fflush(stdout);
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        g_report_fd = fds[1];
        // 1 (not 0): with a piped core_pattern (apport, systemd-coredump) the
        // kernel still invokes the helper for 0; 1 makes it skip the dump.
        struct rlimit rl = {1, 1};
        setrlimit(RLIMIT_CORE, &rl);
        int efd = ::open(err_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (efd >= 0) {
            ::dup2(efd, STDERR_FILENO);
            ::close(efd);
        }
        reset_crash_signals_to_default();
        alarm(10);
        fn();
        _exit(0);
    }
    ::close(fds[1]);
    char buf[1024];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        r.report.append(buf, static_cast<size_t>(n));
    }
    ::close(fds[0]);
    while (::waitpid(pid, &r.status, 0) < 0 && errno == EINTR) {
    }
    r.err = read_file(err_path);
    ::unlink(err_path.c_str());
    return r;
}

bool died_with(const ChildResult &r, int sig) {
    return WIFSIGNALED(r.status) && WTERMSIG(r.status) == sig;
}

std::string expected_msg(int sig, const char *name) {
    return "*** tblog: caught signal " + std::to_string(sig) + " (" + name + ") ***\n";
}

}  // namespace

// ============================================================ crash handler

TEST(crash_segv_reported_and_dies_with_sigsegv) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        null_deref();
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_EQ(r.report, std::string("ON_CRASH\n") + expected_msg(SIGSEGV, "SIGSEGV"));
    EXPECT_TRUE(contains(r.err, expected_msg(SIGSEGV, "SIGSEGV")));
}

TEST(crash_abort_reported_and_dies_with_sigabrt) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        std::abort();
    });
    EXPECT_TRUE(died_with(r, SIGABRT));
    EXPECT_EQ(r.report, std::string("ON_CRASH\n") + expected_msg(SIGABRT, "SIGABRT"));
}

TEST(crash_sigfpe_raise_reported_and_dies_with_sigfpe) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        raise(SIGFPE);
    });
    EXPECT_TRUE(died_with(r, SIGFPE));
    EXPECT_EQ(r.report, std::string("ON_CRASH\n") + expected_msg(SIGFPE, "SIGFPE"));
}

TEST(crash_sigbus_and_sigill_raise) {
    ChildResult r1 = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        raise(SIGBUS);
    });
    EXPECT_TRUE(died_with(r1, SIGBUS));
    EXPECT_EQ(r1.report, std::string("ON_CRASH\n") + expected_msg(SIGBUS, "SIGBUS"));
    ChildResult r2 = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        raise(SIGILL);
    });
    EXPECT_TRUE(died_with(r2, SIGILL));
    EXPECT_EQ(r2.report, std::string("ON_CRASH\n") + expected_msg(SIGILL, "SIGILL"));
}

TEST(crash_backtrace_goes_to_stderr) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        null_deref();
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_TRUE(contains(r.err, "caught signal 11"));
#if !defined(TBLOG_NO_BACKTRACE) && __has_include(<execinfo.h>)
    // backtrace_symbols_fd prints lines like "binary(func+0x..)[0x...]".
    EXPECT_TRUE(contains(r.err, "[0x"));
    EXPECT_TRUE(contains(r.err, "test_crash_sinks"));
#else
    // musl / no-backtrace build: crash still reported, just no stack dump.
    EXPECT_FALSE(contains(r.err, "[0x"));
#endif
}

TEST(crash_reentrant_fault_in_on_crash_during_abort) {
    // on_crash segfaults while handling SIGABRT -> our handler is re-entered
    // for SIGSEGV and must go straight to dying with SIGSEGV.
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        fb.segv_in_on_crash = true;
        tblog::install_crash_handler(&fb);
        std::abort();
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_EQ(r.report, std::string("ON_CRASH\n"));  // on_crash faulted before the marker
}

TEST(crash_reentrant_fault_in_on_crash_during_segv) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        fb.segv_in_on_crash = true;
        tblog::install_crash_handler(&fb);
        null_deref();
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_EQ(r.report, std::string("ON_CRASH\n"));  // on_crash faulted before the marker
}

namespace {
void app_siginfo_handler(int, siginfo_t *, void *) {
    raw_write(g_report_fd, "APP\n");
    _exit(42);
}
void app_plain_handler(int) {
    raw_write(g_report_fd, "APP_PLAIN\n");
    _exit(43);
}
}  // namespace

TEST(crash_chains_to_previous_app_handler) {
    ChildResult r = run_child([] {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = app_siginfo_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        null_deref();  // hardware fault: chained via re-execution
    });
    EXPECT_TRUE(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 42);
    EXPECT_EQ(r.report, "ON_CRASH\n" + expected_msg(SIGSEGV, "SIGSEGV") + "APP\n");

    ChildResult r2 = run_child([] {
        signal(SIGFPE, app_plain_handler);
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        raise(SIGFPE);  // user-sent: chained via re-raise
    });
    EXPECT_TRUE(WIFEXITED(r2.status) && WEXITSTATUS(r2.status) == 43);
    EXPECT_EQ(r2.report, "ON_CRASH\n" + expected_msg(SIGFPE, "SIGFPE") + "APP_PLAIN\n");
}

TEST(crash_uninstall_restores_previous_actions) {
    ChildResult r = run_child([] {
        signal(SIGFPE, app_plain_handler);  // others: SIG_DFL
        struct sigaction before[5], during, after;
        for (int i = 0; i < 5; ++i) sigaction(kCrashSignals[i], nullptr, &before[i]);
        static FakeBackend fb(-1);
        tblog::install_crash_handler(&fb);
        bool ok = true;
        for (int i = 0; i < 5; ++i) {
            sigaction(kCrashSignals[i], nullptr, &during);
            ok = ok && (during.sa_flags & SA_SIGINFO) && (during.sa_flags & SA_ONSTACK);
        }
        stack_t ss;
        ok = ok && sigaltstack(nullptr, &ss) == 0 && !(ss.ss_flags & SS_DISABLE);
        tblog::uninstall_crash_handler();
        for (int i = 0; i < 5; ++i) {
            sigaction(kCrashSignals[i], nullptr, &after);
            ok = ok && after.sa_handler == before[i].sa_handler &&
                 (after.sa_flags & SA_SIGINFO) == (before[i].sa_flags & SA_SIGINFO);
        }
        raw_write(g_report_fd, ok ? "OK" : "BAD");
        _exit(0);
    });
    EXPECT_TRUE(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    EXPECT_EQ(r.report, std::string("OK"));
}

TEST(crash_after_uninstall_not_reported) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        tblog::uninstall_crash_handler();
        null_deref();
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_EQ(r.report, std::string(""));
}

TEST(crash_backend_replace_and_null) {
    ChildResult r = run_child([] {
        static FakeBackend a(g_report_fd, "A:");
        static FakeBackend b(g_report_fd, "B:");
        tblog::install_crash_handler(&a);
        tblog::install_crash_handler(&b);  // replace, must not self-chain
        null_deref();
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_EQ(r.report, "B:ON_CRASH\nB:" + expected_msg(SIGSEGV, "SIGSEGV"));

    ChildResult r2 = run_child([] {
        tblog::install_crash_handler(nullptr);
        std::abort();
    });
    EXPECT_TRUE(died_with(r2, SIGABRT));
    EXPECT_EQ(r2.report, std::string(""));
    EXPECT_TRUE(contains(r2.err, expected_msg(SIGABRT, "SIGABRT")));
}

TEST(crash_fake_backend_records_into_buffer) {
    // Check FakeBackend's fixed buffer + counter: the child reports them to
    // the pipe from inside on_crash.
    ChildResult r = run_child([] {
        static FakeBackend fb(-1);
        struct Reporter final : public tblog::Backend {
            bool append(const char *, uint32_t, tblog_level) noexcept override { return true; }
            void wait_for_work(std::chrono::milliseconds) override {}
            void flush() override {}
            void sync() override {}
            bool consume_urgent() noexcept override { return false; }
            void wake() noexcept override {}
            void shutdown() override {}
            uint64_t dropped() const noexcept override { return 0; }
            // on_crash runs first, then crash_write: report once the marker is recorded.
            void on_crash() noexcept override { fb.on_crash(); }
            void crash_write(const char *d, size_t n) noexcept override {
                fb.crash_write(d, n);
                raw_write(g_report_fd, fb.buf, fb.buf_len);
                raw_write(g_report_fd, fb.on_crash_count.load() == 1 ? "count=1\n" : "count?\n");
            }
        };
        static Reporter rep;
        tblog::install_crash_handler(&rep);
        raise(SIGFPE);
    });
    EXPECT_TRUE(died_with(r, SIGFPE));
    EXPECT_EQ(r.report, expected_msg(SIGFPE, "SIGFPE") + "count=1\n");
}

namespace {
volatile int g_stop_recursion = 0;
__attribute__((noinline)) int recurse(int depth) {
    volatile char pad[512];
    pad[0] = static_cast<char>(depth);
    if (g_stop_recursion) return pad[0];
    return recurse(depth + 1) + pad[0];
}
void *overflow_thread(void *) {
    tblog::crash_altstack_for_this_thread();
    recurse(0);
    return nullptr;
}
void *segv_thread(void *) {
    null_deref();
    return nullptr;
}
}  // namespace

TEST(crash_in_other_thread_reported) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        pthread_t t;
        pthread_create(&t, nullptr, segv_thread, nullptr);
        pthread_join(t, nullptr);
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_EQ(r.report, std::string("ON_CRASH\n") + expected_msg(SIGSEGV, "SIGSEGV"));
}

TEST(crash_stack_overflow_in_thread_with_altstack) {
    ChildResult r = run_child([] {
        static FakeBackend fb(g_report_fd);
        tblog::install_crash_handler(&fb);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_t t;
        pthread_create(&t, &attr, overflow_thread, nullptr);
        pthread_join(t, nullptr);
    });
    EXPECT_TRUE(died_with(r, SIGSEGV));
    EXPECT_EQ(r.report, std::string("ON_CRASH\n") + expected_msg(SIGSEGV, "SIGSEGV"));
}

// ============================================================ console sink

namespace {
std::string console_to_string(const std::string &in, bool color) {
    std::string path = test_dir() + "/console_" + std::to_string(::getpid()) + ".txt";
    int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return "<open failed>";
    tblog::console_write_fd(fd, in.data(), in.size(), color);
    ::close(fd);
    std::string out = read_file(path);
    ::unlink(path.c_str());
    return out;
}
}  // namespace

TEST(console_colorizes_by_level) {
    std::string in =
        "t [TRACE] a\n"
        "d [DEBUG] b\n"
        "i [INFO] c\n"
        "w [WARN] d\n"
        "e [ERROR] e\n"
        "f [FATAL] f\n"
        "plain line\n"
        "[bracket] but [WARNING] no token\n"
        "\n"
        "last [ERROR] no newline";
    std::string want =
        "\033[2mt [TRACE] a\033[0m\n"
        "\033[2md [DEBUG] b\033[0m\n"
        "i [INFO] c\n"
        "\033[33mw [WARN] d\033[0m\n"
        "\033[31me [ERROR] e\033[0m\n"
        "\033[1;31mf [FATAL] f\033[0m\n"
        "plain line\n"
        "[bracket] but [WARNING] no token\n"
        "\n"
        "\033[31mlast [ERROR] no newline\033[0m";
    EXPECT_EQ(console_to_string(in, true), want);
    EXPECT_EQ(console_to_string(in, false), in);
}

TEST(console_large_batch_and_long_line) {
    std::string in;
    for (int i = 0; i < 20000; ++i)
        in += "2026-01-01 00:00:00.000000 [" + std::string(i % 2 ? "WARN" : "INFO") +
              "] [1] f.cpp:1 - message " + std::to_string(i) + "\n";
    in += "x [FATAL] " + std::string(50000, 'z') + "\n";  // longer than the stack buffer
    std::string out = console_to_string(in, true);
    size_t lines = 0, warn = 0;
    for (char c : out) lines += (c == '\n');
    for (size_t p = 0; (p = out.find("\033[33m", p)) != std::string::npos; ++p) ++warn;
    EXPECT_EQ(lines, static_cast<size_t>(20001));
    EXPECT_EQ(warn, static_cast<size_t>(10000));
    EXPECT_EQ(out.size(), in.size() + 10000 * (5 + 4) + (7 + 4));
    EXPECT_TRUE(contains(out, "\033[1;31mx [FATAL] " + std::string(50000, 'z') + "\033[0m\n"));
}

TEST(console_sink_no_color_when_not_tty) {
    std::string path = test_dir() + "/console_sink_" + std::to_string(::getpid()) + ".txt";
    int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    ASSERT_TRUE(fd >= 0);
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
    {
        auto sink = tblog::make_console_sink(true);  // stderr is a file -> no colour
        std::string s = "a [ERROR] x\nb [WARN] y";
        sink->write(s.data(), s.size());
        sink->write("\n", 1);
        sink->flush();
        auto plain = tblog::make_console_sink(false);
        plain->write("c [FATAL] z\n", 12);
    }
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    EXPECT_EQ(read_file(path), std::string("a [ERROR] x\nb [WARN] y\nc [FATAL] z\n"));
    ::unlink(path.c_str());
}

// ============================================================ syslog sink

TEST(syslog_sink_handles_batches) {
    auto s1 = tblog::make_syslog_sink("tblog-unit-test");
    std::string batch =
        "x [TRACE] trace\nx [DEBUG] debug\nx [INFO] info\nx [WARN] warn\n"
        "x [ERROR] error\nx [FATAL] fatal\nno level %s %n %x\n\n"
        "last line without newline";
    s1->write(batch.data(), batch.size());
    s1->write("", 0);
    s1->flush();
    {
        auto s2 = tblog::make_syslog_sink(nullptr);  // shares connection
        s2->write("y [INFO] second sink\n", 21);
    }
    s1->write("z [INFO] after second sink closed\n", 34);
    s1.reset();
    auto s3 = tblog::make_syslog_sink("tblog-unit-test-reopen");
    s3->write("r [WARN] reopened\n", 18);
    EXPECT_TRUE(true);
}

TINY_TEST_MAIN()
