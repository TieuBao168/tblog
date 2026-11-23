/* tblog-lite core: level check, line formatting, static ring buffer, drain.
 *
 * Ring: bytes [tail_, head_) (modulo TBLOG_LITE_BUF_SIZE) hold complete lines.
 * Producers (any task or ISR) append whole lines under the port's critical
 * section and never touch bytes in [tail_, head_). The single active consumer
 * (process/flush, guarded by busy_) reads [tail_, head_) outside the critical
 * section, hands it to write(), then advances tail_ under the critical
 * section. One byte stays free so head_ == tail_ always means "empty".
 */
#include "tblog_lite.h"

#include <string.h>

#if TBLOG_LITE_BUF_SIZE < 2 * TBLOG_LITE_LINE_MAX
#error "TBLOG_LITE_BUF_SIZE must hold at least two full lines"
#endif
#if TBLOG_LITE_LINE_MAX < 32
#error "TBLOG_LITE_LINE_MAX must be at least 32"
#endif

#if defined(__GNUC__)
#define BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define BARRIER() ((void)0)
#endif

#define SIZE ((size_t)TBLOG_LITE_BUF_SIZE)

static char ring_[TBLOG_LITE_BUF_SIZE];
static volatile size_t head_, tail_;
static volatile uint32_t dropped_pending_, dropped_total_;
static volatile unsigned char busy_;
static const tblog_lite_port *volatile port_;

volatile unsigned char tblog_lite_level_ = TBLOG_LITE_DEFAULT_LEVEL;

static uint32_t crit_enter(const tblog_lite_port *p) {
    uint32_t st = (p && p->enter_critical) ? p->enter_critical() : 0;
    BARRIER();
    return st;
}

static void crit_exit(const tblog_lite_port *p, uint32_t st) {
    BARRIER();
    if (p && p->exit_critical) p->exit_critical(st);
}

static uint32_t now(const tblog_lite_port *p) {
    return (p && p->timestamp) ? p->timestamp() : 0;
}

void tblog_lite_init(const tblog_lite_port *port) {
    port_ = port;
}

void tblog_lite_set_level(tblog_level lv) {
    tblog_lite_level_ = (unsigned char)((unsigned)lv > TBLOG_LV_OFF ? TBLOG_LV_OFF : lv);
}

tblog_level tblog_lite_get_level(void) {
    return (tblog_level)tblog_lite_level_;
}

void tblog_lite_set_level_compat_(const char *module, tblog_level lv) {
    if (!module || !module[0]) tblog_lite_set_level(lv);
}

uint32_t tblog_lite_dropped_count(void) {
    return dropped_total_;
}

#if TBLOG_LITE_ESCAPE
/* Bytes written for c once escaped (see TBLOG_LITE_ESCAPE). */
static size_t esc_len(unsigned char c) {
    if ((c >= 0x20 || c == '\t') && c != 0x7f) return 1;
    return (c == '\n' || c == '\r') ? 2 : 4;
}

/* Escapes b[start, end) in place, growing it up to lim (whole escapes only);
 * trailing newlines are dropped first. Returns the new end. Written back to
 * front, so no byte is overwritten before it is read (the text only grows). */
static size_t escape_inplace(char *b, size_t start, size_t end, size_t lim) {
    static const char hex[] = "0123456789abcdef";
    size_t i = start, out, src, w;
    while (end > start && b[end - 1] == '\n') end--;
    while (i < end && esc_len((unsigned char)b[i]) == 1) i++;
    if (i == end) return end;
    out = i;
    for (src = i; src < end; src++) {
        size_t l = esc_len((unsigned char)b[src]);
        if (out + l > lim) break;
        out += l;
    }
    w = out;
    while (src > i) {
        unsigned char c = (unsigned char)b[--src];
        if (esc_len(c) == 1) {
            b[--w] = (char)c;
        } else if (c == '\n' || c == '\r') {
            b[--w] = c == '\n' ? 'n' : 'r';
            b[--w] = '\\';
        } else {
            b[--w] = hex[c & 0xF];
            b[--w] = hex[c >> 4];
            b[--w] = 'x';
            b[--w] = '\\';
        }
    }
    return out;
}
#endif

void tblog_lite_write(tblog_level lv, const char *module, const char *file, int line,
                      const char *fmt, ...) {
    static const char letters[] = "TDIWEF";
    const tblog_lite_port *p = port_;
    char buf[TBLOG_LITE_LINE_MAX];
    size_t len, h, used, first, msg;
    uint32_t st;
    va_list ap;
    int r;

    if ((unsigned)lv > TBLOG_LV_FATAL) return;

    /* Header: "<ts> <L> [module] file:line " */
    r = tblog_lite_snprintf(buf, sizeof buf, "%lu %c ", (unsigned long)now(p), letters[lv]);
    if (module && module[0]) r += tblog_lite_snprintf(buf + r, sizeof buf - (size_t)r, "[%s] ", module);
    if ((size_t)r >= sizeof buf) r = (int)sizeof buf - 1;
    if (file) {
        const char *base = file, *s;
        for (s = file; *s; s++)
            if (*s == '/' || *s == '\\') base = s + 1;
        r += tblog_lite_snprintf(buf + r, sizeof buf - (size_t)r, "%s:%d ", base, line);
        if ((size_t)r >= sizeof buf) r = (int)sizeof buf - 1;
    }

    msg = (size_t)r; /* the message starts here (r <= sizeof buf - 1) */
    va_start(ap, fmt);
    r += tblog_lite_vsnprintf(buf + r, sizeof buf - (size_t)r, fmt, ap);
    va_end(ap);

    /* Terminate with exactly one '\n' (replaces the NUL; may use the last byte). */
    len = (size_t)r < sizeof buf ? (size_t)r : sizeof buf - 1;
#if TBLOG_LITE_ESCAPE
    len = escape_inplace(buf, msg, len, sizeof buf - 1);
#else
    (void)msg;
    if (len && buf[len - 1] == '\n') len--;
#endif
    buf[len++] = '\n';

    st = crit_enter(p);
    h = head_;
    used = h >= tail_ ? h - tail_ : SIZE - tail_ + h;
    if (len > SIZE - 1 - used) {
        dropped_pending_++;
        dropped_total_++;
    } else {
        first = SIZE - h < len ? SIZE - h : len;
        memcpy(ring_ + h, buf, first);
        memcpy(ring_, buf + first, len - first);
        h += len;
        head_ = h >= SIZE ? h - SIZE : h;
    }
    crit_exit(p, st);

    if (lv == TBLOG_LV_FATAL) {
        tblog_lite_flush();
        if (p && p->fatal) p->fatal();
    }
}

static void advance_tail(const tblog_lite_port *p, size_t t) {
    uint32_t st = crit_enter(p);
    tail_ = t;
    crit_exit(p, st);
}

size_t tblog_lite_process(void) {
    const tblog_lite_port *p = port_;
    size_t h, t, total = 0;
    uint32_t st, dropped;

    if (!p || !p->write) return 0;
    st = crit_enter(p);
    if (busy_) {
        crit_exit(p, st);
        return 0;
    }
    busy_ = 1;
    h = head_;
    t = tail_;
    dropped = dropped_pending_;
    dropped_pending_ = 0;
    crit_exit(p, st);

    if (h < t) { /* wrapped: tail..end first, freeing that space early */
        p->write(ring_ + t, SIZE - t);
        total += SIZE - t;
        t = 0;
        advance_tail(p, t);
    }
    if (h > t) {
        p->write(ring_ + t, h - t);
        total += h - t;
        advance_tail(p, h);
    }
    if (dropped) {
        char msg[64];
        int n = tblog_lite_snprintf(msg, sizeof msg, "%lu W tblog-lite: %lu line(s) dropped\n",
                                    (unsigned long)now(p), (unsigned long)dropped);
        p->write(msg, (size_t)n);
        total += (size_t)n;
    }

    st = crit_enter(p);
    busy_ = 0;
    crit_exit(p, st);
    return total;
}

void tblog_lite_flush(void) {
    while (tblog_lite_process()) {
    }
}
