// Tests for time-based rotation (file.rotate = daily|hourly), retention and
// gzip compression of rotated files (src/backend/archiver.*), for RotatingFile
// and the double-buffer backend. Compression tests need a zlib build
// (TBLOG_HAVE_ZLIB). Time is injected through FileOptions::now_fn; TZ is
// forced to UTC so expected names are fixed.
#include "backend/archiver.h"
#include "backend/rotating_file.h"
#include "internal.h"
#include "tiny_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef TBLOG_HAVE_ZLIB
#include <zlib.h>
#endif

using namespace tblog;

namespace {

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
const int kScale = 4;  // sanitizers are slow: shrink the big tests
#else
const int kScale = 1;
#endif

struct UtcTz {
    UtcTz() {
        ::setenv("TZ", "UTC", 1);
        ::tzset();
    }
} g_utc;

// 2030-01-01T00:00:00Z
const int64_t kBase = 1893456000;
const int64_t kDay = 86400;

std::atomic<int64_t> g_now{kBase};
int64_t fake_now() { return g_now.load(); }

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
    std::string d = base_dir() + "/rt_" + name;
    mkdirs(d);
    for (auto &f : list_dir(d)) ::unlink((d + "/" + f).c_str());
    return d;
}

bool exists(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
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

void write_file(const std::string &path, const std::string &data) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

void set_mtime(const std::string &p, int64_t sec) {
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)sec;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    ::utimensat(AT_FDCWD, p.c_str(), ts, 0);
}

#ifdef TBLOG_HAVE_ZLIB
// Decompresses a .gz file; expects a valid gzip stream.
std::string gunzip(const std::string &path) {
    std::string out;
    gzFile g = ::gzopen(path.c_str(), "rb");
    EXPECT_TRUE(g != nullptr);
    if (!g) return out;
    char buf[65536];
    int n;
    while ((n = ::gzread(g, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    EXPECT_TRUE(n == 0);
    EXPECT_TRUE(::gzclose(g) == Z_OK);
    return out;
}
#endif

// Fixed-size 100-byte line (for exact size-rotation arithmetic).
std::string fixed_line(int i) {
    char b[128];
    std::snprintf(b, sizeof b, "line %06d ", i);
    std::string s(b);
    s.resize(99, 'x');
    return s + "\n";
}

// Self-checking line: thread t, sequence s, period p (the writer's clock).
std::string make_line(int t, int s, int p) {
    char head[64];
    int n = std::snprintf(head, sizeof head, "t%d s%d p%d ", t, s, p);
    std::string l(head, (size_t)n);
    l.append((size_t)(s * 7 + t) % 53, (char)('a' + (s + t) % 26));
    return l + "|\n";
}

bool parse_line(const std::string &l, int *t, int *s, int *p) {
    if (std::sscanf(l.c_str(), "t%d s%d p%d ", t, s, p) != 3) return false;
    return make_line(*t, *s, *p) == l + "\n";
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

// Size-rotation index inside a period: "app.YYYY-MM-DD.N.log" -> N, bare -> 0.
int index_of(const std::string &name) {
    int idx = 0;
    if (name.size() > 15 && name[14] == '.' && std::sscanf(name.c_str() + 15, "%d", &idx) == 1)
        return idx;
    return 0;
}

// Day index (0 = kBase's day) of "app.YYYY-MM-DD[.N].log"; -1 if not one.
int day_of(const std::string &name) {
    int y, m, d;
    if (std::sscanf(name.c_str(), "app.%4d-%2d-%2d", &y, &m, &d) != 3) return -1;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    return (int)((::timegm(&t) - kBase) / kDay);
}

FileOptions file_opts(const std::string &dir, RotateMode mode, CompressMode cm,
                      uint64_t max_size = 1ull << 30, int max_files = 50) {
    FileOptions o;
    o.path = dir + "/app.log";
    o.max_size = max_size;
    o.max_files = max_files;
    o.rotate = mode;
    o.compress = cm;
    o.now_fn = fake_now;
    return o;
}

// Backend worker stand-in that counts its cycles.
struct Worker {
    Backend *b;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> cycles{0};
    std::thread th;
    explicit Worker(Backend *be) : b(be) {
        th = std::thread([this] {
            while (!stop.load()) {
                b->wait_for_work(std::chrono::milliseconds(2));
                b->flush();
                cycles.fetch_add(1);
            }
        });
    }
    ~Worker() { halt(); }
    void wait_cycles(uint64_t n) {
        uint64_t c0 = cycles.load();
        while (cycles.load() < c0 + n) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    void halt() {
        if (!th.joinable()) return;
        stop = true;
        b->wake();
        th.join();
    }
};

// Writers append self-checking lines tagged with the day the fake clock showed
// just before the append, until `stop`; per-thread counts are returned.
struct Writers {
    std::vector<std::thread> ths;
    std::vector<int> count;
    std::atomic<bool> stop{false};
    Writers(Backend *b, int T, int max_lines) : count((size_t)T, 0) {
        for (int t = 0; t < T; ++t)
            ths.emplace_back([this, b, t, max_lines] {
                for (int s = 0; s < max_lines && !stop.load(); ++s) {
                    int p = (int)((g_now.load() - kBase) / kDay);
                    std::string l = make_line(t, s, p);
                    if (!b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO)) break;
                    count[(size_t)t] = s + 1;
                    if (s % 64 == 63) std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            });
    }
    void join() {
        stop = true;
        for (auto &th : ths) th.join();
    }
};

// Checks every rotated/live file in `d`: each line once, whole, and the day
// of the file it landed in vs the day the writer saw: never in an earlier
// day's file (the worker reads the clock after taking the data), at most one
// day later. Per thread, files are non-decreasing in time. Returns the number
// of days seen.
int check_rollover(const std::string &d, const std::vector<int> &count, int last_day,
                   bool exact) {
    const int T = (int)count.size();
    std::vector<std::vector<int>> seen((size_t)T);
    for (int t = 0; t < T; ++t) seen[(size_t)t].assign((size_t)count[(size_t)t], -1);
    size_t bad = 0, dup = 0, off = 0, order = 0, extra = 0;
    std::vector<int> last_file_day((size_t)T, -1), last_s((size_t)T, -1);
    std::vector<std::string> files = list_dir(d);
    // Process files oldest first: rotated ones by name, the live file last.
    struct F { int day, idx; std::string name; };
    std::vector<F> ordered;
    std::vector<int> day_list;
    for (auto &f : files) {
        F x;
        x.name = f;
        if (f == "app.log") {
            x.day = last_day;
            x.idx = 1 << 30;  // newest
        } else {
            x.day = day_of(f);
            x.idx = index_of(f);
            EXPECT_TRUE(x.day >= 0);
            if (x.day < 0) continue;
        }
        ordered.push_back(x);
        if (std::find(day_list.begin(), day_list.end(), x.day) == day_list.end())
            day_list.push_back(x.day);
    }
    std::sort(ordered.begin(), ordered.end(), [](const F &a, const F &b) {
        return a.day != b.day ? a.day < b.day : a.idx < b.idx;
    });
    const int days = (int)day_list.size();
    for (auto &f : ordered) {
        std::pair<int, std::string> of(f.day, f.name);
        std::string data = read_file(d + "/" + of.second);
        EXPECT_TRUE(data.find('\0') == std::string::npos);
        EXPECT_TRUE(data.empty() || data.back() == '\n');
        for (auto &l : split_lines(data)) {
            int t, s, p;
            if (!parse_line(l, &t, &s, &p) || t < 0 || t >= T) { ++bad; continue; }
            if (s >= count[(size_t)t]) { ++extra; continue; }
            if (seen[(size_t)t][(size_t)s] >= 0) ++dup;
            seen[(size_t)t][(size_t)s] = of.first;
            if (exact ? of.first != p : (of.first < p || of.first > p + 1)) ++off;
            if (of.first < last_file_day[(size_t)t] || s < last_s[(size_t)t]) ++order;
            last_file_day[(size_t)t] = of.first;
            last_s[(size_t)t] = s;
        }
    }
    size_t missing = 0;
    for (auto &v : seen) missing += (size_t)std::count(v.begin(), v.end(), -1);
    EXPECT_EQ(bad, (size_t)0);
    EXPECT_EQ(extra, (size_t)0);
    EXPECT_EQ(dup, (size_t)0);
    EXPECT_EQ(missing, (size_t)0);
    EXPECT_EQ(off, (size_t)0);
    EXPECT_EQ(order, (size_t)0);
    return days;
}

}  // namespace

// ------------------------------------------------------------ naming / periods
TEST(archiver_names_and_periods) {
    Archiver a;
    FileOptions o;
    o.path = "/x/logs/app.log";
    o.rotate = RotateMode::Daily;
    a.configure(o);
    const int64_t t = kBase + 13 * 3600 + 45 * 60 + 10;  // 2030-01-01 13:45:10
    EXPECT_EQ(a.period_label(t), std::string("2030-01-01"));
    EXPECT_EQ(a.period_start(t), kBase);
    EXPECT_EQ(a.next_boundary(t), kBase + kDay);
    EXPECT_EQ(a.next_boundary(kBase + kDay - 1), kBase + kDay);
    EXPECT_EQ(a.next_boundary(kBase + kDay), kBase + 2 * kDay);
    EXPECT_EQ(a.time_name("2030-01-01", 0), std::string("/x/logs/app.2030-01-01.log"));
    EXPECT_EQ(a.time_name("2030-01-01", 2), std::string("/x/logs/app.2030-01-01.2.log"));
    EXPECT_EQ(a.size_name(3), std::string("/x/logs/app.3.log"));
    EXPECT_TRUE(a.time_mode());

    o.rotate = RotateMode::Hourly;
    a.configure(o);
    EXPECT_EQ(a.period_label(t), std::string("2030-01-01T13"));
    EXPECT_EQ(a.period_start(t), kBase + 13 * 3600);
    EXPECT_EQ(a.next_boundary(t), kBase + 14 * 3600);
    EXPECT_EQ(a.time_name(a.period_label(t), 1), std::string("/x/logs/app.2030-01-01T13.1.log"));

    o.path = "/x/noext";
    a.configure(o);
    EXPECT_EQ(a.time_name("2030-01-01T13", 0), std::string("/x/noext.2030-01-01T13"));

    o.rotate = RotateMode::Size;
    a.configure(o);
    EXPECT_FALSE(a.time_mode());
    EXPECT_EQ(a.next_boundary(t), INT64_MAX);
}

// ------------------------------------------------------------ RotatingFile
TEST(rotating_daily_basic) {
    std::string d = fresh_dir("daily_basic");
    g_now = kBase + 10 * 3600;
    RotatingFile rf;
    ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::None)));
    std::string a = fixed_line(1) + fixed_line(2);
    rf.write_all(a.data(), a.size());
    rf.check_time();  // same day: nothing
    EXPECT_EQ(list_dir(d).size(), (size_t)1);
    g_now = kBase + kDay;  // exactly midnight
    rf.check_time();
    EXPECT_EQ(read_file(d + "/app.2030-01-01.log"), a);
    std::string b = fixed_line(3);
    rf.write_all(b.data(), b.size());
    g_now = kBase + 2 * kDay - 1;  // 23:59:59 the same day
    rf.check_time();
    EXPECT_EQ(list_dir(d).size(), (size_t)2);
    g_now = kBase + 3 * kDay + 5;  // a day with no logs is skipped
    rf.check_time();
    EXPECT_EQ(read_file(d + "/app.2030-01-02.log"), b);
    EXPECT_EQ(read_file(d + "/app.log"), std::string());
    g_now = kBase + 4 * kDay;  // empty live file: no empty rotated file
    rf.check_time();
    rf.close();
    std::vector<std::string> expect = {"app.2030-01-01.log", "app.2030-01-02.log", "app.log"};
    EXPECT_TRUE(list_dir(d) == expect);
}

TEST(rotating_size_within_period_and_hourly) {
    std::string d = fresh_dir("size_time");
    g_now = kBase + 5;
    RotatingFile rf;
    ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::None, 1000)));
    for (int i = 1; i <= 25; ++i) {  // 10 lines per file
        std::string l = fixed_line(i);
        rf.write_all(l.data(), l.size());
    }
    g_now = kBase + kDay + 1;
    rf.check_time();
    std::string l = fixed_line(26);
    rf.write_all(l.data(), l.size());
    rf.close();
    std::string e0, e1, e2;
    for (int i = 1; i <= 10; ++i) e0 += fixed_line(i);
    for (int i = 11; i <= 20; ++i) e1 += fixed_line(i);
    for (int i = 21; i <= 25; ++i) e2 += fixed_line(i);
    EXPECT_EQ(read_file(d + "/app.2030-01-01.log"), e0);    // oldest piece of the day
    EXPECT_EQ(read_file(d + "/app.2030-01-01.1.log"), e1);
    EXPECT_EQ(read_file(d + "/app.2030-01-01.2.log"), e2);  // closed by the day change
    EXPECT_EQ(read_file(d + "/app.log"), fixed_line(26));
    EXPECT_EQ(list_dir(d).size(), (size_t)4);

    // Hourly.
    std::string h = fresh_dir("hourly");
    g_now = kBase + 13 * 3600 + 59 * 60;
    RotatingFile hf;
    ASSERT_TRUE(hf.open(file_opts(h, RotateMode::Hourly, CompressMode::None)));
    hf.write_all(l.data(), l.size());
    g_now = kBase + 14 * 3600;
    hf.check_time();
    hf.write_all(l.data(), l.size());
    g_now = kBase + 15 * 3600 + 1;
    hf.check_time();
    hf.close();
    std::vector<std::string> expect = {"app.2030-01-01T13.log", "app.2030-01-01T14.log",
                                       "app.log"};
    EXPECT_TRUE(list_dir(h) == expect);
}

TEST(rotating_restart_rotates_stale_live_file) {
    std::string d = fresh_dir("restart");
    write_file(d + "/app.log", fixed_line(1));
    set_mtime(d + "/app.log", kBase - 2 * kDay + 100);  // 2029-12-30
    g_now = kBase + 50;
    RotatingFile rf;
    ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::None)));
    EXPECT_EQ(read_file(d + "/app.2029-12-30.log"), fixed_line(1));
    EXPECT_EQ(rf.size(), 0ull);
    // A file of the current period is appended to.
    std::string l = fixed_line(2);
    rf.write_all(l.data(), l.size());
    rf.close();
    set_mtime(d + "/app.log", kBase + 60);
    RotatingFile r3;
    ASSERT_TRUE(r3.open(file_opts(d, RotateMode::Daily, CompressMode::None)));
    EXPECT_EQ(r3.size(), (unsigned long long)l.size());
    r3.close();
}

#ifdef TBLOG_HAVE_ZLIB
TEST(rotating_retention_counts_compressed) {
    std::string d = fresh_dir("retention");
    // Older rotated files from a previous run: one compressed, one not, plus
    // an unrelated file that must survive.
    write_file(d + "/app.2029-12-01.log", "old1\n");
    EXPECT_EQ(::system(("gzip -n -f '" + d + "/app.2029-12-01.log'").c_str()), 0);
    write_file(d + "/app.2029-12-02.log", "old2\n");
    write_file(d + "/app.2029-12-02.log.gz.tmp", "partial");  // interrupted compression
    write_file(d + "/other.log", "keep\n");
    EXPECT_TRUE(exists(d + "/app.2029-12-01.log.gz"));
    g_now = kBase;
    RotatingFile rf;
    // max_files = 3: live file + 2 newest rotated ones.
    ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::Gzip, 1ull << 30, 3)));
    std::vector<std::string> data;
    for (int day = 0; day < 4; ++day) {
        g_now = kBase + day * kDay + 7;
        rf.check_time();
        std::string s = fixed_line(day) + fixed_line(100 + day);
        rf.write_all(s.data(), s.size());
        data.push_back(s);
    }
    rf.close();  // waits for the compressions
    std::vector<std::string> expect = {"app.2030-01-02.log.gz", "app.2030-01-03.log.gz",
                                       "app.log", "other.log"};
    EXPECT_TRUE(list_dir(d) == expect);
    EXPECT_EQ(gunzip(d + "/app.2030-01-02.log.gz"), data[1]);
    EXPECT_EQ(gunzip(d + "/app.2030-01-03.log.gz"), data[2]);
    EXPECT_EQ(read_file(d + "/app.log"), data[3]);
}

TEST(rotating_compress_roundtrip_daily) {
    std::string d = fresh_dir("gz_daily");
    g_now = kBase + 1;
    RotatingFile rf;
    ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::Gzip)));
    std::string all;
    for (int i = 0; i < 3000; ++i) {
        std::string l = make_line(i % 7, i, 0);
        all += l;
        rf.write_all(l.data(), l.size());
    }
    g_now = kBase + kDay;
    rf.check_time();
    std::string l = fixed_line(1);
    rf.write_all(l.data(), l.size());
    rf.close();
    std::vector<std::string> expect = {"app.2030-01-01.log.gz", "app.log"};
    EXPECT_TRUE(list_dir(d) == expect);
    std::string out = gunzip(d + "/app.2030-01-01.log.gz");
    EXPECT_EQ(out.size(), all.size());
    EXPECT_TRUE(out == all);
    EXPECT_EQ(read_file(d + "/app.log"), l);
}

// Size mode + gzip: the index shift carries .gz / in-flight .gz.tmp / queued jobs.
TEST(rotating_compress_size_mode_shift) {
    std::string d = fresh_dir("gz_size");
    RotatingFile rf;
    ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Size, CompressMode::Gzip, 1000, 4)));
    for (int i = 1; i <= 60; ++i) {
        std::string l = fixed_line(i);
        rf.write_all(l.data(), l.size());
        if (i % 7 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rf.close();
    std::vector<std::string> expect = {"app.1.log.gz", "app.2.log.gz", "app.3.log.gz",
                                       "app.log"};
    EXPECT_TRUE(list_dir(d) == expect);
    for (int k = 1; k <= 3; ++k) {
        std::string e;
        for (int i = 51 - 10 * k; i <= 60 - 10 * k; ++i) e += fixed_line(i);
        EXPECT_TRUE(gunzip(d + "/app." + std::to_string(k) + ".log.gz") == e);
    }
}

// A crash during compression leaves name.gz.tmp next to the intact original;
// a crash after the rename leaves both name and a complete name.gz.
TEST(compress_crash_recovery) {
    std::string d = fresh_dir("gz_crash");
    std::string x, y;
    for (int i = 0; i < 500; ++i) x += fixed_line(i);
    for (int i = 500; i < 600; ++i) y += fixed_line(i);
    write_file(d + "/app.2030-01-01.log", x);
    write_file(d + "/app.2030-01-01.log.gz.tmp", std::string("\x1f\x8b\x08\0garbage", 11));
    write_file(d + "/app.2030-01-02.log", y);
    EXPECT_EQ(::system(("gzip -n -k -f '" + d + "/app.2030-01-02.log'").c_str()), 0);
    ASSERT_TRUE(exists(d + "/app.2030-01-02.log.gz") && exists(d + "/app.2030-01-02.log"));
    g_now = kBase + 3 * kDay;
    {
        // Compression off: only the recovery runs.
        RotatingFile rf;
        ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::None)));
        rf.close();
        std::vector<std::string> expect = {"app.2030-01-01.log", "app.2030-01-02.log.gz",
                                           "app.log"};
        EXPECT_TRUE(list_dir(d) == expect);
        EXPECT_TRUE(read_file(d + "/app.2030-01-01.log") == x);  // original kept intact
    }
    // Interrupted again, then a run with compression on finishes the job.
    write_file(d + "/app.2030-01-01.log.gz.tmp", "partial");
    {
        RotatingFile rf;
        ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::Gzip)));
        rf.close();
    }
    std::vector<std::string> expect = {"app.2030-01-01.log.gz", "app.2030-01-02.log.gz",
                                       "app.log"};
    EXPECT_TRUE(list_dir(d) == expect);
    EXPECT_TRUE(gunzip(d + "/app.2030-01-01.log.gz") == x);
    EXPECT_TRUE(gunzip(d + "/app.2030-01-02.log.gz") == y);
}
#else
// Without zlib, file.compress = gzip warns once and behaves as none.
TEST(compress_without_zlib_is_disabled) {
    std::string d = fresh_dir("gz_off");
    g_now = kBase + 1;
    RotatingFile rf;
    std::fprintf(stderr, "(expected 'built without zlib' warning follows)\n");
    ASSERT_TRUE(rf.open(file_opts(d, RotateMode::Daily, CompressMode::Gzip)));
    std::string a = fixed_line(1);
    rf.write_all(a.data(), a.size());
    g_now = kBase + kDay;
    rf.check_time();
    std::string b = fixed_line(2);
    rf.write_all(b.data(), b.size());
    rf.close();
    std::vector<std::string> expect = {"app.2030-01-01.log", "app.log"};
    EXPECT_TRUE(list_dir(d) == expect);
    EXPECT_FALSE(exists(d + "/app.2030-01-01.log.gz"));
    EXPECT_EQ(read_file(d + "/app.2030-01-01.log"), a);
    EXPECT_EQ(read_file(d + "/app.log"), b);
}
#endif

// ------------------------------------------------------------ double buffer backend
TEST(dbuf_time_rotation) {
    std::string d = fresh_dir("dbuf_time");
    g_now = kBase + 100;
    BufferOptions o;
    o.file = file_opts(d, RotateMode::Daily, CompressMode::None);
    auto b = make_double_buffer_backend(o);
    ASSERT_TRUE(b != nullptr);
    std::string a = make_line(0, 0, 0), c = make_line(0, 1, 1);
    b->append(a.data(), (uint32_t)a.size(), TBLOG_LV_INFO);
    b->flush();  // written during day 0
    g_now = kBase + kDay + 1;
    b->flush();  // the worker notices the new day
    b->append(c.data(), (uint32_t)c.size(), TBLOG_LV_INFO);
    b->shutdown();
    EXPECT_EQ(read_file(d + "/app.2030-01-01.log"), a);
    EXPECT_EQ(read_file(d + "/app.log"), c);
}

namespace {
std::unique_ptr<Backend> make_v1(const std::string &d, CompressMode cm, uint64_t max_size) {
    BufferOptions o;
    o.file = file_opts(d, RotateMode::Daily, cm, max_size);
    o.buffer_size = 256u << 10;
    o.policy = FullPolicy::Block;
    return make_double_buffer_backend(o);
}

// Quiesced phases: lines of day k are all appended (by T threads) and taken
// by the worker before the clock moves to day k + 1, so every file must hold
// exactly its own day's lines.
void run_exact(Backend *b, const std::string &d, int days, int S = 1500 / kScale) {
    const int T = 4;
    std::vector<int> count((size_t)T, 0);
    {
        Worker w(b);
        int s0 = 0;
        for (int day = 0; day < days; ++day) {
            g_now = kBase + day * kDay + 3600;
            w.wait_cycles(2);
            std::vector<std::thread> ths;
            for (int t = 0; t < T; ++t)
                ths.emplace_back([&, t] {
                    for (int s = s0; s < s0 + S; ++s) {
                        std::string l = make_line(t, s, day);
                        b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
                    }
                });
            for (auto &th : ths) th.join();
            s0 += S;
            w.wait_cycles(2);  // the worker took everything
        }
        w.halt();
        for (auto &c : count) c = s0;
    }
    b->shutdown();
    EXPECT_EQ(b->dropped(), 0ull);
    EXPECT_EQ(check_rollover(d, count, days - 1, true), days);
}

// Writers run continuously while the day changes several times.
void run_concurrent(Backend *b, const std::string &d) {
    const int T = 4, days = 4;
    {
        Worker w(b);
        g_now = kBase + 10;
        Writers writers(b, T, 1 << 30);
        for (int day = 1; day < days; ++day) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            g_now = kBase + day * kDay;
            w.wait_cycles(3);  // the rollover happened before the next one
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        writers.join();
        w.halt();
        b->shutdown();
        EXPECT_EQ(b->dropped(), 0ull);
        int total = 0;
        for (int c : writers.count) total += c;
        std::printf("  %d lines over %d days\n", total, days);
        EXPECT_EQ(check_rollover(d, writers.count, days - 1, false), days);
    }
}
}  // namespace

TEST(v1_rollover_exact) {
    std::string d = fresh_dir("v1_exact");
    g_now = kBase;
    auto b = make_v1(d, CompressMode::None, 1ull << 30);
    ASSERT_TRUE(b != nullptr);
    run_exact(b.get(), d, 3);
}

TEST(v1_rollover_concurrent) {
    std::string d = fresh_dir("v1_conc");
    g_now = kBase;
    auto b = make_v1(d, CompressMode::None, 1ull << 30);
    ASSERT_TRUE(b != nullptr);
    run_concurrent(b.get(), d);
}

// max_size (64 KB) caps files inside a day: app.<day>.log, app.<day>.1.log,
// ... with only that day's lines.
TEST(v1_size_and_time_naming) {
    std::string d = fresh_dir("v1_size_time");
    g_now = kBase;
    auto b = make_v1(d, CompressMode::None, 64u << 10);
    ASSERT_TRUE(b != nullptr);
    run_exact(b.get(), d, 2, 1500);  // ~270 KB a day
    auto files = list_dir(d);
    for (auto &f : files) std::printf("  %s\n", f.c_str());
    EXPECT_TRUE(std::find(files.begin(), files.end(), "app.2030-01-01.log") != files.end());
    EXPECT_TRUE(std::find(files.begin(), files.end(), "app.2030-01-01.1.log") != files.end());
    EXPECT_TRUE(std::find(files.begin(), files.end(), "app.2030-01-02.log") != files.end());
    for (auto &f : files) {
        struct stat st;
        EXPECT_TRUE(::stat((d + "/" + f).c_str(), &st) == 0 && st.st_size <= (64 << 10));
    }
}

#ifdef TBLOG_HAVE_ZLIB
TEST(v1_size_and_time_compressed) {
    std::string d = fresh_dir("v1_size_time_gz");
    g_now = kBase;
    auto b = make_v1(d, CompressMode::Gzip, 64u << 10);
    ASSERT_TRUE(b != nullptr);
    const int T = 2, S = 2000;  // ~160 KB
    std::string day0;
    {
        Worker w(b.get());
        for (int s = 0; s < S; ++s)
            for (int t = 0; t < T; ++t) {
                std::string l = make_line(t, s, 0);
                b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
            }
        w.wait_cycles(2);
        g_now = kBase + kDay;
        w.wait_cycles(2);
        std::string l = make_line(0, S, 1);
        b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
        w.halt();
    }
    b->shutdown();  // waits for the compressions
    auto files = list_dir(d);
    size_t lines = 0;
    for (auto &f : files) {
        std::printf("  %s\n", f.c_str());
        if (f == "app.log") {
            EXPECT_EQ(read_file(d + "/" + f), make_line(0, S, 1));
            continue;
        }
        EXPECT_TRUE(f.size() > 3 && f.substr(f.size() - 3) == ".gz");
        EXPECT_EQ(day_of(f), 0);
        std::string data = gunzip(d + "/" + f);
        EXPECT_TRUE(data.size() <= (64u << 10));
        for (auto &l : split_lines(data)) {
            int t, s, p;
            EXPECT_TRUE(parse_line(l, &t, &s, &p) && p == 0);
            ++lines;
        }
    }
    EXPECT_EQ(lines, (size_t)(T * S));
    EXPECT_TRUE(std::find(files.begin(), files.end(), "app.2030-01-01.1.log.gz") != files.end());
}

TEST(v1_compress_roundtrip) {
    std::string d = fresh_dir("v1_gz");
    g_now = kBase + 77;
    auto b = make_v1(d, CompressMode::Gzip, 1ull << 30);
    ASSERT_TRUE(b != nullptr);
    std::string day0, day1;
    {
        Worker w(b.get());
        for (int s = 0; s < 3000 / kScale; ++s) {
            std::string l = make_line(1, s, 0);
            day0 += l;
            b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
        }
        w.wait_cycles(2);
        g_now = kBase + kDay + 3;
        w.wait_cycles(2);
        for (int s = 0; s < 100; ++s) {
            std::string l = make_line(2, s, 1);
            day1 += l;
            b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
        }
        w.halt();
    }
    b->shutdown();
    std::vector<std::string> expect = {"app.2030-01-01.log.gz", "app.log"};
    EXPECT_TRUE(list_dir(d) == expect);
    EXPECT_TRUE(gunzip(d + "/app.2030-01-01.log.gz") == day0);
    EXPECT_TRUE(read_file(d + "/app.log") == day1);
}

TEST(v1_restart_rotates_stale_live_file) {
    std::string d = fresh_dir("v1_restart");
    write_file(d + "/app.log", fixed_line(1));
    set_mtime(d + "/app.log", kBase - kDay + 5);  // 2029-12-31
    g_now = kBase + 50;
    auto b = make_v1(d, CompressMode::Gzip, 1ull << 30);
    ASSERT_TRUE(b != nullptr);
    std::string l = fixed_line(2);
    b->append(l.data(), (uint32_t)l.size(), TBLOG_LV_INFO);
    b->shutdown();
    std::vector<std::string> expect = {"app.2029-12-31.log.gz", "app.log"};
    EXPECT_TRUE(list_dir(d) == expect);
    EXPECT_TRUE(gunzip(d + "/app.2029-12-31.log.gz") == fixed_line(1));
    EXPECT_EQ(read_file(d + "/app.log"), l);
}
#endif

TINY_TEST_MAIN()
