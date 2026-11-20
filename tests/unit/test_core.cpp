// Unit tests for src/core: levels/modules, thread context, formatter.
#include "core/formatter.h"
#include "core/levels.h"
#include "core/thread_context.h"
#include "tiny_test.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

using namespace tblog;

namespace {

std::string fmt_line(const Formatter &f, size_t cap, const Record &r, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
std::string fmt_line(const Formatter &f, size_t cap, const Record &r, const char *fmt, ...) {
    std::vector<char> buf(cap + 16, '\x7f');
    va_list ap;
    va_start(ap, fmt);
    size_t n = f.format(buf.data(), cap, r, fmt, ap);
    va_end(ap);
    EXPECT_TRUE(n <= cap);
    for (size_t i = cap; i < buf.size(); ++i) EXPECT_TRUE(buf[i] == '\x7f');  // no overrun
    return std::string(buf.data(), n);
}

Record make_rec(uint64_t ts = 0) {
    Record r;
    r.ts_ns = ts ? ts : now_ns();
    r.tid = 4242;
    r.level = TBLOG_LV_WARN;
    r.module = "net";
    r.file = "/a/b/c/source.cpp";
    r.line = 123;
    r.func = "do_thing";
    return r;
}

// Fixed timestamp: local time computed with localtime_r for comparison.
const uint64_t kTs = 1700000000ull * 1000000000ull + 123456789ull;
std::string expect_date_time(uint64_t ts, bool date) {
    time_t t = (time_t)(ts / 1000000000ull);
    struct tm tm;
    localtime_r(&t, &tm);
    char b[32];
    if (date) strftime(b, sizeof b, "%Y-%m-%d", &tm);
    else strftime(b, sizeof b, "%H:%M:%S", &tm);
    return b;
}

}  // namespace

// ---------------------------------------------------------------- levels

TEST(level_name_all) {
    EXPECT_EQ(std::string(level_name(TBLOG_LV_TRACE)), std::string("TRACE"));
    EXPECT_EQ(std::string(level_name(TBLOG_LV_DEBUG)), std::string("DEBUG"));
    EXPECT_EQ(std::string(level_name(TBLOG_LV_INFO)), std::string("INFO"));
    EXPECT_EQ(std::string(level_name(TBLOG_LV_WARN)), std::string("WARN"));
    EXPECT_EQ(std::string(level_name(TBLOG_LV_ERROR)), std::string("ERROR"));
    EXPECT_EQ(std::string(level_name(TBLOG_LV_FATAL)), std::string("FATAL"));
    EXPECT_EQ(std::string(level_name(TBLOG_LV_OFF)), std::string("OFF"));
    EXPECT_TRUE(level_name((tblog_level)99) != nullptr);
}

TEST(parse_level_cases) {
    tblog_level lv = TBLOG_LV_INFO;
    EXPECT_TRUE(parse_level("trace", &lv)); EXPECT_EQ(lv, TBLOG_LV_TRACE);
    EXPECT_TRUE(parse_level("DEBUG", &lv)); EXPECT_EQ(lv, TBLOG_LV_DEBUG);
    EXPECT_TRUE(parse_level("Info", &lv)); EXPECT_EQ(lv, TBLOG_LV_INFO);
    EXPECT_TRUE(parse_level("wArN", &lv)); EXPECT_EQ(lv, TBLOG_LV_WARN);
    EXPECT_TRUE(parse_level("warning", &lv)); EXPECT_EQ(lv, TBLOG_LV_WARN);
    EXPECT_TRUE(parse_level("ERROR", &lv)); EXPECT_EQ(lv, TBLOG_LV_ERROR);
    EXPECT_TRUE(parse_level("fatal", &lv)); EXPECT_EQ(lv, TBLOG_LV_FATAL);
    EXPECT_TRUE(parse_level("OFF", &lv)); EXPECT_EQ(lv, TBLOG_LV_OFF);
    lv = TBLOG_LV_FATAL;
    EXPECT_FALSE(parse_level("", &lv));
    EXPECT_FALSE(parse_level("inf", &lv));
    EXPECT_FALSE(parse_level("infoo", &lv));
    EXPECT_FALSE(parse_level(" info", &lv));
    EXPECT_FALSE(parse_level("verbose", &lv));
    EXPECT_FALSE(parse_level(nullptr, &lv));
    EXPECT_EQ(lv, TBLOG_LV_FATAL);  // untouched on failure
}

TEST(module_get_stable_and_root) {
    reset_levels_for_test();
    tblog_module *root1 = tblog_module_get(nullptr);
    tblog_module *root2 = tblog_module_get("");
    EXPECT_TRUE(root1 == root2);
    EXPECT_EQ(std::string(root1->name), std::string(""));
    EXPECT_EQ(tblog_module_level(root1), TBLOG_LV_INFO);
    tblog_module *a = tblog_module_get("alpha");
    tblog_module *b = tblog_module_get("beta");
    EXPECT_TRUE(a != b && a != root1);
    EXPECT_TRUE(tblog_module_get("alpha") == a);
    EXPECT_EQ(std::string(a->name), std::string("alpha"));
    EXPECT_EQ(tblog_module_level(a), TBLOG_LV_INFO);
    // truncation to 45 chars
    std::string longname(80, 'x');
    tblog_module *l = tblog_module_get(longname.c_str());
    EXPECT_EQ(std::strlen(l->name), (size_t)45);
    EXPECT_TRUE(tblog_module_get(longname.substr(0, 45).c_str()) == l);
    EXPECT_TRUE(tblog_module_get(longname.substr(0, 50).c_str()) == l);
}

TEST(module_get_concurrent) {
    std::vector<std::thread> th;
    std::vector<tblog_module *> got(8 * 20);
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&, t] {
            for (int i = 0; i < 20; ++i) {
                std::string n = "conc" + std::to_string(i);
                got[t * 20 + i] = tblog_module_get(n.c_str());
                (void)tblog_module_level(got[t * 20 + i]);
            }
        });
    for (auto &x : th) x.join();
    for (int t = 1; t < 8; ++t)
        for (int i = 0; i < 20; ++i) EXPECT_TRUE(got[t * 20 + i] == got[i]);
}

TEST(set_level_root_propagation) {
    reset_levels_for_test();
    tblog_module *root = tblog_module_get(nullptr);
    tblog_module *a = tblog_module_get("prop_a");
    tblog_module *b = tblog_module_get("prop_b");
    tblog_set_level("prop_b", TBLOG_LV_ERROR);
    EXPECT_EQ(tblog_module_level(b), TBLOG_LV_ERROR);
    EXPECT_EQ(b->explicit_, 1);
    tblog_set_level(nullptr, TBLOG_LV_DEBUG);
    EXPECT_EQ(tblog_module_level(root), TBLOG_LV_DEBUG);
    EXPECT_EQ(tblog_module_level(a), TBLOG_LV_DEBUG);
    EXPECT_EQ(tblog_module_level(b), TBLOG_LV_ERROR);  // explicit wins
    tblog_set_level("", TBLOG_LV_TRACE);
    EXPECT_EQ(tblog_get_level(nullptr), TBLOG_LV_TRACE);
    EXPECT_EQ(tblog_get_level("prop_a"), TBLOG_LV_TRACE);
    EXPECT_EQ(tblog_get_level("prop_b"), TBLOG_LV_ERROR);
    // new module inherits current root level
    EXPECT_EQ(tblog_module_level(tblog_module_get("prop_new")), TBLOG_LV_TRACE);
    // unknown module -> root level
    EXPECT_EQ(tblog_get_level("never_created_xyz"), TBLOG_LV_TRACE);
    // set_level on a not-yet-existing module creates it
    tblog_set_level("prop_c", TBLOG_LV_FATAL);
    EXPECT_EQ(tblog_module_level(tblog_module_get("prop_c")), TBLOG_LV_FATAL);
    reset_levels_for_test();
    EXPECT_EQ(tblog_module_level(b), TBLOG_LV_INFO);
    EXPECT_EQ(b->explicit_, 0);
    tblog_set_level(nullptr, TBLOG_LV_WARN);
    EXPECT_EQ(tblog_module_level(b), TBLOG_LV_WARN);
    reset_levels_for_test();
}

TEST(module_overflow_returns_root) {
    tblog_module *root = tblog_module_get(nullptr);
    tblog_module *last = nullptr;
    for (int i = 0; i < 300; ++i) {
        std::string n = "ovf" + std::to_string(i);
        last = tblog_module_get(n.c_str());
    }
    EXPECT_TRUE(last == root);
    EXPECT_TRUE(tblog_module_get("prop_a") != root);  // existing ones still found
    reset_levels_for_test();
}

// ---------------------------------------------------------------- thread ctx

TEST(thread_ctx_tid) {
    ThreadContext &c = thread_ctx();
    EXPECT_TRUE(c.tid != 0);
    EXPECT_TRUE(&thread_ctx() == &c);
    uint32_t other = 0;
    ThreadContext *op = nullptr;
    std::thread t([&] { other = thread_ctx().tid; op = &thread_ctx(); });
    t.join();
    EXPECT_TRUE(other != 0 && other != c.tid);
    EXPECT_TRUE(op != &c);
    uint64_t a = now_ns();
    EXPECT_TRUE(a > 1600000000ull * 1000000000ull);
    uint64_t b = now_ns();
    EXPECT_TRUE(b - a < 10ull * 1000000000ull);  // monotonic-ish within the test
}

// ---------------------------------------------------------------- formatter

TEST(fmt_every_token) {
    Record r = make_rec(kTs);
    std::string D = expect_date_time(kTs, true), T = expect_date_time(kTs, false);
    EXPECT_EQ(fmt_line(Formatter("%D"), 256, r, "x"), D + "\n");
    EXPECT_EQ(fmt_line(Formatter("%T"), 256, r, "x"), T + "\n");
    EXPECT_EQ(fmt_line(Formatter("%us"), 256, r, "x"), std::string("123456\n"));
    EXPECT_EQ(fmt_line(Formatter("%ms"), 256, r, "x"), std::string("123\n"));
    EXPECT_EQ(fmt_line(Formatter("%L"), 256, r, "x"), std::string("WARN\n"));
    EXPECT_EQ(fmt_line(Formatter("%t"), 256, r, "x"), std::string("4242\n"));
    EXPECT_EQ(fmt_line(Formatter("%n"), 256, r, "x"), std::string("net\n"));
    EXPECT_EQ(fmt_line(Formatter("%f"), 256, r, "x"), std::string("source.cpp\n"));
    EXPECT_EQ(fmt_line(Formatter("%l"), 256, r, "x"), std::string("123\n"));
    EXPECT_EQ(fmt_line(Formatter("%F"), 256, r, "x"), std::string("do_thing\n"));
    EXPECT_EQ(fmt_line(Formatter("%m"), 256, r, "v=%d s=%s", 7, "ok"), std::string("v=7 s=ok\n"));
    EXPECT_EQ(fmt_line(Formatter("%%"), 256, r, "x"), std::string("%\n"));
    EXPECT_EQ(fmt_line(Formatter("a%qb%"), 256, r, "x"), std::string("a%qb%\n"));  // unknown verbatim
    EXPECT_EQ(fmt_line(Formatter("%u%x"), 256, r, "x"), std::string("%u%x\n"));
    EXPECT_EQ(fmt_line(Formatter(""), 256, r, "x"), std::string("\n"));
    EXPECT_EQ(fmt_line(Formatter("plain"), 256, r, "x"), std::string("plain\n"));

    Record root = r;
    root.module = "";
    EXPECT_EQ(fmt_line(Formatter("[%n]"), 256, root, "x"), std::string("[-]\n"));
    Record nf = r;
    nf.file = "nodir.c";
    nf.line = 0;
    EXPECT_EQ(fmt_line(Formatter("%f:%l"), 256, nf, "x"), std::string("nodir.c:0\n"));
    nf.line = -5;
    EXPECT_EQ(fmt_line(Formatter("%l"), 256, nf, "x"), std::string("-5\n"));
    Record big = r;
    big.tid = 4294967295u;
    EXPECT_EQ(fmt_line(Formatter("%t"), 256, big, "x"), std::string("4294967295\n"));
    Record z = make_rec(1700000000ull * 1000000000ull + 1000ull);
    EXPECT_EQ(fmt_line(Formatter("%us|%ms"), 256, z, "x"), std::string("000001|000\n"));

    // default pattern
    std::string full = fmt_line(Formatter("%D %T.%us [%L] [%t] %f:%l - %m"), 256, r, "hello %d", 42);
    EXPECT_EQ(full, D + " " + T + ".123456 [WARN] [4242] source.cpp:123 - hello 42\n");
    // all at once incl. after-message tokens
    std::string all = fmt_line(Formatter("%L|%n|%F|%m|%l|%%"), 256, r, "m");
    EXPECT_EQ(all, std::string("WARN|net|do_thing|m|123|%\n"));
}

TEST(fmt_time_cache_changes_second) {
    Formatter f("%D %T");
    for (uint64_t s = 0; s < 5; ++s) {
        uint64_t ts = kTs + s * 1000000000ull * 3601;  // crosses hours / maybe days
        EXPECT_EQ(fmt_line(f, 128, make_rec(ts), "x"),
                  expect_date_time(ts, true) + " " + expect_date_time(ts, false) + "\n");
    }
}

TEST(fmt_truncation) {
    Record r = make_rec(kTs);
    Formatter f("[%L] %m");  // prefix "[WARN] " = 7 bytes
    const size_t cap = 64;
    // message that makes the line exactly cap bytes: 7 + 56 + '\n' = 64
    std::string m56(56, 'a');
    std::string s = fmt_line(f, cap, r, "%s", m56.c_str());
    EXPECT_EQ(s.size(), cap);
    EXPECT_EQ(s, "[WARN] " + m56 + "\n");
    // one more byte -> truncated, ends with "...\n", exactly cap
    std::string m57(57, 'b');
    s = fmt_line(f, cap, r, "%s", m57.c_str());
    EXPECT_EQ(s.size(), cap);
    EXPECT_EQ(s.substr(s.size() - 4), std::string("...\n"));
    EXPECT_EQ(s.substr(0, 7), std::string("[WARN] "));
    // very long message
    std::string huge(100000, 'z');
    s = fmt_line(f, kScratchSize, r, "%s", huge.c_str());
    EXPECT_EQ(s.size(), kScratchSize);
    EXPECT_EQ(s.substr(s.size() - 4), std::string("...\n"));
    EXPECT_TRUE(s.find('\n') == s.size() - 1);
    // tokens after the message overflowing
    Formatter g("%m%F%F%F");
    std::string m50(50, 'c');
    s = fmt_line(g, cap, r, "%s", m50.c_str());
    EXPECT_EQ(s.size(), cap);
    EXPECT_EQ(s.substr(s.size() - 4), std::string("...\n"));
    // prefix literal alone longer than cap
    std::string lp(200, 'L');
    Formatter h(lp + "%m");
    s = fmt_line(h, cap, r, "x");
    EXPECT_EQ(s.size(), cap);
    EXPECT_EQ(s.substr(s.size() - 4), std::string("...\n"));
}

TEST(fmt_newline_always_present) {
    Record r = make_rec();
    const char *pats[] = {"", "%m", "%D %T.%us [%L] [%t] %f:%l - %m", "%%%", "x%"};
    for (const char *p : pats) {
        Formatter f(p);
        for (size_t cap : {64u, 65u, 100u, 1024u}) {
            for (size_t len : {0u, 1u, 30u, 63u, 64u, 65u, 2000u}) {
                std::string m(len, 'q');
                std::string s = fmt_line(f, cap, r, "%s", m.c_str());
                EXPECT_TRUE(!s.empty() && s.back() == '\n' && s.size() <= cap);
            }
        }
    }
    // null fmt / null fields
    Formatter f("%f %F %n %m");
    Record n = r;
    n.file = nullptr;
    n.func = nullptr;
    n.module = nullptr;
    char buf[64];
    auto call = [&](int dummy, ...) {
        va_list ap;
        va_start(ap, dummy);
        size_t k = f.format(buf, sizeof buf, n, nullptr, ap);
        va_end(ap);
        return std::string(buf, k);
    };
    EXPECT_EQ(call(0), std::string("? ? - \n"));
}

TEST(fmt_multithreaded) {
    Formatter f("%D %T.%us [%L] [%t] %n %f:%l %F - %m");
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&, t] {
            ThreadContext &ctx = thread_ctx();
            Record r = make_rec();
            r.tid = ctx.tid;
            char want[64];
            for (int i = 0; i < 10000; ++i) {
                r.ts_ns = now_ns() + (uint64_t)i * 1000000ull;  // sometimes changes second
                r.line = i;
                auto fn = [&](const char *fmt, ...) {
                    va_list ap;
                    va_start(ap, fmt);
                    size_t n = f.format(ctx.scratch, kScratchSize, r, fmt, ap);
                    va_end(ap);
                    return n;
                };
                size_t n = fn("thread %d iter %d", t, i);
                std::snprintf(want, sizeof want, "thread %d iter %d\n", t, i);
                size_t wl = std::strlen(want);
                if (n < wl || std::memcmp(ctx.scratch + n - wl, want, wl) != 0 ||
                    ctx.scratch[4] != '-' || std::memchr(ctx.scratch, '\n', n) != ctx.scratch + n - 1)
                    bad++;
            }
        });
    for (auto &x : th) x.join();
    EXPECT_EQ(bad.load(), 0);
}

TEST(fmt_benchmark) {
    Formatter f("%D %T.%us [%L] [%t] %f:%l - %m");
    Record r = make_rec();
    char buf[kScratchSize];
    const int N = 1000000;
    size_t total = 0;
    auto fn = [&](const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        size_t n = f.format(buf, sizeof buf, r, fmt, ap);
        va_end(ap);
        return n;
    };
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        r.ts_ns += 1000;
        total += fn("request %d done in %s ms", i, "12.5");
    }
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
    std::printf("  [bench] %.1f ns/format (default pattern, avg %zu bytes)\n", ns, total / N);
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        r.ts_ns += 1000;
        total += fn("static message without args");
    }
    auto t3 = std::chrono::steady_clock::now();
    ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / N;
    std::printf("  [bench] %.1f ns/format (no printf args)\n", ns);
    EXPECT_TRUE(total > 0);
}

TINY_TEST_MAIN()
