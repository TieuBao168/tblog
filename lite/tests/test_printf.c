/* Differential test: tblog_lite_snprintf vs the C library's snprintf on
 * randomized conversion specs from the supported subset (undefined-behaviour
 * combinations such as '#' with %d or a precision with %c are excluded).
 * Every byte of the destination buffer (plus a canary tail) must match, and
 * so must the return value. Built twice: TBLOG_LITE_FLOAT=0 and =1. */
#include "tblog_lite.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ITERATIONS
#define ITERATIONS 300000
#endif

typedef int (*snprintf_fn)(char *, size_t, const char *, ...);

/* xorshift64* */
static unsigned long long rng_state = 0x9E3779B97F4A7C15ull;
static unsigned long long rnd(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ull;
}
static int rint_(int lo, int hi) { /* inclusive */
    return lo + (int)(rnd() % (unsigned long long)(hi - lo + 1));
}
static int chance(int pct) { return (int)(rnd() % 100) < pct; }

enum { T_INT, T_UINT, T_LONG, T_ULONG, T_LLONG, T_ULLONG, T_SIZE, T_STR, T_PTR, T_DBL, T_NONE };

typedef struct {
    int type;
    unsigned long long u;
    const char *s;
    void *p;
    double d;
} arg_t;

static int call(snprintf_fn f, char *b, size_t n, const char *fmt, int nst, int s1, int s2, const arg_t *a) {
#define DISPATCH(...)                                                              \
    switch (nst) {                                                                 \
    case 0: return f(b, n, fmt, __VA_ARGS__);                                      \
    case 1: return f(b, n, fmt, s1, __VA_ARGS__);                                  \
    default: return f(b, n, fmt, s1, s2, __VA_ARGS__);                             \
    }
    switch (a->type) {
    case T_INT: DISPATCH((int)a->u)
    case T_UINT: DISPATCH((unsigned)a->u)
    case T_LONG: DISPATCH((long)a->u)
    case T_ULONG: DISPATCH((unsigned long)a->u)
    case T_LLONG: DISPATCH((long long)a->u)
    case T_ULLONG: DISPATCH((unsigned long long)a->u)
    case T_SIZE: DISPATCH((size_t)a->u)
    case T_STR: DISPATCH(a->s)
    case T_PTR: DISPATCH(a->p)
    case T_DBL: DISPATCH(a->d)
    default: DISPATCH(0)
    }
#undef DISPATCH
}

static unsigned long long rand_int_value(void) {
    static const unsigned long long edges[] = {0,          1,          (unsigned long long)-1,
                                               INT_MAX,    (unsigned long long)INT_MIN,
                                               UINT_MAX,   LLONG_MAX,  (unsigned long long)LLONG_MIN,
                                               ULLONG_MAX, 127,        128,
                                               255,        32767,      32768,
                                               65535,      8,          16};
    switch (rint_(0, 3)) {
    case 0: return edges[rint_(0, (int)(sizeof edges / sizeof edges[0]) - 1)];
    case 1: return (unsigned long long)(long long)rint_(-300, 300);
    case 2: return rnd() >> rint_(0, 63);
    default: return rnd();
    }
}

#if TBLOG_LITE_FLOAT
/* Values whose decimal expansion is short enough to be produced exactly by
 * the lite formatter: integer part < 2^31, at most 20 fractional bits. */
static double rand_exact_double(void) {
    double v;
    switch (rint_(0, 9)) {
    case 0: return 0.0;
    case 1: return -0.0;
    case 2: return __builtin_inf();
    case 3: return -__builtin_inf();
    case 4: return __builtin_nan("");
    case 5: return (double)(rnd() >> rint_(11, 63)); /* integers < 2^53 */
    case 6: return (chance(50) ? 1.0 : -1.0) / (double)(1ull << rint_(0, 30));
    default:
        v = (double)(rnd() >> rint_(33, 63)) / (double)(1u << rint_(0, 20));
        return chance(50) ? -v : v;
    }
}
#endif

static const char *const strings[] = {"", "a", "hello", "tblog-lite", "a longer string, with spaces",
                                      "0123456789abcdefghijklmnopqrstuvwxyz", "\t\x7f\x01!", NULL};

static long failures, checks;

static void check(const char *fmt, int nst, int s1, int s2, const arg_t *a) {
    char b1[600], b2[600];
    size_t n;
    int r1, r2;
    r1 = call(snprintf, NULL, 0, fmt, nst, s1, s2, a);
    n = chance(70) ? 512 : (size_t)rint_(0, r1 + 3);
    if (n > 512) n = 512;
    memset(b1, 0xAA, sizeof b1);
    memset(b2, 0xAA, sizeof b2);
    r1 = call(snprintf, n ? b1 : NULL, n, fmt, nst, s1, s2, a);
    r2 = call(tblog_lite_snprintf, n ? b2 : NULL, n, fmt, nst, s1, s2, a);
    checks++;
    if (r1 != r2 || memcmp(b1, b2, sizeof b1) != 0) {
        if (++failures <= 20) {
            printf("MISMATCH fmt=\"%s\" n=%zu stars=%d,%d type=%d u=%llu d=%.17g\n  libc(%d)=\"%.*s\"\n  lite(%d)=\"%.*s\"\n",
                   fmt, n, s1, s2, a->type, a->u, a->d, r1, (int)(n ? n - 1 : 0), b1, r2,
                   (int)(n ? n - 1 : 0), b2);
        }
    }
}

static void random_case(void) {
    static const char convs[] =
#if TBLOG_LITE_FLOAT
        "diuxXocsp%fFeEgGfeg";
#else
        "diuxXocsp%";
#endif
    char fmt[64];
    char *f = fmt;
    char conv = convs[rint_(0, (int)sizeof convs - 2)];
    int isint = strchr("diuxXo", conv) != NULL;
    int isflt = strchr("fFeEgG", conv) != NULL;
    int nst = 0, s1 = 0, s2 = 0, i;
    arg_t a;

    memset(&a, 0, sizeof a);
    a.type = T_NONE;
    for (i = rint_(0, 3); i > 0; i--) *f++ = (char)rint_('a', 'z');
    *f++ = '%';
    if (conv != '%') {
        if (chance(30)) *f++ = '-';
        if (chance(30) && (isint || isflt)) *f++ = '0';
        if (chance(25)) *f++ = '+';
        if (chance(25)) *f++ = ' ';
        if (chance(30) && (isflt || strchr("xXo", conv))) *f++ = '#';
        i = rint_(0, 9);
        if (i < 4) {
            f += sprintf(f, "%d", rint_(1, 25));
        } else if (i < 6) {
            *f++ = '*';
            *(nst ? &s2 : &s1) = rint_(-25, 25);
            nst++;
        }
        if (conv != 'c' && conv != 'p') {
            i = rint_(0, 9);
            if (i < 3) {
                f += sprintf(f, ".%d", rint_(0, isflt ? 12 : 25));
            } else if (i < 4) {
                *f++ = '.';
            } else if (i < 6) {
                f += sprintf(f, ".*");
                *(nst ? &s2 : &s1) = rint_(-5, isflt ? 12 : 25);
                nst++;
            }
        }
    }
    if (isint) {
        static const char *const lens[] = {"", "hh", "h", "l", "ll", "z"};
        int L = rint_(0, 5);
        int is_signed = conv == 'd' || conv == 'i';
        f += sprintf(f, "%s", lens[L]);
        a.u = rand_int_value();
        a.type = L <= 2 ? (is_signed ? T_INT : T_UINT)
               : L == 3 ? (is_signed ? T_LONG : T_ULONG)
               : L == 4 ? (is_signed ? T_LLONG : T_ULLONG)
                        : T_SIZE;
    } else if (conv == 'c') {
        a.type = T_INT;
        a.u = (unsigned long long)rint_(0, 255);
    } else if (conv == 's') {
        a.type = T_STR;
        a.s = strings[rint_(0, (int)(sizeof strings / sizeof strings[0]) - 1)];
    } else if (conv == 'p') {
        a.type = T_PTR;
        a.p = chance(10) ? NULL : (void *)(uintptr_t)(rnd() >> rint_(0, 40));
    }
#if TBLOG_LITE_FLOAT
    else if (isflt) {
        a.type = T_DBL;
        a.d = rand_exact_double();
    }
#endif
    *f++ = conv;
    for (i = rint_(0, 3); i > 0; i--) *f++ = (char)rint_('a', 'z');
    *f = '\0';
    check(fmt, nst, s1, s2, &a);
}

static const char *volatile null_str = NULL;

static void fixed_cases(void) {
    char b1[256], b2[256];
    int r1, r2;
#define FIX(...)                                                       \
    do {                                                               \
        memset(b1, 0, sizeof b1);                                      \
        memset(b2, 0, sizeof b2);                                      \
        r1 = snprintf(b1, sizeof b1, __VA_ARGS__);                     \
        r2 = tblog_lite_snprintf(b2, sizeof b2, __VA_ARGS__);          \
        checks++;                                                      \
        if (r1 != r2 || memcmp(b1, b2, sizeof b1)) {                   \
            failures++;                                                \
            printf("FIXED MISMATCH %s: libc \"%s\" lite \"%s\"\n", #__VA_ARGS__, b1, b2); \
        }                                                              \
    } while (0)
    FIX("plain text, no conversions");
    FIX("%d %i %u %x %X %o %c %s %p %%", -42, 42, 42u, 0xbeefu, 0xbeefu, 8u, 'Z', "str", (void *)0x1234);
    FIX("[%5d|%-5d|%05d|%+d|% d|%.3d|%5.3d]", 42, 42, 42, 42, 42, 42, -42);
    FIX("[%#x|%#X|%#o|%#.0o|%#.0x|%.0d|%5.0d]", 255u, 255u, 8u, 0u, 0u, 0, 0);
    FIX("[%hhd|%hhu|%hd|%hu|%ld|%lu|%lld|%llu|%zu|%zd]", 300, 300u, 70000, 70000u, -5L, 5UL, LLONG_MIN, ULLONG_MAX,
        (size_t)-1, (size_t)7);
    FIX("[%*d|%-*d|%.*d|%*.*s]", 6, 1, 6, 2, 4, 3, 8, 3, "abcdef");
    FIX("[%s|%10s|%-10s|%.2s]", "x", "right", "left", "truncate");
    FIX("[%p|%20p|%-20p]", (void *)0, (void *)0xdeadbeef, (void *)0x10);
    FIX("[%s|%10s]", null_str, null_str);
    FIX("%c%c%c", 'a', 0, 'b');
#if TBLOG_LITE_FLOAT
    FIX("[%f|%e|%g|%F|%E|%G]", 1.5, 1.5, 1.5, 1.5, 1.5, 1.5);
    FIX("[%.0f|%.0f|%.0f|%.0f|%.1f|%.2f]", 0.5, 1.5, 2.5, -0.5, 0.25, 0.125);
    FIX("[%g|%g|%g|%g|%#g|%g]", 100000.0, 1000000.0, 0.0001, 0.00001, 1.0, 123456789.0);
    FIX("[%10.3f|%-10.3e|%+010.2f|% g|%010g]", 3.25, -3.25, 2.5, 2.5, -0.0);
    FIX("[%f|%F|%e|%E|%5g|%-6G|%010f]", __builtin_inf(), __builtin_inf(), -__builtin_inf(), __builtin_nan(""),
        __builtin_nan(""), __builtin_inf(), -__builtin_inf());
    FIX("[%.3e|%.0e|%#.0e|%e]", 9.9995, 9.5, 2.5, 1e-300);
    FIX("[%f|%.10f|%g|%.15g]", 1.0 / 1024, 1.0 / 3, 1e-5, 1.0 / 3);
#endif
#undef FIX
}

#if TBLOG_LITE_FLOAT
/* Arbitrary doubles: not guaranteed exact (see printf docs), report only. */
static void float_info(void) {
    static const char *const fmts[] = {"%.3f", "%f", "%e", "%.4e", "%g", "%.10g", "%.1f"};
    long bad = 0, total = 0, i;
    char b1[128], b2[128];
    for (i = 0; i < 100000; i++) {
        double v = (double)rnd() / 18446744073709551616.0 * 1e6;
        const char *fm = fmts[rint_(0, 6)];
        if (chance(50)) v /= 1e6;
        snprintf(b1, sizeof b1, fm, v);
        tblog_lite_snprintf(b2, sizeof b2, fm, v);
        total++;
        if (strcmp(b1, b2)) bad++;
    }
    printf("info: arbitrary doubles: %ld/%ld differ from libc (approximate digits allowed)\n", bad, total);
}
#endif

int main(void) {
    long i;
    fixed_cases();
    for (i = 0; i < ITERATIONS; i++) random_case();
#if TBLOG_LITE_FLOAT
    float_info();
#endif
    printf("test_printf%s: %ld checks, %ld mismatches\n", TBLOG_LITE_FLOAT ? " (float)" : "", checks, failures);
    return failures ? 1 : 0;
}
