// Linked against the SHARED library (tblog::tblog_shared), public API only.
//
// A process that ends without calling tblog_shutdown() still gets every line
// into the log file: tblog_init registered tblog_shutdown with atexit(). On
// Windows that handler belongs to tblog.dll and runs from DLL_PROCESS_DETACH
// (loader lock held, the worker thread already killed by ExitProcess), so it
// must drain the queue on the exiting thread without joining anything -- a
// join would hang the process forever. Also checks that the DLL's crash
// handler keeps the buffered lines on an access violation / SIGSEGV.
#include "test_os.h"
#include "tiny_test.h"
#include "tblog/log.h"

#include <string>
#include <thread>
#include <vector>

namespace {

std::string fresh(const std::string &name) {
    std::string dir = tos::test_dir() + "/shared_" + name;
    tos::remove_tree(dir);
    tos::make_dirs(dir);
    return dir;
}

std::string config(const std::string &dir) {
    std::string cfg = dir + "/log.conf";
    // Long flush interval: the lines are still buffered when the process ends.
    tos::write_file(cfg, "pattern = [%L] %m\n"
                         "file.path = " + dir + "/app.log\n"
                         "flush.interval_ms = 60000\n"
                         "reload.watch = false\n");
    return cfg;
}

size_t count_lines(const std::string &s, const std::string &prefix) {
    size_t n = 0, pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        if (s.compare(pos, prefix.size(), prefix) == 0) ++n;
        pos = nl + 1;
    }
    return n;
}

}  // namespace

TINY_CHILD(return_without_shutdown) {
    if (tblog_init(config(tos::child_arg()).c_str()) != 0) std::_Exit(2);
    std::thread t([] {
        for (int i = 0; i < 3000; ++i) TBLOG_INFO("bg %d", i);
    });
    for (int i = 0; i < 3000; ++i) TBLOG_INFO("main %d", i);
    t.join();
    std::exit(0);  // like returning from main: atexit handlers, then process exit
}
TEST(exit_without_shutdown_keeps_every_line) {
    const std::string dir = fresh("exit");
    const uint64_t t0 = tos::mono_ns();
    tos::ChildResult r = tos::run_child("return_without_shutdown", dir, std::string(), 60);
    const double secs = double(tos::mono_ns() - t0) / 1e9;
    std::printf("  child ended in %.2f s: %s\n", secs, tos::describe(r).c_str());
    EXPECT_TRUE(tos::exited_with(r, 0));
    EXPECT_TRUE(secs < 30);  // no hang at exit
    const std::string s = tos::read_file(dir + "/app.log");
    EXPECT_EQ(count_lines(s, "[INFO] main "), size_t(3000));
    EXPECT_EQ(count_lines(s, "[INFO] bg "), size_t(3000));
}

TINY_CHILD(shutdown_then_exit) {
    if (tblog_init(config(tos::child_arg()).c_str()) != 0) std::_Exit(2);
    for (int i = 0; i < 1000; ++i) TBLOG_INFO("line %d", i);
    tblog_shutdown();  // explicit: normal join path
    TBLOG_INFO("after shutdown goes to stderr");
    std::exit(0);
}
TEST(explicit_shutdown_then_exit) {
    const std::string dir = fresh("shutdown");
    tos::ChildResult r = tos::run_child("shutdown_then_exit", dir, dir + "/err.txt", 60);
    EXPECT_TRUE(tos::exited_with(r, 0));
    EXPECT_EQ(count_lines(tos::read_file(dir + "/app.log"), "[INFO] line "), size_t(1000));
    EXPECT_TRUE(r.err.find("after shutdown goes to stderr") != std::string::npos);
}

TINY_CHILD(crash_after_logging) {
    if (tblog_init(config(tos::child_arg()).c_str()) != 0) std::_Exit(2);
    for (int i = 0; i < 2000; ++i) TBLOG_INFO("line %d", i);
    volatile int *volatile p = nullptr;
    *p = 1;
}
TEST(crash_keeps_lines) {
    const std::string dir = fresh("crash");
    tos::ChildResult r = tos::run_child("crash_after_logging", dir, dir + "/err.txt", 60);
    EXPECT_TRUE(tos::died_with(r, tos::Crash::Segv));
    const std::string s = tos::read_file(dir + "/app.log");
    EXPECT_EQ(count_lines(s, "[INFO] line "), size_t(2000));
    EXPECT_TRUE(s.find("*** tblog: caught") != std::string::npos);
}

TINY_TEST_MAIN()
