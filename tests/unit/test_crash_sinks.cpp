// Tests for src/crash/crash_handler.cpp (through a real Pipeline and Queue),
// src/sinks/console.cpp, src/sinks/syslog.cpp
//
// Crash tests run in a child process (tos::run_child re-executes this binary
// on a TINY_CHILD scenario). The child reports what the crash handler made
// its sinks write into a report file (tos::RawFile: raw write(2) / WriteFile,
// safe from the handler), the parent checks the report, the child's stderr
// and how it died. POSIX: real signals; Windows: SEH exceptions (unhandled
// exception filter) and SIGABRT.
#include "crash/crash_handler.h"
#include "logger/pipeline.h"
#include "platform/platform.h"
#include "queue/queue.h"
#include "sinks/sink.h"
#include "test_os.h"
#include "tiny_test.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>

using tos::Crash;

namespace {

// ------------------------------------------------------------------ helpers
bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

tos::RawFile g_report;  // child side: the report file (path = tos::child_arg())

void open_report() { g_report = tos::RawFile::open_append(tos::child_arg()); }

void null_deref() {
    volatile int *volatile p = nullptr;
    *p = 42;
}

// Simulated faults for the crash kinds other than SIGSEGV / abort().
void trigger(Crash c) {
    switch (c) {
        case Crash::Segv: null_deref(); break;
        case Crash::Abort: std::abort();
#if defined(_WIN32)
        case Crash::Fpe: RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr); break;
        case Crash::Ill: RaiseException(EXCEPTION_ILLEGAL_INSTRUCTION, 0, 0, nullptr); break;
        case Crash::Bus: break;
#else
        case Crash::Fpe: raise(SIGFPE); break;
        case Crash::Ill: raise(SIGILL); break;
        case Crash::Bus: raise(SIGBUS); break;
#endif
        case Crash::StackOverflow: break;
    }
}

// The marker the crash handler writes for each kind.
std::string expected_msg(Crash c) {
#if defined(_WIN32)
    switch (c) {
        case Crash::Segv: return "*** tblog: caught exception 0xC0000005 (EXCEPTION_ACCESS_VIOLATION) ***\n";
        case Crash::Abort: return "*** tblog: caught signal 22 (SIGABRT) ***\n";
        case Crash::Fpe: return "*** tblog: caught exception 0xC0000094 (EXCEPTION_INT_DIVIDE_BY_ZERO) ***\n";
        case Crash::Ill: return "*** tblog: caught exception 0xC000001D (EXCEPTION_ILLEGAL_INSTRUCTION) ***\n";
        case Crash::Bus: return "";
        case Crash::StackOverflow: return "*** tblog: caught exception 0xC00000FD (EXCEPTION_STACK_OVERFLOW) ***\n";
    }
    return "";
#else
    int sig = SIGSEGV;
    const char *name = "SIGSEGV";
    switch (c) {
        case Crash::Segv: case Crash::StackOverflow: break;
        case Crash::Abort: sig = SIGABRT; name = "SIGABRT"; break;
        case Crash::Fpe: sig = SIGFPE; name = "SIGFPE"; break;
        case Crash::Ill: sig = SIGILL; name = "SIGILL"; break;
        case Crash::Bus: sig = SIGBUS; name = "SIGBUS"; break;
    }
    return "*** tblog: caught signal " + std::to_string(sig) + " (" + name + ") ***\n";
#endif
}

// Crash-safe fake sink: async-signal-safe recording of crash_write calls,
// mirrored to the report file (prefixed with `tag`) when `mirror`.
class FakeSink final : public tblog::LogSink {
public:
    explicit FakeSink(bool mirror, const char *tag = "") : mirror_(mirror), tag_(tag) {}

    void write(const char *, size_t) override {}
    bool crash_safe() const noexcept override { return true; }
    void crash_write(const char *data, size_t len) noexcept override {
        crash_writes.fetch_add(1);
        size_t room = sizeof(buf) - buf_len;
        size_t n = len < room ? len : room;
        std::memcpy(buf + buf_len, data, n);
        buf_len += n;
        if (mirror_) {
            g_report.write(tag_);
            g_report.write(data, len);
        }
        if (segv_in_crash_write) null_deref();
        if (report_on_marker && len > 4 && std::memcmp(data, "*** ", 4) == 0) {
            // The marker is the last crash_write: report what was recorded.
            g_report.write(buf, buf_len);
            g_report.write(crash_writes.load() == 2 ? "count=2\n" : "count?\n");
        }
    }

    char buf[4096];
    size_t buf_len = 0;
    std::atomic<int> crash_writes{0};
    bool segv_in_crash_write = false;
    bool report_on_marker = false;

private:
    bool mirror_;
    const char *tag_;
};

const char kPending[] = "PENDING\n";  // the one line buffered in a fake pipeline

// A Pipeline (never freed: the child dies) whose queue holds kPending and
// whose only sink is a FakeSink. The handler drains kPending into the sink,
// then writes the marker to it.
tblog::Pipeline *fake_pipeline(bool mirror = true, const char *tag = "", FakeSink **out = nullptr) {
    tblog::QueueOptions qo;
    qo.buffer_size = 64u << 10;
    tblog::Pipeline *p = new tblog::Pipeline(tblog::Queue::create(qo));
    FakeSink *s = new FakeSink(mirror, tag);
    p->add_sink(std::unique_ptr<tblog::LogSink>(s));
    p->queue().append(kPending, sizeof kPending - 1, TBLOG_LV_INFO);
    if (out) *out = s;
    return p;
}

struct Run {
    tos::ChildResult r;
    std::string report;  // what the child wrote to the report file
    std::string err;     // child's stderr
};

int g_child_seq = 0;

Run run_scenario(const char *name) {
    const std::string base = tos::test_dir() + "/crash_" + std::to_string(tos::pid()) + "_" +
                             std::to_string(g_child_seq++);
    const std::string report = base + ".report";
    tos::remove_file(report);
    Run run;
    run.r = tos::run_child(name, report, base + ".err");
    run.report = tos::read_file(report);
    run.err = run.r.err;
    tos::remove_file(report);
    if (!run.r.started || run.r.timed_out)
        std::printf("  child %s: %s\n", name, tos::describe(run.r).c_str());
    return run;
}

bool died_with(const Run &run, Crash c) {
    if (tos::died_with(run.r, c)) return true;
    std::printf("  unexpected child outcome: %s\n", tos::describe(run.r).c_str());
    return false;
}

}  // namespace

// ============================================================ crash handler

TINY_CHILD(segv_with_pipeline) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    null_deref();
}
TEST(crash_segv_reported_and_dies_with_sigsegv) {
    Run r = run_scenario("segv_with_pipeline");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    EXPECT_EQ(r.report, std::string(kPending) + expected_msg(Crash::Segv));
    EXPECT_TRUE(contains(r.err, expected_msg(Crash::Segv)));
}

TINY_CHILD(abort_with_pipeline) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    std::abort();
}
TEST(crash_abort_reported_and_dies_with_sigabrt) {
    Run r = run_scenario("abort_with_pipeline");
    EXPECT_TRUE(died_with(r, Crash::Abort));
    EXPECT_EQ(r.report, std::string(kPending) + expected_msg(Crash::Abort));
}

TINY_CHILD(fpe_with_pipeline) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    trigger(Crash::Fpe);
}
TEST(crash_sigfpe_raise_reported_and_dies_with_sigfpe) {
    Run r = run_scenario("fpe_with_pipeline");
    EXPECT_TRUE(died_with(r, Crash::Fpe));
    EXPECT_EQ(r.report, std::string(kPending) + expected_msg(Crash::Fpe));
}

TINY_CHILD(bus_with_pipeline) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    trigger(Crash::Bus);
}
TINY_CHILD(ill_with_pipeline) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    trigger(Crash::Ill);
}
TEST(crash_sigbus_and_sigill_raise) {
#if !defined(_WIN32)  // Windows has no SIGBUS (misaligned / paging faults are SEH exceptions)
    Run r1 = run_scenario("bus_with_pipeline");
    EXPECT_TRUE(died_with(r1, Crash::Bus));
    EXPECT_EQ(r1.report, std::string(kPending) + expected_msg(Crash::Bus));
#endif
    Run r2 = run_scenario("ill_with_pipeline");
    EXPECT_TRUE(died_with(r2, Crash::Ill));
    EXPECT_EQ(r2.report, std::string(kPending) + expected_msg(Crash::Ill));
}

TEST(crash_backtrace_goes_to_stderr) {
    Run r = run_scenario("segv_with_pipeline");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    EXPECT_TRUE(contains(r.err, expected_msg(Crash::Segv)));
#if defined(_WIN32)
    // Return addresses only: "  #0          [0x00007ff6...]".
    EXPECT_TRUE(contains(r.err, "*** tblog: stack (return addresses):"));
    EXPECT_TRUE(contains(r.err, "  #0"));
    EXPECT_TRUE(contains(r.err, "[0x"));
#elif defined(__APPLE__)
    // macOS backtrace_symbols_fd: "0   test_crash_sinks   0x0000000102f1c3a8 func + 24".
    EXPECT_TRUE(contains(r.err, " 0x"));
    EXPECT_TRUE(contains(r.err, "test_crash_sinks"));
#elif !defined(TBLOG_NO_BACKTRACE) && __has_include(<execinfo.h>)
    // backtrace_symbols_fd prints lines like "binary(func+0x..)[0x...]".
    EXPECT_TRUE(contains(r.err, "[0x"));
    EXPECT_TRUE(contains(r.err, "test_crash_sinks"));
#else
    // musl / no-backtrace build: crash still reported, just no stack dump.
    EXPECT_FALSE(contains(r.err, "[0x"));
#endif
}

TINY_CHILD(fault_in_crash_write_during_abort) {
    open_report();
    FakeSink *s = nullptr;
    tblog::Pipeline *p = fake_pipeline(true, "", &s);
    s->segv_in_crash_write = true;
    tblog::install_crash_handler(p);
    std::abort();
}
TEST(crash_reentrant_fault_in_crash_write_during_abort) {
    // The sink segfaults while draining for SIGABRT -> our handler is
    // re-entered for SIGSEGV and must go straight to dying with SIGSEGV.
    Run r = run_scenario("fault_in_crash_write_during_abort");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    EXPECT_EQ(r.report, std::string(kPending));  // faulted before the marker
}

TINY_CHILD(fault_in_crash_write_during_segv) {
    open_report();
    FakeSink *s = nullptr;
    tblog::Pipeline *p = fake_pipeline(true, "", &s);
    s->segv_in_crash_write = true;
    tblog::install_crash_handler(p);
    null_deref();
}
TEST(crash_reentrant_fault_in_crash_write_during_segv) {
    Run r = run_scenario("fault_in_crash_write_during_segv");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    EXPECT_EQ(r.report, std::string(kPending));  // faulted before the marker
}

namespace {
void app_plain_handler(int) {
    g_report.write("APP_PLAIN\n");
    std::_Exit(43);
}
#if defined(_WIN32)
LONG WINAPI app_filter(EXCEPTION_POINTERS *) {
    g_report.write("APP\n");
    std::_Exit(42);
}
#else
void app_siginfo_handler(int, siginfo_t *, void *) {
    g_report.write("APP\n");
    _exit(42);
}
#endif
}  // namespace

TINY_CHILD(chain_to_app_fault_handler) {
    open_report();
#if defined(_WIN32)
    SetUnhandledExceptionFilter(&app_filter);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = app_siginfo_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
#endif
    tblog::install_crash_handler(fake_pipeline());
    null_deref();  // hardware fault: chained via re-execution / the previous filter
}
TINY_CHILD(chain_to_app_plain_handler) {
    open_report();
#if defined(_WIN32)
    std::signal(SIGABRT, app_plain_handler);
    tblog::install_crash_handler(fake_pipeline());
    std::abort();  // chained via re-raise
#else
    signal(SIGFPE, app_plain_handler);
    tblog::install_crash_handler(fake_pipeline());
    raise(SIGFPE);  // user-sent: chained via re-raise
#endif
}
TEST(crash_chains_to_previous_app_handler) {
    Run r = run_scenario("chain_to_app_fault_handler");
    EXPECT_TRUE(tos::exited_with(r.r, 42));
    EXPECT_EQ(r.report, kPending + expected_msg(Crash::Segv) + "APP\n");

    Run r2 = run_scenario("chain_to_app_plain_handler");
    EXPECT_TRUE(tos::exited_with(r2.r, 43));
#if defined(_WIN32)
    EXPECT_EQ(r2.report, kPending + expected_msg(Crash::Abort) + "APP_PLAIN\n");
#else
    EXPECT_EQ(r2.report, kPending + expected_msg(Crash::Fpe) + "APP_PLAIN\n");
#endif
}

TINY_CHILD(uninstall_restores) {
    open_report();
    bool ok = true;
#if defined(_WIN32)
    std::signal(SIGABRT, app_plain_handler);
    LPTOP_LEVEL_EXCEPTION_FILTER before = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(before);
    tblog::install_crash_handler(fake_pipeline(false));
    LPTOP_LEVEL_EXCEPTION_FILTER during = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(during);
    void (*during_abort)(int) = std::signal(SIGABRT, SIG_DFL);
    std::signal(SIGABRT, during_abort);
    ok = ok && during != nullptr && during != before && during_abort != &app_plain_handler &&
         during_abort != SIG_DFL;
    tblog::uninstall_crash_handler();
    LPTOP_LEVEL_EXCEPTION_FILTER after = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(after);
    void (*after_abort)(int) = std::signal(SIGABRT, SIG_DFL);
    ok = ok && after == before && after_abort == &app_plain_handler;
#else
    static const int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    signal(SIGFPE, app_plain_handler);  // others: SIG_DFL
    struct sigaction before[5], during, after;
    for (int i = 0; i < 5; ++i) sigaction(kCrashSignals[i], nullptr, &before[i]);
    tblog::install_crash_handler(fake_pipeline(false));
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
#endif
    g_report.write(ok ? "OK" : "BAD");
}
TEST(crash_uninstall_restores_previous_actions) {
    Run r = run_scenario("uninstall_restores");
    EXPECT_TRUE(tos::exited_with(r.r, 0));
    EXPECT_EQ(r.report, std::string("OK"));
}

TINY_CHILD(segv_after_uninstall) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    tblog::uninstall_crash_handler();
    null_deref();
}
TEST(crash_after_uninstall_not_reported) {
    Run r = run_scenario("segv_after_uninstall");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    EXPECT_EQ(r.report, std::string(""));
}

TINY_CHILD(pipeline_replaced) {
    open_report();
    tblog::install_crash_handler(fake_pipeline(true, "A:"));
    tblog::install_crash_handler(fake_pipeline(true, "B:"));  // replace, no self-chain
    null_deref();
}
TINY_CHILD(pipeline_null_abort) {
    open_report();
    tblog::install_crash_handler(nullptr);
    std::abort();
}
TEST(crash_pipeline_replace_and_null) {
    Run r = run_scenario("pipeline_replaced");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    EXPECT_EQ(r.report, std::string("B:") + kPending + "B:" + expected_msg(Crash::Segv));

    Run r2 = run_scenario("pipeline_null_abort");
    EXPECT_TRUE(died_with(r2, Crash::Abort));
    EXPECT_EQ(r2.report, std::string(""));
    EXPECT_TRUE(contains(r2.err, expected_msg(Crash::Abort)));
}

TINY_CHILD(fake_sink_records) {
    open_report();
    FakeSink *s = nullptr;
    tblog::Pipeline *p = fake_pipeline(false, "", &s);
    s->report_on_marker = true;
    tblog::install_crash_handler(p);
    trigger(Crash::Fpe);
}
TEST(crash_fake_sink_records_into_buffer) {
    // Check FakeSink's fixed buffer + counter: the child reports them from
    // inside the handler, once the marker (2nd crash_write) is in.
    Run r = run_scenario("fake_sink_records");
    EXPECT_TRUE(died_with(r, Crash::Fpe));
    EXPECT_EQ(r.report, kPending + expected_msg(Crash::Fpe) + "count=2\n");
}

TINY_CHILD(drain_every_crash_safe_sink) {
    open_report();
    tblog::QueueOptions qo;
    tblog::Pipeline *p = new tblog::Pipeline(tblog::Queue::create(qo));
    p->add_sink(std::unique_ptr<tblog::LogSink>(new FakeSink(true, "1:")));
    struct Plain final : tblog::LogSink {
        void write(const char *, size_t) override {}
        void crash_write(const char *, size_t) noexcept override { g_report.write("PLAIN?\n"); }
    };
    p->add_sink(std::unique_ptr<tblog::LogSink>(new Plain));
    p->add_sink(std::unique_ptr<tblog::LogSink>(new FakeSink(true, "2:")));
    p->queue().append("one\n", 4, TBLOG_LV_INFO);
    p->queue().append("two\n", 4, TBLOG_LV_INFO);
    tblog::install_crash_handler(p);
    null_deref();
}
TEST(crash_drains_into_every_crash_safe_sink_in_order) {
    // Two crash-safe sinks and a non-crash-safe one: both crash-safe sinks
    // get every buffered line (in order, whole) and then the marker; the
    // other sink gets nothing from the handler.
    Run r = run_scenario("drain_every_crash_safe_sink");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    const std::string m = expected_msg(Crash::Segv);
    EXPECT_EQ(r.report, "1:one\ntwo\n2:one\ntwo\n1:" + m + "2:" + m);
}

TINY_CHILD(console_sink_crash) {
    open_report();
    tblog::QueueOptions qo;
    tblog::Pipeline *p = new tblog::Pipeline(tblog::Queue::create(qo));
    p->add_sink(tblog::make_console_sink(true));  // stderr is a file: no colour
    p->queue().append("a [INFO] one\n", 13, TBLOG_LV_INFO);
    p->queue().append("b [ERROR] two\n", 14, TBLOG_LV_ERROR);
    tblog::install_crash_handler(p);
    null_deref();
}
TEST(crash_console_sink_gets_lines_then_marker_once) {
    // The console sink is crash-safe: buffered lines reach stderr, followed by
    // the marker, which is then not printed a second time.
    Run r = run_scenario("console_sink_crash");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    const std::string m = expected_msg(Crash::Segv);
    EXPECT_EQ(r.err.substr(0, 27 + m.size()), "a [INFO] one\nb [ERROR] two\n" + m);
    size_t count = 0;
    for (size_t pos = 0; (pos = r.err.find("*** tblog: caught", pos)) != std::string::npos; ++pos)
        ++count;
    EXPECT_EQ(count, size_t(1));
}

namespace {
volatile int g_stop_recursion = 0;
TOS_NOINLINE int recurse(int depth) {
    volatile char pad[512];
    pad[0] = static_cast<char>(depth);
    if (g_stop_recursion) return pad[0];
    return recurse(depth + 1) + pad[0];
}
void overflow_thread() {
    tblog::crash_altstack_for_this_thread();
    recurse(0);
}
}  // namespace

TINY_CHILD(segv_in_other_thread) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    std::thread t([] { null_deref(); });
    t.join();
}
TEST(crash_in_other_thread_reported) {
    Run r = run_scenario("segv_in_other_thread");
    EXPECT_TRUE(died_with(r, Crash::Segv));
    EXPECT_EQ(r.report, std::string(kPending) + expected_msg(Crash::Segv));
}

TINY_CHILD(stack_overflow_in_thread) {
    open_report();
    tblog::install_crash_handler(fake_pipeline());
    tos::run_thread_with_stack(256 * 1024, &overflow_thread);
}
TEST(crash_stack_overflow_in_thread_with_altstack) {
    Run r = run_scenario("stack_overflow_in_thread");
    EXPECT_TRUE(died_with(r, Crash::StackOverflow));
#if defined(__APPLE__)
    // Guard-page hit on a secondary thread: SIGBUS on macOS (see tos::died_with).
    if (r.r.signal == SIGBUS) {
        EXPECT_EQ(r.report, std::string(kPending) + expected_msg(Crash::Bus));
        return;
    }
#endif
    EXPECT_EQ(r.report, std::string(kPending) + expected_msg(Crash::StackOverflow));
}

// ============================================================ console sink

namespace {
std::string console_to_string(const std::string &in, bool color) {
    std::string path = tos::test_dir() + "/console_" + std::to_string(tos::pid()) + ".txt";
    tblog::pf::FileHandle h = tblog::pf::file_open(path, tblog::pf::OpenMode::Truncate);
    if (!h.valid()) return "<open failed>";
    tblog::console_write_to(h, in.data(), in.size(), color);
    tblog::pf::file_close(h);
    std::string out = tos::read_file(path);
    tos::remove_file(path);
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
    std::string path = tos::test_dir() + "/console_sink_" + std::to_string(tos::pid()) + ".txt";
    {
        tos::StderrToFile redirect(path);
        ASSERT_TRUE(redirect.ok());
        auto sink = tblog::make_console_sink(true);  // stderr is a file -> no colour
        std::string s = "a [ERROR] x\nb [WARN] y";
        sink->write(s.data(), s.size());
        sink->write("\n", 1);
        sink->flush();
        auto plain = tblog::make_console_sink(false);
        plain->write("c [FATAL] z\n", 12);
    }
    EXPECT_EQ(tos::read_file(path), std::string("a [ERROR] x\nb [WARN] y\nc [FATAL] z\n"));
    tos::remove_file(path);
}

// ============================================================ syslog sink

TEST(syslog_sink_handles_batches) {
    if (!tblog::pf::syslog_available()) {
        std::fprintf(stderr, "(expected 'syslog is not supported' warning follows)\n");
        EXPECT_TRUE(tblog::make_syslog_sink("tblog-unit-test") == nullptr);
        TINY_SKIP("no system log on this platform");
    }
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
