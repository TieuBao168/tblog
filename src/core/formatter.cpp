// Pattern formatter. The pattern is compiled once into a list of ops; format()
// walks the ops writing into the caller's buffer without allocating.
#include "internal.h"

#include <cstdio>
#include <cstring>
#include <time.h>

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
    if (__builtin_expect(sec != tc.sec, 0)) {
        time_t t = (time_t)sec;
        struct tm tm;
        if (!localtime_r(&t, &tm)) std::memset(&tm, 0, sizeof tm);
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
        if (*p == '/') b = p + 1;
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

}  // namespace

struct Formatter::Impl {
    std::string text;  // concatenated literal text
    std::vector<Op> ops;

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

Formatter::Formatter(const std::string &pattern) : impl_(new Impl) {
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

namespace {

// Async-signal-safe local date/time from a fixed UTC offset (civil-from-days,
// proleptic Gregorian). Used by the crash path instead of localtime_r.
void civil_time(int64_t sec, long utc_offset_s, TimeCache &tc) {
    int64_t t = sec + utc_offset_s;
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t rem = t - days * 86400;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    const int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) ++y;
    if (y < 0) y = 0;
    if (y > 9999) y = 9999;
    tc.date[0] = (char)('0' + y / 1000);
    tc.date[1] = (char)('0' + y / 100 % 10);
    tc.date[2] = (char)('0' + y / 10 % 10);
    tc.date[3] = (char)('0' + y % 10);
    tc.date[4] = '-';
    put2(tc.date + 5, m);
    tc.date[7] = '-';
    put2(tc.date + 8, d);
    put2(tc.time, (int)(rem / 3600));
    tc.time[2] = ':';
    put2(tc.time + 3, (int)(rem / 60 % 60));
    tc.time[5] = ':';
    put2(tc.time + 6, (int)(rem % 60));
    tc.sec = sec;
}

// Walks the compiled ops. `msg(dst, avail)` writes the message like
// vsnprintf (returns its full length, < 0 drops it); `tm(ts)` returns the
// date/time strings of the record's second.
template <class MsgFn, class TimeFn>
size_t run_ops(const std::string &lits, const std::vector<Op> &ops, char *out, size_t cap,
               const Record &rec, MsgFn msg, TimeFn tm) {
    if (cap == 0) return 0;
    if (cap < 5) {  // contract says cap >= 64; stay safe anyway
        out[0] = '\n';
        return 1;
    }
    Out o{out, 0, cap - 1, false};
    const char *text = lits.data();
    char num[24];
    char *const num_end = num + sizeof num;

    for (const Op &op : ops) {
        if (o.trunc) break;
        switch (op.kind) {
            case OpKind::Literal: o.put(text + op.off, op.len); break;
            case OpKind::Date: o.put(tm(rec.ts_ns).date, 10); break;
            case OpKind::Time: o.put(tm(rec.ts_ns).time, 8); break;
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
                // The message may use the reserved '\n' byte for its NUL.
                size_t avail = cap - o.pos;
                int n = msg(out + o.pos, avail);
                if (n < 0) break;  // encoding error: drop message
                if ((size_t)n >= avail) {
                    o.pos = o.lim;
                    o.trunc = true;
                } else {
                    o.pos += (size_t)n;
                }
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

}  // namespace

size_t Formatter::format(char *out, size_t cap, const Record &rec, const char *fmt,
                         va_list ap) const {
    return run_ops(
        impl_->text, impl_->ops, out, cap, rec,
        [&](char *dst, size_t avail) -> int {
            if (!fmt) return 0;
            va_list cp;
            va_copy(cp, ap);
            int n = std::vsnprintf(dst, avail, fmt, cp);
            va_end(cp);
            return n;
        },
        [](uint64_t ts) -> const TimeCache & { return time_cache(ts); });
}

size_t Formatter::format_message(char *out, size_t cap, const Record &rec, const char *msg,
                                 size_t len, const long *utc_offset_s) const noexcept {
    auto put_msg = [&](char *dst, size_t avail) -> int {
        if (len > 0x7fffffffu) return -1;  // vsnprintf would fail (EOVERFLOW)
        // Mirror vsnprintf: at most avail - 1 bytes, then a NUL.
        size_t n = len < avail ? len : avail - 1;
        std::memcpy(dst, msg, n);
        dst[n] = '\0';
        return (int)len;
    };
    if (utc_offset_s) {
        const long off = *utc_offset_s;
        TimeCache local;
        return run_ops(impl_->text, impl_->ops, out, cap, rec, put_msg,
                       [&](uint64_t ts) -> const TimeCache & {
                           int64_t sec = (int64_t)(ts / 1000000000ull);
                           if (local.sec != sec) civil_time(sec, off, local);
                           return local;
                       });
    }
    return run_ops(impl_->text, impl_->ops, out, cap, rec, put_msg,
                   [](uint64_t ts) -> const TimeCache & { return time_cache(ts); });
}

}  // namespace tblog
