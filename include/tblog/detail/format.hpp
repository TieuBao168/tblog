// tblog — allocation-free "{}" formatting engine used by <tblog/tblog.hpp>.
// Header-only, C++11. Public names: tblog::Sink, tblog::FormatSpec,
// tblog::format_to. Everything in tblog::detail / tblog::adl is internal.
#ifndef TBLOG_DETAIL_FORMAT_HPP
#define TBLOG_DETAIL_FORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <streambuf>
#include <string>
#include <type_traits>
#include <utility>
#if __cplusplus >= 201703L
#include <string_view>
#endif

namespace tblog {

// A parsed replacement-field spec: {[index]:[[fill]align][sign][#][0][width][.precision][type]}
struct FormatSpec {
    char fill;       // ' ' unless given
    char align;      // 0 (type default), '<', '>' or '^'
    char sign;       // 0 (same as '-'), '+', '-' or ' '
    bool alt;        // '#'
    bool zero;       // '0'
    int width;       // -1 = none
    int precision;   // -1 = none
    char type;       // 0 = none, else one of d x X o b B c e E f F g G s p
    FormatSpec() : fill(' '), align(0), sign(0), alt(false), zero(false), width(-1), precision(-1), type(0) {}
    // True when only fill/align/width were given (what strings-like values accept).
    bool only_padding() const { return !sign && !alt && !zero && precision < 0 && !type; }
};

class Sink;
namespace detail {
struct Arg;
void vformat(Sink &s, const char *fmt, const Arg *args, std::size_t nargs);
template <class T> Arg make_arg(const T &v);
}  // namespace detail

// Fixed-capacity output buffer handed to custom formatters. Never allocates,
// never writes past `cap`; output beyond capacity is dropped and truncated()
// becomes true.
class Sink {
public:
    Sink(char *buf, std::size_t cap) : buf_(buf), cap_(cap), len_(0), trunc_(false) {}
    Sink(const Sink &) = delete;
    Sink &operator=(const Sink &) = delete;

    void put(char c) {
        if (len_ < cap_) buf_[len_++] = c;
        else trunc_ = true;
    }
    void write(const char *p, std::size_t n) {
        const std::size_t room = cap_ - len_;
        if (n > room) { n = room; trunc_ = true; }
        if (n) std::memcpy(buf_ + len_, p, n);
        len_ += n;
    }
    void write(const char *s) { if (s) write(s, std::strlen(s)); }
    void write(const std::string &s) { write(s.data(), s.size()); }
    void fill(std::size_t n, char c) {
        const std::size_t room = cap_ - len_;
        if (n > room) { n = room; trunc_ = true; }
        std::memset(buf_ + len_, c, n);
        len_ += n;
    }
    // Nested formatting with the same "{}" syntax (allocation-free).
    template <class... A> void format(const char *fmt, const A &...args);

    const char *data() const { return buf_; }
    std::size_t size() const { return len_; }
    std::size_t capacity() const { return cap_; }
    bool truncated() const { return trunc_; }

    // --- used by the engine ---
    // Inserts n copies of c at position `at`, shifting [at, size) right.
    void insert(std::size_t at, std::size_t n, char c) {
        if (at > len_) at = len_;
        if (n > cap_ - at) { n = cap_ - at; trunc_ = true; }
        std::size_t tail = len_ - at;
        if (tail > cap_ - at - n) { tail = cap_ - at - n; trunc_ = true; }
        if (tail) std::memmove(buf_ + at + n, buf_ + at, tail);
        std::memset(buf_ + at, c, n);
        len_ = at + n + tail;
    }
    void set_size(std::size_t n) { len_ = n < cap_ ? n : cap_; }
    void mark_truncated() { trunc_ = true; }
    char *raw() { return buf_; }

private:
    char *buf_;
    std::size_t cap_;
    std::size_t len_;
    bool trunc_;
};

namespace detail {

// Visible error markers (functions, not namespace-scope arrays, to keep the
// inline functions below ODR-clean).
inline void bad_spec(Sink &s) { s.write("{!bad-spec}", 11); }
inline void bad_format(Sink &s) { s.write("{!bad-format}", 13); }
inline void missing_arg(Sink &s) { s.write("{!missing-arg}", 14); }
inline void extra_args(Sink &s) { s.write(" {!extra-args}", 14); }

typedef void (*CustomFn)(Sink &, const void *, const FormatSpec &);

struct Arg {
    enum Type : unsigned char { None, Bool, Char, Int, UInt, Float, Double, LongDouble, CStr, Str, Ptr, Custom };
    struct StrV { const char *p; std::size_t n; };
    struct CustomV { const void *obj; CustomFn fn; };
    Type type;
    union {
        long long i;
        unsigned long long u;
        double d;
        long double ld;
        const char *cs;
        StrV s;
        const void *ptr;
        CustomV c;
    };
    Arg() : type(None), u(0) {}
};

// ---------------------------------------------------------------- padding --
// Pads the bytes written since `start` to spec.width. `zero_at` >= 0 means
// "sign-aware zero padding": insert '0's at that offset instead of filling.
inline void pad(Sink &s, std::size_t start, const FormatSpec &sp, char default_align, long zero_at = -1) {
    if (sp.width < 0 || s.truncated()) return;
    const std::size_t len = s.size() - start;
    if (len >= static_cast<std::size_t>(sp.width)) return;
    const std::size_t n = static_cast<std::size_t>(sp.width) - len;
    if (zero_at >= 0 && !sp.align) {
        s.insert(start + static_cast<std::size_t>(zero_at), n, '0');
        return;
    }
    const char a = sp.align ? sp.align : default_align;
    std::size_t left = 0;
    if (a == '>') left = n;
    else if (a == '^') left = n / 2;
    if (left) s.insert(start, left, sp.fill);
    if (n - left) s.fill(n - left, sp.fill);
}

inline void write_padded(Sink &s, const char *p, std::size_t n, const FormatSpec &sp, char default_align) {
    const std::size_t start = s.size();
    s.write(p, n);
    pad(s, start, sp, default_align);
}

// --------------------------------------------------------------- integers --
inline void format_uint(Sink &s, unsigned long long v, bool neg, const FormatSpec &sp) {
    char t = sp.type ? sp.type : 'd';
    if (sp.precision >= 0) { bad_spec(s); return; }
    if (t == 'c') {
        if (sp.sign || sp.alt || sp.zero || neg || v > 255) { bad_spec(s); return; }
        const char ch = static_cast<char>(static_cast<unsigned char>(v));
        write_padded(s, &ch, 1, sp, '<');
        return;
    }
    unsigned base;
    const char *digits = "0123456789abcdef";
    const char *prefix = "";
    switch (t) {
        case 'd': base = 10; break;
        case 'x': base = 16; prefix = "0x"; break;
        case 'X': base = 16; prefix = "0X"; digits = "0123456789ABCDEF"; break;
        case 'o': base = 8; prefix = v ? "0" : ""; break;
        case 'b': base = 2; prefix = "0b"; break;
        case 'B': base = 2; prefix = "0B"; break;
        default: bad_spec(s); return;
    }
    char tmp[72];  // 64 binary digits + sign + prefix
    char *end = tmp + sizeof tmp, *p = end;
    do { *--p = digits[v % base]; v /= base; } while (v);
    std::size_t head = 0;  // sign + prefix length (zero padding goes after it)
    if (sp.alt) {
        const std::size_t pl = std::strlen(prefix);
        p -= pl;
        std::memcpy(p, prefix, pl);
        head += pl;
    }
    if (neg) { *--p = '-'; ++head; }
    else if (sp.sign == '+' || sp.sign == ' ') { *--p = sp.sign; ++head; }
    const std::size_t start = s.size();
    s.write(p, static_cast<std::size_t>(end - p));
    pad(s, start, sp, '>', sp.zero ? static_cast<long>(head) : -1);
}

inline void format_int(Sink &s, long long v, const FormatSpec &sp) {
    const bool neg = v < 0;
    const unsigned long long u = neg ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    format_uint(s, u, neg, sp);
}

// ------------------------------------------------------------------ floats --
// Writes snprintf output; falls back to formatting straight into the sink when
// the result does not fit the stack scratch (huge precision / %f of 1e300).
template <class F>
inline void snprintf_into(Sink &s, const char *cfmt, F v) {
    char tmp[256];
    int n = std::snprintf(tmp, sizeof tmp, cfmt, v);
    if (n < 0) { bad_spec(s); return; }
    if (static_cast<std::size_t>(n) < sizeof tmp) { s.write(tmp, static_cast<std::size_t>(n)); return; }
    const std::size_t room = s.capacity() - s.size();
    if (room == 0) { s.mark_truncated(); return; }
    // snprintf needs n+1 bytes (NUL); if it does not fit it is truncated anyway.
    std::snprintf(s.raw() + s.size(), room, cfmt, v);
    if (static_cast<std::size_t>(n) < room) {
        s.set_size(s.size() + static_cast<std::size_t>(n));
    } else {
        s.set_size(s.capacity());
        s.mark_truncated();
    }
}

// Appends the decimal digits of v (0 <= v <= 9999) at q; returns the new end.
inline char *put_small_uint(char *q, int v) {
    char tmp[8];
    int n = 0;
    do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (n) *q++ = tmp[--n];
    return q;
}

// Fast exact path for "%.{prec}f" (and integral values under "{}") without
// snprintf. Returns false when the result could differ from printf (near a
// rounding tie, too large, non-finite): the caller then uses snprintf.
inline bool fast_fixed(Sink &s, double v, int prec, const FormatSpec &sp) {
    static const double pow10[16] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
                                     1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
    if (prec < 0 || prec > 15 || !(v == v)) return false;
    const bool neg = std::signbit(v);
    const double r = (neg ? -v : v) * pow10[prec];  // exact power of ten
    if (!(r < 1e15)) return false;                   // also rejects inf
    const double fl = std::floor(r);
    const double frac = r - fl;                      // exact (Sterbenz)
    // The product carries <= 0.5 ulp of error: stay clear of the .5 boundary.
    if (std::fabs(frac - 0.5) <= r * 4.5e-16 + 1e-300) return false;
    unsigned long long q = static_cast<unsigned long long>(fl) + (frac > 0.5 ? 1 : 0);
    char tmp[48];
    char *end = tmp + sizeof tmp, *p = end;
    for (int i = 0; i < prec; ++i) { *--p = static_cast<char>('0' + q % 10); q /= 10; }
    if (prec > 0 || sp.alt) *--p = '.';
    do { *--p = static_cast<char>('0' + q % 10); q /= 10; } while (q);
    std::size_t head = 0;
    if (neg) { *--p = '-'; head = 1; }
    else if (sp.sign == '+' || sp.sign == ' ') { *--p = sp.sign; head = 1; }
    const std::size_t start = s.size();
    s.write(p, static_cast<std::size_t>(end - p));
    pad(s, start, sp, '>', sp.zero ? static_cast<long>(head) : -1);
    return true;
}

// kind: 0 = float, 1 = double, 2 = long double
inline void format_float(Sink &s, long double v, int kind, const FormatSpec &sp) {
    char t = sp.type;
    switch (t) {
        case 0: case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': break;
        default: bad_spec(s); return;
    }
    if (kind != 2) {
        const double d = static_cast<double>(v);
        if ((t == 'f' || t == 'F') && fast_fixed(s, d, sp.precision < 0 ? 6 : sp.precision, sp)) return;
        // Integral values print as integers under "{}" (what %.17g would give).
        if (t == 0 && sp.precision < 0 && !sp.alt && std::floor(d) == d && std::fabs(d) < 1e15 &&
            fast_fixed(s, d, 0, sp))
            return;
    }
    char cf[48];
    char *q = cf;
    *q++ = '%';
    if (sp.sign == '+' || sp.sign == ' ') *q++ = sp.sign;
    if (sp.alt) *q++ = '#';
    // Width is only delegated to printf for sign-aware zero padding.
    const bool zero = sp.zero && !sp.align && sp.width > 0;
    if (zero) {
        *q++ = '0';
        q = put_small_uint(q, sp.width > 4096 ? 4096 : sp.width);
    }
    char *const flags_end = q;
    const bool shortest = (t == 0 && sp.precision < 0);
    if (sp.precision >= 0) {
        *q++ = '.';
        q = put_small_uint(q, sp.precision > 4096 ? 4096 : sp.precision);
    }
    if (kind == 2) *q++ = 'L';
    *q++ = t ? t : 'g';
    *q = 0;

    const std::size_t start = s.size();
    if (shortest) {
        // Shortest of %.{6..9|15..17|18..21}g that round-trips.
        static const int lo[3] = {6, 15, 18}, hi[3] = {9, 17, 21};
        char buf[64];
        int n = 0;
        for (int p = lo[kind]; p <= hi[kind]; ++p) {
            char *w = flags_end;
            *w++ = '.';
            w = put_small_uint(w, p);
            if (kind == 2) *w++ = 'L';
            *w++ = 'g';
            *w = 0;
            n = kind == 2 ? std::snprintf(buf, sizeof buf, cf, v) : std::snprintf(buf, sizeof buf, cf, static_cast<double>(v));
            if (n < 0 || static_cast<std::size_t>(n) >= sizeof buf) { n = -1; break; }
            bool same;
            if (kind == 0) same = std::strtof(buf, nullptr) == static_cast<float>(v);
            else if (kind == 1) same = std::strtod(buf, nullptr) == static_cast<double>(v);
            else same = std::strtold(buf, nullptr) == v;
            if (same || v != v) break;  // NaN never compares equal
        }
        if (n < 0) { bad_spec(s); return; }
        s.write(buf, static_cast<std::size_t>(n));
    } else if (kind == 2) {
        snprintf_into(s, cf, v);
    } else {
        snprintf_into(s, cf, static_cast<double>(v));
    }
    if (!zero) pad(s, start, sp, '>');
}

// ----------------------------------------------------------------- strings --
inline void format_str(Sink &s, const char *p, std::size_t n, const FormatSpec &sp) {
    if (sp.sign || sp.alt || sp.zero || (sp.type && sp.type != 's')) { bad_spec(s); return; }
    if (sp.precision >= 0 && static_cast<std::size_t>(sp.precision) < n) n = static_cast<std::size_t>(sp.precision);
    write_padded(s, p, n, sp, '<');
}

inline void format_ptr(Sink &s, const void *p, const FormatSpec &sp) {
    if (sp.sign || sp.alt || sp.zero || sp.precision >= 0 || (sp.type && sp.type != 'p')) {
        bad_spec(s);
        return;
    }
    char tmp[2 + 2 * sizeof(void *)];
    char *end = tmp + sizeof tmp, *q = end;
    unsigned long long v = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p));
    do { *--q = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    *--q = 'x';
    *--q = '0';
    write_padded(s, q, static_cast<std::size_t>(end - q), sp, '>');
}

inline bool is_int_type(char t) {
    return t == 'd' || t == 'x' || t == 'X' || t == 'o' || t == 'b' || t == 'B' || t == 'c';
}

inline void format_arg(Sink &s, const Arg &a, const FormatSpec &sp) {
    switch (a.type) {
        case Arg::Bool:
            if (is_int_type(sp.type) && sp.type != 'c') format_uint(s, a.u, false, sp);
            else if (a.u) format_str(s, "true", 4, sp);
            else format_str(s, "false", 5, sp);
            break;
        case Arg::Char:
            if (is_int_type(sp.type) && sp.type != 'c') {
                format_int(s, a.i, sp);
            } else {
                FormatSpec sp2 = sp;
                if (sp2.type == 'c') sp2.type = 0;
                if (sp2.precision >= 0) { bad_spec(s); break; }
                const char ch = static_cast<char>(a.i);
                format_str(s, &ch, 1, sp2);
            }
            break;
        case Arg::Int: format_int(s, a.i, sp); break;
        case Arg::UInt: format_uint(s, a.u, false, sp); break;
        case Arg::Float: format_float(s, a.d, 0, sp); break;
        case Arg::Double: format_float(s, a.d, 1, sp); break;
        case Arg::LongDouble: format_float(s, a.ld, 2, sp); break;
        case Arg::CStr:
            if (sp.type == 'p') format_ptr(s, a.cs, sp);
            else if (a.cs) format_str(s, a.cs, std::strlen(a.cs), sp);
            else format_str(s, "(null)", 6, sp);
            break;
        case Arg::Str:
            if (sp.type == 'p') format_ptr(s, a.s.p, sp);
            else format_str(s, a.s.p, a.s.n, sp);
            break;
        case Arg::Ptr: format_ptr(s, a.ptr, sp); break;
        case Arg::Custom: a.c.fn(s, a.c.obj, sp); break;
        case Arg::None: missing_arg(s); break;
    }
}

// ------------------------------------------------------------ spec parsing --
inline bool parse_num(const char *&p, int &out) {
    if (*p < '0' || *p > '9') return false;
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        if (v > 1000000) return false;
    }
    out = static_cast<int>(v);
    return true;
}

inline bool is_align(char c) { return c == '<' || c == '>' || c == '^'; }

// Parses the spec starting after ':' up to (not including) the closing '}'.
// Returns false on anything malformed.
inline bool parse_spec(const char *p, const char *end, FormatSpec &sp) {
    if (p < end && p + 1 < end && is_align(p[1])) {
        if (*p == '{' || *p == '}') return false;
        sp.fill = *p;
        sp.align = p[1];
        p += 2;
    } else if (p < end && is_align(*p)) {
        sp.align = *p++;
    }
    if (p < end && (*p == '+' || *p == '-' || *p == ' ')) sp.sign = *p++;
    if (p < end && *p == '#') { sp.alt = true; ++p; }
    if (p < end && *p == '0') { sp.zero = true; ++p; }
    if (p < end && *p >= '1' && *p <= '9') {
        if (!parse_num(p, sp.width)) return false;
    }
    if (p < end && *p == '.') {
        ++p;
        if (!parse_num(p, sp.precision)) return false;
    }
    if (p < end) {
        switch (*p) {
            case 'd': case 'x': case 'X': case 'o': case 'b': case 'B': case 'c':
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 's': case 'p':
                sp.type = *p++;
                break;
            default: return false;
        }
    }
    return p == end;
}

// Finds the '}' closing a field that starts after '{', honouring nesting so
// that "{:{}}" is skipped as a whole. Returns nullptr when unterminated.
inline const char *find_close(const char *p) {
    int depth = 1;
    for (; *p; ++p) {
        if (*p == '{') ++depth;
        else if (*p == '}' && --depth == 0) return p;
    }
    return nullptr;
}

inline void vformat(Sink &s, const char *fmt, const Arg *args, std::size_t nargs) {
    if (!fmt) { s.write("(null)", 6); return; }
    unsigned long long used = 0;  // bit i: argument i referenced (first 64)
    std::size_t next = 0;
    const char *p = fmt;
    while (*p) {
        const char *lit = p;
        while (*p && *p != '{' && *p != '}') ++p;
        if (p != lit) s.write(lit, static_cast<std::size_t>(p - lit));
        if (!*p) break;
        if (*p == '}') {  // "}}" -> "}"; a lone '}' is kept literally
            s.put('}');
            p += (p[1] == '}') ? 2 : 1;
            continue;
        }
        if (p[1] == '{') { s.put('{'); p += 2; continue; }
        const char *close = find_close(p + 1);
        if (!close) { bad_format(s); return; }
        const char *q = p + 1;
        p = close + 1;
        std::size_t idx;
        if (*q >= '0' && *q <= '9') {
            int v = 0;
            if (!parse_num(q, v)) { bad_format(s); continue; }
            idx = static_cast<std::size_t>(v);
        } else {
            idx = next++;
        }
        if (q != close && *q != ':') { bad_format(s); continue; }
        if (idx < nargs && idx < 64) used |= 1ull << idx;
        FormatSpec sp;
        if (*q == ':' && !parse_spec(q + 1, close, sp)) { bad_spec(s); continue; }
        if (idx >= nargs) { missing_arg(s); continue; }
        format_arg(s, args[idx], sp);
    }
    const std::size_t tracked = nargs < 64 ? nargs : 64;
    const unsigned long long all = tracked == 64 ? ~0ull : ((1ull << tracked) - 1);
    if ((used & all) != all) extra_args(s);
}

}  // namespace detail

// ------------------------------------------------------- argument capture --
// Customization point lookup lives in its own namespace whose only
// `tblog_format` is a dummy, so user overloads are found by ADL.
namespace adl {
void tblog_format();

template <int N> struct rank : rank<N - 1> {};
template <> struct rank<0> {};

template <class T> struct dependent_false : std::false_type {};

template <class T> void call_spec(Sink &s, const void *o, const FormatSpec &sp) {
    tblog_format(s, *static_cast<const T *>(o), sp);
}
template <class T> void call_plain(Sink &s, const void *o, const FormatSpec &sp) {
    if (!sp.only_padding()) { detail::bad_spec(s); return; }
    const std::size_t start = s.size();
    tblog_format(s, *static_cast<const T *>(o));
    detail::pad(s, start, sp, '<');
}

// streambuf writing straight into a Sink (no ostringstream buffer).
class SinkBuf : public std::streambuf {
public:
    explicit SinkBuf(Sink &s) : s_(s) {}
protected:
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) s_.put(traits_type::to_char_type(c));
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char *p, std::streamsize n) override {
        if (n > 0) s_.write(p, static_cast<std::size_t>(n));
        return n;
    }
private:
    Sink &s_;
};

template <class T> void call_ostream(Sink &s, const void *o, const FormatSpec &sp) {
    if (!sp.only_padding()) { detail::bad_spec(s); return; }
    const std::size_t start = s.size();
    SinkBuf sb(s);
    std::ostream os(&sb);
    os << *static_cast<const T *>(o);
    detail::pad(s, start, sp, '<');
}

template <class T> detail::Arg custom(const T &v, detail::CustomFn fn) {
    detail::Arg a;
    a.type = detail::Arg::Custom;
    a.c.obj = static_cast<const void *>(&v);
    a.c.fn = fn;
    return a;
}

template <class T>
auto pick(const T &v, rank<4>) -> decltype(tblog_format(std::declval<Sink &>(), v, std::declval<const FormatSpec &>()), detail::Arg()) {
    return custom(v, &call_spec<T>);
}
template <class T>
auto pick(const T &v, rank<3>) -> decltype(tblog_format(std::declval<Sink &>(), v), detail::Arg()) {
    return custom(v, &call_plain<T>);
}
template <class T>
auto pick(const T &v, rank<2>) -> typename std::enable_if<std::is_enum<T>::value, detail::Arg>::type {
    typedef typename std::underlying_type<T>::type U;
    detail::Arg a;
    if (std::is_signed<U>::value) { a.type = detail::Arg::Int; a.i = static_cast<long long>(v); }
    else { a.type = detail::Arg::UInt; a.u = static_cast<unsigned long long>(v); }
    return a;
}
template <class T>
auto pick(const T &v, rank<1>) -> typename std::enable_if<
    std::is_class<T>::value, decltype(std::declval<std::ostream &>() << v, detail::Arg())>::type {
    return custom(v, &call_ostream<T>);
}
template <class T> detail::Arg pick(const T &, rank<0>) {
    static_assert(dependent_false<T>::value,
                  "tblog: type is not formattable; provide tblog_format(tblog::Sink&, const T&) "
                  "in T's namespace or an operator<<(std::ostream&, const T&)");
    return detail::Arg();
}
}  // namespace adl

namespace detail {

inline Arg mk(Arg::Type t) { Arg a; a.type = t; return a; }
inline Arg builtin(bool v) { Arg a = mk(Arg::Bool); a.u = v ? 1 : 0; return a; }
inline Arg builtin(char v) { Arg a = mk(Arg::Char); a.i = v; return a; }
inline Arg builtin(float v) { Arg a = mk(Arg::Float); a.d = v; return a; }
inline Arg builtin(double v) { Arg a = mk(Arg::Double); a.d = v; return a; }
inline Arg builtin(long double v) { Arg a = mk(Arg::LongDouble); a.ld = v; return a; }
inline Arg builtin(const char *v) { Arg a = mk(Arg::CStr); a.cs = v; return a; }
inline Arg builtin(char *v) { return builtin(static_cast<const char *>(v)); }
inline Arg builtin(const std::string &v) { Arg a = mk(Arg::Str); a.s.p = v.data(); a.s.n = v.size(); return a; }
#if __cplusplus >= 201703L
inline Arg builtin(std::string_view v) { Arg a = mk(Arg::Str); a.s.p = v.data(); a.s.n = v.size(); return a; }
#endif
inline Arg builtin(std::nullptr_t) { Arg a = mk(Arg::Ptr); a.ptr = nullptr; return a; }

template <class T> struct is_char_array : std::false_type {};
template <std::size_t N> struct is_char_array<char[N]> : std::true_type {};
template <std::size_t N> struct is_char_array<const char[N]> : std::true_type {};

template <class T>
struct is_builtin
    : std::integral_constant<bool, std::is_arithmetic<T>::value || std::is_same<T, std::string>::value ||
#if __cplusplus >= 201703L
                                       std::is_same<T, std::string_view>::value ||
#endif
                                       std::is_same<T, std::nullptr_t>::value || is_char_array<T>::value ||
                                       (std::is_pointer<T>::value &&
                                        !std::is_function<typename std::remove_pointer<T>::type>::value)> {};

template <class T> Arg builtin_ptr(const T &v, std::false_type) { return builtin(v); }
// char arrays: may be a buffer holding a shorter string -> stop at NUL.
template <std::size_t N> Arg builtin_ptr(const char (&v)[N], std::false_type) {
    Arg a = mk(Arg::Str);
    a.s.p = v;
    const void *z = std::memchr(v, 0, N);
    a.s.n = z ? static_cast<std::size_t>(static_cast<const char *>(z) - v) : N;
    return a;
}
inline Arg builtin_ptr(const char *v, std::true_type) { return builtin(v); }
inline Arg builtin_ptr(char *v, std::true_type) { return builtin(v); }
template <class T> Arg builtin_ptr(T *v, std::true_type) {
    Arg a = mk(Arg::Ptr);
    a.ptr = const_cast<const void *>(static_cast<const volatile void *>(v));
    return a;
}

// Integers other than bool/char (which have exact overloads above).
template <class T>
typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value && !std::is_same<T, char>::value,
                        Arg>::type
builtin_any(const T &v) {
    Arg a;
    if (std::is_signed<T>::value) { a.type = Arg::Int; a.i = static_cast<long long>(v); }
    else { a.type = Arg::UInt; a.u = static_cast<unsigned long long>(v); }
    return a;
}
template <class T>
typename std::enable_if<!std::is_integral<T>::value || std::is_same<T, bool>::value || std::is_same<T, char>::value,
                        Arg>::type
builtin_any(const T &v) { return builtin_ptr(v, std::is_pointer<T>()); }

template <class T> Arg make_arg_impl(const T &v, std::true_type) { return builtin_any(v); }
template <class T> Arg make_arg_impl(const T &v, std::false_type) { return adl::pick(v, adl::rank<4>()); }
template <class T> Arg make_arg(const T &v) { return make_arg_impl(v, is_builtin<T>()); }

template <class... A> inline std::size_t format_args(Sink &s, const char *fmt, const A &...args) {
    const Arg arr[sizeof...(A) + 1] = {make_arg(args)..., Arg()};
    vformat(s, fmt, arr, sizeof...(A));
    return s.size();
}

}  // namespace detail

template <class... A> inline void Sink::format(const char *fmt, const A &...args) {
    detail::format_args(*this, fmt, args...);
}

// Formats into buf[0..cap) (no NUL terminator is written). Returns the length.
// If the output does not fit, it is cut and the last 3 bytes become "...".
template <class... A> inline std::size_t format_to(char *buf, std::size_t cap, const char *fmt, const A &...args) {
    Sink s(buf, cap);
    detail::format_args(s, fmt, args...);
    if (s.truncated() && cap >= 3) std::memcpy(buf + cap - 3, "...", 3);
    return s.size();
}

}  // namespace tblog

#endif  // TBLOG_DETAIL_FORMAT_HPP
