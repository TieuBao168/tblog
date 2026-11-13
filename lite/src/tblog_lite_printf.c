/* tblog-lite formatter: a small, freestanding vsnprintf.
 *
 * Supported: %d %i %u %x %X %o %c %s %p %% ; flags '-' '0' '+' ' ' '#' ;
 * width and precision as a number or '*' ; length hh h l ll z ;
 * %f %F %e %E %g %G when TBLOG_LITE_FLOAT=1.
 * Output matches glibc snprintf for that subset (see tests/test_printf.c),
 * including "(nil)" for a NULL %p and "(null)" for a NULL %s.
 * Unknown conversions are printed verbatim.
 */
#include "tblog_lite.h"

typedef struct {
    char *buf;
    size_t cap; /* bytes available in buf, including the terminating NUL */
    size_t pos; /* characters produced so far (may exceed cap) */
} out_t;

static void put(out_t *o, char c) {
    if (o->pos + 1 < o->cap) o->buf[o->pos] = c;
    o->pos++;
}

static void put_n(out_t *o, char c, int n) {
    while (n-- > 0) put(o, c);
}

static void put_s(out_t *o, const char *s, int n) {
    while (n-- > 0) put(o, *s++);
}

enum { F_MINUS = 1, F_ZERO = 2, F_PLUS = 4, F_SPACE = 8, F_HASH = 16, F_UPPER = 32 };

#if TBLOG_LITE_LONG_LONG
typedef unsigned long long uval_t;
#else
typedef unsigned long uval_t;
#endif

/* Emits: [spaces] prefix [zeros] body [spaces], where `zeros` covers the
 * precision padding and, with the '0' flag, the width padding. */
static void put_field(out_t *o, int flags, int width, const char *pre, int npre, int zeros,
                      const char *body, int nbody) {
    int len = npre + zeros + nbody;
    if ((flags & (F_ZERO | F_MINUS)) == F_ZERO && width > len) {
        zeros += width - len;
        len = width;
    }
    if (!(flags & F_MINUS)) put_n(o, ' ', width - len);
    put_s(o, pre, npre);
    put_n(o, '0', zeros);
    put_s(o, body, nbody);
    if (flags & F_MINUS) put_n(o, ' ', width - len);
}

static void fmt_int(out_t *o, uval_t v, int neg, int base, int flags, int width, int prec) {
    char tmp[24]; /* 64-bit octal = 22 digits */
    char pre[3]; /* sign + "0x" (%+p) */
    int nd = 0, npre = 0, zeros;
    const char *digits = (flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    char *end = tmp + sizeof tmp;

    while (v) {
        end[-++nd] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    if (prec < 0) {
        prec = 1;
    } else {
        flags &= ~F_ZERO; /* an explicit precision disables '0' */
    }
    if (neg) pre[npre++] = '-';
    else if (flags & F_PLUS) pre[npre++] = '+';
    else if (flags & F_SPACE) pre[npre++] = ' ';
    if (flags & F_HASH) {
        if (base == 8 && prec <= nd && (nd == 0 || end[-nd] != '0')) prec = nd + 1;
        if (base == 16 && nd) {
            pre[npre++] = '0';
            pre[npre++] = (flags & F_UPPER) ? 'X' : 'x';
        }
    }
    zeros = prec > nd ? prec - nd : 0;
    put_field(o, flags, width, pre, npre, zeros, end - nd, nd);
}

#if TBLOG_LITE_FLOAT
/* Decimal digits of a finite double >= 0: value = 0.d[0]d[1]... * 10^exp.
 * Exact while the integer part is below 2^64 and the expansion fits in
 * FDIG digits (always the case for values with few significant bits);
 * otherwise the trailing digits are approximate. */
#define FDIG 40

typedef struct {
    char d[FDIG];
    int n;      /* digits stored */
    int exp;    /* decimal exponent of 0.d */
    int sticky; /* nonzero digits exist beyond d[n-1] */
} fdig_t;

static void float_digits(double x, fdig_t *f) {
    char tmp[20];
    int ni = 0, i;
    int scale = 0;
    unsigned long long ip;

    f->n = 0;
    f->exp = 0;
    f->sticky = 0;
    while (x >= 18446744073709551616.0) { /* >= 2^64: scale down (inexact) */
        x /= 1e10;
        scale += 10;
    }
    ip = (unsigned long long)x;
    x -= (double)ip;
    while (ip) {
        tmp[ni++] = (char)('0' + (int)(ip % 10));
        ip /= 10;
    }
    for (i = ni - 1; i >= 0; i--) f->d[f->n++] = tmp[i];
    f->exp = ni + scale;
    if (ni == 0 && x > 0) {
        while (x < 0.1) { /* skip leading fractional zeros */
            x *= 10;
            f->exp--;
        }
    }
    while (x > 0 && f->n < FDIG) {
        int dg;
        x *= 10;
        dg = (int)x;
        x -= dg;
        f->d[f->n++] = (char)('0' + dg);
    }
    if (x > 0) f->sticky = 1;
    if (f->n == 0) f->exp = 1; /* zero: "0.", exp 1 -> one integer digit */
}

/* Keep `keep` digits (counted from d[0]), rounding half to even.
 * keep may be <= 0 or > n. */
static void float_round(fdig_t *f, int keep) {
    int up = 0, i;
    if (keep >= f->n) return;
    if (keep < 0) {
        f->n = 0;
        return;
    }
    if (f->d[keep] > '5') {
        up = 1;
    } else if (f->d[keep] == '5') {
        up = f->sticky;
        for (i = keep + 1; i < f->n && !up; i++) up = f->d[i] != '0';
        if (!up) up = keep > 0 ? ((f->d[keep - 1] - '0') & 1) : 0; /* tie: to even */
    }
    f->n = keep;
    f->sticky = 0;
    if (!up) return;
    for (i = keep - 1; i >= 0; i--) {
        if (f->d[i] != '9') {
            f->d[i]++;
            return;
        }
        f->d[i] = '0';
    }
    /* carry out of the first digit: 999 -> 1000 */
    for (i = (f->n < FDIG ? f->n : FDIG - 1); i > 0; i--) f->d[i] = f->d[i - 1];
    f->d[0] = '1';
    if (f->n < FDIG) f->n++;
    f->exp++;
}

static char fdig_at(const fdig_t *f, int i) {
    return (i >= 0 && i < f->n) ? f->d[i] : '0';
}

/* Writes the body of %f (style 0) or %e (style 1) with `prec` decimals;
 * `trim` removes trailing zeros (and a bare '.') for %g. Returns length. */
static int float_body(out_t *o, const fdig_t *f, int style, int prec, int flags, int trim, int emit) {
    int len = 0, i, ip_digits, e;
    int dot;
    char eb[8];
    int ne = 0;

    if (trim) { /* drop trailing zero decimals */
        int first = style ? 1 : f->exp; /* index of first decimal digit */
        while (prec > 0 && fdig_at(f, first + prec - 1) == '0') prec--;
    }
    dot = prec > 0 || (flags & F_HASH);
    if (style == 0) {
        ip_digits = f->exp > 0 ? f->exp : 0;
        if (ip_digits == 0) {
            if (emit) put(o, '0');
            len++;
        }
        for (i = 0; i < ip_digits; i++, len++)
            if (emit) put(o, fdig_at(f, i));
        if (dot) {
            if (emit) put(o, '.');
            len++;
        }
        for (i = 0; i < prec; i++, len++)
            if (emit) put(o, fdig_at(f, f->exp + i));
    } else {
        if (emit) put(o, fdig_at(f, 0));
        len++;
        if (dot) {
            if (emit) put(o, '.');
            len++;
        }
        for (i = 0; i < prec; i++, len++)
            if (emit) put(o, fdig_at(f, 1 + i));
        e = f->n ? f->exp - 1 : 0;
        eb[ne++] = (flags & F_UPPER) ? 'E' : 'e';
        eb[ne++] = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        if (e >= 100) eb[ne++] = (char)('0' + e / 100);
        eb[ne++] = (char)('0' + e / 10 % 10);
        eb[ne++] = (char)('0' + e % 10);
        if (emit) put_s(o, eb, ne);
        len += ne;
    }
    return len;
}

static void fmt_float(out_t *o, double x, char conv, int flags, int width, int prec) {
    union {
        double d;
        unsigned long long u;
    } bits;
    char pre[1];
    int npre = 0, style, trim = 0, len, keep;
    fdig_t f;

    bits.d = x;
    if (bits.u >> 63) {
        pre[npre++] = '-';
        x = -x;
    } else if (flags & F_PLUS) {
        pre[npre++] = '+';
    } else if (flags & F_SPACE) {
        pre[npre++] = ' ';
    }
    if (x != x || x > 1.7976931348623157e308) { /* nan / inf: no '0' padding */
        const char *s = (x != x) ? ((flags & F_UPPER) ? "NAN" : "nan") : ((flags & F_UPPER) ? "INF" : "inf");
        put_field(o, flags & ~F_ZERO, width, pre, npre, 0, s, 3);
        return;
    }
    if (prec < 0) prec = 6;
    float_digits(x, &f);
    if (conv == 'f') {
        style = 0;
        float_round(&f, f.exp + prec);
        if (f.n == 0) f.exp = 1;
    } else if (conv == 'e') {
        style = 1;
        float_round(&f, prec + 1);
    } else { /* %g */
        int p = prec ? prec : 1, X;
        float_round(&f, p);
        X = f.n ? f.exp - 1 : 0;
        if (p > X && X >= -4) {
            style = 0;
            prec = p - 1 - X;
        } else {
            style = 1;
            prec = p - 1;
        }
        trim = !(flags & F_HASH);
    }
    if (f.n == 0) f.exp = style ? 1 : (f.exp > 0 ? f.exp : 1);
    keep = 0;
    len = float_body(o, &f, style, prec, flags, trim, 0) + npre;
    if (width > len) keep = width - len;
    if (!(flags & F_MINUS) && !(flags & F_ZERO)) put_n(o, ' ', keep);
    put_s(o, pre, npre);
    if (!(flags & F_MINUS) && (flags & F_ZERO)) put_n(o, '0', keep);
    float_body(o, &f, style, prec, flags, trim, 1);
    if (flags & F_MINUS) put_n(o, ' ', keep);
}
#endif /* TBLOG_LITE_FLOAT */

int tblog_lite_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    out_t o;
    o.buf = buf;
    o.cap = n;
    o.pos = 0;

    while (*fmt) {
        int flags = 0, width = 0, prec = -1, lng = 0, base = 10;
        const char *spec = fmt;
        uval_t v;
        char c;

        if (*fmt != '%') {
            put(&o, *fmt++);
            continue;
        }
        fmt++;
        for (;; fmt++) { /* flags */
            if (*fmt == '-') flags |= F_MINUS;
            else if (*fmt == '0') flags |= F_ZERO;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_HASH;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_MINUS;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }
        /* length: lng = -2 hh, -1 h, 0 none, 1 l, 2 ll, 3 z */
        if (*fmt == 'h') {
            lng = -1;
            if (*++fmt == 'h') {
                lng = -2;
                fmt++;
            }
        } else if (*fmt == 'l') {
            lng = 1;
            if (*++fmt == 'l') {
                lng = 2;
                fmt++;
            }
        } else if (*fmt == 'z') {
            lng = 3;
            fmt++;
        }
        c = *fmt;
        if (c) fmt++;
        if (c == 'E' || c == 'F' || c == 'G') {
            flags |= F_UPPER;
            c = (char)(c - 'A' + 'a');
        }
        switch (c) {
        case 'd':
        case 'i': {
            long long sv;
            if (lng == 2) sv = va_arg(ap, long long);
            else if (lng == 1) sv = va_arg(ap, long);
            else if (lng == 3) sv = (long long)(ptrdiff_t)va_arg(ap, size_t);
            else sv = va_arg(ap, int);
            if (lng == -1) sv = (short)sv;
            else if (lng == -2) sv = (signed char)sv;
            v = sv < 0 ? (uval_t)0 - (uval_t)sv : (uval_t)sv;
            fmt_int(&o, v, sv < 0, 10, flags & ~F_HASH, width, prec);
            continue;
        }
        case 'X':
            flags |= F_UPPER;
            /* fall through */
        case 'x':
            base = 16;
            goto unsigned_conv;
        case 'o':
            base = 8;
            /* fall through */
        case 'u':
        unsigned_conv:
            if (lng == 2) v = (uval_t)va_arg(ap, unsigned long long);
            else if (lng == 1) v = va_arg(ap, unsigned long);
            else if (lng == 3) v = va_arg(ap, size_t);
            else v = va_arg(ap, unsigned int);
            if (lng == -1) v = (unsigned short)v;
            else if (lng == -2) v = (unsigned char)v;
            fmt_int(&o, v, 0, base, flags & ~(F_PLUS | F_SPACE | (base == 10 ? F_HASH : 0)), width, prec);
            continue;
        case 'p': {
            void *p = va_arg(ap, void *);
            if (!p) {
                put_field(&o, flags & F_MINUS, width, "", 0, 0, "(nil)", 5);
            } else {
                fmt_int(&o, (uval_t)(uintptr_t)p, 0, 16, (flags & (F_MINUS | F_PLUS | F_SPACE)) | F_HASH,
                        width, prec);
            }
            continue;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            put_field(&o, flags & F_MINUS, width, "", 0, 0, &ch, 1);
            continue;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = 0;
            if (!s) s = (prec < 0 || prec >= 6) ? "(null)" : "";
            while ((prec < 0 || len < prec) && s[len]) len++;
            put_field(&o, flags & F_MINUS, width, "", 0, 0, s, len);
            continue;
        }
#if TBLOG_LITE_FLOAT
        case 'f':
        case 'e':
        case 'g':
            fmt_float(&o, va_arg(ap, double), c, flags, width, prec);
            continue;
#endif
        case '%':
            put(&o, '%');
            continue;
        default: /* unsupported: print the spec as written */
            put_s(&o, spec, (int)(fmt - spec));
            continue;
        }
    }
    if (n) o.buf[o.pos < n ? o.pos : n - 1] = '\0';
    return (int)o.pos;
}

int tblog_lite_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = tblog_lite_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
