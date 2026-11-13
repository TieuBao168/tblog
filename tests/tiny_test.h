// Minimal header-only test framework (no external deps).
// Usage: TEST(name) { EXPECT_TRUE(x); EXPECT_EQ(a, b); }  then  TINY_TEST_MAIN()
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
struct Reg { Reg(const char *n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
template <class T> std::string str(const T &v) { std::ostringstream o; o << v; return o.str(); }
inline std::string str(const char *v) { return v ? std::string("\"") + v + "\"" : "null"; }
inline std::string str(const std::string &v) { return "\"" + v + "\""; }
inline int run(int argc, char **argv) {
    int ran = 0;
    for (auto &c : cases()) {
        if (argc > 1 && std::strstr(c.name, argv[1]) == nullptr) continue;
        int before = failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("%s %s\n", failures() == before ? "[  OK  ]" : "[ FAIL ]", c.name);
        ++ran;
    }
    std::printf("%d test(s), %d failure(s)\n", ran, failures());
    return failures() ? 1 : 0;
}
}  // namespace tiny

#define TINY_CAT2(a, b) a##b
#define TINY_CAT(a, b) TINY_CAT2(a, b)
#define TEST(name)                                                        \
    static void TINY_CAT(tt_, name)();                                    \
    static tiny::Reg TINY_CAT(tt_reg_, name)(#name, TINY_CAT(tt_, name)); \
    static void TINY_CAT(tt_, name)()

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
