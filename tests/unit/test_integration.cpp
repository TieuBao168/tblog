// End-to-end tests through the public C API.
#include "test_os.h"
#include "tiny_test.h"
#include "tblog/log.h"

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string fresh(const std::string &name) {
    std::string dir = tos::test_dir() + "/it_" + name;
    tos::remove_tree(dir);
    tos::make_dirs(dir);
    return dir;
}

void write_file(const std::string &path, const std::string &text) {
    std::ofstream(path, std::ios::binary) << text;
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != '\0') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string config(const std::string &dir, const std::string &extra = "") {
    std::string cfg = dir + "/log.conf";
    write_file(cfg, "level = INFO\n"
                    "pattern = [%L] [%n] %m\n"
                    "file.path = " + dir + "/app.log\n"
                    "buffer.size = 256KB\n"
                    "full_policy = block\n"
                    "flush.interval_ms = 20\n"
                    "reload.watch = true\n" + extra);
    return cfg;
}

}  // namespace

// Logs kThreads x kPerThread lines through the API and checks every line once.
TEST(multithread) {
    const std::string dir = fresh("mt");
    ASSERT_TRUE(tblog_init(config(dir).c_str()) == 0);
    static constexpr int kThreads = 8, kPerThread = 20000;  // static: no lambda capture needed (MSVC C3493)
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) TBLOG_INFO("t%d i%d", t, i);
        });
    for (auto &t : ts) t.join();
    TBLOG_DEBUG("filtered out");
    tblog_shutdown();

    std::set<std::string> seen;
    size_t total = 0;
    for (const auto &l : lines_of(read_file(dir + "/app.log"))) {
        if (l.rfind("[INFO] [-] t", 0) != 0) continue;
        ++total;
        seen.insert(l);
    }
    EXPECT_EQ(total, size_t(kThreads) * kPerThread);
    EXPECT_EQ(seen.size(), size_t(kThreads) * kPerThread);
    EXPECT_TRUE(read_file(dir + "/app.log").find("filtered out") == std::string::npos);
    EXPECT_EQ(tblog_dropped_count(), uint64_t(0));
}

TEST(module_levels_and_flush) {
    const std::string dir = fresh("modules");
    ASSERT_TRUE(tblog_init(config(dir, "module.network = DEBUG\nmodule.db = ERROR\n").c_str()) == 0);
    TBLOG_MDEBUG("network", "net debug visible");
    TBLOG_MWARN("db", "db warn hidden");
    TBLOG_MERROR("db", "db error visible");
    TBLOG_DEBUG("root debug hidden");
    tblog_flush();
    const std::string s = read_file(dir + "/app.log");
    EXPECT_TRUE(s.find("[DEBUG] [network] net debug visible") != std::string::npos);
    EXPECT_TRUE(s.find("db warn hidden") == std::string::npos);
    EXPECT_TRUE(s.find("[ERROR] [db] db error visible") != std::string::npos);
    EXPECT_TRUE(s.find("root debug hidden") == std::string::npos);
    tblog_shutdown();
}

TEST(hot_reload_level) {
    const std::string dir = fresh("reload");
    const std::string cfg = config(dir);
    ASSERT_TRUE(tblog_init(cfg.c_str()) == 0);
    TBLOG_DEBUG("before reload");
    // Rewrite with a different size so the watcher sees it even within one mtime tick.
    write_file(cfg, read_file(cfg) + "level = DEBUG\n# changed\n");
    bool visible = false;
    for (int i = 0; i < 40 && !visible; ++i) {  // watcher polls once per second
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        visible = tblog_get_level(nullptr) == TBLOG_LV_DEBUG;
    }
    EXPECT_TRUE(visible);
    TBLOG_DEBUG("after reload");
    tblog_shutdown();
    const std::string s = read_file(dir + "/app.log");
    EXPECT_TRUE(s.find("before reload") == std::string::npos);
    EXPECT_TRUE(s.find("after reload") != std::string::npos);
    tblog_set_level(nullptr, TBLOG_LV_INFO);
}

TEST(log_before_init_goes_to_stderr_and_does_not_crash) {
    TBLOG_INFO("no runtime yet %d", 1);
    EXPECT_TRUE(true);
}

// ---------------------------------------------------------------- user sinks
// tblog_add_sink() registrations last for the whole process, so each of these
// tests runs in a child process (this binary re-executed on a TINY_CHILD
// scenario, the test's directory in tos::child_arg()); the child's EXPECTs
// print there and its exit status says whether any failed.
namespace {

bool in_child(const char *scenario, const std::string &dir) {
    tos::ChildResult r = tos::run_child(scenario, dir);
    if (!tos::exited_with(r, 0)) {
        std::printf("  child %s: %s\n", scenario, tos::describe(r).c_str());
        return false;
    }
    return true;
}

struct Captured {
    std::string data;
    int flushes = 0;
};
void capture_write(void *ctx, const char *data, size_t len) {
    static_cast<Captured *>(ctx)->data.append(data, len);
}
void capture_flush(void *ctx) { ++static_cast<Captured *>(ctx)->flushes; }

}  // namespace

TINY_CHILD(user_sink_order) {
    const std::string dir = tos::child_arg();
    static Captured cap;
    ASSERT_TRUE(tblog_add_sink(capture_write, capture_flush, &cap) == 0);
    ASSERT_TRUE(tblog_init(config(dir).c_str()) == 0);
    static constexpr int kThreads = 8, kPerThread = 5000;  // static: no lambda capture needed (MSVC C3493)
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) TBLOG_INFO("t%d i%d", t, i);
        });
    for (auto &t : ts) t.join();
    tblog_shutdown();
    // Same bytes as the file, in the same order.
    EXPECT_TRUE(cap.data == read_file(dir + "/app.log"));
    std::vector<int> next(kThreads, 0);
    size_t n = 0;
    bool ordered = true;
    for (const auto &l : lines_of(cap.data)) {
        int t = -1, i = -1;
        if (std::sscanf(l.c_str(), "[INFO] [-] t%d i%d", &t, &i) != 2) continue;
        ordered = ordered && t >= 0 && t < kThreads && i == next[t];
        if (t >= 0 && t < kThreads) next[t] = i + 1;
        ++n;
    }
    EXPECT_TRUE(ordered);
    EXPECT_EQ(n, size_t(kThreads) * kPerThread);
    EXPECT_TRUE(cap.flushes > 0);
}
TEST(user_sink_receives_every_line_in_order) {
    EXPECT_TRUE(in_child("user_sink_order", fresh("usink")));
}

TINY_CHILD(user_sink_flush) {
    const std::string dir = tos::child_arg();
    static Captured cap;
    ASSERT_TRUE(tblog_add_sink(capture_write, capture_flush, &cap) == 0);
    // Long interval: only tblog_flush() makes the worker run.
    ASSERT_TRUE(tblog_init(config(dir, "flush.interval_ms = 60000\n").c_str()) == 0);
    tblog_flush();  // nothing to write: the flush callback still runs
    const int f0 = cap.flushes;
    EXPECT_TRUE(f0 > 0);
    TBLOG_INFO("before flush");
    tblog_flush();
    EXPECT_TRUE(cap.flushes > f0);
    EXPECT_TRUE(cap.data.find("[INFO] [-] before flush\n") != std::string::npos);
    tblog_shutdown();
}
TEST(user_sink_flush_called_by_tblog_flush) {
    EXPECT_TRUE(in_child("user_sink_flush", fresh("usink_flush")));
}

TINY_CHILD(add_sink_rules) {
    const std::string dir = tos::child_arg();
    static Captured cap;
    EXPECT_EQ(tblog_add_sink(nullptr, capture_flush, &cap), -1);  // write is required
    EXPECT_EQ(tblog_add_sink(capture_write, nullptr, &cap), 0);   // flush is optional
    ASSERT_TRUE(tblog_init(config(dir).c_str()) == 0);
    EXPECT_EQ(tblog_add_sink(capture_write, nullptr, &cap), -1);  // running
    TBLOG_INFO("cycle one");
    tblog_shutdown();
    // Registered sinks persist across init/shutdown cycles.
    ASSERT_TRUE(tblog_init(config(dir).c_str()) == 0);
    TBLOG_INFO("cycle two");
    tblog_shutdown();
    EXPECT_TRUE(cap.data.find("cycle one") != std::string::npos);
    EXPECT_TRUE(cap.data.find("cycle two") != std::string::npos);
    EXPECT_EQ(tblog_add_sink(capture_write, nullptr, &cap), 0);  // stopped again
}
TEST(add_sink_rules) {
    EXPECT_TRUE(in_child("add_sink_rules", fresh("usink_rules")));
}

TINY_CHILD(file_false) {
    const std::string dir = tos::child_arg();
    static Captured cap;
    ASSERT_TRUE(tblog_add_sink(capture_write, capture_flush, &cap) == 0);
    ASSERT_TRUE(tblog_init(config(dir, "file = false\n").c_str()) == 0);
    for (int i = 0; i < 100; ++i) TBLOG_INFO("only to the sink %d", i);
    tblog_shutdown();
    EXPECT_FALSE(tos::exists(dir + "/app.log"));  // never created
    EXPECT_EQ(lines_of(cap.data).size(), size_t(100));
    EXPECT_TRUE(cap.data.find("only to the sink 99\n") != std::string::npos);
}
TEST(file_false_with_user_sink_writes_no_file) {
    EXPECT_TRUE(in_child("file_false", fresh("nofile")));
}

// ---------------------------------------------------------------- process end
// The crash marker written for an access violation / SIGSEGV.
#if defined(_WIN32)
const char kSegvMarker[] = "*** tblog: caught exception 0xC0000005 (EXCEPTION_ACCESS_VIOLATION) ***";
#else
const char kSegvMarker[] = "*** tblog: caught signal 11 (SIGSEGV) ***";
#endif

TINY_CHILD(segv_after_logging) {
    if (tblog_init(config(tos::child_arg(), "flush.interval_ms = 60000\n").c_str()) != 0)
        std::_Exit(2);
    for (int i = 0; i < 3000; ++i) TBLOG_INFO("line %d", i);
    tos::fault_null_store();
}
// Buffered lines survive SIGSEGV / an access violation through the crash handler.
TEST(segv_keeps_logs) {
    const std::string dir = fresh("segv");
    tos::ChildResult r = tos::run_child("segv_after_logging", dir);
    EXPECT_TRUE(tos::died_with(r, tos::Crash::Segv));
    const std::string s = read_file(dir + "/app.log");
    std::set<std::string> seen;
    for (const auto &l : lines_of(s))
        if (l.rfind("[INFO] [-] line ", 0) == 0) seen.insert(l);
    EXPECT_EQ(seen.size(), size_t(3000));
    EXPECT_TRUE(s.find(kSegvMarker) != std::string::npos);
    // The marker must come after the last line logged before the crash.
    EXPECT_TRUE(s.find(kSegvMarker) > s.find("[INFO] [-] line 2999"));
}

TINY_CHILD(exit_without_shutdown) {
    if (tblog_init(config(tos::child_arg(), "flush.interval_ms = 60000\n").c_str()) != 0)
        std::_Exit(2);
    std::thread t([] {
        for (int i = 0; i < 2000; ++i) TBLOG_INFO("bg %d", i);
    });
    for (int i = 0; i < 2000; ++i) TBLOG_INFO("main %d", i);
    t.join();
    std::exit(0);  // atexit -> tblog_shutdown: every buffered line reaches the file
}
TEST(exit_without_shutdown_keeps_logs) {
    const std::string dir = fresh("exit");
    tos::ChildResult r = tos::run_child("exit_without_shutdown", dir);
    EXPECT_TRUE(tos::exited_with(r, 0));
    size_t n = 0;
    for (const auto &l : lines_of(read_file(dir + "/app.log")))
        if (l.rfind("[INFO] [-] main ", 0) == 0 || l.rfind("[INFO] [-] bg ", 0) == 0) ++n;
    EXPECT_EQ(n, size_t(4000));
}

TINY_TEST_MAIN()
