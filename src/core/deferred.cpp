// Deferred formatting (format.mode = deferred). See "Deferred formatting" in
// src/internal.h.
//
// Per call site the format string is compiled once into a Plan (one Spec per
// conversion: literal length before it, argument kind, flags, static
// width/precision) cached in tblog_site::cache_. The caller then only walks
// the plan, copying raw argument bytes into a record:
//
//   u8 level | u32 tid | u64 ts_ns | tblog_module* | tblog_site* | args...
//   args: [i32 '*' width] [i32 '*' precision] value, per conversion:
//         int/long/long long/size_t/intmax_t/ptrdiff_t at their own size,
//         double, long double, void*, and %s as u16 length (0xFFFF = NULL)
//         + bytes + NUL (copied by value, capped, see kStrCap).
//
// The worker renders a record with an exact mini-printf for integer, %c and
// %s conversions (the property test checks it against glibc byte for byte)
// and snprintf() of the single original conversion spec for floating point,
// %p and anything using the "'" flag. Everything outside the supported set
// (%n, %ls/%lc, %m, positional args, undefined flag combinations, ...) makes
// the plan "eager": that call site is always formatted on the caller.
//
// The crash renderer reuses the mini-printf (async-signal-safe) and swaps the
// snprintf() conversions for approximate AS-safe ones.
#include "internal.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace tblog {
namespace {

// Bytes of a %s argument copied into a record. A line is at most
// kScratchSize bytes, so with a left-aligned or unpadded string the cut never
// shows; a right-aligned one whose width exceeds the copy goes eager.
constexpr size_t kStrCap = kScratchSize;
constexpr uint16_t kNullStr = 0xFFFF;
constexpr int kMaxWidth = 100000;  // larger width/precision -> eager
constexpr size_t kMaxSpecLen = 63;
constexpr size_t kHdr = 1 + 4 + 8 + 2 * sizeof(void *);

enum Kind : uint8_t { kPct, kInt, kLong, kLLong, kSize, kIntMax, kPtrDiff, kDouble, kLDouble, kPtr, kStr };

enum : uint8_t {
    fStarW = 1,
    fStarP = 2,
    fLeft = 4,
    fPlus = 8,
    fSpace = 16,
    fAlt = 32,
    fZero = 64,
    fQuote = 128,
};

struct Spec {
    uint16_t lit;   // literal bytes before this conversion
    uint8_t len;    // spec text length including '%'
    uint8_t kind;   // Kind
    uint8_t flags;  // f*
    char conv;      // conversion character
    char lenmod;    // 0, 'H' (hh), 'h', 'l', 'q' (ll), 'z', 'j', 't', 'L'
    int32_t width;  // static width (-1: none)
    int32_t prec;   // static precision (-1: none)
};

struct Plan {
    uint32_t eager;   // 1: never defer this site
    uint32_t nspecs;
    uint32_t tail;    // literal bytes after the last conversion
    const Spec *specs;
};

Plan g_eager_plan = {1, 0, 0, nullptr};

std::atomic<long> g_utc_offset{0};
TBLOG_ASSERT_LOCK_FREE(long);

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Parses one conversion at p (pointing at '%'). Returns false -> eager.
bool parse_spec(const char *p, Spec &s, const char **next) {
    const char *q = p + 1;
    s.flags = 0;
    s.width = -1;
    s.prec = -1;
    s.lenmod = 0;
    if (*q == '%') {
        s.kind = kPct;
        s.conv = '%';
        *next = q + 1;
        s.len = 2;
        return true;
    }
    for (;; ++q) {
        switch (*q) {
            case '-': s.flags |= fLeft; continue;
            case '+': s.flags |= fPlus; continue;
            case ' ': s.flags |= fSpace; continue;
            case '#': s.flags |= fAlt; continue;
            case '0': s.flags |= fZero; continue;
            case '\'': s.flags |= fQuote; continue;
            default: break;
        }
        break;
    }
    if (*q == '*') {
        s.flags |= fStarW;
        ++q;
        if (is_digit(*q)) return false;  // "*1$"
    } else if (is_digit(*q)) {
        long n = 0;
        while (is_digit(*q)) {
            n = n * 10 + (*q++ - '0');
            if (n > kMaxWidth) return false;
        }
        if (*q == '$') return false;  // positional
        s.width = (int32_t)n;
    }
    if (*q == '.') {
        ++q;
        if (*q == '*') {
            s.flags |= fStarP;
            ++q;
            if (is_digit(*q)) return false;
        } else {
            long n = 0;
            while (is_digit(*q)) {
                n = n * 10 + (*q++ - '0');
                if (n > kMaxWidth) return false;
            }
            s.prec = (int32_t)n;
        }
    }
    switch (*q) {
        case 'h':
            if (q[1] == 'h') { s.lenmod = 'H'; q += 2; }
            else { s.lenmod = 'h'; ++q; }
            break;
        case 'l':
            if (q[1] == 'l') { s.lenmod = 'q'; q += 2; }
            else { s.lenmod = 'l'; ++q; }
            break;
        case 'z': case 'j': case 't': case 'L': s.lenmod = *q++; break;
        default: break;
    }
    const char c = *q;
    if (!c) return false;
    s.conv = c;
    const uint8_t f = s.flags;
    switch (c) {
        case 'd': case 'i': case 'u':
            if (f & fAlt) return false;  // undefined
            /* fallthrough */
        case 'o': case 'x': case 'X':
            switch (s.lenmod) {
                case 0: case 'H': case 'h': s.kind = kInt; break;
                case 'l': s.kind = kLong; break;
                case 'q': s.kind = kLLong; break;
                case 'z': s.kind = kSize; break;
                case 'j': s.kind = kIntMax; break;
                case 't': s.kind = kPtrDiff; break;
                default: return false;  // %Ld etc.
            }
            break;
        case 'c':
            if (s.lenmod || (f & (fAlt | fZero)) || s.prec >= 0 || (f & fStarP)) return false;
            s.kind = kInt;
            break;
        case 's':
            if (s.lenmod || (f & (fAlt | fZero))) return false;
            s.kind = kStr;
            break;
        case 'p':
            if (s.lenmod) return false;
            s.kind = kPtr;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            if (s.lenmod == 0 || s.lenmod == 'l') s.kind = kDouble;
            else if (s.lenmod == 'L') s.kind = kLDouble;
            else return false;
            break;
        default: return false;  // %n, %m, %C, %S, unknown
    }
    ++q;
    if ((size_t)(q - p) > kMaxSpecLen) return false;
    s.len = (uint8_t)(q - p);
    *next = q;
    return true;
}

const Plan *build_plan(const char *fmt) {
    size_t flen = std::strlen(fmt);
    if (flen > 65535) return &g_eager_plan;
    size_t maxspecs = 0;
    for (const char *p = fmt; *p; ++p)
        if (*p == '%') ++maxspecs;
    if (maxspecs > 255) return &g_eager_plan;
    void *mem = std::malloc(sizeof(Plan) + maxspecs * sizeof(Spec));
    if (!mem) return &g_eager_plan;
    Plan *pl = static_cast<Plan *>(mem);
    Spec *specs = reinterpret_cast<Spec *>(pl + 1);
    uint32_t n = 0;
    const char *p = fmt, *lit = fmt;
    while (*p) {
        if (*p != '%') {
            ++p;
            continue;
        }
        Spec s;
        s.lit = (uint16_t)(p - lit);
        const char *next = nullptr;
        if (!parse_spec(p, s, &next)) {
            std::free(mem);
            return &g_eager_plan;
        }
        specs[n++] = s;
        p = lit = next;
    }
    pl->eager = 0;
    pl->nspecs = n;
    pl->tail = (uint32_t)(p - lit);
    pl->specs = specs;
    return pl;
}

const Plan *site_plan(tblog_site *site) {
    void *c = __atomic_load_n(&site->cache_, __ATOMIC_ACQUIRE);
    if (__builtin_expect(c != nullptr, 1)) return static_cast<const Plan *>(c);
    const Plan *pl = build_plan(site->fmt);
    void *expected = nullptr;
    void *mine = const_cast<Plan *>(pl);
    if (__atomic_compare_exchange_n(&site->cache_, &expected, mine, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return pl;
    if (pl != &g_eager_plan) std::free(mine);
    return static_cast<const Plan *>(expected);
}

// ------------------------------------------------------------------ encoding
struct Writer {
    char *p;
    char *end;
    template <class T>
    bool put(T v) {
        if ((size_t)(end - p) < sizeof v) return false;
        std::memcpy(p, &v, sizeof v);
        p += sizeof v;
        return true;
    }
};

// ------------------------------------------------------------------ decoding
struct Reader {
    const char *p;
    const char *end;
    template <class T>
    bool get(T &v) {
        if ((size_t)(end - p) < sizeof v) return false;
        std::memcpy(&v, p, sizeof v);
        p += sizeof v;
        return true;
    }
};

// Bounded message writer that keeps counting past its capacity (like
// vsnprintf's return value).
struct MsgOut {
    char *buf;
    size_t cap;
    size_t total;
    bool err;

    void put(const char *s, size_t n) {
        if (total < cap) {
            size_t room = cap - total;
            std::memcpy(buf + total, s, n < room ? n : room);
        }
        total += n;
    }
    void fill(char c, size_t n) {
        if (total < cap) {
            size_t room = cap - total;
            std::memset(buf + total, c, n < room ? n : room);
        }
        total += n;
    }
    char *dst() { return total < cap ? buf + total : nullptr; }
    size_t room() const { return total < cap ? cap - total : 0; }
};

// Exact printf integer conversion (d i u o x X). Async-signal-safe.
void put_int(MsgOut &o, char conv, unsigned flags, int width, int prec, bool neg, uint64_t mag) {
    char digits[24];
    size_t nd = 0;
    if (!(mag == 0 && prec == 0)) {
        const unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : 10;
        const char *dig = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char tmp[24];
        do {
            tmp[nd++] = dig[mag % base];
            mag /= base;
        } while (mag);
        for (size_t i = 0; i < nd; ++i) digits[i] = tmp[nd - 1 - i];
    }
    size_t zeros = prec > 0 && (size_t)prec > nd ? (size_t)prec - nd : 0;
    if (conv == 'o' && (flags & fAlt) && zeros == 0 && (nd == 0 || digits[0] != '0')) zeros = 1;
    char prefix[2];
    size_t np = 0;
    if (conv == 'd' || conv == 'i') {
        if (neg) prefix[np++] = '-';
        else if (flags & fPlus) prefix[np++] = '+';
        else if (flags & fSpace) prefix[np++] = ' ';
    } else if ((conv == 'x' || conv == 'X') && (flags & fAlt) && nd > 0 &&
               !(nd == 1 && digits[0] == '0')) {
        prefix[np++] = '0';
        prefix[np++] = conv;
    }
    const size_t body = np + zeros + nd;
    const size_t pad = width > 0 && (size_t)width > body ? (size_t)width - body : 0;
    if (flags & fLeft) {
        o.put(prefix, np);
        o.fill('0', zeros);
        o.put(digits, nd);
        o.fill(' ', pad);
    } else if ((flags & fZero) && prec < 0) {
        o.put(prefix, np);
        o.fill('0', zeros + pad);
        o.put(digits, nd);
    } else {
        o.fill(' ', pad);
        o.put(prefix, np);
        o.fill('0', zeros);
        o.put(digits, nd);
    }
}

void put_padded(MsgOut &o, unsigned flags, int width, const char *s, size_t n) {
    const size_t pad = width > 0 && (size_t)width > n ? (size_t)width - n : 0;
    if (!(flags & fLeft)) o.fill(' ', pad);
    o.put(s, n);
    if (flags & fLeft) o.fill(' ', pad);
}

// ---------------------------------------------------- approximate floats (crash)
// Decimal digits of a >= 0: dg[0..nd) with value 0.d0d1d2... x 10^(e10+1).
struct Digits {
    char dg[24];
    int nd;
    int e10;
};

void make_digits(long double a, Digits &d) {
    d.e10 = 0;
    d.nd = 0;
    if (a == 0) return;
    while (a >= 1e32L) { a /= 1e32L; d.e10 += 32; }
    while (a >= 10) { a /= 10; ++d.e10; }
    while (a < 1e-32L) { a *= 1e32L; d.e10 -= 32; }
    while (a < 1) { a *= 10; --d.e10; }
    for (d.nd = 0; d.nd < 19; ++d.nd) {
        int x = (int)a;
        if (x > 9) x = 9;
        if (x < 0) x = 0;
        d.dg[d.nd] = (char)('0' + x);
        a = (a - x) * 10;
    }
    while (d.nd > 0 && d.dg[d.nd - 1] == '0') --d.nd;
}

// Keep `keep` significant digits, rounding half up.
void round_digits(Digits &d, int keep) {
    if (keep >= d.nd) return;
    if (keep < 0) {
        d.nd = 0;
        return;
    }
    bool up = d.dg[keep] >= '5';
    d.nd = keep;
    if (!up) return;
    int i = keep - 1;
    while (i >= 0 && d.dg[i] == '9') --i;
    if (i < 0) {  // 99.9 -> 100 (also keep == 0)
        d.dg[0] = '1';
        d.nd = 1;
        ++d.e10;
        return;
    }
    ++d.dg[i];
    d.nd = i + 1;
}

inline char digit_at(const Digits &d, int pos) {  // pos = power of ten
    int i = d.e10 - pos;
    return i >= 0 && i < d.nd ? d.dg[i] : '0';
}

void put_float_safe(MsgOut &o, char conv, unsigned flags, int width, int prec, long double v) {
    const bool upper = conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A';
    const bool neg = __builtin_signbit(v) != 0;
    char body[160];
    size_t n = 0;
    bool finite = true;
    if (v != v) {
        std::memcpy(body, upper ? "NAN" : "nan", 3);
        n = 3;
        finite = false;
    } else if (v - v != 0) {
        std::memcpy(body, upper ? "INF" : "inf", 3);
        n = 3;
        finite = false;
    } else {
        Digits d;
        make_digits(neg ? -v : v, d);
        if (prec < 0) prec = 6;
        if (prec > 60) prec = 60;
        char style = conv | 0x20;
        if (style == 'a') style = 'e';  // approximation: decimal instead of hex
        bool strip = false;
        if (style == 'g') {
            const int P = prec == 0 ? 1 : prec;
            Digits t = d;
            round_digits(t, P);
            const int X = t.nd ? t.e10 : 0;
            if (X < P && X >= -4) {
                style = 'f';
                prec = P - 1 - X;
            } else {
                style = 'e';
                prec = P - 1;
            }
            strip = !(flags & fAlt);
        }
        if (style == 'f' && d.e10 > 60) style = 'e';  // keep the body bounded
        if (style == 'f') {
            round_digits(d, d.e10 + 1 + prec);
            if (d.nd == 0) d.e10 = 0;
            if (d.e10 >= 0)
                for (int k = d.e10; k >= 0; --k) body[n++] = digit_at(d, k);
            else
                body[n++] = '0';
            if (prec > 0 || (flags & fAlt)) body[n++] = '.';
            for (int k = 1; k <= prec; ++k) body[n++] = digit_at(d, -k);
        } else {
            round_digits(d, 1 + prec);
            const int e = d.nd ? d.e10 : 0;
            body[n++] = d.nd ? d.dg[0] : '0';
            if (prec > 0 || (flags & fAlt)) body[n++] = '.';
            for (int k = 1; k <= prec; ++k) body[n++] = k < d.nd ? d.dg[k] : '0';
            if (strip) {
                while (n > 0 && body[n - 1] == '0') --n;
                if (n > 0 && body[n - 1] == '.') --n;
            }
            body[n++] = upper ? 'E' : 'e';
            body[n++] = e < 0 ? '-' : '+';
            int ae = e < 0 ? -e : e;
            char eb[8];
            int ne = 0;
            do {
                eb[ne++] = (char)('0' + ae % 10);
                ae /= 10;
            } while (ae);
            if (ne < 2) eb[ne++] = '0';
            while (ne) body[n++] = eb[--ne];
            strip = false;
        }
        if (strip) {
            bool dot = false;
            for (size_t i = 0; i < n; ++i) dot |= body[i] == '.';
            if (dot) {
                while (n > 0 && body[n - 1] == '0') --n;
                if (n > 0 && body[n - 1] == '.') --n;
            }
        }
    }
    char sign = neg ? '-' : (flags & fPlus) ? '+' : (flags & fSpace) ? ' ' : 0;
    const size_t len = n + (sign ? 1 : 0);
    const size_t pad = width > 0 && (size_t)width > len ? (size_t)width - len : 0;
    if (flags & fLeft) {
        if (sign) o.put(&sign, 1);
        o.put(body, n);
        o.fill(' ', pad);
    } else if ((flags & fZero) && finite) {
        if (sign) o.put(&sign, 1);
        o.fill('0', pad);
        o.put(body, n);
    } else {
        o.fill(' ', pad);
        if (sign) o.put(&sign, 1);
        o.put(body, n);
    }
}

void put_ptr_safe(MsgOut &o, unsigned flags, int width, const void *p) {
    char b[24];
    size_t n = 0;
    uintptr_t v = (uintptr_t)p;
    if (!v) {
        std::memcpy(b, "(nil)", 5);
        n = 5;
    } else {
        char tmp[20];
        size_t nd = 0;
        do {
            tmp[nd++] = "0123456789abcdef"[v & 15];
            v >>= 4;
        } while (v);
        b[n++] = '0';
        b[n++] = 'x';
        while (nd) b[n++] = tmp[--nd];
    }
    put_padded(o, flags, width, b, n);
}

// ---------------------------------------------------- one conversion via libc
template <class T>
int libc_conv(MsgOut &o, const char *spec, unsigned flags, int w, int p, T v) {
    char *d = o.dst();
    size_t r = o.room();
    if ((flags & (fStarW | fStarP)) == (fStarW | fStarP)) return std::snprintf(d, r, spec, w, p, v);
    if (flags & fStarW) return std::snprintf(d, r, spec, w, v);
    if (flags & fStarP) return std::snprintf(d, r, spec, p, v);
    return std::snprintf(d, r, spec, v);
}

void add_libc(MsgOut &o, int n) {
    if (n < 0) o.err = true;
    else o.total += (size_t)n;
}

// Renders the message of a record. `safe`: async-signal-safe (crash) mode.
// Returns false if the record is malformed.
bool render_msg(MsgOut &o, const Plan &pl, const char *fmt, Reader &r, bool safe) {
    const char *f = fmt;
    for (uint32_t i = 0; i < pl.nspecs; ++i) {
        const Spec &s = pl.specs[i];
        o.put(f, s.lit);
        const char *spec_text = f + s.lit;
        f = spec_text + s.len;
        if (s.kind == kPct) {
            o.put("%", 1);
            continue;
        }
        unsigned flags = s.flags;
        int width = s.width, prec = s.prec;
        int32_t sw = 0, sp = 0;
        if (flags & fStarW) {
            if (!r.get(sw)) return false;
            width = sw;
            if (width < 0) {
                flags |= fLeft;
                width = -width;
            }
        }
        if (flags & fStarP) {
            if (!r.get(sp)) return false;
            prec = sp < 0 ? -1 : sp;
        }
        char spec[kMaxSpecLen + 1];
        std::memcpy(spec, spec_text, s.len);
        spec[s.len] = '\0';
        const bool libc = !safe;
        const char c = s.conv;

        // Integers.
        if (s.kind >= kInt && s.kind <= kPtrDiff) {
            int64_t sv = 0;
            uint64_t uv = 0;
            switch (s.kind) {
                case kInt: { int v; if (!r.get(v)) return false; sv = v; uv = (unsigned)v; break; }
                case kLong: { long v; if (!r.get(v)) return false; sv = v; uv = (unsigned long)v; break; }
                case kLLong: { long long v; if (!r.get(v)) return false; sv = v; uv = (unsigned long long)v; break; }
                case kSize: { size_t v; if (!r.get(v)) return false; sv = (int64_t)v; uv = v; break; }
                case kIntMax: { intmax_t v; if (!r.get(v)) return false; sv = v; uv = (uintmax_t)v; break; }
                default: { ptrdiff_t v; if (!r.get(v)) return false; sv = v; uv = (uint64_t)v; break; }
            }
            if (libc && (flags & fQuote)) {
                int n;
                switch (s.kind) {
                    case kInt: n = libc_conv(o, spec, flags, sw, sp, (int)sv); break;
                    case kLong: n = libc_conv(o, spec, flags, sw, sp, (long)sv); break;
                    case kLLong: n = libc_conv(o, spec, flags, sw, sp, (long long)sv); break;
                    case kSize: n = libc_conv(o, spec, flags, sw, sp, (size_t)uv); break;
                    case kIntMax: n = libc_conv(o, spec, flags, sw, sp, (intmax_t)sv); break;
                    default: n = libc_conv(o, spec, flags, sw, sp, (ptrdiff_t)sv); break;
                }
                add_libc(o, n);
                continue;
            }
            if (c == 'c') {
                char ch = (char)(unsigned char)sv;
                put_padded(o, flags, width, &ch, 1);
                continue;
            }
            if (s.lenmod == 'H') {
                sv = (signed char)sv;
                uv = (unsigned char)uv;
            } else if (s.lenmod == 'h') {
                sv = (short)sv;
                uv = (unsigned short)uv;
            } else if (s.kind == kInt) {
                uv = (unsigned)uv;
            }
            if (c == 'd' || c == 'i') {
                const bool neg = sv < 0;
                put_int(o, c, flags, width, prec, neg, neg ? 0 - (uint64_t)sv : (uint64_t)sv);
            } else {
                put_int(o, c, flags, width, prec, false, uv);
            }
            continue;
        }
        switch (s.kind) {
            case kStr: {
                uint16_t n;
                if (!r.get(n)) return false;
                if (n == kNullStr) {
                    if (libc) {
                        add_libc(o, libc_conv(o, spec, flags, sw, sp, (const char *)nullptr));
                    } else if (prec < 0 || prec >= 6) {
                        put_padded(o, flags, width, "(null)", 6);
                    } else {
                        put_padded(o, flags, width, "", 0);
                    }
                    break;
                }
                if ((size_t)(r.end - r.p) < (size_t)n + 1) return false;
                const char *str = r.p;
                r.p += n + 1;
                size_t len = n;
                if (prec >= 0 && (size_t)prec < len) len = (size_t)prec;
                put_padded(o, flags, width, str, len);
                break;
            }
            case kPtr: {
                void *v;
                if (!r.get(v)) return false;
                if (libc) add_libc(o, libc_conv(o, spec, flags, sw, sp, v));
                else put_ptr_safe(o, flags, width, v);
                break;
            }
            case kDouble: {
                double v;
                if (!r.get(v)) return false;
                if (libc) add_libc(o, libc_conv(o, spec, flags, sw, sp, v));
                else put_float_safe(o, c, flags, width, prec, v);
                break;
            }
            case kLDouble: {
                long double v;
                if (!r.get(v)) return false;
                if (libc) add_libc(o, libc_conv(o, spec, flags, sw, sp, v));
                else put_float_safe(o, c, flags, width, prec, v);
                break;
            }
            default: return false;
        }
    }
    o.put(f, pl.tail);
    return true;
}

struct Header {
    tblog_level lv;
    uint32_t tid;
    uint64_t ts;
    const tblog_module *m;
    const tblog_site *site;
    const Plan *plan;
};

bool read_header(Reader &r, Header &h) {
    uint8_t lv;
    if (!r.get(lv) || !r.get(h.tid) || !r.get(h.ts) || !r.get(h.m) || !r.get(h.site)) return false;
    h.lv = (tblog_level)lv;
    if (!h.site || !h.site->fmt) return false;
    h.plan = static_cast<const Plan *>(
        __atomic_load_n(&const_cast<tblog_site *>(h.site)->cache_, __ATOMIC_ACQUIRE));
    return h.plan && !h.plan->eager;
}

size_t render(char *out, const Formatter &f, const char *rec, size_t len, bool safe) {
    Reader r{rec, rec + len};
    Header h;
    char msg[kScratchSize];
    MsgOut o{msg, sizeof msg, 0, false};
    static const char kBad[] = "<tblog: unreadable deferred record>";
    if (!read_header(r, h)) {
        std::memcpy(out, kBad, sizeof kBad - 1);
        out[sizeof kBad - 1] = '\n';
        return sizeof kBad;
    }
    Record rec_meta{h.ts, h.tid, h.lv, h.m ? h.m->name : "", h.site->file, h.site->line,
                    h.site->func};
    size_t mlen;
    if (!render_msg(o, *h.plan, h.site->fmt, r, safe)) {
        o.total = 0;
        o.put(kBad, sizeof kBad - 1);
        mlen = o.total;
    } else {
        mlen = o.err ? SIZE_MAX : o.total;
    }
    if (safe) {
        const long off = g_utc_offset.load(std::memory_order_relaxed);
        return f.format_message(out, kScratchSize, rec_meta, msg, mlen, &off);
    }
    return f.format_message(out, kScratchSize, rec_meta, msg, mlen, nullptr);
}

}  // namespace

size_t deferred_encode(char *out, size_t cap, uint64_t ts_ns, uint32_t tid, tblog_level lv,
                       const tblog_module *m, tblog_site *site, const char *fmt,
                       va_list ap) noexcept {
    if (!site || !fmt || fmt != site->fmt || cap < kHdr) return 0;
    const Plan *pl = site_plan(site);
    if (pl->eager) return 0;
    Writer w{out, out + cap};
    w.put((uint8_t)lv);
    w.put(tid);
    w.put(ts_ns);
    w.put(m);
    w.put((const tblog_site *)site);
    for (uint32_t i = 0; i < pl->nspecs; ++i) {
        const Spec &s = pl->specs[i];
        int width = s.width, prec = s.prec;
        bool left = (s.flags & fLeft) != 0;
        if (s.flags & fStarW) {
            int v = va_arg(ap, int);
            if (v > kMaxWidth || v < -kMaxWidth) return 0;
            if (!w.put((int32_t)v)) return 0;
            if (v < 0) {
                left = true;
                v = -v;
            }
            width = v;
        }
        if (s.flags & fStarP) {
            int v = va_arg(ap, int);
            if (v > kMaxWidth) return 0;
            if (!w.put((int32_t)v)) return 0;
            prec = v < 0 ? -1 : v;
        }
        bool ok = true;
        switch (s.kind) {
            case kPct: break;
            case kInt: ok = w.put(va_arg(ap, int)); break;
            case kLong: ok = w.put(va_arg(ap, long)); break;
            case kLLong: ok = w.put(va_arg(ap, long long)); break;
            case kSize: ok = w.put(va_arg(ap, size_t)); break;
            case kIntMax: ok = w.put(va_arg(ap, intmax_t)); break;
            case kPtrDiff: ok = w.put(va_arg(ap, ptrdiff_t)); break;
            case kDouble: ok = w.put(va_arg(ap, double)); break;
            case kLDouble: ok = w.put(va_arg(ap, long double)); break;
            case kPtr: ok = w.put(va_arg(ap, void *)); break;
            case kStr: {
                const char *str = va_arg(ap, const char *);
                if (!str) {
                    ok = w.put(kNullStr);
                    break;
                }
                size_t lim = kStrCap;
                if (prec >= 0 && (size_t)prec < lim) lim = (size_t)prec;
                const size_t n = strnlen(str, lim);
                // Cut short of what printf would print: only safe when the
                // cut lies beyond the line limit, i.e. not right-aligned.
                if (n == kStrCap && (prec < 0 || (size_t)prec > kStrCap) && !left &&
                    width > (int)n)
                    return 0;
                if ((size_t)(w.end - w.p) < sizeof(uint16_t) + n + 1) return 0;
                w.put((uint16_t)n);
                std::memcpy(w.p, str, n);
                w.p[n] = '\0';
                w.p += n + 1;
                break;
            }
            default: return 0;
        }
        if (!ok) return 0;
    }
    return (size_t)(w.p - out);
}

size_t deferred_render(char *out, const Formatter &f, const char *rec, size_t len) {
    if (len >= kHdr) {
        uint64_t ts;
        std::memcpy(&ts, rec + 1 + 4, sizeof ts);
        deferred_note_utc_offset(ts);
    }
    return render(out, f, rec, len, false);
}

size_t deferred_render_crash(char *out, const Formatter &f, const char *rec,
                             size_t len) noexcept {
    return render(out, f, rec, len, true);
}

void deferred_note_utc_offset(uint64_t ts_ns) {
    static thread_local int64_t last_min = INT64_MIN;
    const int64_t min = (int64_t)(ts_ns / 60000000000ull);
    if (min == last_min) return;
    last_min = min;
    time_t t = (time_t)(ts_ns / 1000000000ull);
    struct tm tm;
    if (localtime_r(&t, &tm)) g_utc_offset.store((long)tm.tm_gmtoff, std::memory_order_relaxed);
}

void deferred_site_reset(tblog_site *site) {
    void *c = __atomic_exchange_n(&site->cache_, (void *)nullptr, __ATOMIC_ACQ_REL);
    if (c && c != &g_eager_plan) std::free(c);
}

}  // namespace tblog
