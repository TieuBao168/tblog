// Minimal header-only test framework (no external deps).
// Usage: TEST(name) { EXPECT_TRUE(x); EXPECT_EQ(a, b); }  then  TINY_TEST_MAIN()
//
// TINY_SKIP("reason") ends the current test as skipped (printed as
// "[ SKIP ] name (reason)", counted apart from passes and failures).
//
// Child scenarios: TINY_CHILD(name) { ... } registers code that only runs
// when the test binary is started as `<binary> --tiny-child=name` (see
// tos::run_child in test_os.h, which re-executes the binary that way). The
// child exits with 0 if no EXPECT failed in it, else 1, without running
// atexit handlers. An optional hook (tiny::child_setup()) runs first.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace tiny {
struct Case { const char *name; std::function<void()> fn; };
inline std::vector<Case> &cases() { static std::vector<Case> v; return v; }
inline int &failures() { static int f = 0; return f; }
inline std::string &skip_reason() { static std::string s; return s; }
inline bool &skipped() { static bool b = false; return b; }
inline void skip(const char *why) { skipped() = true; skip_reason() = why ? why : ""; }
struct Reg { Reg(const char *n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };

struct ChildCase { const char *name; void (*fn)(); };
inline std::vector<ChildCase> &children() { static std::vector<ChildCase> v; return v; }
struct ChildReg { ChildReg(const char *n, void (*f)()) { children().push_back({n, f}); } };
inline void (*&child_setup())() { static void (*hook)() = nullptr; return hook; }
inline const char *&argv0() { static const char *a = ""; return a; }

template <class T> std::string str(const T &v) { std::ostringstream o; o << v; return o.str(); }
inline std::string str(const char *v) { return v ? std::string("\"") + v + "\"" : "null"; }
inline std::string str(const std::string &v) { return "\"" + v + "\""; }

inline int run_child_case(const char *name) {
    for (auto &c : children()) {
        if (std::strcmp(c.name, name) != 0) continue;
        if (child_setup()) child_setup()();
        c.fn();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(failures() ? 1 : 0);
    }
    std::printf("unknown child scenario %s\n", name);
    return 2;
}

inline int run(int argc, char **argv) {
    argv0() = argc > 0 ? argv[0] : "";
    static const char kChild[] = "--tiny-child=";
    if (argc > 1 && std::strncmp(argv[1], kChild, sizeof kChild - 1) == 0)
        return run_child_case(argv[1] + sizeof kChild - 1);
    int ran = 0, skips = 0;
    for (auto &c : cases()) {
        if (argc > 1 && std::strstr(c.name, argv[1]) == nullptr) continue;
        int before = failures();
        skipped() = false;
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        if (skipped() && failures() == before) {
            std::printf("[ SKIP ] %s (%s)\n", c.name, skip_reason().c_str());
            ++skips;
        } else {
            std::printf("%s %s\n", failures() == before ? "[  OK  ]" : "[ FAIL ]", c.name);
        }
        ++ran;
    }
    std::printf("%d test(s), %d failure(s), %d skipped\n", ran, failures(), skips);
    return failures() ? 1 : 0;
}
}  // namespace tiny

#define TINY_CAT2(a, b) a##b
#define TINY_CAT(a, b) TINY_CAT2(a, b)
#define TEST(name)                                                        \
    static void TINY_CAT(tt_, name)();                                    \
    static tiny::Reg TINY_CAT(tt_reg_, name)(#name, TINY_CAT(tt_, name)); \
    static void TINY_CAT(tt_, name)()
#define TINY_CHILD(name)                                                              \
    static void TINY_CAT(tc_, name)();                                                \
    static tiny::ChildReg TINY_CAT(tc_reg_, name)(#name, TINY_CAT(tc_, name));       \
    static void TINY_CAT(tc_, name)()
#define TINY_SKIP(reason) do { tiny::skip(reason); return; } while (0)

#define EXPECT_TRUE(x)                                                            \
    do { if (!(x)) { ++tiny::failures();                                          \
        std::printf("  %s:%d: EXPECT_TRUE(%s) failed\n", __FILE__, __LINE__, #x); } } while (0)
#define EXPECT_FALSE(x) EXPECT_TRUE(!(x))
#define EXPECT_EQ(a, b)                                                           \
    do { auto tt_a = (a); auto tt_b = (b); if (!(tt_a == tt_b)) { ++tiny::failures(); \
        std::printf("  %s:%d: EXPECT_EQ(%s, %s): %s vs %s\n", __FILE__, __LINE__, #a, #b, \
                    tiny::str(tt_a).c_str(), tiny::str(tt_b).c_str()); } } while (0)
#define ASSERT_TRUE(x)                                                            \
    do { if (!(x)) { ++tiny::failures();                                          \
        std::printf("  %s:%d: ASSERT_TRUE(%s) failed\n", __FILE__, __LINE__, #x); return; } } while (0)

#define TINY_TEST_MAIN() int main(int argc, char **argv) { return tiny::run(argc, argv); }
