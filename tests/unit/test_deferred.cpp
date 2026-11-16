// Deferred formatting (format.mode = deferred): byte-identity with eager
// formatting, fallbacks, config, multithreaded completeness, crash drain.
#include "internal.h"
#include "tiny_test.h"

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cwchar>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace tblog;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
static const int kScale = 10;  // sanitizers are slow: shrink the big tests
#else
static const int kScale = 1;
#endif

namespace {

// ------------------------------------------------------------------ helpers
std::string test_dir() {
    const char *d = std::getenv("TBLOG_TEST_DIR");
    std::string dir = d && *d ? d : "build/tmp";
    std::string cmd = "mkdir -p '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::abort();
    return dir;
}

std::string fresh(const std::string &name) {
    std::string dir = test_dir() + "/deferred_" + std::to_string(getpid()) + "_" + name;
    std::string cmd = "rm -rf '" + dir + "' && mkdir -p '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::abort();
    return dir;
}

void cleanup(const std::string &dir) {
    std::string cmd = "rm -rf '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::abort();
}

void write_file(const std::string &path, const std::string &text) { std::ofstream(path) << text; }

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
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string config(const std::string &dir, const std::string &extra) {
    std::string cfg = dir + "/log.conf";
    write_file(cfg, "level = INFO\n"
                    "pattern = [%L] [%n] %f:%l %m\n"
                    "backend = buffer\n"
                    "format.mode = deferred\n"
                    "file.path = " + dir + "/app.log\n"
                    "buffer.size = 256KB\n"
                    "full_policy = block\n"
                    "flush.interval_ms = 20\n"
                    "reload.watch = false\n" + extra);
    return cfg;
}

std::string printf_str(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string printf_str(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return std::string(buf, n < 0 ? 0 : (size_t)n);
}

// ------------------------------------------------------------ direct paths
const char *kPattern = "%D %T.%us [%L] [%t] [%n] %f:%l %F - %m";
const uint64_t kTs = 1731500000123456789ull;
const uint32_t kTid = 4242;

size_t eager_line(const Formatter &f, char *out, const Record &r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = f.format(out, kScratchSize, r, fmt, ap);
    va_end(ap);
    return n;
}

size_t encode(char *out, tblog_module *m, tblog_site *site, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t n = deferred_encode(out, kRecordMax, kTs, kTid, TBLOG_LV_WARN, m, site, fmt, ap);
    va_end(ap);
    return n;
}

struct Stats {
    long cases = 0, deferred = 0, mismatches = 0, crash_checked = 0, crash_mismatches = 0;
};

// Formats with both paths and compares. crash_exact: also the AS-safe path.
template <class... A>
void compare(Stats &st, const std::string &fmt, bool crash_exact, A... a) {
    static const Formatter f(kPattern);
    static tblog_module *m = tblog_module_get("prop");
    tblog_site site = {"src/some/file.cpp", "some_func", fmt.c_str(), 77, nullptr};
    Record r{kTs, kTid, TBLOG_LV_WARN, m->name, site.file, site.line, site.func};
    char e[kScratchSize], d[kScratchSize], rec[kRecordMax];
    const size_t en = eager_line(f, e, r, fmt.c_str(), a...);
    ++st.cases;
    const size_t rn = encode(rec, m, &site, fmt.c_str(), a...);
    if (rn) {
        ++st.deferred;
        const size_t dn = deferred_render(d, f, rec, rn);
        if (std::string(e, en) != std::string(d, dn)) {
            if (++st.mismatches <= 10)
                std::printf("  MISMATCH fmt=\"%s\"\n    eager:    %s    deferred: %s", fmt.c_str(),
                            std::string(e, en).c_str(), std::string(d, dn).c_str());
        }
        if (crash_exact) {
            ++st.crash_checked;
            deferred_note_utc_offset(kTs);
            const size_t cn = deferred_render_crash(d, f, rec, rn);
            if (std::string(e, en) != std::string(d, cn) && ++st.crash_mismatches <= 10)
                std::printf("  CRASH MISMATCH fmt=\"%s\"\n    eager: %s    crash: %s", fmt.c_str(),
                            std::string(e, en).c_str(), std::string(d, cn).c_str());
        }
    }
    deferred_site_reset(&site);
}

// ---------------------------------------------------------- random formats
typedef std::mt19937_64 Rng;

int rnd(Rng &r, int n) { return (int)(r() % (uint64_t)n); }

std::string rand_literal(Rng &r) {
    static const char chars[] = "abcXYZ019 :=,.-_[]()/\\\"'";
    std::string s;
    int n = rnd(r, 6);
    for (int i = 0; i < n; ++i) {
        if (rnd(r, 12) == 0) s += "%%";
        else s += chars[rnd(r, (int)sizeof chars - 1)];
    }
    return s;
}

// stars: bit0 width '*', bit1 precision '*'.
std::string rand_spec(Rng &r, const char *lenmods, const char *convs, int stars) {
    std::string s = "%";
    static const char flags[] = "-+ #0";
    int nf = rnd(r, 4);
    for (int i = 0; i < nf; ++i) s += flags[rnd(r, 5)];
    if (rnd(r, 50) == 0) s += '\'';
    if (stars & 1) s += '*';
    else if (rnd(r, 2)) s += std::to_string(rnd(r, 3) ? rnd(r, 12) : rnd(r, 40));
    if (stars & 2) s += ".*";
    else if (rnd(r, 2)) {
        s += '.';
        if (rnd(r, 5)) s += std::to_string(rnd(r, 3) ? rnd(r, 8) : rnd(r, 25));
    }
    // lenmods: '|'-separated list
    std::vector<std::string> lm;
    std::string cur;
    for (const char *p = lenmods;; ++p) {
        if (*p == '|' || !*p) {
            lm.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else {
            cur += *p;
        }
    }
    s += lm[rnd(r, (int)lm.size())];
    s += convs[rnd(r, (int)std::strlen(convs))];
    return s;
}

int rand_int(Rng &r) {
    switch (rnd(r, 8)) {
        case 0: return 0;
        case 1: return INT_MIN;
        case 2: return INT_MAX;
        case 3: return -1;
        case 4: return rnd(r, 256);
        default: return (int)(uint32_t)r();
    }
}
long long rand_ll(Rng &r) {
    switch (rnd(r, 7)) {
        case 0: return 0;
        case 1: return LLONG_MIN;
        case 2: return LLONG_MAX;
        case 3: return -7;
        default: return (long long)r();
    }
}
double rand_double(Rng &r) {
    switch (rnd(r, 14)) {
        case 0: return 0.0;
        case 1: return -0.0;
        case 2: return NAN;
        case 3: return -INFINITY;
        case 4: return INFINITY;
        case 5: return DBL_MAX;
        case 6: return DBL_MIN / 4;  // subnormal
        case 7: return 0.5;
        case 8: return 2.5;
        default: {
            double m = (double)(int64_t)r() / 9.2e18;
            return std::ldexp(m, rnd(r, 200) - 100);
        }
    }
}

struct StrPool {
    std::vector<std::string> v;
    explicit StrPool(Rng &r) {
        v.push_back("");
        v.push_back("x");
        v.push_back("hello world");
        v.push_back("tab\there");
        v.push_back(std::string(1500, 'L'));  // longer than a line
        v.push_back(std::string(3000, 'M'));  // longer than the copy cap
        for (int i = 0; i < 20; ++i) {
            std::string s;
            int n = rnd(r, 30);
            for (int j = 0; j < n; ++j) s += (char)(32 + rnd(r, 95));
            v.push_back(s);
        }
    }
    const char *pick(Rng &r) {
        if (rnd(r, 25) == 0) return nullptr;
        return v[rnd(r, (int)v.size())].c_str();
    }
};

const char *kIntConvs = "diouxX";

}  // namespace

// ================================================================ property

TEST(property_deferred_matches_eager_byte_for_byte) {
    Rng r(0x7b10c0de);
    StrPool pool(r);
    Stats st;
    const int N = 3000 / kScale + 200;
    for (int i = 0; i < N; ++i) {
        std::string L = rand_literal(r);
        // Single conversions of every argument type.
        compare(st, L + rand_spec(r, "|hh|h", kIntConvs, 0) + rand_literal(r), true, rand_int(r));
        compare(st, L + rand_spec(r, "", "c", 0), true, rand_int(r));
        compare(st, L + rand_spec(r, "l", kIntConvs, 0), true, (long)rand_ll(r));
        compare(st, L + rand_spec(r, "ll", kIntConvs, 0), true, rand_ll(r));
        compare(st, L + rand_spec(r, "z", kIntConvs, 0), true, (size_t)rand_ll(r));
        compare(st, L + rand_spec(r, "j", kIntConvs, 0), true, (intmax_t)rand_ll(r));
        compare(st, L + rand_spec(r, "t", kIntConvs, 0), true, (ptrdiff_t)rand_ll(r));
        compare(st, L + rand_spec(r, "", "s", 0) + rand_literal(r), true, pool.pick(r));
        compare(st, L + rand_spec(r, "|l", "fFeEgGaA", 0), false, rand_double(r));
        compare(st, L + rand_spec(r, "L", "fFeEgGaA", 0), false, (long double)rand_double(r) * 3);
        compare(st, L + rand_spec(r, "", "p", 0), false,
                rnd(r, 4) ? (void *)(uintptr_t)r() : (void *)nullptr);
        // '*' width / precision.
        const int w = rnd(r, 5) ? rnd(r, 30) - 10 : rnd(r, 2000) - 1000;
        const int p = rnd(r, 4) ? rnd(r, 12) - 2 : -1;
        compare(st, rand_spec(r, "|ll", kIntConvs, 1), true, w, rand_ll(r));
        compare(st, rand_spec(r, "", "s", 3) + L, true, w, p, pool.pick(r));
        compare(st, rand_spec(r, "", "fge", 3), false, w, p, rand_double(r));
        compare(st, rand_spec(r, "", "d", 2), true, p, rand_int(r));
        // Mixed argument lists.
        compare(st,
                L + rand_spec(r, "", "d", 0) + " " + rand_spec(r, "", "s", 0) + " " +
                    rand_spec(r, "", "f", 0) + rand_literal(r) + rand_spec(r, "ll", "u", 0) +
                    rand_spec(r, "", "c", 0) + rand_spec(r, "", "p", 0),
                false, rand_int(r), pool.pick(r), rand_double(r), rand_ll(r), rand_int(r),
                (void *)&st);
        compare(st,
                rand_spec(r, "h", "x", 0) + rand_spec(r, "z", "u", 0) + rand_spec(r, "", "s", 0) +
                    rand_spec(r, "", "s", 0) + rand_spec(r, "j", "d", 0) + rand_literal(r),
                true, rand_int(r), (size_t)rand_ll(r), pool.pick(r), pool.pick(r),
                (intmax_t)rand_ll(r));
    }
    std::printf("  %ld cases, %ld deferred, %ld crash-exact checked\n", st.cases, st.deferred,
                st.crash_checked);
    EXPECT_EQ(st.mismatches, 0L);
    EXPECT_EQ(st.crash_mismatches, 0L);
    // Most random specs are in the supported set (the rest are UB combos).
    EXPECT_TRUE(st.deferred * 10 > st.cases * 6);
}

TEST(fixed_cases_match) {
    Stats st;
    compare(st, "plain text, no args", true);
    compare(st, "%%d %% 100%%", true);
    compare(st, "%d %i %u %o %x %X %c %s", true, -5, 7, 3000000000u, 8, 255, 255, 'A', "str");
    compare(st, "%hhd %hhu %hd %hu", true, 300, -1, 70000, -1);
    compare(st, "%#o %#x %#X %#.0o %.0d|%5.0d|", true, 8, 255, 255, 0, 0, 0);
    compare(st, "%+d % d %+5d %-5d| %05d %-05d %05.3d", true, 3, 3, 3, 3, -3, -3, -3);
    compare(st, "%f %F %e %E %g %G %a %A", false, 3.14, -2.5, 1e-10, 1e300, 0.0001, 1e20, 1.0,
            -0.5);
    compare(st, "%Lf %Lg %.30Lf", false, 1.0L / 3, 2.0L / 3, 1.0L / 7);
    compare(st, "%10.3s| %-10s| %.*s| %*s|", true, "abcdef", "ab", 2, "xyz", -6, "q");
    compare(st, "%s %.3s %.6s %10s %-8s|", true, (const char *)nullptr, (const char *)nullptr,
            (const char *)nullptr, (const char *)nullptr, (const char *)nullptr);
    compare(st, "%p %p %20p", false, (void *)nullptr, (void *)0x1234, (void *)&st);
    compare(st, "%zu %zd %jd %td %lld %llu %ld %lu", true, (size_t)SIZE_MAX, (size_t)-5,
            (intmax_t)INTMAX_MIN, (ptrdiff_t)-9, LLONG_MIN, ULLONG_MAX, LONG_MIN, ULONG_MAX);
    compare(st, "%c|%c|", true, 0, 255);  // embedded NUL byte in the message
    compare(st, "%'d %'.2f", false, 1234567, 1234567.891);
    // Truncation: long left-aligned string, long output, huge padding.
    compare(st, "%-s and more", true, std::string(5000, 'z').c_str());
    compare(st, "%s", true, std::string(1500, 'y').c_str());
    compare(st, "%*d|", true, 5000, 1);
    compare(st, "%.*f", false, 3000, 1.0);
    EXPECT_EQ(st.mismatches, 0L);
    EXPECT_EQ(st.crash_mismatches, 0L);
    EXPECT_EQ(st.deferred, st.cases);
}

// ================================================================ fallbacks

namespace {
template <class... A>
bool encodes(const char *fmt, bool expect, A... a) {
    char rec[kRecordMax];
    tblog_site site = {"f.c", "fn", fmt, 1, nullptr};
    size_t n = encode(rec, tblog_module_get(nullptr), &site, fmt, a...);
    deferred_site_reset(&site);
    if ((n != 0) != expect) std::printf("  unexpected for \"%s\": %zu\n", fmt, n);
    return (n != 0) == expect;
}

template <class... A>
std::string crash(const char *fmt, A... a) {
    static const Formatter f("%m");
    char rec[kRecordMax], out[kScratchSize];
    tblog_site site = {"f.c", "fn", fmt, 1, nullptr};
    size_t n = encode(rec, tblog_module_get(nullptr), &site, fmt, a...);
    size_t k = n ? deferred_render_crash(out, f, rec, n) : 0;
    deferred_site_reset(&site);
    return std::string(out, k);
}
}  // namespace

TEST(fallback_cases_are_eager) {
    tblog_module *m = tblog_module_get(nullptr);
    char rec[kRecordMax];
    int x = 0;
    EXPECT_TRUE(encodes("%n", false, &x));
    EXPECT_TRUE(encodes("%ls", false, L"wide"));
    EXPECT_TRUE(encodes("%lc", false, (wint_t)L'w'));
    EXPECT_TRUE(encodes("%m", false, 0));
    EXPECT_TRUE(encodes("%1$d", false, 1));
    EXPECT_TRUE(encodes("%*1$d", false, 1));
    EXPECT_TRUE(encodes("%Ld", false, 1LL));
    EXPECT_TRUE(encodes("%hf", false, 1.0));
    EXPECT_TRUE(encodes("%5%", false, 0));
    EXPECT_TRUE(encodes("%#d", false, 1));
    EXPECT_TRUE(encodes("%05s", false, "s"));
    EXPECT_TRUE(encodes("%.3c", false, 'c'));
    EXPECT_TRUE(encodes("%k", false, 1));
    EXPECT_TRUE(encodes("trailing %", false, 0));
    EXPECT_TRUE(encodes("%999999d", false, 1));
    EXPECT_TRUE(encodes("%*d", false, 1000000, 1));  // runtime width too large
    // Right-aligned string longer than the copy cap: the cut would show.
    const std::string big(3000, 'b');
    EXPECT_TRUE(encodes("%5000s", false, big.c_str()));
    EXPECT_TRUE(encodes("%-5000s", true, big.c_str()));
    // Record larger than kRecordMax.
    const std::string k(1000, 'k');
    EXPECT_TRUE(encodes("%s%s%s%s%s", false, k.c_str(), k.c_str(), k.c_str(), k.c_str(), k.c_str()));
    EXPECT_TRUE(encodes("%s%s%s", true, k.c_str(), k.c_str(), k.c_str()));
    // fmt is not the site's constant format.
    tblog_site site = {"f.c", "fn", "%d", 1, nullptr};
    EXPECT_EQ(encode(rec, m, &site, "%d ", 1), size_t(0));
    tblog_site nofmt = {"f.c", "fn", nullptr, 1, nullptr};
    EXPECT_EQ(encode(rec, m, &nofmt, "%d", 1), size_t(0));
    deferred_site_reset(&site);
}

TEST(crash_render_floats_are_readable) {
    EXPECT_EQ(crash("%.3f|%f|%.2f", 1.5, 0.25, -2.75), std::string("1.500|0.250000|-2.75\n"));
    EXPECT_EQ(crash("%e|%g|%g|%g", 12345.0, 0.0001, 100000.0, 1e6),
              std::string("1.234500e+04|0.0001|100000|1e+06\n"));
    EXPECT_EQ(crash("%8.2f|%-8.1f|%+08.2f", 3.14159, 2.0, 1.0),
              std::string("    3.14|2.0     |+0001.00\n"));
    EXPECT_EQ(crash("%f %F %f", (double)NAN, (double)INFINITY, -(double)INFINITY),
              std::string("nan INF -inf\n"));
    EXPECT_EQ(crash("%p %p", (void *)nullptr, (void *)0xabc), std::string("(nil) 0xabc\n"));
    EXPECT_EQ(crash("%Lf", 0.5L), std::string("0.500000\n"));
    // Huge values stay bounded and printable.
    std::string s = crash("%f %e %.40f", DBL_MAX, DBL_MIN, 1.0 / 3);
    for (char c : s) EXPECT_TRUE(c == '\n' || (c >= 32 && c < 127));
}

// ================================================================ end to end

TEST(config_format_mode_parsing) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(c.format_mode == FormatMode::Eager);
    EXPECT_TRUE(parse_config_string("format.mode = deferred\n", c, &w));
    EXPECT_TRUE(c.format_mode == FormatMode::Deferred);
    EXPECT_TRUE(parse_config_string("format.mode = EAGER\n", c, &w));
    EXPECT_TRUE(c.format_mode == FormatMode::Eager);
    EXPECT_FALSE(parse_config_string("format.mode = lazy\n", c, &w));
    EXPECT_TRUE(c.format_mode == FormatMode::Eager);
    EXPECT_EQ(w.size(), size_t(1));
}

namespace {

// Logs a fixed set of lines through the public API; returns what is expected.
std::string at(int line, const std::string &msg) {
    return "test_deferred.cpp:" + std::to_string(line) + " " + msg + "\n";
}

std::vector<std::string> log_sample() {
    std::vector<std::string> exp;
    const char *volatile nul = nullptr;  // volatile: hide the NULL from -Wformat-overflow
    char dyn[32];
    std::strcpy(dyn, "dynamic %s");  // not a literal: always eager
    // clang-format off
    TBLOG_INFO("no args");                                         exp.push_back(at(__LINE__, "no args"));
    TBLOG_INFO("ints %d %u %lld %zu", -1, 2u, 3LL, (size_t)4);     exp.push_back(at(__LINE__, "ints -1 2 3 4"));
    TBLOG_INFO("str [%s] [%-6s] [%.2s] [%s]", "a", "b", "cde", nul); exp.push_back(at(__LINE__, "str [a] [b     ] [cd] [(null)]"));
    TBLOG_INFO("dbl %.3f %g %e", 1.25, 0.5, 100.0);               exp.push_back(at(__LINE__, printf_str("dbl %.3f %g %e", 1.25, 0.5, 100.0)));
    TBLOG_MWARN("netw", "mixed %c %x %p %5.1f %s %d", 'z', 255u, (void *)0x10, 2.25, "end", 9);
    exp.push_back(at(__LINE__ - 1, printf_str("mixed %c %x %p %5.1f %s %d", 'z', 255u, (void *)0x10, 2.25, "end", 9)));
    errno = EACCES;
    TBLOG_INFO("errno %m");                                        exp.push_back(at(__LINE__, printf_str("errno %s", std::strerror(EACCES))));
    TBLOG_INFO(dyn, "fmt");                                        exp.push_back(at(__LINE__, "dynamic fmt"));
    TBLOG_INFO("star %*d|%-*d|%.*s", 4, 1, 3, 2, 2, "xyz");        exp.push_back(at(__LINE__, "star    1|2  |xy"));
    // clang-format on
    return exp;
}

bool contains_in_order(const std::string &s, const std::vector<std::string> &v) {
    size_t pos = 0;
    for (const auto &x : v) {
        size_t p = s.find(x, pos);
        if (p == std::string::npos) {
            std::printf("  missing: %s\n", x.c_str());
            return false;
        }
        pos = p + x.size();
    }
    return true;
}

}  // namespace

TEST(end_to_end_deferred_output) {
    const std::string dir = fresh("e2e");
    ASSERT_TRUE(tblog_init(config(dir, "").c_str()) == 0);
    std::vector<std::string> exp = log_sample();
    // A direct site call: proves the deferred path ran (plan cached in site).
    static const char kFmt[] = "site %d";
    static tblog_site site = {__FILE__, "direct", kFmt, 1, nullptr};
    tblog_write_site(tblog_module_get(nullptr), TBLOG_LV_INFO, &site, kFmt, 5);
    tblog_shutdown();
    const std::string s = read_file(dir + "/app.log");
    EXPECT_TRUE(contains_in_order(s, exp));
    EXPECT_TRUE(s.find("[WARN] [netw] test_deferred.cpp:") != std::string::npos);
    EXPECT_TRUE(s.find("test_deferred.cpp:1 site 5\n") != std::string::npos);
    EXPECT_TRUE(site.cache_ != nullptr);
    for (char c : s) EXPECT_TRUE(c == '\n' || c == '\t' || (c >= 32 && c < 127));
    cleanup(dir);
}

TEST(eager_mode_and_mmap_do_not_defer) {
    for (const char *extra : {"format.mode = eager\n", "backend = mmap\nmmap.region_size = 256KB\n"}) {
        const std::string dir = fresh("eager");
        ASSERT_TRUE(tblog_init(config(dir, extra).c_str()) == 0);
        std::vector<std::string> exp = log_sample();
        static const char kFmt[] = "site %d";
        tblog_site site = {__FILE__, "direct", kFmt, 1, nullptr};
        tblog_write_site(tblog_module_get(nullptr), TBLOG_LV_INFO, &site, kFmt, 6);
        tblog_shutdown();
        const std::string s = read_file(dir + "/app.log");
        EXPECT_TRUE(contains_in_order(s, exp));
        EXPECT_TRUE(s.find("test_deferred.cpp:1 site 6\n") != std::string::npos);
        EXPECT_TRUE(site.cache_ == nullptr);  // never encoded
        cleanup(dir);
    }
}

TEST(multithread_completeness_and_order) {
    const std::string dir = fresh("mt");
    ASSERT_TRUE(tblog_init(config(dir, "").c_str()) == 0);
    const int kThreads = 8, kPerThread = 40000 / kScale;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([t, kPerThread] {
            for (int i = 0; i < kPerThread; ++i) {
                if (i % 7 == 0) TBLOG_INFO("t=%d i=%d m%m", t, i);  // eager fallback, mixed in
                else TBLOG_INFO("t=%d i=%d s=%s d=%.2f", t, i, "abc", i * 0.25);
            }
        });
    for (auto &t : ts) t.join();
    tblog_shutdown();
    std::vector<int> next(kThreads, 0);
    size_t total = 0, bad = 0;
    for (const auto &l : lines_of(read_file(dir + "/app.log"))) {
        size_t p = l.find(" t=");
        if (p == std::string::npos) continue;
        int t = -1, i = -1;
        if (std::sscanf(l.c_str() + p, " t=%d i=%d", &t, &i) != 2 || t < 0 || t >= kThreads) {
            ++bad;
            continue;
        }
        ++total;
        if (i != next[t]) ++bad;  // per-thread order, no loss, no duplicates
        next[t] = i + 1;
        if (i % 7 && l.find(printf_str(" s=abc d=%.2f", i * 0.25)) == std::string::npos) ++bad;
    }
    EXPECT_EQ(total, size_t(kThreads) * kPerThread);
    EXPECT_EQ(bad, size_t(0));
    EXPECT_EQ(tblog_dropped_count(), uint64_t(0));
    cleanup(dir);
}

TEST(crash_drains_deferred_records_as_text) {
    const std::string dir = fresh("segv");
    const std::string cfg = config(dir, "flush.interval_ms = 60000\npattern = %D %T [%L] %m\n");
    const int kLines = 2000;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);  // see segv_keeps_logs_with_buffer
        if (tblog_init(cfg.c_str()) != 0) _exit(2);
        for (int i = 0; i < kLines; ++i) {
            if (i % 100 == 0) TBLOG_INFO("eager %m %d", i);
            TBLOG_INFO("line %d s=%s x=%#x f=%.2f c=%c", i, "str", i, i * 0.25, 'A' + i % 26);
        }
        struct rlimit rl = {1, 1};
        setrlimit(RLIMIT_CORE, &rl);
        volatile int *volatile p = nullptr;
        *p = 1;
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    EXPECT_TRUE(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    const std::string s = read_file(dir + "/app.log");
    // Text only: never raw record bytes.
    size_t nonprint = 0;
    for (char c : s)
        if (!(c == '\n' || (c >= 32 && c < 127))) ++nonprint;
    EXPECT_EQ(nonprint, size_t(0));
    std::set<int> seen;
    size_t eager = 0, shaped = 0;
    for (const auto &l : lines_of(s)) {
        // "YYYY-MM-DD HH:MM:SS [INFO] ..."
        if (l.size() > 20 && l[4] == '-' && l[7] == '-' && l[10] == ' ' && l[13] == ':' &&
            l[16] == ':' && l.compare(19, 8, " [INFO] ") == 0)
            ++shaped;
        size_t p = l.find("[INFO] line ");
        if (p != std::string::npos) {
            int i = std::atoi(l.c_str() + p + 12);
            std::string want = printf_str("[INFO] line %d s=str x=%#x f=%.2f c=%c", i, i, i * 0.25,
                                          'A' + i % 26);
            if (l.compare(p, std::string::npos, want) == 0) seen.insert(i);
            else std::printf("  bad crash line: %s\n", l.c_str());
        }
        if (l.find("[INFO] eager ") != std::string::npos) ++eager;
    }
    EXPECT_EQ(seen.size(), size_t(kLines));
    EXPECT_EQ(eager, size_t(kLines / 100));
    EXPECT_EQ(shaped, size_t(kLines + kLines / 100));
    EXPECT_TRUE(s.find("caught signal 11") != std::string::npos);
    EXPECT_TRUE(s.find("caught signal 11") > s.find(printf_str("[INFO] line %d ", kLines - 1)));
    cleanup(dir);
}

TINY_TEST_MAIN()
