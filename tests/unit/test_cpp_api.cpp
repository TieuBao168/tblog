// Tests for the header-only C++ API (<tblog/tblog.hpp>). Must build as C++11.
#include "test_os.h"
#include "tiny_test.h"
#include "tblog/tblog.hpp"

#include <atomic>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ------------------------------------------------------------- helpers --
template <class... A> std::string F(const char *fmt, const A &...args) {
    char buf[2048];
    const std::size_t n = tblog::format_to(buf, sizeof buf, fmt, args...);
    return std::string(buf, n);
}

TOS_PRINTF(1, 2) std::string P(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
}

std::string fresh(const std::string &name) {
    std::string dir = tos::test_dir() + "/cpp_" + name;
    tos::remove_tree(dir);
    tos::make_dirs(dir);
    return dir;
}

std::string read_file(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
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

std::string config(const std::string &dir, const std::string &pattern = "[%L] [%n] %m",
                   const std::string &extra = "") {
    std::string cfg = dir + "/log.conf";
    std::ofstream(cfg.c_str()) << "level = INFO\n"
                                  "pattern = " << pattern << "\n"
                                  "file.path = " << dir << "/app.log\n"
                                  "buffer.size = 256KB\n"
                                  "full_policy = block\n"
                                  "flush.interval_ms = 20\n" << extra;
    return cfg;
}

uint64_t now_ns() { return tos::mono_ns(); }

}  // namespace

// ------------------------------------------------------ user types --
namespace user {
struct Point { int x, y; };
// Allocation-free customization point, found by ADL.
void tblog_format(tblog::Sink &s, const Point &p) { s.format("({}, {})", p.x, p.y); }

struct Money { long long cents; };
// Spec-aware customization: receives the parsed spec, owns all of it.
void tblog_format(tblog::Sink &s, const Money &m, const tblog::FormatSpec &sp) {
    if (sp.type == 'd') s.format("{}", m.cents);
    else s.format("${}.{:02}", m.cents / 100, m.cents % 100);
}

struct Streamed { int v; };
std::ostream &operator<<(std::ostream &o, const Streamed &s) { return o << "S<" << s.v << ">"; }

enum Color { Red, Green = 5 };
enum class Mode : unsigned char { Off = 0, On = 200 };
enum class Named { A, B };
void tblog_format(tblog::Sink &s, Named n) { s.write(n == Named::A ? "Named::A" : "Named::B"); }

// Counts how many times it is formatted.
std::atomic<int> g_formats(0);
struct Counted { int v; };
void tblog_format(tblog::Sink &s, const Counted &c) {
    g_formats.fetch_add(1);
    s.format("counted{}", c.v);
}
}  // namespace user

namespace {
std::atomic<int> g_evals(0);
int eval_arg() { return g_evals.fetch_add(1) + 1; }
}  // namespace

// ============================================================ integers ==
TEST(int_specs_match_snprintf) {
    const long long vals[] = {0, 1, -1, 7, 42, -42, 255, 1000000, INT_MAX, INT_MIN, LLONG_MAX, LLONG_MIN};
    for (std::size_t i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
        const long long v = vals[i];
        EXPECT_EQ(F("{}", v), P("%lld", v));
        EXPECT_EQ(F("{:d}", v), P("%lld", v));
        EXPECT_EQ(F("{:12}", v), P("%12lld", v));
        EXPECT_EQ(F("{:>12}", v), P("%12lld", v));
        EXPECT_EQ(F("{:<12}", v), P("%-12lld", v));
        EXPECT_EQ(F("{:012}", v), P("%012lld", v));
        EXPECT_EQ(F("{:+}", v), P("%+lld", v));
        EXPECT_EQ(F("{: }", v), P("% lld", v));
        EXPECT_EQ(F("{:+08d}", v), P("%+08lld", v));
        EXPECT_EQ(F("{:-}", v), P("%lld", v));
        if (v >= 0) {
            const unsigned long long u = static_cast<unsigned long long>(v);
            EXPECT_EQ(F("{:x}", v), P("%llx", u));
            EXPECT_EQ(F("{:X}", v), P("%llX", u));
            EXPECT_EQ(F("{:o}", v), P("%llo", u));
            EXPECT_EQ(F("{:#o}", v), P("%#llo", u));
            EXPECT_EQ(F("{:10x}", v), P("%10llx", u));
            EXPECT_EQ(F("{:<10X}", v), P("%-10llX", u));
            EXPECT_EQ(F("{:010x}", v), P("%010llx", u));
            if (v) EXPECT_EQ(F("{:#x}", v), P("%#llx", u));
            if (v) EXPECT_EQ(F("{:#X}", v), P("%#llX", u));
            if (v) EXPECT_EQ(F("{:#012x}", v), P("%#012llx", u));
        }
    }
}

TEST(int_all_types_and_extremes) {
    EXPECT_EQ(F("{} {} {} {}", (short)-3, (unsigned short)65535, 3000000000u, -5L), std::string("-3 65535 3000000000 -5"));
    EXPECT_EQ(F("{}", ULLONG_MAX), std::string("18446744073709551615"));
    EXPECT_EQ(F("{}", LLONG_MIN), std::string("-9223372036854775808"));
    EXPECT_EQ(F("{:x}", ULLONG_MAX), std::string("ffffffffffffffff"));
    EXPECT_EQ(F("{:b}", 5), std::string("101"));
    EXPECT_EQ(F("{:#b}", 5), std::string("0b101"));
    EXPECT_EQ(F("{:#B}", 5), std::string("0B101"));
    EXPECT_EQ(F("{:#010b}", 5), std::string("0b00000101"));
    EXPECT_EQ(F("{:b}", ULLONG_MAX), std::string(64, '1'));
    EXPECT_EQ(F("{:b}", 0), std::string("0"));
    EXPECT_EQ(F("{:x}", -42), std::string("-2a"));      // sign + magnitude, like std::format
    EXPECT_EQ(F("{:#x}", -255), std::string("-0xff"));
    EXPECT_EQ(F("{:#x}", 0), std::string("0x0"));       // printf would give "0"
    EXPECT_EQ(F("{:#o}", 0), std::string("0"));
    EXPECT_EQ(F("{:08x}", -42), std::string("-000002a"));
    // signed/unsigned char are integers, like std::format
    EXPECT_EQ(F("{} {}", (signed char)-5, (unsigned char)200), std::string("-5 200"));
    EXPECT_EQ(F("{:c}", 66), std::string("B"));
    EXPECT_EQ(F("{:>3c}", 66), std::string("  B"));
}

TEST(int_fill_align_center) {
    EXPECT_EQ(F("{:*^7}", 42), std::string("**42***"));
    EXPECT_EQ(F("{:*^8}", 42), std::string("***42***"));
    EXPECT_EQ(F("{:_<6}", -1), std::string("-1____"));
    EXPECT_EQ(F("{:_>6}", -1), std::string("____-1"));
    EXPECT_EQ(F("{:x<05}", 3), std::string("3xxxx"));   // '0' ignored when align given
    EXPECT_EQ(F("{:1}", 12345), std::string("12345"));   // width smaller than content
}

// ============================================================== floats ==
TEST(float_specs_match_snprintf) {
    const double vals[] = {0.0, 1.5, -2.25, 3.14159265358979, 1e-7, 1e20, 123456.789, -0.001};
    for (std::size_t i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
        const double v = vals[i];
        EXPECT_EQ(F("{:f}", v), P("%f", v));
        EXPECT_EQ(F("{:F}", v), P("%F", v));
        EXPECT_EQ(F("{:.2f}", v), P("%.2f", v));
        EXPECT_EQ(F("{:.0f}", v), P("%.0f", v));
        EXPECT_EQ(F("{:#.0f}", v), P("%#.0f", v));
        EXPECT_EQ(F("{:e}", v), P("%e", v));
        EXPECT_EQ(F("{:.3e}", v), P("%.3e", v));
        EXPECT_EQ(F("{:E}", v), P("%E", v));
        EXPECT_EQ(F("{:g}", v), P("%g", v));
        EXPECT_EQ(F("{:G}", v), P("%G", v));
        EXPECT_EQ(F("{:.10g}", v), P("%.10g", v));
        EXPECT_EQ(F("{:.3}", v), P("%.3g", v));
        EXPECT_EQ(F("{:14.3f}", v), P("%14.3f", v));
        EXPECT_EQ(F("{:<14.3f}", v), P("%-14.3f", v));
        EXPECT_EQ(F("{:+.1f}", v), P("%+.1f", v));
        EXPECT_EQ(F("{: .2e}", v), P("% .2e", v));
        EXPECT_EQ(F("{:014.2f}", v), P("%014.2f", v));
        EXPECT_EQ(F("{:+014.2e}", v), P("%+014.2e", v));
    }
    EXPECT_EQ(F("{:.3f}", 2.5f), P("%.3f", 2.5));
    EXPECT_EQ(F("{:.4e}", 1.25L), P("%.4Le", 1.25L));
    EXPECT_EQ(F("{:.3f}", 1e-3L), P("%.3Lf", 1e-3L));
    EXPECT_EQ(F("{:*^12.2f}", 3.14159), std::string("****3.14****"));
}

// The fixed-point fast path must agree with printf bit for bit, including
// values sitting on or next to a rounding tie.
TEST(float_fixed_fast_path_matches_snprintf) {
    const double ties[] = {0.125, 0.375, 2.5, 3.5, 0.5, 1.5, -2.5, 1.005, 2.675, 1.115, 0.045, 1e14 + 0.5,
                           999999999999999.0, 1e15, 1e16, 123.4565, -0.0, 0.0, -0.0004, 9.9999999, 0.95, 4503599627370497.0};
    for (std::size_t i = 0; i < sizeof ties / sizeof ties[0]; ++i)
        for (int p = 0; p <= 17; ++p) {
            char f[16];
            std::snprintf(f, sizeof f, "{:.%df}", p);
            EXPECT_EQ(F(f, ties[i]), P("%.*f", p, ties[i]));
        }
    std::srand(777);
    int checked = 0, bad = 0;
    for (int i = 0; i < 20000; ++i) {
        const double mag = std::pow(10.0, std::rand() % 20 - 8);
        double v = (std::rand() / double(RAND_MAX) - 0.5) * mag;
        if (i % 3 == 0) v = std::floor(v * 1000) / 1000 + 0.0005;  // near decimal ties
        const int p = std::rand() % 16;
        char f[16];
        std::snprintf(f, sizeof f, "{:+.%df}", p);
        ++checked;
        if (F(f, v) != P("%+.*f", p, v)) ++bad;
        if (F("{}", std::floor(v)) != P("%.17g", std::floor(v))) ++bad;
    }
    EXPECT_EQ(bad, 0);
    EXPECT_EQ(checked, 20000);
    EXPECT_EQ(F("{:#.0f}", 3.0), std::string("3."));
    EXPECT_EQ(F("{:08.2f}", -1.5), std::string("-0001.50"));
    EXPECT_EQ(F("{:+08.2f}", 1.5), std::string("+0001.50"));
    EXPECT_EQ(F("{:f}", HUGE_VAL), P("%f", HUGE_VAL));
    EXPECT_EQ(F("{:F}", -HUGE_VAL), P("%F", -HUGE_VAL));
}

TEST(float_default_is_shortest_roundtrip) {
    EXPECT_EQ(F("{}", 0.1), std::string("0.1"));
    EXPECT_EQ(F("{}", 1.0), std::string("1"));
    EXPECT_EQ(F("{}", -2.5), std::string("-2.5"));
    EXPECT_EQ(F("{}", 1e300), std::string("1e+300"));
    EXPECT_EQ(F("{}", 0.1f), std::string("0.1"));     // float precision, not 0.100000001
    EXPECT_EQ(F("{}", 1.0 / 3), std::string("0.3333333333333333"));
    EXPECT_EQ(F("{}", 0.1L), std::string("0.1"));
    EXPECT_EQ(F("{}", HUGE_VAL), std::string("inf"));
    EXPECT_EQ(F("{}", -HUGE_VAL), std::string("-inf"));
    EXPECT_EQ(F("{}", static_cast<double>(NAN)), std::string("nan"));
    EXPECT_EQ(F("{:>6}", 1.5), std::string("   1.5"));
    EXPECT_EQ(F("{:+}", 1.5), std::string("+1.5"));
    EXPECT_EQ(F("{:08}", -1.5), std::string("-00001.5"));
    // Round-trips for a spread of values.
    std::srand(12345);
    for (int i = 0; i < 2000; ++i) {
        const double v = (std::rand() - RAND_MAX / 2) * std::pow(10.0, std::rand() % 40 - 20) / 7.0;
        EXPECT_TRUE(std::strtod(F("{}", v).c_str(), nullptr) == v);
        const float fv = static_cast<float>(v);
        EXPECT_TRUE(std::strtof(F("{}", fv).c_str(), nullptr) == fv);
    }
}

TEST(float_huge_precision_does_not_overflow) {
    const std::string s = F("{:.600f}", 1e300);
    EXPECT_EQ(s, P("%.600f", 1e300));  // 901 chars, fits the 2048 test buffer
    char small[64];
    const std::size_t n = tblog::format_to(small, sizeof small, "x={:.600f}", 1e300);
    EXPECT_EQ(n, sizeof small);
    EXPECT_EQ(std::string(small + sizeof small - 3, 3), std::string("..."));
    EXPECT_EQ(std::string(small, 5), std::string("x=100"));
}

// ===================================================== strings & co. ==
TEST(strings) {
    const std::string s = "hello";
    const char *cs = "world";
    char arr[16] = "abc";  // shorter than the array: stops at NUL
    const char *null_cs = nullptr;
    EXPECT_EQ(F("{} {} {} {}", s, cs, arr, "lit"), std::string("hello world abc lit"));
    EXPECT_EQ(F("{:s}", s), std::string("hello"));
    EXPECT_EQ(F("{:8}|", s), P("%-8s|", "hello"));
    EXPECT_EQ(F("{:>8}|", s), P("%8s|", "hello"));
    EXPECT_EQ(F("{:<8}|", cs), P("%-8s|", "world"));
    EXPECT_EQ(F("{:.3}", s), P("%.3s", "hello"));
    EXPECT_EQ(F("{:>6.2}", cs), P("%6.2s", "world"));
    EXPECT_EQ(F("{:.10}", s), std::string("hello"));
    EXPECT_EQ(F("{:-^9}", "ab"), std::string("---ab----"));
    EXPECT_EQ(F("{}", null_cs), std::string("(null)"));
    EXPECT_EQ(F("{}", std::string("with\0nul", 8)), std::string("with\0nul", 8));
    EXPECT_EQ(F("{}", std::string()), std::string());
#if __cplusplus >= 201703L
    EXPECT_EQ(F("{:>4}", std::string_view("abcdef", 2)), std::string("  ab"));
#endif
}

TEST(chars_and_bools) {
    EXPECT_EQ(F("{}", 'A'), std::string("A"));
    EXPECT_EQ(F("{:c}", 'A'), std::string("A"));
    EXPECT_EQ(F("{:d}", 'A'), std::string("65"));
    EXPECT_EQ(F("{:#x}", 'A'), std::string("0x41"));
    EXPECT_EQ(F("{:>3}", 'A'), std::string("  A"));
    EXPECT_EQ(F("{} {}", true, false), std::string("true false"));
    EXPECT_EQ(F("{:s}", true), std::string("true"));
    EXPECT_EQ(F("{:d} {:d}", true, false), std::string("1 0"));
    EXPECT_EQ(F("{:>6}|{:<6}|", true, false), std::string("  true|false |"));
    EXPECT_EQ(F("{:+}", 'A'), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:.2}", 'A'), std::string("{!bad-spec}"));
}

// "{}" of a pointer is 0x + lower-case hex digits, which is what glibc's %p
// prints; Windows' %p prints zero-padded digits without 0x, so the tests
// compare against this instead.
std::string hex_ptr(const void *p, int width = 0) {
    char hex[32];
    std::snprintf(hex, sizeof hex, "0x%" PRIxPTR, reinterpret_cast<uintptr_t>(p));
    return P("%*s", width, hex);
}

TEST(pointers) {
    int x = 0;
    const void *vp = &x;
    int *ip = &x;
    EXPECT_EQ(F("{}", vp), hex_ptr(vp));
    EXPECT_EQ(F("{:p}", ip), hex_ptr(ip));
    EXPECT_EQ(F("{}", static_cast<const int *>(ip)), hex_ptr(vp));
    const char *cs = "str";
    EXPECT_EQ(F("{:p}", cs), hex_ptr(cs));  // {:p} on a C string
    EXPECT_EQ(F("{}", nullptr), std::string("0x0"));
    EXPECT_EQ(F("{}", static_cast<void *>(nullptr)), std::string("0x0"));
    EXPECT_EQ(F("{:>20}", vp), hex_ptr(vp, 20));
    EXPECT_EQ(F("{:x}", vp), std::string("{!bad-spec}"));
}

// ========================================================== structure ==
TEST(escapes) {
    EXPECT_EQ(F("{{}}"), std::string("{}"));
    EXPECT_EQ(F("{{{}}}", 5), std::string("{5}"));
    EXPECT_EQ(F("a {{ b }} c"), std::string("a { b } c"));
    EXPECT_EQ(F("lone } brace"), std::string("lone } brace"));
    EXPECT_EQ(F("no fields at all"), std::string("no fields at all"));
    EXPECT_EQ(F(""), std::string());
    EXPECT_EQ(F("100%% %d %s"), std::string("100%% %d %s"));  // printf syntax is literal
}

TEST(positional_args) {
    EXPECT_EQ(F("{1} {0} {1}", "a", "b"), std::string("b a b"));
    EXPECT_EQ(F("{0:>4}|{0:<4}|{0:x}", 10), std::string("  10|10  |a"));
    EXPECT_EQ(F("{2}{1}{0}", 1, 2, 3), std::string("321"));
    // Mixing is allowed: automatic indexing has its own counter.
    EXPECT_EQ(F("{} {0} {}", "x", "y"), std::string("x x y"));
}

TEST(mismatch_markers) {
    EXPECT_EQ(F("{} {}", 1), std::string("1 {!missing-arg}"));
    EXPECT_EQ(F("{}"), std::string("{!missing-arg}"));
    EXPECT_EQ(F("{5}", 1), std::string("{!missing-arg} {!extra-args}"));
    EXPECT_EQ(F("{}", 1, 2), std::string("1 {!extra-args}"));
    EXPECT_EQ(F("none", 1), std::string("none {!extra-args}"));
    EXPECT_EQ(F("{1}", 1, 2), std::string("2 {!extra-args}"));
    // bad specs
    EXPECT_EQ(F("[{:q}]", 1), std::string("[{!bad-spec}]"));
    EXPECT_EQ(F("{:.2d}", 1), std::string("{!bad-spec}"));    // precision on an integer
    EXPECT_EQ(F("{:.}", 1.0), std::string("{!bad-spec}"));    // missing precision digits
    EXPECT_EQ(F("{:+}", "s"), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:#}", "s"), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:05}", "s"), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:x}", "s"), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:d}", 1.5), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:f}", 1), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:c}", 300), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:<<<}", 1), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:dd}", 1), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:99999999999}", 1), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{:{}}", 1, 2), std::string("{!bad-spec} {!extra-args}"));  // dynamic width unsupported
    // bad syntax
    EXPECT_EQ(F("{name}", 1), std::string("{!bad-format} {!extra-args}"));
    EXPECT_EQ(F("{0x}", 1), std::string("{!bad-format} {!extra-args}"));
    EXPECT_EQ(F("abc {0", 1), std::string("abc {!bad-format}"));
    EXPECT_EQ(F("abc {", 1), std::string("abc {!bad-format}"));
    EXPECT_EQ(F("{:>5} after {", 1), std::string("    1 after {!bad-format}"));
    // null format string
    EXPECT_EQ(F(static_cast<const char *>(nullptr)), std::string("(null)"));
}

// ========================================================= user types ==
TEST(user_types) {
    EXPECT_EQ(F("p={}", user::Point{1, -2}), std::string("p=(1, -2)"));
    EXPECT_EQ(F("[{:>10}]", user::Point{1, 2}), std::string("[    (1, 2)]"));
    EXPECT_EQ(F("[{:*<10}]", user::Point{1, 2}), std::string("[(1, 2)****]"));
    EXPECT_EQ(F("[{:^10}]", user::Point{1, 2}), std::string("[  (1, 2)  ]"));
    EXPECT_EQ(F("{:x}", user::Point{1, 2}), std::string("{!bad-spec}"));  // 2-arg form: padding only
    EXPECT_EQ(F("{} {:d}", user::Money{1234}, user::Money{1234}), std::string("$12.34 1234"));
    EXPECT_EQ(F("{} {:>8}|", user::Streamed{7}, user::Streamed{8}), std::string("S<7>     S<8>|"));
    EXPECT_EQ(F("{:.2}", user::Streamed{7}), std::string("{!bad-spec}"));
    EXPECT_EQ(F("{} {} {:#x}", user::Red, user::Green, user::Mode::On), std::string("0 5 0xc8"));
    EXPECT_EQ(F("{}", user::Named::B), std::string("Named::B"));
    // std types with operator<< go through the ostream adapter.
    EXPECT_EQ(F("{}", std::string("s")), std::string("s"));
    const std::vector<user::Point> pts(2, user::Point{3, 4});
    EXPECT_EQ(F("{} and {}", pts[0], pts[1]), std::string("(3, 4) and (3, 4)"));
}

// ========================================================== truncation ==
TEST(truncation_and_bounds) {
    // Fits exactly: no marker.
    char b[16];
    EXPECT_EQ(tblog::format_to(b, sizeof b, "{}", "0123456789abcdef"), sizeof b);
    EXPECT_EQ(std::string(b, 16), std::string("0123456789abcdef"));
    // One byte over: last 3 bytes become "...".
    EXPECT_EQ(tblog::format_to(b, sizeof b, "{}x", "0123456789abcdef"), sizeof b);
    EXPECT_EQ(std::string(b, 16), std::string("0123456789abc..."));
    // Canary: never writes past cap, also for padding inserts and ostream/custom types.
    char guard[64];
    const char *fmts[] = {"{:>200}", "{:^200}", "{:0200}", "{:<200}", "{:>200}{:>200}", "{:>60}{:>60}"};
    for (std::size_t i = 0; i < sizeof fmts / sizeof fmts[0]; ++i) {
        std::memset(guard, '#', sizeof guard);
        const std::size_t n = tblog::format_to(guard, 32, fmts[i], 7, 8);
        EXPECT_EQ(n, std::size_t(32));
        EXPECT_EQ(std::string(guard + 29, 3), std::string("..."));
        EXPECT_EQ(std::string(guard + 32, 32), std::string(32, '#'));
    }
    std::memset(guard, '#', sizeof guard);
    EXPECT_EQ(tblog::format_to(guard, 32, "{:>200.3f}", 2.5), std::size_t(32));
    EXPECT_EQ(std::string(guard, 64), std::string(29, ' ') + "..." + std::string(32, '#'));  // content pushed out
    std::memset(guard, '#', sizeof guard);
    tblog::format_to(guard, 8, "{}{}{}", user::Point{123456, 7890}, user::Streamed{123456789}, 1.0 / 3);
    EXPECT_EQ(std::string(guard, 8), std::string("(1234..."));
    EXPECT_EQ(std::string(guard + 8, 56), std::string(56, '#'));
    // Zero / tiny capacity must not crash.
    EXPECT_EQ(tblog::format_to(guard, 0, "{}", 12345), std::size_t(0));
    EXPECT_EQ(tblog::format_to(guard, 2, "{}", 12345), std::size_t(2));
}

TEST(truncation_through_logger) {
    const std::string dir = fresh("trunc");
    ASSERT_TRUE(tblog_init(config(dir, "%m").c_str()) == 0);
    const std::string big(3000, 'x');
    TBLOGF_INFO("start {} end", big);
    TBLOGF_INFO("short {}", 1);
    tblog_shutdown();
    const std::vector<std::string> ls = lines_of(read_file(dir + "/app.log"));
    ASSERT_TRUE(ls.size() == 2u);
    EXPECT_TRUE(ls[0].size() <= 1024u);
    EXPECT_TRUE(ls[0].rfind("start xxx", 0) == 0);
    EXPECT_EQ(ls[0].substr(ls[0].size() - 3), std::string("..."));
    EXPECT_EQ(ls[1], std::string("short 1"));
}

// ===================================================== level filtering ==
TEST(filtered_calls_do_not_evaluate_or_format) {
    const std::string dir = fresh("filter");
    ASSERT_TRUE(tblog_init(config(dir, "[%L] [%n] %m", "module.cppnet = DEBUG\n").c_str()) == 0);
    user::g_formats = 0;
    g_evals = 0;
    for (int i = 0; i < 100; ++i) {
        TBLOGF_DEBUG("dbg {} {}", user::Counted{i}, eval_arg());
        TBLOGF_TRACE("trc {} {}", user::Counted{i}, eval_arg());
        TBLOGF_MTRACE("cppnet", "trc {} {}", user::Counted{i}, eval_arg());
        TBLOGF_MINFO("cppdb", "x {}", user::Counted{i});  // cppdb inherits root INFO -> visible
    }
    EXPECT_EQ(g_evals.load(), 0);
    EXPECT_EQ(user::g_formats.load(), 100);
    TBLOGF_INFO("visible {} {}", user::Counted{1}, eval_arg());
    TBLOGF_MDEBUG("cppnet", "net visible {}", user::Counted{2});
    EXPECT_EQ(g_evals.load(), 1);
    EXPECT_EQ(user::g_formats.load(), 102);

    // Logger object: formatting skipped when filtered (arguments are evaluated).
    tblog::Logger net("cppnet"), root;
    user::g_formats = 0;
    net.trace("t {}", user::Counted{0});
    root.debug("d {}", user::Counted{0});
    root.log(TBLOG_LV_DEBUG, "d {}", user::Counted{0});
    EXPECT_EQ(user::g_formats.load(), 0);
    EXPECT_FALSE(root.enabled(TBLOG_LV_DEBUG));
    EXPECT_TRUE(net.enabled(TBLOG_LV_DEBUG));
    // TBLOGF_LOGGER keeps the no-evaluation guarantee for Logger objects.
    g_evals = 0;
    TBLOGF_LOGGER(net, TBLOG_LV_TRACE, "x {}", eval_arg());
    TBLOGF_LOGGER(root, TBLOG_LV_DEBUG, "x {}", eval_arg());
    EXPECT_EQ(g_evals.load(), 0);
    TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "logger macro {}", eval_arg());
    EXPECT_EQ(g_evals.load(), 1);

    // Runtime level changes are honoured by both forms.
    tblog_set_level("cppnet", TBLOG_LV_ERROR);
    user::g_formats = 0;
    net.warn("w {}", user::Counted{0});
    TBLOGF_MWARN("cppnet", "w {}", user::Counted{0});
    EXPECT_EQ(user::g_formats.load(), 0);
    tblog_set_level("cppnet", TBLOG_LV_DEBUG);
    tblog_shutdown();

    const std::string s = read_file(dir + "/app.log");
    EXPECT_TRUE(s.find("[INFO] [-] visible counted1 1") != std::string::npos);
    EXPECT_TRUE(s.find("[DEBUG] [cppnet] net visible counted2") != std::string::npos);
    EXPECT_TRUE(s.find("[INFO] [cppdb] x counted99") != std::string::npos);
    EXPECT_TRUE(s.find("[DEBUG] [cppnet] logger macro 1") != std::string::npos);
    EXPECT_TRUE(s.find("dbg") == std::string::npos);
    EXPECT_TRUE(s.find("trc") == std::string::npos);
    EXPECT_TRUE(s.find(" w ") == std::string::npos);
}

TEST(module_variants_and_call_site) {
    const std::string dir = fresh("modules");
    ASSERT_TRUE(tblog_init(config(dir, "[%L] [%n] %f:%l %F| %m", "module.network = DEBUG\nmodule.db = ERROR\n").c_str()) == 0);
    TBLOGF_MDEBUG("network", "net {}", "debug");
    TBLOGF_MWARN("db", "db warn hidden");
    TBLOGF_MERROR("db", "db error {:05.1f}", 2.5);
    TBLOGF_WARN("root warn {}", -1);
    const int line_macro = __LINE__ - 1;
    tblog::Logger net("network");
    net.debug("logger implicit {}", 1);
    const int line_implicit = __LINE__ - 1;
    net.error(TBLOG_HERE, "logger here {}", 2);
    const int line_here = __LINE__ - 1;
    net.log(TBLOG_LV_INFO, "logger log {}", 3);
    EXPECT_EQ(std::string(net.name()), std::string("network"));
    tblog_shutdown();
    const std::string s = read_file(dir + "/app.log");
    const std::string fn = "test_cpp_api.cpp:";
    EXPECT_TRUE(s.find("[DEBUG] [network] " + fn) != std::string::npos);
    EXPECT_TRUE(s.find("| net debug") != std::string::npos);
    EXPECT_TRUE(s.find("db warn hidden") == std::string::npos);
    EXPECT_TRUE(s.find("| db error 002.5") != std::string::npos);
    EXPECT_TRUE(s.find("[WARN] [-] " + fn + P("%d", line_macro) + " ") != std::string::npos);
    EXPECT_TRUE(s.find("[DEBUG] [network] " + fn + P("%d", line_implicit) + " ") != std::string::npos);
    EXPECT_TRUE(s.find("[ERROR] [network] " + fn + P("%d", line_here) + " ") != std::string::npos);
    EXPECT_TRUE(s.find("| logger implicit 1") != std::string::npos);
    EXPECT_TRUE(s.find("| logger here 2") != std::string::npos);
    EXPECT_TRUE(s.find("[INFO] [network]") != std::string::npos);
    // The function name is captured too (both by the macro and the Logger).
    EXPECT_TRUE(s.find(" tt_module_variants_and_call_site| logger implicit") != std::string::npos);
}

// ======================================================= multithreaded ==
TEST(multithread) {
    const std::string dir = fresh("mt");
    ASSERT_TRUE(tblog_init(config(dir).c_str()) == 0);
    static constexpr int kThreads = 8, kPerThread = 10000;  // static: no capture (MSVC C3493, clang -Wunused-lambda-capture)
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([t] {
            tblog::Logger lg("mt");
            const std::string tag = "s" + std::to_string(t);
            for (int i = 0; i < kPerThread; ++i) {
                if (i % 2) TBLOGF_INFO("t{} i{} {} {:.1f}", t, i, tag, i * 0.5);
                else lg.info("t{} i{} {} {:.1f}", t, i, tag, i * 0.5);
            }
        });
    for (std::size_t i = 0; i < ts.size(); ++i) ts[i].join();
    TBLOGF_DEBUG("filtered out");
    tblog_shutdown();

    std::set<std::string> seen;
    std::size_t total = 0, bad = 0;
    const std::vector<std::string> ls = lines_of(read_file(dir + "/app.log"));
    for (std::size_t k = 0; k < ls.size(); ++k) {
        const std::string &l = ls[k];
        int t = -1, i = -1;
        const char *msg = nullptr;
        if (l.rfind("[INFO] [-] t", 0) == 0) msg = l.c_str() + 11;
        else if (l.rfind("[INFO] [mt] t", 0) == 0) msg = l.c_str() + 12;
        else continue;
        ++total;
        if (std::sscanf(msg, "t%d i%d", &t, &i) != 2 ||
            std::string(msg) != F("t{} i{} s{} {:.1f}", t, i, t, i * 0.5) ||
            (i % 2 == 1) != (l[8] == '-'))
            ++bad;
        seen.insert(l);
    }
    EXPECT_EQ(total, std::size_t(kThreads) * kPerThread);
    EXPECT_EQ(seen.size(), std::size_t(kThreads) * kPerThread);
    EXPECT_EQ(bad, std::size_t(0));
    EXPECT_TRUE(read_file(dir + "/app.log").find("filtered out") == std::string::npos);
    EXPECT_EQ(tblog_dropped_count(), uint64_t(0));
}

// =========================================================== benchmark ==
// Not a pass/fail test: prints ns/call of the printf macro vs the {} macro
// through the buffer backend (and the pure formatting cost).
TEST(benchmark_vs_printf_macro) {
    const std::string dir = fresh("bench");
    ASSERT_TRUE(tblog_init(config(dir, "%D %T [%L] [%t] %m", "buffer.size = 4MB\n").c_str()) == 0);
    const int N = 20000;
    const char *payload = "abcdefghijklmnop";
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; ++i) TBLOG_INFO("bench i=%d value=%.3f payload=%s", i, i * 0.5, payload);
    const double c_ns = double(now_ns() - t0) / N;
    t0 = now_ns();
    for (int i = 0; i < N; ++i) TBLOGF_INFO("bench i={} value={:.3f} payload={}", i, i * 0.5, payload);
    const double cpp_ns = double(now_ns() - t0) / N;
    t0 = now_ns();
    for (int i = 0; i < N; ++i) TBLOGF_DEBUG("bench i={} value={:.3f} payload={}", i, i * 0.5, payload);
    const double off_ns = double(now_ns() - t0) / N;
    char buf[256];
    std::size_t sink = 0;
    t0 = now_ns();
    for (int i = 0; i < N; ++i) sink += tblog::format_to(buf, sizeof buf, "bench i={} value={:.3f} payload={}", i, i * 0.5, payload);
    const double fmt_ns = double(now_ns() - t0) / N;
    t0 = now_ns();
    for (int i = 0; i < N; ++i) sink += std::snprintf(buf, sizeof buf, "bench i=%d value=%.3f payload=%s", i, i * 0.5, payload);
    const double snp_ns = double(now_ns() - t0) / N;
    tblog_shutdown();
    std::printf("  bench: TBLOG_INFO %.0f ns/call, TBLOGF_INFO %.0f ns/call, filtered TBLOGF_DEBUG %.1f ns/call\n",
                c_ns, cpp_ns, off_ns);
    std::printf("  bench: format_to %.0f ns, snprintf %.0f ns (formatting only, %zu bytes)\n", fmt_ns, snp_ns, sink);
    EXPECT_TRUE(sink > 0);
}

TINY_TEST_MAIN()
