/* Core tests: header format, levels, truncation, ordering, wraparound,
 * overflow + drop counting/reporting, FATAL hook, re-entrancy, pre-init
 * buffering, full-tblog compatibility names. */
#include "tblog_lite.h"
#include "tblog_lite_port_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                \
    } while (0)

static char cap[1 << 22];
static size_t cap_len;
static int write_calls;
static uint32_t fake_ts = 1234;
static int fatal_called;
static int log_from_write; /* write() logs a line itself (re-entrancy) */
static size_t nested_process_result = 99;

static void cap_write(const char *d, size_t n) {
    write_calls++;
    if (cap_len + n <= sizeof cap) memcpy(cap + cap_len, d, n);
    cap_len += n;
    if (log_from_write) {
        log_from_write = 0;
        TBLOG_WARN("logged from write()");
        nested_process_result = tblog_lite_process();
    }
}
static uint32_t ts(void) { return fake_ts; }
static void on_fatal(void) { fatal_called++; }

static const tblog_lite_port port = {cap_write, ts, tblog_lite_posix_enter, tblog_lite_posix_exit, on_fatal};

static void reset_cap(void) {
    memset(cap, 0, cap_len < sizeof cap ? cap_len : sizeof cap); /* keep strstr() honest */
    cap_len = 0;
    write_calls = 0;
}
static int cap_is(const char *s) {
    return cap_len == strlen(s) && memcmp(cap, s, cap_len) == 0;
}

static void test_preinit(void) {
    /* before tblog_lite_init: queued, printed by the first process() */
    TBLOG_INFO("early %d", 1);
    CHECK(tblog_lite_process() == 0); /* no port yet */
    tblog_lite_init(&port);
    reset_cap();
    tblog_lite_process();
    CHECK(cap_len > 0 && strstr(cap, " I test_ring.c:") && strstr(cap, "early 1\n"));
}

static void test_format_and_levels(void) {
    char exp[256];
    int line;
    reset_cap();
    TBLOG_MINFO("net", "hello %d %s", 5, "x"); line = __LINE__;
    TBLOG_ERROR("plain"); /* root module: no [..] */
    tblog_lite_process();
    snprintf(exp, sizeof exp, "1234 I [net] test_ring.c:%d hello 5 x\n1234 E test_ring.c:%d plain\n", line, line + 1);
    CHECK(cap_is(exp));

    /* default level INFO: TRACE/DEBUG filtered at runtime */
    reset_cap();
    CHECK(tblog_lite_get_level() == TBLOG_LV_INFO);
    TBLOG_DEBUG("hidden");
    TBLOG_TRACE("hidden");
    tblog_lite_process();
    CHECK(cap_len == 0);
    tblog_lite_set_level(TBLOG_LV_TRACE);
    TBLOG_MTRACE("m", "t");
    TBLOG_MDEBUG("m", "d");
    tblog_lite_set_level(TBLOG_LV_OFF);
    TBLOG_FATAL("off means nothing, even fatal");
    tblog_lite_set_level(TBLOG_LV_INFO);
    tblog_lite_process();
    CHECK(strstr(cap, " T [m] ") && strstr(cap, " D [m] ") && !strstr(cap, "off means"));

    /* arguments are not evaluated when the level is filtered */
    {
        int evals = 0;
        TBLOG_DEBUG("%d", ++evals);
        CHECK(evals == 0);
        TBLOG_INFO("%d", ++evals);
        CHECK(evals == 1);
        tblog_lite_process();
    }
}

static void test_truncation(void) {
    char big[400];
    reset_cap();
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    TBLOG_INFO("%s", big);
    TBLOG_INFO("trailing newline is not doubled\n");
    tblog_lite_process();
    CHECK(cap_len > TBLOG_LITE_LINE_MAX);
    CHECK(cap[TBLOG_LITE_LINE_MAX - 1] == '\n');
    CHECK(memchr(cap, '\n', TBLOG_LITE_LINE_MAX - 1) == NULL);
    CHECK(cap[cap_len - 1] == '\n' && cap[cap_len - 2] == 'd');
}

/* Variable-length lines, drained after a random number of calls: forces many
 * wraparounds; output must be exactly the sequence that was logged. */
static void test_order_wrap(void) {
    static char expect[1 << 22];
    size_t elen = 0;
    unsigned seq = 0, wraps = 0, i;
    unsigned rng = 12345;
    char pad[80];
    memset(pad, 'p', sizeof pad);
    reset_cap();
    for (i = 0; i < 3000; i++) {
        /* bounded so nothing is dropped: at most this many full lines fit */
        const unsigned kmax = (TBLOG_LITE_BUF_SIZE - 1) / TBLOG_LITE_LINE_MAX;
        unsigned k = ((rng = rng * 1103515245u + 12345u) >> 16) % kmax, j;
        for (j = 0; j <= k; j++) {
            int plen = (int)((rng = rng * 1103515245u + 12345u) >> 16) % 60; /* line < 100 bytes */
            TBLOG_MINFO("w", "seq=%u %.*s", seq, plen, pad);
            elen += (size_t)snprintf(expect + elen, sizeof expect - elen, "1234 I [w] test_ring.c:%d seq=%u %.*s\n",
                                     __LINE__ - 2, seq, plen, pad);
            seq++;
        }
        {
            int before = write_calls;
            tblog_lite_process();
            if (write_calls - before == 2) wraps++;
        }
    }
    CHECK(tblog_lite_dropped_count() == 0);
    CHECK(cap_len == elen && memcmp(cap, expect, elen) == 0);
    CHECK(wraps > 10);
    printf("order/wrap: %u lines, %zu bytes, %u wrapped drains\n", seq, elen, wraps);
}

static void test_overflow(void) {
    const char *line_fmt = "overflow line %04d";
    /* "1234 W test_ring.c:NNN overflow line 0000\n" */
    size_t L, accepted, i;
    uint32_t d0 = tblog_lite_dropped_count();
    int ln = __LINE__ + 3;
    char one[128];

    for (i = 0; i < 1000; i++) TBLOG_WARN(line_fmt, (int)i);
    L = (size_t)snprintf(one, sizeof one, "1234 W test_ring.c:%d overflow line 0000\n", ln);
    accepted = (TBLOG_LITE_BUF_SIZE - 1) / L;
    reset_cap();
    CHECK(tblog_lite_dropped_count() - d0 == 1000 - accepted);
    fake_ts = 99;
    tblog_lite_process();
    {
        char note[96];
        snprintf(note, sizeof note, "99 W tblog-lite: %u line(s) dropped\n", (unsigned)(1000 - accepted));
        CHECK(cap_len == accepted * L + strlen(note));
        CHECK(memcmp(cap + accepted * L, note, strlen(note)) == 0);
        /* the accepted ones are the FIRST ones, in order */
        for (i = 0; i < accepted; i++) {
            char want[32];
            snprintf(want, sizeof want, "overflow line %04d\n", (int)i);
            CHECK(memcmp(cap + i * L + L - strlen(want), want, strlen(want)) == 0);
        }
    }
    /* reported once only */
    reset_cap();
    TBLOG_INFO("after");
    tblog_lite_process();
    CHECK(cap_len > 0 && !strstr(cap, "dropped"));
    fake_ts = 1234;
}

static void test_fatal_and_reentrancy(void) {
    reset_cap();
    TBLOG_FATAL("boom %d", 7);
    /* FATAL flushes synchronously, then calls the port hook */
    CHECK(fatal_called == 1);
    CHECK(strstr(cap, " F test_ring.c:") && strstr(cap, "boom 7\n"));

    reset_cap();
    log_from_write = 1;
    TBLOG_INFO("first");
    tblog_lite_process();
    CHECK(nested_process_result == 0); /* nested process() refused */
    CHECK(strstr(cap, "first") && !strstr(cap, "logged from write"));
    tblog_lite_process();
    CHECK(strstr(cap, "logged from write()"));
}

static void test_compat(void) {
    tblog_set_level(NULL, TBLOG_LV_ERROR);
    CHECK(tblog_get_level("anything") == TBLOG_LV_ERROR);
    tblog_set_level("module", TBLOG_LV_TRACE); /* ignored: no per-module levels */
    CHECK(tblog_get_level(NULL) == TBLOG_LV_ERROR);
    tblog_set_level("", TBLOG_LV_INFO);
    reset_cap();
    TBLOG_INFO("compat");
    tblog_flush();
    CHECK(strstr(cap, "compat\n") != NULL);
    CHECK(tblog_dropped_count() == (uint64_t)tblog_lite_dropped_count());
}

/* TBLOG_LITE_ESCAPE (default 1): one call is one line, no raw control bytes. */
static void test_escape(void) {
    char big[300];
    size_t i, nl = 0;
    reset_cap();
    TBLOG_INFO("user %s", "bob\n1234 E forged\x1b[31m\x7f");
    tblog_lite_process();
    CHECK(strstr(cap, "user bob\\n1234 E forged\\x1b[31m\\x7f\n") != NULL);
    for (i = 0; i < cap_len; i++) nl += cap[i] == '\n';
    CHECK(nl == 1);
    reset_cap();
    TBLOG_INFO("done\n\n");
    tblog_lite_process();
    CHECK(cap_len > 6 && memcmp(cap + cap_len - 6, " done\n", 6) == 0);
    /* Long message of control bytes: cut at the line size, whole escapes only. */
    reset_cap();
    memset(big, '\x01', sizeof big - 1);
    big[sizeof big - 1] = 0;
    TBLOG_INFO("%s", big);
    tblog_lite_process();
    CHECK(cap_len <= TBLOG_LITE_LINE_MAX && cap[cap_len - 1] == '\n');
    for (i = 0; i + 1 < cap_len; i++) CHECK((unsigned char)cap[i] >= 0x20);
    CHECK(cap_len >= 5 && memcmp(cap + cap_len - 5, "\\x01\n", 5) == 0);
}

int main(void) {
    test_preinit();
    test_format_and_levels();
    test_truncation();
    test_order_wrap();
    test_overflow();
    test_fatal_and_reentrancy();
    test_compat();
    test_escape();
    printf("test_ring: %s (%d failure(s))\n", fails ? "FAILED" : "ok", fails);
    return fails ? 1 : 0;
}
