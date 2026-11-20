// Tests for the file storage: RotatingFile and the double-buffer backend.
#include "backend/backend.h"
#include "backend/rotating_file.h"
#include "sinks/sink.h"
#include "tiny_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <ftw.h>
#include <sys/stat.h>
#include <system_error>

using namespace tblog;

// Minimal POSIX stand-in for the few std::filesystem calls used here, so the
// tests also build as C++11/14 (std::filesystem is C++17).
namespace fs {
inline bool exists(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
inline unsigned long long file_size(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 ? static_cast<unsigned long long>(st.st_size) : ~0ull;
}
inline int rm_entry(const char *p, const struct stat *, int, struct FTW *) { return ::remove(p); }
inline void remove_all(const std::string &p, std::error_code &ec) {
    ec.clear();
    if (exists(p) && ::nftw(p.c_str(), rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
        ec = std::error_code(errno, std::generic_category());
}
inline void remove(const std::string &p, std::error_code &ec) {
    ec.clear();
    if (::remove(p.c_str()) != 0) ec = std::error_code(errno, std::generic_category());
}
inline void create_directories(const std::string &p) {
    for (size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/') ::mkdir(p.substr(0, i).c_str(), 0755);
}
}  // namespace fs

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
static const int kScale = 10;  // sanitizers are slow: shrink the big tests
#else
static const int kScale = 1;
#endif

// ---------------------------------------------------------------- helpers
static std::string base_dir() {
    const char *d = std::getenv("TBLOG_TEST_DIR");
    return (d && *d) ? d : "build/tmp";
}

struct TempDir {
    std::string path;
    explicit TempDir(const char *name) {
        path = base_dir() + "/backend_v1_" + std::to_string(getpid()) + "_" + name;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::remove(base_dir() + "/", ec);  // only if empty
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
    Backend *b;
    std::atomic<bool> stop{false};
    std::thread th;
    explicit Worker(Backend *be, int delay_ms = 0) : b(be) {
        th = std::thread([this, delay_ms] {
            if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            while (!stop.load()) {
                b->wait_for_work(std::chrono::milliseconds(20));
                b->flush();
                if (b->consume_urgent()) b->sync();
            }
        });
    }
    void join() {
        stop = true;
        b->wake();
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
    ::unlink(o.path.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    rf.write_all("two\n", 4);
    EXPECT_EQ(read_file(o.path), std::string("two\n"));
    // External rename (logrotate style).
    ::rename(o.path.c_str(), d.file("moved.log").c_str());
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
    BufferOptions bo;
    bo.file = o;
    EXPECT_TRUE(make_double_buffer_backend(bo) == nullptr);
}

TEST(rotating_write_error_counts_loss) {
    if (access("/dev/full", W_OK) != 0) return;
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
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.file.max_size = 1ull << 40;
    o.buffer_size = bufsize;
    o.policy = policy;
    auto b = make_double_buffer_backend(o);
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
                if (b->append(line, n, TBLOG_LV_INFO)) ++mine;
            }
            ok += mine;
        });
    for (auto &x : th) x.join();
    w.join();
    b->shutdown();
    uint64_t reported = 0;
    long good = verify_lines(read_file(o.file.path), T, N, expect_all, &reported);
    EXPECT_EQ(good, ok.load());
    EXPECT_EQ(static_cast<uint64_t>(good) + b->dropped(), static_cast<uint64_t>(T) * N);
    EXPECT_EQ(reported, b->dropped());
    if (expect_all) EXPECT_EQ(b->dropped(), 0ull);
    std::printf("  policy=%s buf=%zu: %ld lines written, %llu dropped\n",
                policy == FullPolicy::Block ? "block" : "drop", bufsize, good,
                static_cast<unsigned long long>(b->dropped()));
}

TEST(dbuf_8_threads_all_lines_intact_block) {
    run_mt_test(FullPolicy::Block, 256 << 10, 8, 50000 / kScale, true);
}

TEST(dbuf_8_threads_drop_accounting) {
    // Small buffer so drops are likely; every surviving line must be intact and
    // survivors + dropped == total, and the "N lines dropped" notes must add up.
    run_mt_test(FullPolicy::Drop, 16 << 10, 8, 50000 / kScale, false);
}

TEST(dbuf_drop_policy_stalled_worker) {
    TempDir d("drop");
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 4096;
    o.policy = FullPolicy::Drop;
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    // Sharded: a single thread always uses one shard, whose capacity is
    // max(64 KB, 4096 / N) = 64 KB = 1024 lines of 64 bytes.
    std::string line(63, 'z');
    line.push_back('\n');
    int ok = 0;
    for (int i = 0; i < 3000; ++i) ok += b->append(line.data(), 64, TBLOG_LV_INFO);
    EXPECT_EQ(ok, 1024);
    EXPECT_EQ(b->dropped(), 1976ull);
    // Too long to ever fit -> dropped even under Block.
    std::string huge(70000, 'h');
    EXPECT_FALSE(b->append(huge.data(), static_cast<uint32_t>(huge.size()), TBLOG_LV_INFO));
    EXPECT_EQ(b->dropped(), 1977ull);
    b->flush();
    std::string expect;
    for (int i = 0; i < 1024; ++i) expect += line;
    expect += "tblog: 1977 lines dropped\n";
    EXPECT_TRUE(read_file(o.file.path) == expect);
    b->flush();  // nothing new: no second drop note
    EXPECT_TRUE(read_file(o.file.path) == expect);
    EXPECT_TRUE(b->append(line.data(), 64, TBLOG_LV_INFO));
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == expect + line);
}

TEST(dbuf_block_policy_loses_nothing) {
    TempDir d("block");
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 4096;
    o.policy = FullPolicy::Block;
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    Worker w(b.get(), 200);  // stalled at first: producers must block
    const int T = 4, N = 20000 / kScale;
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->append(line, n, TBLOG_LV_INFO));
            }
        });
    for (auto &x : th) x.join();
    w.join();
    b->shutdown();
    EXPECT_EQ(b->dropped(), 0ull);
    EXPECT_EQ(verify_lines(read_file(o.file.path), T, N, true), static_cast<long>(T * N));
}

TEST(dbuf_block_released_by_shutdown) {
    TempDir d("blockshut");
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 128;
    o.policy = FullPolicy::Block;
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    std::string line(63, 'q');
    line.push_back('\n');
    // One thread fills its shard (64 KB = 1024 lines), then blocks on the next.
    std::atomic<int> filled{0};
    std::atomic<int> res{-1};
    std::thread t([&] {
        int n = 0;
        for (int i = 0; i < 1024; ++i) n += b->append(line.data(), 64, TBLOG_LV_INFO);
        filled = n;
        res = b->append(line.data(), 64, TBLOG_LV_INFO);  // blocks
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(filled.load(), 1024);
    EXPECT_EQ(res.load(), -1);
    b->shutdown();
    t.join();
    EXPECT_EQ(res.load(), 0);
    EXPECT_EQ(fs::file_size(o.file.path), 65536u);
}

TEST(dbuf_urgent_and_notify) {
    TempDir d("urgent");
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 1000;
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    using clk = std::chrono::steady_clock;
    std::string line(99, 'u');
    line.push_back('\n');

    // Below 75%: no notification, wait_for_work times out.
    EXPECT_TRUE(b->append(line.data(), 100, TBLOG_LV_INFO));
    auto t0 = clk::now();
    b->wait_for_work(std::chrono::milliseconds(100));
    EXPECT_TRUE(clk::now() - t0 >= std::chrono::milliseconds(90));
    EXPECT_FALSE(b->consume_urgent());

    // Urgent line: flag + immediate wake.
    EXPECT_TRUE(b->append(line.data(), 100, TBLOG_LV_ERROR));
    t0 = clk::now();
    b->wait_for_work(std::chrono::seconds(5));
    EXPECT_TRUE(clk::now() - t0 < std::chrono::seconds(1));
    EXPECT_TRUE(b->consume_urgent());
    EXPECT_FALSE(b->consume_urgent());
    b->flush();

    // >= 75% of the (64 KB) shard: wake without urgency.
    for (int i = 0; i < 500; ++i) EXPECT_TRUE(b->append(line.data(), 100, TBLOG_LV_DEBUG));
    t0 = clk::now();
    b->wait_for_work(std::chrono::seconds(5));
    EXPECT_TRUE(clk::now() - t0 < std::chrono::seconds(1));
    EXPECT_FALSE(b->consume_urgent());

    // Notify from another thread while the worker sleeps.
    b->flush();
    std::thread p([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        b->append(line.data(), 100, TBLOG_LV_FATAL);
    });
    t0 = clk::now();
    b->wait_for_work(std::chrono::seconds(5));
    EXPECT_TRUE(clk::now() - t0 < std::chrono::seconds(1));
    p.join();
    b->shutdown();
    EXPECT_EQ(fs::file_size(o.file.path), 50300u);
}

TEST(dbuf_rotation) {
    TempDir d("dbuf_rot");
    BufferOptions o;
    o.file.path = d.file("rot.log");
    o.file.max_size = 1000;
    o.file.max_files = 2;
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    for (int i = 0; i < 25; ++i) {
        std::string l = fixed_line(i);
        EXPECT_TRUE(b->append(l.data(), static_cast<uint32_t>(l.size()), TBLOG_LV_INFO));
    }
    b->shutdown();
    EXPECT_EQ(fs::file_size(d.file("rot.log")), 500u);
    EXPECT_EQ(fs::file_size(d.file("rot.1.log")), 1000u);
    EXPECT_FALSE(fs::exists(d.file("rot.2.log")));
    EXPECT_EQ(read_file(d.file("rot.log")).substr(0, 5), std::string("L0020"));
}

TEST(dbuf_shutdown_idempotent) {
    TempDir d("shutdown");
    BufferOptions o;
    o.file.path = d.file("app.log");
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    EXPECT_TRUE(b->append("last words\n", 11, TBLOG_LV_INFO));
    b->shutdown();
    b->shutdown();
    EXPECT_FALSE(b->append("too late\n", 9, TBLOG_LV_INFO));
    b->flush();
    b->sync();
    b->wake();
    b->wait_for_work(std::chrono::milliseconds(1));
    b->on_crash();  // fd closed: no-op
    b->crash_write("x\n", 2);
    b.reset();  // destructor after shutdown
    EXPECT_EQ(read_file(o.file.path), std::string("last words\n"));
}

struct CaptureSink : Sink {
    std::string data;
    int flushes = 0;
    Backend *crash_from_write = nullptr;
    void write(const char *d, size_t n) override {
        data.append(d, n);
        if (crash_from_write) crash_from_write->on_crash();
    }
    void flush() override { ++flushes; }
};

TEST(dbuf_sinks_receive_everything) {
    TempDir d("sinks");
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.buffer_size = 4096;
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    CaptureSink s1, s2;
    b->add_sink(&s1);
    b->add_sink(&s2);
    char line[128];
    for (int i = 0; i < 500; ++i) {
        int n = make_line(line, 0, i);
        b->append(line, n, TBLOG_LV_INFO);
        if (i % 50 == 0) b->flush();
    }
    b->shutdown();
    std::string f = read_file(o.file.path);
    EXPECT_TRUE(s1.data == f);
    EXPECT_TRUE(s2.data == f);
    EXPECT_TRUE(s1.flushes > 0);
}

TEST(dbuf_on_crash_writes_buffered) {
    TempDir d("crash");
    BufferOptions o;
    o.file.path = d.file("app.log");
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    char line[128];
    std::string expect;
    for (int i = 0; i < 100; ++i) {
        int n = make_line(line, 1, i);
        b->append(line, n, TBLOG_LV_INFO);
        expect.append(line, n);
    }
    b->flush();  // first 100 already on disk
    for (int i = 100; i < 200; ++i) {
        int n = make_line(line, 1, i);
        b->append(line, n, TBLOG_LV_INFO);
        expect.append(line, n);
    }
    EXPECT_TRUE(read_file(o.file.path).size() < expect.size());
    const char m[] = "*** tblog: caught signal 11 (SIGSEGV) ***\n";
    b->crash_write(m, sizeof m - 1);
    b->on_crash();
    std::string got = read_file(o.file.path);
    EXPECT_TRUE(got.find(m) != std::string::npos);
    std::string without = got;
    without.erase(without.find(m), sizeof m - 1);
    EXPECT_TRUE(without == expect);
    // Dead after crash: nothing more written, no duplicates at shutdown.
    EXPECT_FALSE(b->append("after\n", 6, TBLOG_LV_INFO));
    b->on_crash();
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == got);
}

TEST(dbuf_on_crash_during_flush_no_duplicates) {
    TempDir d("crash2");
    BufferOptions o;
    o.file.path = d.file("app.log");
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    CaptureSink s;
    s.crash_from_write = b.get();  // "signal" arrives while the worker is mid-flush
    b->add_sink(&s);
    char line[128];
    std::string expect;
    for (int i = 0; i < 50; ++i) {
        int n = make_line(line, 2, i);
        b->append(line, n, TBLOG_LV_INFO);
        expect.append(line, n);
    }
    b->flush();
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == expect);
}

TEST(dbuf_sharded_8x50k_complete_and_ordered) {
    // Block: every line exactly once, each thread's lines in increasing order.
    // Default-sized buffer (several shards in use, one per thread).
    TempDir d("sharded");
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.file.max_size = 1ull << 40;
    o.policy = FullPolicy::Block;
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    Worker w(b.get());
    const int T = 8, N = 50000 / kScale;
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t)
        th.emplace_back([&, t] {
            char line[128];
            for (int i = 0; i < N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->append(line, n, TBLOG_LV_INFO));
            }
        });
    for (auto &x : th) x.join();
    w.join();
    b->shutdown();
    EXPECT_EQ(b->dropped(), 0ull);
    // expect_all: per thread seq must be exactly 0,1,2,... (once, in order).
    EXPECT_EQ(verify_lines(read_file(o.file.path), T, N, true), static_cast<long>(T) * N);
}

TEST(dbuf_on_crash_drains_all_shards) {
    TempDir d("crash_shards");
    BufferOptions o;
    o.file.path = d.file("app.log");
    o.file.max_size = 1ull << 40;
    auto b = make_double_buffer_backend(o);
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
                EXPECT_TRUE(b->append(line, n, TBLOG_LV_INFO));
            }
            phase.fetch_add(1);
            while (phase.load() < 2 * T) std::this_thread::yield();
            for (int i = N; i < 2 * N; ++i) {
                int n = make_line(line, t, i);
                EXPECT_TRUE(b->append(line, n, TBLOG_LV_INFO));
            }
            phase.fetch_add(1);
        });
    while (phase.load() < T) std::this_thread::yield();
    b->flush();  // first half on disk
    phase.fetch_add(T);
    while (phase.load() < 3 * T) std::this_thread::yield();
    for (auto &x : th) x.join();
    size_t before = read_file(o.file.path).size();
    EXPECT_TRUE(before > 0);
    b->on_crash();  // lock-free drain of every shard
    std::string got = read_file(o.file.path);
    EXPECT_TRUE(got.size() > before);
    EXPECT_EQ(verify_lines(got, T, 2 * N, true), static_cast<long>(T) * 2 * N);
    EXPECT_FALSE(b->append("after\n", 6, TBLOG_LV_INFO));
    b->shutdown();
    EXPECT_TRUE(read_file(o.file.path) == got);
}

// ---------------------------------------------------------------- benchmark
static void bench(const char *name, Backend *b, int T, int N) {
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
                b->append(line, n, TBLOG_LV_INFO);
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
                static_cast<unsigned long long>(b->dropped()));
}

TEST(bench_dbuf_threads_scaling) {
    TempDir d("bench_scale");
    for (int T : {1, 4, 8}) {
        BufferOptions o;
        o.file.path = d.file("scale.log");
        o.file.max_size = 1ull << 40;
        o.policy = FullPolicy::Drop;
        auto db = make_double_buffer_backend(o);
        ASSERT_TRUE(db != nullptr);
        bench("dbuf(sharded)", db.get(), T, 200000 / kScale);
    }
}

TEST(bench_append_latency) {
    TempDir d("bench");
    BufferOptions o;
    o.file.path = d.file("dbuf.log");
    o.file.max_size = 1ull << 40;
    o.policy = FullPolicy::Drop;
    auto db = make_double_buffer_backend(o);
    ASSERT_TRUE(db != nullptr);
    bench("double_buffer", db.get(), 4, 250000 / kScale);
    o.file.path = d.file("dbuf_block.log");
    o.policy = FullPolicy::Block;
    auto dbb = make_double_buffer_backend(o);
    ASSERT_TRUE(dbb != nullptr);
    bench("dbuf(block)", dbb.get(), 4, 250000 / kScale);
}

TINY_TEST_MAIN()
