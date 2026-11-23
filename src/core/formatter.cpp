// Pattern formatter. The pattern is compiled once into a list of ops; format()
// walks the ops writing into the caller's buffer without allocating.
#include "core/formatter.h"

#include "core/compat.h"
#include "core/levels.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace tblog {
namespace {

enum class OpKind : uint8_t { Literal, Date, Time, Micros, Millis, Level, Tid, Module, File, Line, Func, Message };

struct Op {
    OpKind kind;
    uint32_t off;  // Literal: offset into Impl::text
    uint32_t len;  // Literal: length
};

// Per-thread cache of the local date/time strings for one second.
struct TimeCache {
    int64_t sec = -1;
    char date[10];  // YYYY-MM-DD
    char time[8];   // HH:MM:SS
};

inline void put2(char *p, int v) {
    p[0] = (char)('0' + v / 10);
    p[1] = (char)('0' + v % 10);
}

const TimeCache &time_cache(uint64_t ts_ns) {
    static thread_local TimeCache tc;
    int64_t sec = (int64_t)(ts_ns / 1000000000ull);
    if (TBLOG_UNLIKELY(sec != tc.sec)) {
        std::tm tm;
        if (!pf::local_time(sec, &tm)) std::memset(&tm, 0, sizeof tm);
        int y = tm.tm_year + 1900;
        if (y < 0) y = 0;
        if (y > 9999) y = 9999;
        tc.date[0] = (char)('0' + y / 1000);
        tc.date[1] = (char)('0' + y / 100 % 10);
        tc.date[2] = (char)('0' + y / 10 % 10);
        tc.date[3] = (char)('0' + y % 10);
        tc.date[4] = '-';
        put2(tc.date + 5, tm.tm_mon + 1);
        tc.date[7] = '-';
        put2(tc.date + 8, tm.tm_mday);
        put2(tc.time, tm.tm_hour);
        tc.time[2] = ':';
        put2(tc.time + 3, tm.tm_min);
        tc.time[5] = ':';
        put2(tc.time + 6, tm.tm_sec);
        tc.sec = sec;
    }
    return tc;
}

// Unsigned to decimal; writes into the END of a 20-byte buffer, returns start.
inline char *utoa_rev(uint64_t v, char *end) {
    do {
        *--end = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    return end;
}

inline void put_fixed(char *p, uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; --i) {
        p[i] = (char)('0' + v % 10);
        v /= 10;
    }
}

inline const char *basename_of(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; ++p)
        if (pf::is_path_separator(*p)) b = p + 1;
    return b;
}

// Bounded writer. `lim` = cap - 1 (one byte reserved for the final '\n').
struct Out {
    char *buf;
    size_t pos;
    size_t lim;
    bool trunc;

    inline void put(const char *s, size_t n) {
        if (trunc) return;
        size_t room = lim - pos;
        if (n > room) {
            n = room;
            trunc = true;
        }
        std::memcpy(buf + pos, s, n);
        pos += n;
    }
    inline void puts(const char *s) { put(s, std::strlen(s)); }
};

// ---------------------------------------------------------- message.escape
inline bool needs_escape(unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7f; }

// Bytes written for c: "\n" / "\r" -> 2, other control -> 4 ("\xHH"), else 1.
inline size_t escaped_len(unsigned char c) {
    if (!needs_escape(c)) return 1;
    return (c == '\n' || c == '\r') ? 2 : 4;
}

// Escapes the message bytes buf[start, *end) in place (see Formatter in
// formatter.h), growing the text up to `lim`. Trailing '\n' are dropped
// first. Returns false if the escaped text had to be cut at `lim`.
// No allocation: the common case (no control byte) is one scan; otherwise
// the escapes are written back to front, which never overwrites a byte that
// is still to be read because the text only grows.
// True if some byte of w is below 0x20 (tab included) or equal to 0x7F
// (bit tricks "has less than" / "has value"; exact as to existence, bytes
// >= 0x80 never match). Lets the common case skip 8 bytes per step.
inline bool word_has_control(uint64_t w) {
    const uint64_t ones = 0x0101010101010101ull, high = 0x8080808080808080ull;
    const uint64_t below = (w - ones * 0x20) & ~w & high;
    const uint64_t x = w ^ (ones * 0x7f);
    const uint64_t del = (x - ones) & ~x & high;
    return (below | del) != 0;
}

bool escape_message(char *buf, size_t start, size_t *end, size_t lim) {
    size_t e = *end;
    while (e > start && buf[e - 1] == '\n') --e;
    size_t i = start;
    for (uint64_t w; i + 8 <= e; i += 8) {
        std::memcpy(&w, buf + i, 8);
        if (word_has_control(w)) break;
    }
    while (i < e && !needs_escape(static_cast<unsigned char>(buf[i]))) ++i;
    if (i == e) {
        *end = e;
        return true;
    }
    // How many source bytes fit once escaped (only whole escapes).
    size_t out = i, src = i;
    while (src < e) {
        const size_t l = escaped_len(static_cast<unsigned char>(buf[src]));
        if (out + l > lim) break;
        out += l;
        ++src;
    }
    const bool complete = src == e;
    static const char kHex[] = "0123456789abcdef";
    size_t w = out;
    while (src > i) {
        const unsigned char c = static_cast<unsigned char>(buf[--src]);
        if (!needs_escape(c)) {
            buf[--w] = static_cast<char>(c);
        } else if (c == '\n' || c == '\r') {
            buf[--w] = c == '\n' ? 'n' : 'r';
            buf[--w] = '\\';
        } else {
            buf[--w] = kHex[c & 0xF];
            buf[--w] = kHex[c >> 4];
            buf[--w] = 'x';
            buf[--w] = '\\';
        }
    }
    *end = out;
    return complete;
}

}  // namespace

struct Formatter::Impl {
    std::string text;  // concatenated literal text
    std::vector<Op> ops;
    bool escape = true;  // message.escape

    void add_literal(const char *s, size_t n) {
        if (n == 0) return;
        if (!ops.empty() && ops.back().kind == OpKind::Literal &&
            ops.back().off + ops.back().len == text.size()) {
            ops.back().len += (uint32_t)n;
        } else {
            ops.push_back({OpKind::Literal, (uint32_t)text.size(), (uint32_t)n});
        }
        text.append(s, n);
    }
    void add(OpKind k) { ops.push_back({k, 0, 0}); }
};

Formatter::Formatter(const std::string &pattern, bool escape_message) : impl_(new Impl) {
    impl_->escape = escape_message;
    const char *p = pattern.c_str();
    const char *end = p + pattern.size();
    const char *lit = p;
    while (p < end) {
        if (*p != '%') {
            ++p;
            continue;
        }
        impl_->add_literal(lit, (size_t)(p - lit));
        const char *t = p + 1;
        size_t used = 2;  // bytes consumed including '%'
        if (t >= end) {   // trailing lone '%'
            impl_->add_literal(p, 1);
            p = lit = end;
            break;
        }
        switch (*t) {
            case 'D': impl_->add(OpKind::Date); break;
            case 'T': impl_->add(OpKind::Time); break;
            case 'L': impl_->add(OpKind::Level); break;
            case 't': impl_->add(OpKind::Tid); break;
            case 'n': impl_->add(OpKind::Module); break;
            case 'f': impl_->add(OpKind::File); break;
            case 'l': impl_->add(OpKind::Line); break;
            case 'F': impl_->add(OpKind::Func); break;
            case '%': impl_->add_literal("%", 1); break;
            case 'u':
                if (t + 1 < end && t[1] == 's') {
                    impl_->add(OpKind::Micros);
                    used = 3;
                } else {
                    impl_->add_literal(p, 2);
                }
                break;
            case 'm':
                if (t + 1 < end && t[1] == 's') {
                    impl_->add(OpKind::Millis);
                    used = 3;
                } else {
                    impl_->add(OpKind::Message);
                }
                break;
            default: impl_->add_literal(p, 2); break;  // unknown: verbatim
        }
        p += used;
        lit = p;
    }
    impl_->add_literal(lit, (size_t)(end - lit));
}

Formatter::~Formatter() { delete impl_; }

size_t Formatter::format(char *out, size_t cap, const Record &rec, const char *fmt,
                         va_list ap) const {
    if (cap == 0) return 0;
    if (cap < 5) {  // contract says cap >= 64; stay safe anyway
        out[0] = '\n';
        return 1;
    }
    Out o{out, 0, cap - 1, false};
    const char *text = impl_->text.data();
    char num[24];
    char *const num_end = num + sizeof num;

    for (const Op &op : impl_->ops) {
        if (o.trunc) break;
        switch (op.kind) {
            case OpKind::Literal: o.put(text + op.off, op.len); break;
            case OpKind::Date: o.put(time_cache(rec.ts_ns).date, 10); break;
            case OpKind::Time: o.put(time_cache(rec.ts_ns).time, 8); break;
            case OpKind::Micros:
                put_fixed(num, (uint32_t)(rec.ts_ns % 1000000000ull / 1000u), 6);
                o.put(num, 6);
                break;
            case OpKind::Millis:
                put_fixed(num, (uint32_t)(rec.ts_ns % 1000000000ull / 1000000u), 3);
                o.put(num, 3);
                break;
            case OpKind::Level: o.puts(level_name(rec.level)); break;
            case OpKind::Tid: {
                char *s = utoa_rev(rec.tid, num_end);
                o.put(s, (size_t)(num_end - s));
                break;
            }
            case OpKind::Module:
                if (rec.module && rec.module[0]) o.puts(rec.module);
                else o.put("-", 1);
                break;
            case OpKind::File: o.puts(rec.file ? basename_of(rec.file) : "?"); break;
            case OpKind::Line: {
                bool neg = rec.line < 0;
                uint64_t v = neg ? (uint64_t)(-(int64_t)rec.line) : (uint64_t)rec.line;
                char *s = utoa_rev(v, num_end);
                if (neg) *--s = '-';
                o.put(s, (size_t)(num_end - s));
                break;
            }
            case OpKind::Func: o.puts(rec.func ? rec.func : "?"); break;
            case OpKind::Message: {
                if (!fmt) break;
                // vsnprintf may use the reserved '\n' byte for its NUL.
                size_t avail = cap - o.pos;
                va_list cp;
                va_copy(cp, ap);
                int n = std::vsnprintf(out + o.pos, avail, fmt, cp);
                va_end(cp);
                if (n < 0) break;  // encoding error: drop message
                const size_t start = o.pos;
                if ((size_t)n >= avail) {
                    o.pos = o.lim;
                    o.trunc = true;
                } else {
                    o.pos += (size_t)n;
                }
                if (impl_->escape && !escape_message(out, start, &o.pos, o.lim)) o.trunc = true;
                break;
            }
        }
    }

    if (o.trunc) {
        size_t p = o.pos > cap - 4 ? cap - 4 : o.pos;
        std::memcpy(out + p, "...\n", 4);
        return p + 4;
    }
    out[o.pos++] = '\n';
    return o.pos;
}

}  // namespace tblog
