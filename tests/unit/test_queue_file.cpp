// Tests for the queue and the file output: RotatingFile, Queue (sharded double
// buffers: ordering, drop / block, urgent wake-up, take / release, crash
// drain) and the Pipeline that feeds the FileSink and other sinks from it.
#include "logger/pipeline.h"
#include "pipeline_util.h"
#include "queue/queue.h"
#include "sinks/file_sink.h"
#include "sinks/rotating_file.h"
#include "sinks/sink.h"
#include "test_os.h"
#include "tiny_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace tblog;

// The few std::filesystem-like calls used here (std::filesystem is C++17;
// these tests also build as C++11/14).
namespace fs {
inline bool exists(const std::string &p) { return tos::exists(p); }
inline unsigned long long file_size(const std::string &p) { return tos::file_size(p); }
inline void create_directories(const std::string &p) { tos::make_dirs(p); }
}  // namespace fs

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
static const int kScale = 10;  // sanitizers are slow: shrink the big tests
#else
static const int kScale = 1;
#endif

// ---------------------------------------------------------------- helpers
static std::string base_dir() { return tos::test_dir(); }

struct TempDir {
    std::string path;
    explicit TempDir(const char *name) {
        path = base_dir() + "/queue_file_" + std::to_string(tos::pid()) + "_" + name;
        tos::remove_tree(path);
    }
    ~TempDir() {
        tos::remove_tree(path);
        tos::remove_empty_dir(base_dir());  // only if empty
    }
    std::string file(const char *f) const { return path + "/" + f; }
};

static std::string read_file(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

// Fixed 100-byte line "L0007 xxxx...\n".
static std::string fixed_line(int i) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "L%04d ", i);
    std::string s(buf);
    s.append(99 - s.size(), 'x');
    s.push_back('\n');
    return s;
}

// Self-checking line: "T03 S00001234 <payload of (seq%40) copies of one char>\n".
static int make_line(char *out, int t, int seq) {
    int n = std::snprintf(out, 64, "T%02d S%08d ", t, seq);
    int plen = seq % 40;
    char c = static_cast<char>('a' + (t + seq) % 26);
    for (int i = 0; i < plen; ++i) out[n++] = c;
    out[n++] = '\n';
    return n;
}

static bool parse_line(const std::string &l, int *t, int *seq) {
    if (l.size() < 13 || l[0] != 'T' || l[3] != ' ' || l[4] != 'S' || l[13] != ' ')
        return false;
    for (int i : {1, 2}) if (l[i] < '0' || l[i] > '9') return false;
    for (int i = 5; i < 13; ++i) if (l[i] < '0' || l[i] > '9') return false;
    *t = std::atoi(l.substr(1, 2).c_str());
    *seq = std::atoi(l.substr(5, 8).c_str());
    std::string payload = l.substr(14);
    if (static_cast<int>(payload.size()) != *seq % 40) return false;
    char c = static_cast<char>('a' + (*t + *seq) % 26);
    return std::all_of(payload.begin(), payload.end(), [c](char x) { return x == c; });
}

// Worker loop as the logger would run it.
struct Worker {
    Pipeline *b;
    std::atomic<bool> stop{false};
    std::thread th;
    explicit Worker(Pipeline *be, int delay_ms = 0) : b(be) {
        th = std::thread([this, delay_ms] {
            if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            while (!stop.load()) {
                b->queue().wait_for_work(std::chrono::milliseconds(20));
                b->drain();
                if (b->queue().consume_urgent()) b->sync();
            }
        });
    }
    void join() {
        stop = true;
        b->queue().wake();
        th.join();
    }
};

// Verify per-thread sequences in file content. Returns number of good lines;
// counts "tblog: N lines dropped" into *drop_reported.
static long verify_lines(const std::string &content, int threads, int per_thread,
                         bool expect_all, uint64_t *drop_reported = nullptr) {
    std::vector<int> last(threads, -1);
    std::vector<long> count(threads, 0);
    long good = 0, bad = 0, order = 0;
    uint64_t reported = 0;
    for (const std::string &l : split_lines(content)) {
        unsigned long long n;
        if (std::sscanf(l.c_str(), "tblog: %llu lines dropped", &n) == 1) {
            reported += n;
            continue;
        }
        int t, s;
        if (!parse_line(l, &t, &s) || t >= threads || s >= per_thread) { ++bad; continue; }
        if (s <= last[t] || (expect_all && s != last[t] + 1)) ++order;
        last[t] = s;
        ++count[t];
        ++good;
    }
    EXPECT_EQ(bad, 0L);
    EXPECT_EQ(order, 0L);
    if (expect_all)
        for (int t = 0; t < threads; ++t) EXPECT_EQ(count[t], static_cast<long>(per_thread));
    if (drop_reported) *drop_reported = reported;
    return good;
}

// ---------------------------------------------------------------- RotatingFile
TEST(rotating_names) {
    TempDir d("names");
    RotatingFile rf;
    FileOptions o;
    o.path = d.file("sub/dir/app.log");
    ASSERT_TRUE(rf.open(o));  // creates sub/dir
    EXPECT_TRUE(fs::exists(o.path));
    EXPECT_EQ(rf.rotated_name(0), o.path);
    EXPECT_EQ(rf.rotated_name(1), d.file("sub/dir/app.1.log"));
    EXPECT_EQ(rf.rotated_name(4), d.file("sub/dir/app.4.log"));
    EXPECT_TRUE(rf.is_open());

    RotatingFile r2;
    o.path = d.file("noext");
    ASSERT_TRUE(r2.open(o));
    EXPECT_EQ(r2.rotated_name(2), d.file("noext.2"));
    RotatingFile r3;
    o.path = d.file("a.b/.hidden");
    ASSERT_TRUE(r3.open(o));
    EXPECT_EQ(r3.rotated_name(1), d.file("a.b/.hidden.1"));
}

TEST(rotating_rotation_count) {
    TempDir d("rot");
    RotatingFile rf;
    FileOptions o;
    o.path = d.file("app.log");
    o.max_size = 1000;  // exactly 10 fixed lines
    o.max_files = 3;
    ASSERT_TRUE(rf.open(o));
    for (int i = 1; i <= 50; ++i) {
        std::string l = fixed_line(i);
        rf.write_all(l.data(), l.size());
    }
    rf.close();
    std::string expect0, expect1, expect2;
    for (int i = 41; i <= 50; ++i) expect0 += fixed_line(i);
    for (int i = 31; i <= 40; ++i) expect1 += fixed_line(i);
    for (int i = 21; i <= 30; ++i) expect2 += fixed_line(i);
    EXPECT_TRUE(read_file(d.file("app.log")) == expect0);
    EXPECT_TRUE(read_file(d.file("app.1.log")) == expect1);
    EXPECT_TRUE(read_file(d.file("app.2.log")) == expect2);
    EXPECT_FALSE(fs::exists(d.file("app.3.log")));
    EXPECT_EQ(rf.lost_bytes(), 0ull);
}

TEST(rotating_splits_big_chunk_at_newline) {
    TempDir d("split");
    RotatingFile rf;
    FileOptions o;
    o.path = d.file("app.log");
    o.max_size = 1050;  // 10.5 lines: must split at a line boundary
    o.max_files = 4;
    ASSERT_TRUE(rf.open(o));
    std::string chunk;
    for (int i = 0; i < 25; ++i) chunk += fixed_line(i);
    rf.write_all(chunk.data(), chunk.size());
    rf.close();
    std::string all = read_file(d.file("app.2.log")) + read_file(d.file("app.1.log")) +
                      read_file(d.file("app.log"));
    EXPECT_TRUE(all == chunk);
    EXPECT_EQ(fs::file_size(d.file("app.2.log")), 1000u);
    EXPECT_EQ(fs::file_size(d.file("app.1.log")), 1000u);
    EXPECT_EQ(fs::file_size(d.file("app.log")), 500u);
}

TEST(rotating_appends_existing_and_tracks_size) {
    TempDir d("append");
    FileOptions o;
    o.path = d.file("app.log");
    {
        RotatingFile rf;
        ASSERT_TRUE(rf.open(o));
        rf.write_all("hello\n", 6);
    }
    RotatingFile rf;
    ASSERT_TRUE(rf.open(o));
    EXPECT_EQ(rf.size(), 6ull);
    rf.write_all("world\n", 6);
    EXPECT_EQ(rf.size(), 12ull);
    EXPECT_EQ(read_file(o.path), std::string("hello\nworld\n"));
}

TEST(rotating_reopen_after_external_delete) {
    TempDir d("reopen");
    RotatingFile rf;
    FileOptions o;
    o.path = d.file("app.log");
    ASSERT_TRUE(rf.open(o));
    rf.write_all("one\n", 4);
    tos::remove_file(o.path);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    rf.write_all("two\n", 4);
    EXPECT_EQ(read_file(o.path), std::string("two\n"));
    // External rename (logrotate style).
    tos::rename_replace(o.path, d.file("moved.log"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    rf.write_all("three\n", 6);
    EXPECT_EQ(read_file(o.path), std::string("three\n"));
    EXPECT_EQ(read_file(d.file("moved.log")), std::string("two\n"));
}

TEST(rotating_open_failure) {
    TempDir d("fail");
    fs::create_directories(d.path);
    std::ofstream(d.file("blocker")) << "x";
    RotatingFile rf;
    FileOptions o;
    o.path = d.file("blocker/app.log");  // parent is a regular file
    std::fprintf(stderr, "(expected error follows)\n");
    EXPECT_FALSE(rf.open(o));
    EXPECT_TRUE(make_file_sink(o) == nullptr);
    PipeOptions po;
    po.file = o;
    EXPECT_TRUE(make_pipe(po) == nullptr);
}

TEST(rotating_write_error_counts_loss) {
#if defined(_WIN32)
    TINY_SKIP("needs /dev/full (no Windows equivalent)");
#else
    if (access("/dev/full", W_OK) != 0) TINY_SKIP("needs /dev/full (Linux; not on macOS)");
#endif
    RotatingFile rf;
    FileOptions o;
    o.path = "/dev/full";
    ASSERT_TRUE(rf.open(o));
    std::fprintf(stderr, "(expected ENOSPC warning follows, once)\n");
    for (int i = 0; i < 100; ++i) rf.write_all("0123456789\n", 11);
    EXPECT_EQ(rf.lost_bytes(), 1100ull);
}

// ---------------------------------------------------------------- double buffer
static void run_mt_test(FullPolicy policy, size_t bufsize, int T, int N, bool expect_all) {
    TempDir d(policy == FullPolicy::Block ? "mt_block" : "mt_drop");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.file.max_size = 1ull << 40;
    o.buffer_size = bufsize;
    o.policy = policy;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    Worker w(b.get());
    std::atomic<long> ok{0};
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            long mine = 0;
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                if (b->queue().append(line, n, TBLOG_LV_INFO)) ++mine;
            }
            ok += mine;
        });
    for (auto &x : th) x.join();
    w.join();
    b->shutdown();
    uint64_t reported = 0;
    long good = verify_lines(read_file(o.file.path), T, N, expect_all, &reported);
    EXPECT_EQ(good, ok.load());
    EXPECT_EQ(static_cast<uint64_t>(good) + b->queue().dropped(), static_cast<uint64_t>(T) * N);
    EXPECT_EQ(reported, b->queue().dropped());
    if (expect_all) EXPECT_EQ(b->queue().dropped(), 0ull);
    std::printf("  policy=%s buf=%zu: %ld lines written, %llu dropped\n",
                policy == FullPolicy::Block ? "block" : "drop", bufsize, good,
                static_cast<unsigned long long>(b->queue().dropped()));
}

TEST(queue_8_threads_all_lines_intact_block) {
    run_mt_test(FullPolicy::Block, 256 << 10, 8, 50000 / kScale, true);
}

TEST(queue_8_threads_drop_accounting) {
    // Small buffer so drops are likely; every surviving line must be intact and
    // survivors + dropped == total, and the "N lines dropped" notes must add up.
    run_mt_test(FullPolicy::Drop, 16 << 10, 8, 50000 / kScale, false);
}

TEST(queue_drop_policy_stalled_worker) {
    TempDir d("drop");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 4096;
    o.policy = FullPolicy::Drop;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    // Sharded: a single thread always uses one shard, whose capacity is
    // max(64 KB, 4096 / N) = 64 KB = 1024 lines of 64 bytes.
    std::string line(63, 'z');
    line.push_back('\n');
    int ok = 0;
    for (int i = 0; i < 3000; ++i) ok += b->queue().append(line.data(), 64, TBLOG_LV_INFO);
    EXPECT_EQ(ok, 1024);
    EXPECT_EQ(b->queue().dropped(), 1976ull);
    // Too long to ever fit -> dropped even under Block.
    std::string huge(70000, 'h');
    EXPECT_FALSE(b->queue().append(huge.data(), static_cast<uint32_t>(huge.size()), TBLOG_LV_INFO));
    EXPECT_EQ(b->queue().dropped(), 1977ull);
    b->drain();
    std::string expect;
    for (int i = 0; i < 1024; ++i) expect += line;
    expect += "tblog: 1977 lines dropped\n";
    EXPECT_TRUE(read_file(o.file.path) == expect);
    b->drain();  // nothing new: no second drop note
    EXPECT_TRUE(read_file(o.file.path) == expect);
    EXPECT_TRUE(b->queue().append(line.data(), 64, TBLOG_LV_INFO));
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == expect + line);
}

TEST(queue_block_policy_loses_nothing) {
    TempDir d("block");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 4096;
    o.policy = FullPolicy::Block;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    Worker w(b.get(), 200);  // stalled at first: producers must block
    const int T = 4, N = 20000 / kScale;
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->queue().append(line, n, TBLOG_LV_INFO));
            }
        });
    for (auto &x : th) x.join();
    w.join();
    b->shutdown();
    EXPECT_EQ(b->queue().dropped(), 0ull);
    EXPECT_EQ(verify_lines(read_file(o.file.path), T, N, true), static_cast<long>(T * N));
}

TEST(queue_block_released_by_shutdown) {
    TempDir d("blockshut");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 128;
    o.policy = FullPolicy::Block;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    std::string line(63, 'q');
    line.push_back('\n');
    // One thread fills its shard (64 KB = 1024 lines), then blocks on the next.
    std::atomic<int> filled{0};
    std::atomic<int> res{-1};
    std::thread t([&] {
        int n = 0;
        for (int i = 0; i < 1024; ++i) n += b->queue().append(line.data(), 64, TBLOG_LV_INFO);
        filled = n;
        res = b->queue().append(line.data(), 64, TBLOG_LV_INFO);  // blocks
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(filled.load(), 1024);
    EXPECT_EQ(res.load(), -1);
    b->shutdown();
    t.join();
    EXPECT_EQ(res.load(), 0);
    EXPECT_EQ(fs::file_size(o.file.path), 65536u);
}

TEST(queue_urgent_and_notify) {
    TempDir d("urgent");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 1000;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    using clk = std::chrono::steady_clock;
    std::string line(99, 'u');
    line.push_back('\n');

    // Below 75%: no notification, wait_for_work times out.
    EXPECT_TRUE(b->queue().append(line.data(), 100, TBLOG_LV_INFO));
    auto t0 = clk::now();
    b->queue().wait_for_work(std::chrono::milliseconds(100));
    EXPECT_TRUE(clk::now() - t0 >= std::chrono::milliseconds(90));
    EXPECT_FALSE(b->queue().consume_urgent());

    // Urgent line: flag + immediate wake.
    EXPECT_TRUE(b->queue().append(line.data(), 100, TBLOG_LV_ERROR));
    t0 = clk::now();
    b->queue().wait_for_work(std::chrono::seconds(5));
    EXPECT_TRUE(clk::now() - t0 < std::chrono::seconds(1));
    EXPECT_TRUE(b->queue().consume_urgent());
    EXPECT_FALSE(b->queue().consume_urgent());
    b->drain();

    // >= 75% of the (64 KB) shard: wake without urgency.
    for (int i = 0; i < 500; ++i) EXPECT_TRUE(b->queue().append(line.data(), 100, TBLOG_LV_DEBUG));
    t0 = clk::now();
    b->queue().wait_for_work(std::chrono::seconds(5));
    EXPECT_TRUE(clk::now() - t0 < std::chrono::seconds(1));
    EXPECT_FALSE(b->queue().consume_urgent());

    // Notify from another thread while the worker sleeps.
    b->drain();
    std::thread p([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        b->queue().append(line.data(), 100, TBLOG_LV_FATAL);
    });
    t0 = clk::now();
    b->queue().wait_for_work(std::chrono::seconds(5));
    EXPECT_TRUE(clk::now() - t0 < std::chrono::seconds(1));
    p.join();
    b->shutdown();
    EXPECT_EQ(fs::file_size(o.file.path), 50300u);
}

TEST(queue_rotation) {
    TempDir d("queue_rot");
    PipeOptions o;
    o.file.path = d.file("rot.log");
    o.file.max_size = 1000;
    o.file.max_files = 2;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    for (int i = 0; i < 25; ++i) {
        std::string l = fixed_line(i);
        EXPECT_TRUE(b->queue().append(l.data(), static_cast<uint32_t>(l.size()), TBLOG_LV_INFO));
    }
    b->shutdown();
    EXPECT_EQ(fs::file_size(d.file("rot.log")), 500u);
    EXPECT_EQ(fs::file_size(d.file("rot.1.log")), 1000u);
    EXPECT_FALSE(fs::exists(d.file("rot.2.log")));
    EXPECT_EQ(read_file(d.file("rot.log")).substr(0, 5), std::string("L0020"));
}

TEST(queue_shutdown_idempotent) {
    TempDir d("shutdown");
    PipeOptions o;
    o.file.path = d.file("app.log");
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    EXPECT_TRUE(b->queue().append("last words\n", 11, TBLOG_LV_INFO));
    b->shutdown();
    b->shutdown();
    EXPECT_FALSE(b->queue().append("too late\n", 9, TBLOG_LV_INFO));
    b->drain();
    b->drain(true);
    b->sync();
    b->queue().wake();
    b->queue().wait_for_work(std::chrono::milliseconds(1));
    b->queue().close();
    b->crash("x\n", 2);  // file closed: no-op
    b->crash(nullptr, 0);
    b.reset();  // destructor after shutdown
    EXPECT_EQ(read_file(o.file.path), std::string("last words\n"));
}

// What a CaptureSink saw. Owned by the test (the pipeline owns the sink).
struct Capture {
    std::string data;
    std::string events;  // B = begin_batch, W = write, E = end_batch, F = flush, S = shutdown
    int flushes = 0;
    Pipeline *crash_from_write = nullptr;  // "signal" during the first write
};

struct CaptureSink : LogSink {
    explicit CaptureSink(Capture *c) : c_(c) {}
    void write(const char *d, size_t n) override {
        c_->data.append(d, n);
        c_->events += 'W';
        if (Pipeline *p = c_->crash_from_write) {
            c_->crash_from_write = nullptr;
            p->crash(nullptr, 0);
        }
    }
    void begin_batch() override { c_->events += 'B'; }
    void end_batch() override { c_->events += 'E'; }
    void flush() override {
        ++c_->flushes;
        c_->events += 'F';
    }
    void shutdown() override { c_->events += 'S'; }
    Capture *c_;
};

static std::unique_ptr<LogSink> capture_sink(Capture *c) {
    return std::unique_ptr<LogSink>(new CaptureSink(c));
}

TEST(pipeline_sinks_receive_everything) {
    TempDir d("sinks");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 4096;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    Capture s1, s2;
    b->add_sink(capture_sink(&s1));
    b->add_sink(capture_sink(&s2));
    b->add_sink(nullptr);  // ignored (e.g. syslog unsupported)
    EXPECT_EQ(b->sink_count(), size_t(3));
    char line[128];
    for (int i = 0; i < 500; ++i) {
        int n = make_line(line, 0, i);
        b->queue().append(line, n, TBLOG_LV_INFO);
        if (i % 50 == 0) b->drain();
    }
    b->shutdown();
    std::string f = read_file(o.file.path);
    EXPECT_TRUE(s1.data == f);
    EXPECT_TRUE(s2.data == f);
    EXPECT_TRUE(s1.flushes > 0);
    EXPECT_EQ(s1.events.back(), 'S');
}

TEST(pipeline_batch_hooks_order) {
    TempDir d("hooks");
    PipeOptions o;
    o.file.path = d.file("app.log");
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    Capture c;
    b->add_sink(capture_sink(&c));
    b->drain();  // nothing taken: hooks run (time rotation check), no flush
    EXPECT_EQ(c.events, std::string("BE"));
    c.events.clear();
    b->drain(true);  // tblog_flush: flush even when nothing was written
    EXPECT_EQ(c.events, std::string("BEF"));
    c.events.clear();
    EXPECT_TRUE(b->queue().append("one\n", 4, TBLOG_LV_INFO));
    b->drain();
    EXPECT_EQ(c.events, std::string("BWEF"));
    c.events.clear();
    b->shutdown();  // final drain (flush forced) + LogSink::shutdown
    EXPECT_EQ(c.events, std::string("BEFS"));
    EXPECT_EQ(c.data, std::string("one\n"));
    EXPECT_EQ(read_file(o.file.path), std::string("one\n"));
}

TEST(pipeline_crash_writes_buffered_then_marker) {
    TempDir d("crash");
    PipeOptions o;
    o.file.path = d.file("app.log");
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    char line[128];
    std::string expect;
    for (int i = 0; i < 100; ++i) {
        int n = make_line(line, 1, i);
        b->queue().append(line, n, TBLOG_LV_INFO);
        expect.append(line, n);
    }
    b->drain();  // first 100 already on disk
    for (int i = 100; i < 200; ++i) {
        int n = make_line(line, 1, i);
        b->queue().append(line, n, TBLOG_LV_INFO);
        expect.append(line, n);
    }
    EXPECT_TRUE(read_file(o.file.path).size() < expect.size());
    EXPECT_FALSE(b->queue().crashed());
    const char m[] = "*** tblog: caught signal 11 (SIGSEGV) ***\n";
    b->crash(m, sizeof m - 1);
    EXPECT_TRUE(b->queue().crashed());
    std::string got = read_file(o.file.path);
    EXPECT_TRUE(got == expect + m);  // drained lines first, then the marker
    // After the crash an append is written at once, synchronously, after the
    // marker; the worker writes nothing more (no duplicates at shutdown); a
    // second crash() drains nothing (only its marker; none here).
    EXPECT_TRUE(b->queue().append("after\n", 6, TBLOG_LV_INFO));
    EXPECT_TRUE(read_file(o.file.path) == got + "after\n");
    b->crash(nullptr, 0);
    b->drain(true);
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == got + "after\n");
}

TEST(pipeline_crash_during_flush_no_duplicates) {
    TempDir d("crash2");
    PipeOptions o;
    o.file.path = d.file("app.log");
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    Capture s;
    s.crash_from_write = b.get();  // "signal" arrives while the worker is mid-flush
    b->add_sink(capture_sink(&s));  // not crash-safe: written after the file
    char line[128];
    std::string expect;
    for (int i = 0; i < 50; ++i) {
        int n = make_line(line, 2, i);
        b->queue().append(line, n, TBLOG_LV_INFO);
        expect.append(line, n);
    }
    b->drain();
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == expect);
    EXPECT_TRUE(s.data == expect);
}

// Queue alone: take() hands over whole lines in per-thread order, release()
// recycles the buffers, mark_written() / crash_drain() agree on what is left.
struct Visited {
    std::string data;
    int calls = 0;
};
static void visit_into(void *ctx, const char *data, size_t len) {
    Visited *v = static_cast<Visited *>(ctx);
    v->data.append(data, len);
    ++v->calls;
}

TEST(queue_take_release_chunks) {
    QueueOptions qo;
    auto q = Queue::create(qo);
    ASSERT_TRUE(q != nullptr);
    Queue::Batch b;
    EXPECT_EQ(q->take(b), size_t(0));
    // 4 threads x 2000 lines; take() repeatedly while they run.
    const int T = 4, N = 2000;
    std::atomic<int> done{0};
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(q->append(line, n, TBLOG_LV_INFO));
            }
            done.fetch_add(1);
        });
    std::string all;
    size_t chunks = 0;
    bool whole = true, shard_order = true;
    for (;;) {
        const bool last = done.load() == T;  // read before take: nothing after
        q->take(b);
        int prev_shard = -1;
        for (size_t i = 0; i < b.size; ++i) {
            const Queue::Chunk &c = b.chunks[i];
            whole = whole && c.len > 0 && c.data[c.len - 1] == '\n';
            shard_order = shard_order && static_cast<int>(b.shard[i]) > prev_shard;
            prev_shard = b.shard[i];
            all.append(c.data, c.len);
            q->mark_written(b, i);
            ++chunks;
        }
        q->release(b);
        EXPECT_EQ(b.size, size_t(0));
        if (last) break;
        std::this_thread::yield();
    }
    for (auto &x : th) x.join();
    EXPECT_TRUE(whole);
    EXPECT_TRUE(shard_order);
    EXPECT_TRUE(chunks > 0);
    EXPECT_EQ(verify_lines(all, T, N, true), static_cast<long>(T) * N);
    EXPECT_EQ(q->take(b), size_t(0));
    EXPECT_EQ(q->dropped(), 0ull);
}

TEST(queue_crash_drain_pending_then_active) {
    QueueOptions qo;
    auto q = Queue::create(qo);
    ASSERT_TRUE(q != nullptr);
    EXPECT_TRUE(q->append("A1\n", 3, TBLOG_LV_INFO));
    EXPECT_TRUE(q->append("A2\n", 3, TBLOG_LV_INFO));
    Queue::Batch b;
    EXPECT_EQ(q->take(b), size_t(1));  // A1 A2 pending (the worker is "writing" them)
    EXPECT_TRUE(q->append("B1\n", 3, TBLOG_LV_INFO));  // active
    Visited v;
    EXPECT_TRUE(q->crash_drain(&visit_into, &v));
    EXPECT_EQ(v.data, std::string("A1\nA2\nB1\n"));  // older (pending) data first
    EXPECT_EQ(v.calls, 2);
    Visited v2;
    EXPECT_FALSE(q->crash_drain(&visit_into, &v2));  // once only
    EXPECT_EQ(v2.calls, 0);
    // Not buffered any more: written at once, through the first visitor.
    EXPECT_TRUE(q->append("C\n", 2, TBLOG_LV_INFO));
    EXPECT_EQ(v.data, std::string("A1\nA2\nB1\nC\n"));
    EXPECT_EQ(v.calls, 3);
    q->release(b);

    // mark_written(): the pending chunk reached the crash-safe sinks, so the
    // crash handler must not write it again.
    auto q2 = Queue::create(qo);
    ASSERT_TRUE(q2 != nullptr);
    EXPECT_TRUE(q2->append("A1\n", 3, TBLOG_LV_INFO));
    EXPECT_EQ(q2->take(b), size_t(1));
    q2->mark_written(b, 0);
    EXPECT_TRUE(q2->append("B1\n", 3, TBLOG_LV_INFO));
    Visited v3;
    EXPECT_TRUE(q2->crash_drain(&visit_into, &v3));
    EXPECT_EQ(v3.data, std::string("B1\n"));
    q2->release(b);
}

// The worker stops at a crash and releases its batch while the crash handler
// is still walking the shards (another thread): chunks it never wrote must
// stay visible to the handler.
struct ReleasingVisitor {
    Queue *q;
    Queue::Batch *b;
    std::string data;
};
static void visit_and_release(void *ctx, const char *data, size_t len) {
    ReleasingVisitor *v = static_cast<ReleasingVisitor *>(ctx);
    v->data.append(data, len);
    v->q->release(*v->b);  // the worker, between two shards of the handler
}

TEST(queue_release_after_crash_keeps_unwritten) {
    QueueOptions qo;
    auto q = Queue::create(qo);
    ASSERT_TRUE(q != nullptr);
    // Two new threads get consecutive ids, i.e. two different shards
    // (unless the machine has a single shard).
    std::thread([&] { EXPECT_TRUE(q->append("A\n", 2, TBLOG_LV_INFO)); }).join();
    std::thread([&] { EXPECT_TRUE(q->append("B\n", 2, TBLOG_LV_INFO)); }).join();
    Queue::Batch b;
    const size_t n = q->take(b);
    ReleasingVisitor v{q.get(), &b, std::string()};
    EXPECT_TRUE(q->crash_drain(&visit_and_release, &v));
    if (n != 2) std::printf("  (single shard: the two-shard race is not exercised)\n");
    // Each line exactly once. Their order is the shards' order, which depends
    // on how many shards this machine has and which ids the two threads got
    // (lines of different threads have no defined order).
    EXPECT_TRUE(v.data == "A\nB\n" || v.data == "B\nA\n");
    q->release(b);
}

TEST(queue_close_rejects_and_keeps_accepted) {
    QueueOptions qo;
    auto q = Queue::create(qo);
    ASSERT_TRUE(q != nullptr);
    EXPECT_TRUE(q->append("kept\n", 5, TBLOG_LV_INFO));
    q->close();
    q->close();  // idempotent
    EXPECT_FALSE(q->append("late\n", 5, TBLOG_LV_INFO));
    Queue::Batch b;
    ASSERT_TRUE(q->take(b) == 1);
    EXPECT_EQ(std::string(b.chunks[0].data, b.chunks[0].len), std::string("kept\n"));
    q->release(b);
    EXPECT_EQ(q->take(b), size_t(0));
}

// User sink (tblog_add_sink's CallbackSink) behind a multi-threaded queue:
// receives exactly the bytes of the file, each thread's lines in order.
struct UserCapture {
    std::string data;
    int flushes = 0;
};
static void user_write(void *ctx, const char *d, size_t n) {
    static_cast<UserCapture *>(ctx)->data.append(d, n);
}
static void user_flush(void *ctx) { ++static_cast<UserCapture *>(ctx)->flushes; }

TEST(pipeline_callback_sink_multithread_order) {
    TempDir d("callback");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.file.max_size = 1ull << 40;
    o.buffer_size = 64u << 10;
    o.policy = FullPolicy::Block;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    EXPECT_TRUE(make_callback_sink(nullptr, nullptr, nullptr) == nullptr);
    UserCapture u, u_noflush;
    b->add_sink(make_callback_sink(&user_write, &user_flush, &u));
    b->add_sink(make_callback_sink(&user_write, nullptr, &u_noflush));
    Worker w(b.get());
    const int T = 8, N = 20000 / kScale;
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->queue().append(line, n, TBLOG_LV_INFO));
            }
        });
    for (auto &x : th) x.join();
    w.join();
    b->shutdown();
    EXPECT_EQ(verify_lines(u.data, T, N, true), static_cast<long>(T) * N);
    EXPECT_TRUE(u.data == read_file(o.file.path));
    EXPECT_TRUE(u_noflush.data == u.data);
    EXPECT_TRUE(u.flushes > 0);
}

TEST(queue_sharded_8x50k_complete_and_ordered) {
    // Block: every line exactly once, each thread's lines in increasing order.
    // Default-sized buffer (several shards in use, one per thread).
    TempDir d("sharded");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.file.max_size = 1ull << 40;
    o.policy = FullPolicy::Block;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    Worker w(b.get());
    const int T = 8, N = 50000 / kScale;
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->queue().append(line, n, TBLOG_LV_INFO));
            }
        });
    for (auto &x : th) x.join();
    w.join();
    b->shutdown();
    EXPECT_EQ(b->queue().dropped(), 0ull);
    // expect_all: per thread seq must be exactly 0,1,2,... (once, in order).
    EXPECT_EQ(verify_lines(read_file(o.file.path), T, N, true), static_cast<long>(T) * N);
}

TEST(queue_crash_drains_all_shards) {
    TempDir d("crash_shards");
    PipeOptions o;
    o.file.path = d.file("app.log");
    o.file.max_size = 1ull << 40;
    auto b = make_pipe(o);
    ASSERT_TRUE(b != nullptr);
    const int T = 8, N = 300;
    std::atomic<int> phase{0};
    std::vector<std::thread> th;
    // Each thread: N lines, (main flushes -> some are pending/written), N more.
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->queue().append(line, n, TBLOG_LV_INFO));
            }
            phase.fetch_add(1);
            while (phase.load() < 2 * T) std::this_thread::yield();
            for (int i = N; i < 2 * N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->queue().append(line, n, TBLOG_LV_INFO));
            }
            phase.fetch_add(1);
        });
    while (phase.load() < T) std::this_thread::yield();
    b->drain();  // first half on disk
    phase.fetch_add(T);
    while (phase.load() < 3 * T) std::this_thread::yield();
    for (auto &x : th) x.join();
    size_t before = read_file(o.file.path).size();
    EXPECT_TRUE(before > 0);
    b->crash(nullptr, 0);  // lock-free drain of every shard
    std::string got = read_file(o.file.path);
    EXPECT_TRUE(got.size() > before);
    EXPECT_EQ(verify_lines(got, T, 2 * N, true), static_cast<long>(T) * 2 * N);
    EXPECT_TRUE(b->queue().append("after\n", 6, TBLOG_LV_INFO));  // written at once
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == got + "after\n");
}

// ---------------------------------------------------------------- after a crash
// Once crash_drain() started, append() no longer buffers: it waits for the
// drain (and its tail, the marker) and then writes the line itself through
// the drain's visitor, one thread at a time.

TEST(queue_append_after_crash_written_after_drain_and_tail) {
    QueueOptions qo;
    auto q = Queue::create(qo);
    ASSERT_TRUE(q != nullptr);
    EXPECT_TRUE(q->append("A\n", 2, TBLOG_LV_INFO));
    Visited v;
    EXPECT_TRUE(q->crash_drain(&visit_into, &v, "M\n", 2));
    EXPECT_EQ(v.data, std::string("A\nM\n"));
    bool ok = false;
    std::thread([&] { ok = q->append("other thread\n", 13, TBLOG_LV_INFO); }).join();
    EXPECT_TRUE(ok);
    EXPECT_TRUE(q->append("same thread\n", 12, TBLOG_LV_INFO));
    EXPECT_EQ(v.data, std::string("A\nM\nother thread\nsame thread\n"));
    // A second drain writes only its tail, the post-crash way.
    Visited v2;
    EXPECT_FALSE(q->crash_drain(&visit_into, &v2, "M2\n", 3));
    EXPECT_EQ(v2.calls, 0);
    EXPECT_EQ(v.data, std::string("A\nM\nother thread\nsame thread\nM2\n"));
    Queue::Batch b;
    EXPECT_EQ(q->take(b), size_t(0));  // nothing was buffered after the crash
    EXPECT_EQ(q->dropped(), 0ull);
}

// An append that arrives while the drain is still running (the handler is
// slow) waits for it: its line comes after the drained data and the marker,
// and the call does not return before it is written.
struct SlowVisit {
    std::string data;
    int calls = 0;
    std::atomic<bool> draining{false};
    std::atomic<bool> appender_in{false};
    std::atomic<bool> appender_done{false};
    bool returned_early = false;
};
static void slow_visit(void *ctx, const char *data, size_t len) {
    SlowVisit *v = static_cast<SlowVisit *>(ctx);
    if (v->calls++ == 0) {
        v->draining.store(true);
        while (!v->appender_in.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        v->returned_early = v->appender_done.load();
    }
    v->data.append(data, len);
}

TEST(queue_append_waits_for_running_drain) {
    QueueOptions qo;
    auto q = Queue::create(qo);
    ASSERT_TRUE(q != nullptr);
    EXPECT_TRUE(q->append("A1\n", 3, TBLOG_LV_INFO));
    EXPECT_TRUE(q->append("A2\n", 3, TBLOG_LV_INFO));
    SlowVisit v;
    bool ok = false;
    std::thread appender([&] {
        while (!v.draining.load()) std::this_thread::yield();
        v.appender_in.store(true);
        ok = q->append("late\n", 5, TBLOG_LV_INFO);
        v.appender_done.store(true);
    });
    EXPECT_TRUE(q->crash_drain(&slow_visit, &v, "MARK\n", 5));
    appender.join();
    EXPECT_TRUE(ok);
    EXPECT_FALSE(v.returned_early);
    EXPECT_EQ(v.data, std::string("A1\nA2\nMARK\nlate\n"));
}

// The chunk in flight: the worker is inside a crash-safe sink's write() when
// the crash handler starts. The handler waits for that write, then drains
// exactly what is left: every line once, the marker last.
struct SlowSafeSink : LogSink {
    Queue *q = nullptr;
    std::string data;
    std::string events;  // W = write() returned, C = crash_write()
    std::atomic<bool> in_write{false};
    bool first = true;
    void write(const char *d, size_t n) override {
        if (first) {
            first = false;
            in_write.store(true);
            while (!q->crashed()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        data.append(d, n);
        events += 'W';
    }
    bool crash_safe() const noexcept override { return true; }
    void crash_write(const char *d, size_t n) noexcept override {
        data.append(d, n);
        events += 'C';
    }
};

TEST(pipeline_crash_waits_for_write_in_flight_no_duplicates) {
    QueueOptions qo;
    Pipeline p(Queue::create(qo));
    SlowSafeSink *s = new SlowSafeSink;
    s->q = &p.queue();
    p.add_sink(std::unique_ptr<LogSink>(s));
    // Two threads: two shards, i.e. two chunks (one if the machine has a
    // single shard).
    const int T = 2, N = 200;
    for (int t = 0; t < T; ++t)
        std::thread([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(p.queue().append(line, n, TBLOG_LV_INFO));
            }
        }).join();
    std::thread worker([&] { p.drain(); });
    while (!s->in_write.load()) std::this_thread::yield();
    const char m[] = "*** tblog: caught signal 11 (SIGSEGV) ***\n";
    p.crash(m, sizeof m - 1);
    worker.join();
    // The first chunk by the worker (completed before the handler went on),
    // everything else by the handler, then the marker.
    EXPECT_EQ(s->events.substr(0, 1), std::string("W"));
    EXPECT_TRUE(s->events.find('W', 1) == std::string::npos);  // the worker stopped
    const size_t at = s->data.find("***");
    ASSERT_TRUE(at != std::string::npos);
    EXPECT_EQ(s->data.substr(at), std::string(m));
    EXPECT_EQ(verify_lines(s->data.substr(0, at), T, N, false), static_cast<long>(T) * N);
    // Exactly once each (verify_lines checks order; count the total).
    EXPECT_EQ(split_lines(s->data.substr(0, at)).size(), static_cast<size_t>(T) * N);
}

// Real threads, real worker, real file: 4 threads log while another thread
// "crashes"; they keep logging after the crash. Every line whose append()
// returned true is in the file exactly once, in each thread's order, the
// marker exactly once. Repeated: the interesting interleavings are races.
TEST(pipeline_crash_while_threads_log_loses_nothing) {
    TempDir d("crash_race");
    const int T = 4;
    const int rounds = 20 / kScale < 2 ? 2 : 20 / kScale;
    const char m[] = "*** tblog: caught signal 11 (SIGSEGV) ***\n";
    long total_lines = 0;
    for (int r = 0; r < rounds; ++r) {
        PipeOptions o;
        const std::string name = "r" + std::to_string(r) + ".log";
        o.file.path = d.file(name.c_str());
        o.file.max_size = 1ull << 40;
        o.buffer_size = 256u << 10;  // small shards: frequent swaps, chunks in flight
        o.policy = FullPolicy::Block;  // no drops: every line is acknowledged
        auto b = make_pipe(o);
        ASSERT_TRUE(b != nullptr);
        Worker w(b.get());
        std::atomic<bool> stop{false};
        std::vector<long> acked(T, 0);
        std::vector<std::thread> th;
        for (int t = 0; t < T; ++t)
            th.emplace_back([&, t] {
                char line[128];
                const auto start = std::chrono::steady_clock::now();
                for (long i = 0; !stop.load(std::memory_order_relaxed); ++i) {
                    // ~500k lines/s per thread.
                    while (std::chrono::steady_clock::now() - start < std::chrono::microseconds(2 * i)) {
                    }
                    int n = make_line(line, t, static_cast<int>(i));
                    if (!b->queue().append(line, n, TBLOG_LV_INFO)) {
                        EXPECT_TRUE(false);
                        break;
                    }
                    acked[t] = i + 1;
                }
            });
        tos::sleep_ms(5 + (r % 5) * 4);
        b->crash(m, sizeof m - 1);
        tos::sleep_ms(10);  // the threads keep logging after the crash
        stop.store(true);
        for (auto &x : th) x.join();
        w.join();
        const std::string got = read_file(o.file.path);
        std::vector<long> next(T, 0);
        long bad = 0, markers = 0, order = 0;
        for (const std::string &l : split_lines(got)) {
            if (l + "\n" == m) {
                ++markers;
                continue;
            }
            int t, s;
            if (!parse_line(l, &t, &s) || t >= T) {
                ++bad;
                continue;
            }
            if (s != next[t]) ++order;  // lost, duplicated or reordered
            next[t] = s + 1;
        }
        EXPECT_EQ(bad, 0L);
        EXPECT_EQ(markers, 1L);
        EXPECT_EQ(order, 0L);
        for (int t = 0; t < T; ++t) {
            EXPECT_EQ(next[t], acked[t]);
            total_lines += acked[t];
        }
        b->shutdown();
    }
    std::printf("  %d rounds, %ld lines\n", rounds, total_lines);
}

// ---------------------------------------------------------------- benchmark
static void bench(const char *name, Pipeline *b, int T, int N) {
    Worker w(b);
    std::vector<std::vector<uint32_t>> lat(T);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[160];
            int n = std::snprintf(line, sizeof line,
                                  "2026-11-09 12:00:00.000000 [INFO] [%d] bench.cpp:1 - "
                                  "a typical log line of about one hundred bytes\n", t);
            auto &v = lat[t];
            v.reserve(N);
            for (int i = 0; i < N; ++i) {
                auto a = std::chrono::steady_clock::now();
                b->queue().append(line, n, TBLOG_LV_INFO);
                auto z = std::chrono::steady_clock::now();
                v.push_back(static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(z - a).count()));
            }
        });
    for (auto &x : th) x.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    w.join();
    b->shutdown();
    std::vector<uint32_t> all;
    for (auto &v : lat) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    std::printf("  [bench] %-14s %d thr x %d: p50 %u ns, p99 %u ns, p99.9 %u ns, "
                "%.2f M lines/s, dropped %llu\n",
                name, T, N, all[all.size() / 2], all[all.size() * 99 / 100],
                all[all.size() * 999 / 1000], all.size() / secs / 1e6,
                static_cast<unsigned long long>(b->queue().dropped()));
}

TEST(bench_queue_threads_scaling) {
    TempDir d("bench_scale");
    for (int T : {1, 4, 8}) {
        PipeOptions o;
        o.file.path = d.file("scale.log");
        o.file.max_size = 1ull << 40;
        o.policy = FullPolicy::Drop;
        auto db = make_pipe(o);
        ASSERT_TRUE(db != nullptr);
        bench("queue(sharded)", db.get(), T, 200000 / kScale);
    }
}

TEST(bench_append_latency) {
    TempDir d("bench");
    PipeOptions o;
    o.file.path = d.file("drop.log");
    o.file.max_size = 1ull << 40;
    o.policy = FullPolicy::Drop;
    auto db = make_pipe(o);
    ASSERT_TRUE(db != nullptr);
    bench("queue(drop)", db.get(), 4, 250000 / kScale);
    o.file.path = d.file("block.log");
    o.policy = FullPolicy::Block;
    auto dbb = make_pipe(o);
    ASSERT_TRUE(dbb != nullptr);
    bench("queue(block)", dbb.get(), 4, 250000 / kScale);
}

TINY_TEST_MAIN()
