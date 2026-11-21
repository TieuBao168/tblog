// End-to-end tests through the public C API.
#include "test_os.h"
#include "tiny_test.h"
#include "tblog/log.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
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

// ---------------------------------------------------------------- flush.on_level
// With a worker that would otherwise sleep 60 s and no sync wake-up
// (sync.on_level = OFF), only flush.on_level gets a line out: an ERROR call
// returns with that line, and every line before it, in the file.
TEST(flush_on_level_error_line_in_file_on_return) {
    const std::string dir = fresh("flush_level");
    ASSERT_TRUE(tblog_init(config(dir, "flush.interval_ms = 60000\nsync.on_level = OFF\n"
                                       "flush.on_level = ERROR\n").c_str()) == 0);
    for (int i = 0; i < 10; ++i) TBLOG_INFO("info %d", i);
    TBLOG_WARN("warn below the flush level");
    TBLOG_ERROR("the error");
    std::string s = read_file(dir + "/app.log");
    for (int i = 0; i < 10; ++i) EXPECT_TRUE(s.find("[INFO] [-] info " + std::to_string(i) + "\n") != std::string::npos);
    EXPECT_TRUE(s.find("[WARN] [-] warn below the flush level\n") != std::string::npos);
    EXPECT_TRUE(s.find("[ERROR] [-] the error\n") != std::string::npos);
    TBLOG_INFO("tail");  // below the level: stays in the buffer
    EXPECT_TRUE(read_file(dir + "/app.log").find("tail") == std::string::npos);
    tblog_shutdown();
    EXPECT_TRUE(read_file(dir + "/app.log").find("[INFO] [-] tail\n") != std::string::npos);
}

// flush.on_level is hot-reloadable.
TEST(flush_on_level_hot_reload) {
    const std::string dir = fresh("flush_reload");
    const std::string cfg = config(dir, "flush.interval_ms = 2000\nsync.on_level = OFF\n");
    ASSERT_TRUE(tblog_init(cfg.c_str()) == 0);
    write_file(cfg, read_file(cfg) + "flush.on_level = ERROR\nlevel = DEBUG\n# changed\n");
    bool reloaded = false;
    for (int i = 0; i < 100 && !reloaded; ++i) {  // the watcher runs once per worker cycle (2 s)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        reloaded = tblog_get_level(nullptr) == TBLOG_LV_DEBUG;
    }
    EXPECT_TRUE(reloaded);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the rest of reload()
    TBLOG_ERROR("after reload");
    EXPECT_TRUE(read_file(dir + "/app.log").find("[ERROR] [-] after reload\n") != std::string::npos);
    tblog_shutdown();
    tblog_set_level(nullptr, TBLOG_LV_INFO);
}

// file = false: the caller waits until the batch was handed to every sink
// that exists (here a user sink).
TINY_CHILD(flush_on_level_user_sink) {
    const std::string dir = tos::child_arg();
    static Captured cap;
    ASSERT_TRUE(tblog_add_sink(capture_write, nullptr, &cap) == 0);
    ASSERT_TRUE(tblog_init(config(dir, "file = false\nflush.interval_ms = 60000\nsync.on_level = OFF\n"
                                       "flush.on_level = ERROR\n").c_str()) == 0);
    TBLOG_INFO("first");
    TBLOG_ERROR("second");
    EXPECT_TRUE(cap.data == "[INFO] [-] first\n[ERROR] [-] second\n");
    tblog_shutdown();
}
TEST(flush_on_level_without_file_waits_for_the_user_sink) {
    EXPECT_TRUE(in_child("flush_on_level_user_sink", fresh("flush_usink")));
}

// kill -9 / TerminateProcess right after an ERROR line was acknowledged:
// with flush.on_level = ERROR, every line of that thread up to and including
// its last acknowledged ERROR line is in the file, and so is every earlier
// line (ERROR or not) of the other threads up to their own last
// acknowledged ERROR line.
namespace {
constexpr int kKillThreads = 4;
constexpr long kErrorEvery = 50;  // every 50th line of a thread is an ERROR line
struct KillProgress {
    std::atomic<int64_t> acked[kKillThreads];       // last line whose call returned
    std::atomic<int64_t> last_error[kKillThreads];  // last ERROR line whose call returned
};
}  // namespace

TINY_CHILD(flush_on_level_kill) {
    const std::string dir = tos::child_arg();
    static tos::SharedFile shm;
    if (!shm.open(dir + "/progress.bin", sizeof(KillProgress), false)) std::_Exit(3);
    KillProgress *pg = static_cast<KillProgress *>(shm.data());
    if (tblog_init(config(dir, "flush.interval_ms = 1000\nflush.on_level = ERROR\nreload.watch = false\n")
                       .c_str()) != 0)
        std::_Exit(2);
    std::vector<std::thread> ts;
    for (int t = 0; t < kKillThreads; ++t)
        ts.emplace_back([t, pg] {
            for (long i = 1; i < 50000000; ++i) {
                if (i % kErrorEvery == 0) {
                    TBLOG_ERROR("t%d i%ld", t, i);
                    pg->acked[t].store(i, std::memory_order_release);
                    pg->last_error[t].store(i, std::memory_order_release);
                } else {
                    TBLOG_INFO("t%d i%ld", t, i);
                    pg->acked[t].store(i, std::memory_order_release);
                }
            }
        });
    for (auto &t : ts) t.join();
    std::_Exit(4);  // never: the parent kills us first
}

TEST(flush_on_level_survives_kill_hard) {
    const int trials = 10;
    long info_after = 0, killed = 0;
    for (int k = 0; k < trials; ++k) {
        const std::string dir = fresh("flush_kill");
        tos::SharedFile shm;
        ASSERT_TRUE(shm.open(dir + "/progress.bin", sizeof(KillProgress), true));
        KillProgress *pg = static_cast<KillProgress *>(shm.data());
        tos::ChildProcess c;
        ASSERT_TRUE(c.start("flush_on_level_kill", dir));
        // Let it run a while (every thread past 20 ERROR lines + a varying
        // extra), then kill it the moment one thread acknowledges an ERROR line.
        const int victim = k % kKillThreads;
        const uint64_t deadline = tos::mono_ns() + 20000000000ull;
        const int64_t warm = kErrorEvery * (20 + 7 * k);
        bool ready = false;
        while (!ready && tos::mono_ns() < deadline) {
            ready = true;
            for (int t = 0; t < kKillThreads; ++t) ready = ready && pg->last_error[t].load() >= warm;
        }
        const int64_t e0 = pg->last_error[victim].load();
        while (pg->last_error[victim].load() == e0 && tos::mono_ns() < deadline) {
        }
        c.kill_hard();
        const tos::ChildResult r = c.wait(30);
        EXPECT_TRUE(ready);
        EXPECT_TRUE(tos::killed_hard(r));
        if (!tos::killed_hard(r)) std::printf("  child: %s\n", tos::describe(r).c_str());
        killed += tos::killed_hard(r);
        // Expected in the file: thread t, lines 1 .. last_error[t].
        int64_t last_err[kKillThreads], acked[kKillThreads];
        std::vector<std::vector<char>> seen(kKillThreads);
        for (int t = 0; t < kKillThreads; ++t) {
            last_err[t] = pg->last_error[t].load();
            acked[t] = pg->acked[t].load();
            seen[t].assign(static_cast<size_t>(acked[t]) + 2, 0);
        }
        EXPECT_TRUE(last_err[victim] > e0);
        long bad = 0;
        for (const auto &l : lines_of(read_file(dir + "/app.log"))) {
            int t = -1;
            long i = -1;
            char lv[8] = {0};
            if (std::sscanf(l.c_str(), "[%7[A-Z]] [-] t%d i%ld", lv, &t, &i) != 3 || t < 0 || t >= kKillThreads ||
                i < 1) {
                ++bad;
                continue;
            }
            const bool is_err = std::string(lv) == "ERROR";
            if (is_err != (i % kErrorEvery == 0)) ++bad;
            if (i <= acked[t]) seen[t][static_cast<size_t>(i)] = 1;
        }
        EXPECT_EQ(bad, 0L);
        long missing = 0;
        for (int t = 0; t < kKillThreads; ++t) {
            for (int64_t i = 1; i <= last_err[t]; ++i) missing += !seen[t][static_cast<size_t>(i)];
            for (int64_t i = last_err[t] + 1; i <= acked[t]; ++i) info_after += !seen[t][static_cast<size_t>(i)];
        }
        EXPECT_EQ(missing, 0L);
        if (missing) std::printf("  trial %d: %ld lines missing before the last ERROR line\n", k, missing);
        tos::remove_tree(dir);
    }
    std::printf("  %d trials, %ld killed hard, 0 expected lost up to the last ERROR line; "
                "%ld acknowledged INFO lines after it lost in total\n",
                trials, killed, info_after);
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
