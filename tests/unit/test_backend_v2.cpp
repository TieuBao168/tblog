// Tests for the v2 mmap backend (src/backend/mmap_file.cpp).
#include "internal.h"
#include "tiny_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace tblog;

namespace {

std::string base_dir() {
    const char *e = std::getenv("TBLOG_TEST_DIR");
    return (e && *e) ? e : "build/tmp";
}

void mkdirs(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0755);
}

std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> out;
    if (DIR *d = ::opendir(dir.c_str())) {
        while (dirent *e = ::readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        ::closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string fresh_dir(const char *name) {
    std::string d = base_dir() + "/v2_" + name;
    mkdirs(d);
    for (auto &f : list_dir(d)) ::unlink((d + "/" + f).c_str());
    return d;
}

std::string read_file(const std::string &path) {
    std::string s;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

// Deterministic, self-checking line: "t<T> s<S> <filler>|\n"
std::string make_line(int t, int s) {
    char head[48];
    int n = std::snprintf(head, sizeof head, "t%d s%d ", t, s);
    std::string l(head, (size_t)n);
    l.append((size_t)(s * 7 + t) % 61, (char)('a' + (s + t) % 26));
    l += "|\n";
    return l;
}

// Parses and verifies a line (without '\n'). Returns false if malformed.
bool parse_line(const std::string &l, int *t, int *s) {
    if (std::sscanf(l.c_str(), "t%d s%d ", t, s) != 2) return false;
    return make_line(*t, *s) == l + "\n";
}

std::vector<std::string> split_lines(const std::string &data) {
    std::vector<std::string> out;
    size_t b = 0;
    for (size_t i = 0; i < data.size(); ++i)
        if (data[i] == '\n') {
            out.push_back(data.substr(b, i - b));
            b = i + 1;
        }
    if (b < data.size()) out.push_back(data.substr(b));  // incomplete tail
    return out;
}

std::string strip_blank_lines(const std::string &data) {
    std::string out;
    for (auto &l : split_lines(data))
        if (!l.empty()) out += l + "\n";
    return out;
}

struct Worker {
    Backend *b;
    std::atomic<bool> stop{false};
    std::thread th;
    explicit Worker(Backend *be) : b(be) {
        th = std::thread([this] {
            while (!stop.load()) {
                b->wait_for_work(std::chrono::milliseconds(5));
                b->flush();
                if (b->consume_urgent()) b->sync();
            }
        });
    }
    ~Worker() { halt(); }
    void halt() {
        if (!th.joinable()) return;
        stop = true;
        b->wake();
        th.join();
    }
};

MmapOptions opts(const std::string &dir, size_t region, FullPolicy pol,
                 uint64_t max_size = 1ull << 30, int max_files = 5) {
    MmapOptions o;
    o.file.path = dir + "/app.log";
    o.file.max_size = max_size;
    o.file.max_files = max_files;
    o.region_size = region;
    o.policy = pol;
    return o;
}

// Verifies every (t, s) for t < T, s < S appears exactly once over `datas`.
// With `missing_out`, absent lines are reported to the caller instead of
// failing (Drop-policy tests compare them with the backend's drop count).
void check_all_once(const std::vector<std::string> &datas, int T, int S,
                    size_t *missing_out = nullptr) {
    std::vector<unsigned char> seen((size_t)T * S, 0);
    size_t bad = 0, dup = 0;
    for (auto &data : datas)
        for (auto &l : split_lines(data)) {
            if (l.empty()) continue;  // region padding
            int t, s;
            if (!parse_line(l, &t, &s) || t < 0 || t >= T || s < 0 || s >= S) {
                ++bad;
                continue;
            }
            if (seen[(size_t)t * S + s]++) ++dup;
        }
    size_t missing = (size_t)std::count(seen.begin(), seen.end(), 0);
    EXPECT_EQ(bad, (size_t)0);
    EXPECT_EQ(dup, (size_t)0);
    if (missing_out)
        *missing_out = missing;
    else
        EXPECT_EQ(missing, (size_t)0);
}

}  // namespace

// 1. single thread, ordered, exact length.
TEST(single_thread_exact) {
    std::string d = fresh_dir("single");
    auto b = make_mmap_file_backend(opts(d, 1 << 20, FullPolicy::Drop));
    ASSERT_TRUE(b != nullptr);
    std::string expect;
    for (int i = 0; i < 1000; ++i) {
        std::string l = make_line(0, i);
        EXPECT_TRUE(b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO));
        expect += l;
    }
    EXPECT_FALSE(b->consume_urgent());
    std::string e = make_line(0, 1000);
    EXPECT_TRUE(b->append(e.data(), (uint32_t)e.size(), TBLOG_LV_ERROR));
    expect += e;
    EXPECT_TRUE(b->consume_urgent());
    EXPECT_FALSE(b->consume_urgent());
    b->crash_write("*** crash line ***\n", 19);
    expect += "*** crash line ***\n";
    b->sync();
    b->shutdown();
    b->shutdown();  // idempotent
    EXPECT_FALSE(b->append("x\n", 2, TBLOG_LV_INFO));
    std::string got = read_file(d + "/app.log");
    EXPECT_EQ(got.size(), expect.size());
    EXPECT_TRUE(got == expect);
    EXPECT_EQ(b->dropped(), (uint64_t)0);
}

// 1b. single thread across many small regions (padding) stays in order.
TEST(single_thread_many_regions) {
    std::string d = fresh_dir("single_regions");
    auto b = make_mmap_file_backend(opts(d, 4096, FullPolicy::Block));
    ASSERT_TRUE(b != nullptr);
    Worker w(b.get());
    std::string expect;
    for (int i = 0; i < 5000; ++i) {
        std::string l = make_line(0, i);
        EXPECT_TRUE(b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO));
        expect += l;
    }
    w.halt();
    b->shutdown();
    std::string got = read_file(d + "/app.log");
    EXPECT_TRUE(got.find('\0') == std::string::npos);
    EXPECT_TRUE(!got.empty() && got.back() == '\n');
    EXPECT_TRUE(strip_blank_lines(got) == expect);
}

// 2. 8 threads x 100k lines, 64 KB regions -> many switches.
TEST(multi_thread_region_switches) {
    std::string d = fresh_dir("mt");
    auto b = make_mmap_file_backend(opts(d, 64 << 10, FullPolicy::Block));
    ASSERT_TRUE(b != nullptr);
    const int T = 8, S = 100000;
    {
        Worker w(b.get());
        std::vector<std::thread> ths;
        for (int t = 0; t < T; ++t)
            ths.emplace_back([&, t] {
                for (int s = 0; s < S; ++s) {
                    std::string l = make_line(t, s);
                    b->append(l.data(), (uint32_t)l.size(),
                              s % 1000 == 0 ? TBLOG_LV_ERROR : TBLOG_LV_INFO);
                }
            });
        for (auto &th : ths) th.join();
        w.halt();
    }
    b->shutdown();
    EXPECT_EQ(b->dropped(), (uint64_t)0);
    std::string got = read_file(d + "/app.log");
    EXPECT_TRUE(got.find('\0') == std::string::npos);
    EXPECT_TRUE(!got.empty() && got.back() == '\n');
    check_all_once({got}, T, S);
}

// 3. rotation: files bounded, only complete lines, nothing lost.
TEST(rotation) {
    std::string d = fresh_dir("rot");
    const uint64_t max_size = 128 << 10;
    auto b = make_mmap_file_backend(opts(d, 32 << 10, FullPolicy::Block, max_size, 50));
    ASSERT_TRUE(b != nullptr);
    const int T = 4, S = 6000;  // ~1.1 MB -> ~9-10 files
    {
        Worker w(b.get());
        std::vector<std::thread> ths;
        for (int t = 0; t < T; ++t)
            ths.emplace_back([&, t] {
                for (int s = 0; s < S; ++s) {
                    std::string l = make_line(t, s);
                    b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
                }
            });
        for (auto &th : ths) th.join();
        w.halt();
    }
    b->shutdown();
    auto files = list_dir(d);
    std::printf("  rotation produced %zu files\n", files.size());
    EXPECT_TRUE(files.size() >= 8 && files.size() <= 50);
    std::vector<std::string> datas;
    for (auto &f : files) {
        EXPECT_TRUE(f == "app.log" || (f.compare(0, 4, "app.") == 0 &&
                                       f.size() > 8 && f.substr(f.size() - 4) == ".log"));
        std::string data = read_file(d + "/" + f);
        EXPECT_TRUE(data.size() <= max_size);
        EXPECT_TRUE(data.find('\0') == std::string::npos);
        EXPECT_TRUE(!data.empty() && data.back() == '\n');
        datas.push_back(data);
    }
    check_all_once(datas, T, S);
    // Order across files: app.(k).log is older than app.(k-1).log.
    int t0, s0, t1, s1;
    std::string newest = strip_blank_lines(read_file(d + "/app.log"));
    std::string older = strip_blank_lines(read_file(d + "/app.1.log"));
    auto nl = split_lines(newest), ol = split_lines(older);
    ASSERT_TRUE(!nl.empty() && !ol.empty());
    EXPECT_TRUE(parse_line(ol.back(), &t0, &s0) && parse_line(nl.front(), &t1, &s1));
}

// 3b. max_files bound: oldest files are deleted.
TEST(rotation_max_files) {
    std::string d = fresh_dir("rot_max");
    auto b = make_mmap_file_backend(opts(d, 16 << 10, FullPolicy::Block, 32 << 10, 3));
    ASSERT_TRUE(b != nullptr);
    {
        Worker w(b.get());
        for (int s = 0; s < 20000; ++s) {
            std::string l = make_line(0, s);
            b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
        }
        w.halt();
    }
    b->shutdown();
    auto files = list_dir(d);
    EXPECT_EQ(files.size(), (size_t)3);
    if (files.size() == 3) {
        EXPECT_EQ(files[0], std::string("app.1.log"));
        EXPECT_EQ(files[1], std::string("app.2.log"));
        EXPECT_EQ(files[2], std::string("app.log"));
    }
    // The newest file ends with the last line written.
    auto nl = split_lines(strip_blank_lines(read_file(d + "/app.log")));
    ASSERT_TRUE(!nl.empty());
    EXPECT_TRUE(nl.back() + "\n" == make_line(0, 19999));
}

namespace {
// Child writes N lines with a running worker, signals, sleeps; parent SIGKILLs.
// Returns the raw file content (no recovery applied).
std::string crash_child(const std::string &d, int N) {
    int pfd[2];
    if (::pipe(pfd) != 0) return "";
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pfd[0]);
        auto b = make_mmap_file_backend(opts(d, 64 << 10, FullPolicy::Block));
        if (!b) ::_exit(2);
        Worker w(b.get());
        for (int s = 0; s < N; ++s) {
            std::string l = make_line(0, s);
            if (!b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO)) ::_exit(3);
        }
        // Let the worker fill the lookahead queue so the kill hits a file
        // with prepared-but-unused regions.
        ::usleep(50 * 1000);
        if (::write(pfd[1], "k", 1) != 1) ::_exit(4);
        for (;;) ::pause();
    }
    ::close(pfd[1]);
    char c = 0;
    ssize_t n = ::read(pfd[0], &c, 1);
    ::close(pfd[0]);
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);
    EXPECT_EQ(n, (ssize_t)1);
    EXPECT_TRUE(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    return read_file(d + "/app.log");
}
}  // namespace

// 4. kill -9 durability: no recovery code, all lines present.
TEST(kill9_durability) {
    std::string d = fresh_dir("kill9");
    const int N = 20000;
    std::string raw = crash_child(d, N);
    size_t end = raw.find_last_not_of('\0');
    ASSERT_TRUE(end != std::string::npos);
    std::printf("  file %zu bytes, %zu trailing zero bytes\n", raw.size(),
                raw.size() - end - 1);
    std::string data = raw.substr(0, end + 1);
    EXPECT_TRUE(data.find('\0') == std::string::npos);
    check_all_once({data}, 1, N);
    // Preallocation: at most the current region plus K = 2 queued ones.
    EXPECT_TRUE(raw.size() - end - 1 <= (size_t)3 * (64 << 10));
}

// 5. reopen after crash: zeros trimmed, new lines appended right after.
TEST(reopen_after_crash) {
    std::string d = fresh_dir("reopen");
    const int N = 3000;
    std::string raw = crash_child(d, N);
    size_t end = raw.find_last_not_of('\0');
    ASSERT_TRUE(end != std::string::npos);
    EXPECT_TRUE(end + 1 < raw.size());  // preallocation left zeros
    std::string old = raw.substr(0, end + 1);
    auto b = make_mmap_file_backend(opts(d, 64 << 10, FullPolicy::Drop));
    ASSERT_TRUE(b != nullptr);
    std::string add;
    for (int s = 0; s < 100; ++s) {
        std::string l = make_line(1, s);
        EXPECT_TRUE(b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO));
        add += l;
    }
    b->shutdown();
    std::string got = read_file(d + "/app.log");
    EXPECT_EQ(got.size(), old.size() + add.size());
    EXPECT_TRUE(got == old + add);
}

// 6. Drop policy without a worker: first region fills, then drops are counted.
TEST(drop_without_worker) {
    std::string d = fresh_dir("drop");
    auto b = make_mmap_file_backend(opts(d, 4096, FullPolicy::Drop));
    ASSERT_TRUE(b != nullptr);
    std::string line(59, 'x');
    line += '\n';  // 60 bytes -> 68 fit in 4096, 16 bytes padding
    int ok = 0, bad = 0;
    for (int i = 0; i < 200; ++i)
        (b->append(line.data(), (uint32_t)line.size(), TBLOG_LV_INFO) ? ok : bad)++;
    EXPECT_EQ(ok, 68);
    EXPECT_EQ(b->dropped(), (uint64_t)bad);
    std::string big(5000, 'y');
    EXPECT_FALSE(b->append(big.data(), (uint32_t)big.size(), TBLOG_LV_INFO));
    EXPECT_EQ(b->dropped(), (uint64_t)bad + 1);
    b->shutdown();
    std::string got = read_file(d + "/app.log");
    EXPECT_EQ(got.size(), (size_t)68 * 60);  // padding truncated away
}

namespace {
// For every thread, its s values must appear strictly increasing when the
// datas are read in order (oldest first). Verifies regions are used in file
// order and never skipped/reordered by the lookahead queue.
void check_per_thread_order(const std::vector<std::string> &datas, int T) {
    std::vector<int> last((size_t)T, -1);
    size_t bad = 0;
    for (auto &data : datas)
        for (auto &l : split_lines(data)) {
            int t, s;
            if (l.empty() || !parse_line(l, &t, &s) || t < 0 || t >= T) continue;
            if (s <= last[(size_t)t]) ++bad;
            last[(size_t)t] = s;
        }
    EXPECT_EQ(bad, (size_t)0);
}

off_t file_size(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? st.st_size : -1;
}

// Writes T x S lines with a worker, returns the files (oldest first).
std::vector<std::string> run_rotation(const std::string &d, size_t region, uint64_t max_size,
                                      int T, int S, uint64_t *dropped) {
    auto b = make_mmap_file_backend(opts(d, region, FullPolicy::Block, max_size, 1000));
    if (!b) return {};
    {
        Worker w(b.get());
        std::vector<std::thread> ths;
        for (int t = 0; t < T; ++t)
            ths.emplace_back([&, t] {
                for (int s = 0; s < S; ++s) {
                    std::string l = make_line(t, s);
                    b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
                }
            });
        for (auto &th : ths) th.join();
        w.halt();
    }
    b->shutdown();
    *dropped = b->dropped();
    auto files = list_dir(d);
    std::vector<std::string> datas;
    // app.(n-1).log is the oldest, app.log the newest; names must be contiguous.
    for (size_t i = files.size(); i-- > 0;) {
        std::string name = i == 0 ? "app.log" : "app." + std::to_string(i) + ".log";
        EXPECT_TRUE(std::find(files.begin(), files.end(), name) != files.end());
        datas.push_back(read_file(d + "/" + name));
    }
    return datas;
}
}  // namespace

// 8. lookahead stress: 8 threads, 64 KB regions, Drop policy, worker running.
// The K prepared regions must keep up: nothing dropped, every line once.
TEST(lookahead_stress_drop) {
    std::string d = fresh_dir("la_stress");
    auto b = make_mmap_file_backend(opts(d, 64 << 10, FullPolicy::Drop));
    ASSERT_TRUE(b != nullptr);
    const int T = 8, S = 50000;
    {
        Worker w(b.get());
        std::vector<std::thread> ths;
        for (int t = 0; t < T; ++t)
            ths.emplace_back([&, t] {
                for (int s = 0; s < S; ++s) {
                    std::string l = make_line(t, s);
                    b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
                }
            });
        for (auto &th : ths) th.join();
        w.halt();
    }
    b->shutdown();
    std::string got = read_file(d + "/app.log");
    EXPECT_TRUE(got.find('\0') == std::string::npos);
    EXPECT_TRUE(!got.empty() && got.back() == '\n');
    // Correctness (always): nothing corrupt or duplicated, and every line not
    // written is accounted for as a counted drop (no silent loss).
    size_t missing = 0;
    check_all_once({got}, T, S, &missing);
    check_per_thread_order({got}, T);
    EXPECT_EQ((uint64_t)missing, b->dropped());
    // Performance (timing-dependent on a shared machine): lookahead keeps
    // drops near zero. Without it this setup dropped >1500 lines; a loaded
    // machine was seen to drop 8 of 400k (1 run in 20).
    const uint64_t budget = (uint64_t)T * S / 10000;  // 0.01%
    if (b->dropped() > 0)
        std::printf("  note: %llu of %d lines dropped (budget %llu)\n",
                    (unsigned long long)b->dropped(), T * S, (unsigned long long)budget);
    EXPECT_TRUE(b->dropped() <= budget);
}

// 9. rotation with lookahead: 3 regions per file, so the queue (K = 2) keeps
// crossing into <path>.next; and 1 region per file, where it must stall at
// the second file boundary.
TEST(rotation_with_lookahead) {
    struct Case { size_t region; uint64_t max_size; int T, S; };
    for (Case c : {Case{16 << 10, 48 << 10, 4, 4000}, Case{16 << 10, 20 << 10, 2, 3000}}) {
        std::string d = fresh_dir("la_rot");
        uint64_t dropped = 0;
        auto datas = run_rotation(d, c.region, c.max_size, c.T, c.S, &dropped);
        EXPECT_EQ(dropped, (uint64_t)0);
        auto files = list_dir(d);
        uint64_t total = 0;
        for (auto &data : datas) {
            // Files hold whole regions only, so they are at most
            // max(max_size rounded down to regions, 1 region).
            uint64_t per_file = std::max<uint64_t>(c.max_size / c.region, 1) * c.region;
            EXPECT_TRUE(data.size() <= per_file);
            EXPECT_TRUE(data.find('\0') == std::string::npos);
            EXPECT_TRUE(!data.empty() && data.back() == '\n');
            total += data.size();
        }
        uint64_t per_file = std::max<uint64_t>(c.max_size / c.region, 1) * c.region;
        std::printf("  region %zu max_size %llu: %zu files, %llu bytes\n", c.region,
                    (unsigned long long)c.max_size, files.size(), (unsigned long long)total);
        // Every file but the newest is completely filled: all its regions
        // used, only the last region's tail padding (< one line) truncated.
        EXPECT_TRUE(files.size() >= 2);
        for (size_t i = 0; i + 1 < datas.size(); ++i)
            EXPECT_TRUE(datas[i].size() > per_file - 128);
        EXPECT_EQ(files.size(), (size_t)((total + per_file - 1) / per_file));
        for (auto &f : files) EXPECT_TRUE(f.find(".next") == std::string::npos);
        check_all_once(datas, c.T, c.S);
        check_per_thread_order(datas, c.T);
    }
}

// 10. shutdown with K prepared-but-unused regions: exact file length, and a
// prepared-but-unused <path>.next is removed.
TEST(shutdown_with_prepared_regions) {
    const size_t region = 64 << 10;
    {
        std::string d = fresh_dir("la_shut");
        auto b = make_mmap_file_backend(opts(d, region, FullPolicy::Drop));
        ASSERT_TRUE(b != nullptr);
        std::string expect;
        for (int i = 0; i < 100; ++i) {
            std::string l = make_line(0, i);
            EXPECT_TRUE(b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO));
            expect += l;
        }
        b->flush();  // acting as the worker: queue K = 2 regions
        EXPECT_EQ(file_size(d + "/app.log"), (off_t)(3 * region));
        b->flush();  // idempotent: queue already full
        EXPECT_EQ(file_size(d + "/app.log"), (off_t)(3 * region));
        b->shutdown();
        std::string got = read_file(d + "/app.log");
        EXPECT_EQ(got.size(), expect.size());
        EXPECT_TRUE(got == expect);
    }
    {
        // Prepared region in <path>.next (max_size = 2 regions), never used.
        std::string d = fresh_dir("la_shut_next");
        auto b = make_mmap_file_backend(opts(d, region, FullPolicy::Drop, 2 * region));
        ASSERT_TRUE(b != nullptr);
        std::string expect;
        for (int i = 0; i < 100; ++i) {
            std::string l = make_line(0, i);
            EXPECT_TRUE(b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO));
            expect += l;
        }
        b->flush();
        EXPECT_EQ(file_size(d + "/app.log"), (off_t)(2 * region));
        EXPECT_EQ(file_size(d + "/app.log.next"), (off_t)region);
        b->shutdown();
        auto files = list_dir(d);
        EXPECT_EQ(files.size(), (size_t)1);
        std::string got = read_file(d + "/app.log");
        EXPECT_EQ(got.size(), expect.size());
        EXPECT_TRUE(got == expect);
    }
}

// 7. informational micro-benchmark.
TEST(bench_append_latency) {
    for (int T : {1, 4}) {
        std::string d = fresh_dir("bench");
        auto b = make_mmap_file_backend(opts(d, 16 << 20, FullPolicy::Drop));
        ASSERT_TRUE(b != nullptr);
        const int S = 200000;
        std::vector<std::vector<uint32_t>> lat((size_t)T);
        {
            Worker w(b.get());
            std::vector<std::thread> ths;
            for (int t = 0; t < T; ++t)
                ths.emplace_back([&, t] {
                    std::string l = make_line(t, 12345);
                    l.resize(100, 'z');
                    l.back() = '\n';
                    auto &v = lat[(size_t)t];
                    v.reserve(S);
                    for (int s = 0; s < S; ++s) {
                        auto a = std::chrono::steady_clock::now();
                        b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
                        auto z = std::chrono::steady_clock::now();
                        v.push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        z - a).count());
                    }
                });
            for (auto &th : ths) th.join();
            w.halt();
        }
        b->shutdown();
        std::vector<uint32_t> all;
        for (auto &v : lat) all.insert(all.end(), v.begin(), v.end());
        std::sort(all.begin(), all.end());
        // Timer overhead baseline.
        std::vector<uint32_t> base;
        for (int i = 0; i < 100000; ++i) {
            auto a = std::chrono::steady_clock::now();
            auto z = std::chrono::steady_clock::now();
            base.push_back((uint32_t)std::chrono::duration_cast<std::chrono::nanoseconds>(z - a)
                               .count());
        }
        std::sort(base.begin(), base.end());
        std::printf("  %d thread(s), 100 B lines: p50 %u ns, p99 %u ns, p99.9 %u ns "
                    "(timer overhead p50 %u ns), dropped %llu\n",
                    T, all[all.size() / 2], all[all.size() * 99 / 100],
                    all[all.size() * 999 / 1000], base[base.size() / 2],
                    (unsigned long long)b->dropped());
    }
}

TINY_TEST_MAIN()
