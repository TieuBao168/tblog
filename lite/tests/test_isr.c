/* Simulated ISR: a SIGALRM handler driven by setitimer() logs (and sometimes
 * calls tblog_lite_process()) while the main loop logs and drains. The posix
 * port's critical section blocks signals, which is what disabling interrupts
 * does on an MCU. Every line written out is parsed and compared byte-for-byte
 * with what its producer logged; per-producer sequence numbers must increase,
 * and lines seen + lines reported dropped must equal lines logged. */
#define _POSIX_C_SOURCE 200809L
#include "tblog_lite.h"
#include "tblog_lite_port_posix.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifndef MAIN_LINES
#define MAIN_LINES 300000u
#endif

static volatile sig_atomic_t isr_seq;  /* lines logged by the handler */
static volatile sig_atomic_t isr_drains;
static unsigned main_seq;

static int fails;
static unsigned long seen[2], bad_lines, drops_reported, isr_process_ran;
static long last_seq[2] = {-1, -1};
static char partial[512];
static size_t plen;

static const char PAD[] = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";

/* body after the header, identical rule for both producers */
#define BODY_FMT "seq=%u h=%08x %.*s"
#define BODY_ARGS(s) (s), (unsigned)((s) * 2654435761u), (int)((s) % 40u), PAD

static void check_line(const char *l, size_t n) {
    char want[256];
    unsigned long ts;
    int off = 0, src;
    unsigned s;
    const char *rest;

    if (sscanf(l, "%lu %n", &ts, &off) != 1 || off == 0) goto bad;
    rest = l + off;
    if (strncmp(rest, "W tblog-lite: ", 14) == 0) {
        unsigned long d;
        if (sscanf(rest + 14, "%lu line(s) dropped", &d) != 1) goto bad;
        drops_reported += d;
        return;
    }
    if (strncmp(rest, "I [isr] ", 8) == 0) src = 0;
    else if (strncmp(rest, "I [main] ", 9) == 0) src = 1;
    else goto bad;
    rest = strstr(rest, "seq=");
    if (!rest || sscanf(rest, "seq=%u", &s) != 1) goto bad;
    snprintf(want, sizeof want, BODY_FMT, BODY_ARGS(s));
    if ((size_t)(l + n - rest) != strlen(want) || memcmp(rest, want, strlen(want)) != 0) goto bad;
    if ((long)s <= last_seq[src]) goto bad;
    last_seq[src] = (long)s;
    seen[src]++;
    return;
bad:
    if (++bad_lines <= 5) printf("BAD LINE: %.*s\n", (int)n, l);
}

/* Lines may be split across two write() calls (ring wraparound). */
static void parse_write(const char *d, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (d[i] == '\n') {
            check_line(partial, plen);
            plen = 0;
        } else if (plen < sizeof partial - 1) {
            partial[plen++] = d[i];
            partial[plen] = 0;
        } else {
            bad_lines++;
        }
    }
}

static const tblog_lite_port port = {parse_write, tblog_lite_posix_ms, tblog_lite_posix_enter,
                                     tblog_lite_posix_exit, 0};

static void on_alarm(int sig) {
    unsigned s = (unsigned)isr_seq;
    (void)sig;
    TBLOG_MINFO("isr", BODY_FMT, BODY_ARGS(s));
    isr_seq = (sig_atomic_t)(s + 1);
    if ((s & 15) == 0) { /* drain from "interrupt context" sometimes */
        if (tblog_lite_process()) isr_drains++;
    }
}

int main(void) {
    struct sigaction sa;
    struct itimerval it;
    unsigned burst = 0;

    tblog_lite_init(&port);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 20;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);

    while (main_seq < MAIN_LINES) {
        TBLOG_MINFO("main", BODY_FMT, BODY_ARGS(main_seq));
        main_seq++;
        /* usually drain often; sometimes stall to force overflow and drops */
        if (burst) {
            burst--;
        } else if ((main_seq & 7) == 0) {
            tblog_lite_process();
            if ((main_seq & 4095) == 0) burst = 300;
        }
    }
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, NULL);
    tblog_lite_flush();
    isr_process_ran = (unsigned long)isr_drains;

    {
        unsigned long logged = (unsigned long)main_seq + (unsigned long)isr_seq;
        unsigned long got = seen[0] + seen[1];
        printf("test_isr: main logged %u, isr logged %ld, written %lu, dropped %lu (counter %lu), "
               "isr drains %lu, bad lines %lu\n",
               main_seq, (long)isr_seq, got, drops_reported, (unsigned long)tblog_lite_dropped_count(),
               isr_process_ran, bad_lines);
        if (bad_lines) fails++;
        if (plen) fails++;
        if (got + drops_reported != logged) fails++;
        if (drops_reported != tblog_lite_dropped_count()) fails++;
        if (isr_seq < 1000) {
            printf("too few signals delivered to be meaningful\n");
            fails++;
        }
        if (drops_reported == 0) {
            printf("expected some drops from the stalls\n");
            fails++;
        }
    }
    printf("test_isr: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
