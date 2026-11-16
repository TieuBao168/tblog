// tblog configuration parser. See src/internal.h ("Config") for the contract.
//
// Format: one `key = value` per line. Leading/trailing whitespace is trimmed,
// blank lines and lines whose first non-blank char is '#' are ignored. An
// inline comment starts at a '#' that is preceded by whitespace (or is the
// first character of the value) -- except for `pattern`, whose value is the
// whole rest of the line. A value may be wrapped in double quotes to keep
// leading/trailing spaces (and, for non-pattern keys, to contain " #").
// Invalid values produce a warning and leave the field unchanged.
#include "internal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

namespace tblog {
namespace {

constexpr uint64_t kPage = 4096;
constexpr uint32_t kFlushMin = 1, kFlushMax = 60000;
constexpr uint32_t kSyncMax = 3600000;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string to_lower(const std::string &s) {
    std::string r(s);
    for (char &c : r) c = lower(c);
    return r;
}

bool parse_level_local(const std::string &v, tblog_level *out) {
    static const struct { const char *name; tblog_level lv; } kTab[] = {
        {"trace", TBLOG_LV_TRACE}, {"debug", TBLOG_LV_DEBUG}, {"info", TBLOG_LV_INFO},
        {"warn", TBLOG_LV_WARN},   {"warning", TBLOG_LV_WARN}, {"error", TBLOG_LV_ERROR},
        {"fatal", TBLOG_LV_FATAL}, {"off", TBLOG_LV_OFF},
    };
    std::string l = to_lower(v);
    for (const auto &e : kTab)
        if (l == e.name) { *out = e.lv; return true; }
    return false;
}

bool parse_bool(const std::string &v, bool *out) {
    std::string l = to_lower(v);
    if (l == "true" || l == "1" || l == "yes" || l == "on") { *out = true; return true; }
    if (l == "false" || l == "0" || l == "no" || l == "off") { *out = false; return true; }
    return false;
}

// Unsigned decimal integer, digits only. *overflow set if > UINT64_MAX.
// Returns number of digits consumed (0 = none).
size_t parse_digits(const std::string &s, size_t pos, uint64_t *out, bool *overflow) {
    uint64_t v = 0;
    size_t i = pos;
    *overflow = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        unsigned d = unsigned(s[i] - '0');
        if (v > (std::numeric_limits<uint64_t>::max() - d) / 10) *overflow = true;
        else v = v * 10 + d;
        ++i;
    }
    *out = v;
    return i - pos;
}

enum class NumErr { Ok, Garbage, Negative, Overflow };

// Size with optional B/K/KB/M/MB/G/GB suffix (case-insensitive, 1024-based).
NumErr parse_size(const std::string &v, uint64_t *out) {
    if (!v.empty() && v[0] == '-') return NumErr::Negative;
    size_t i = (!v.empty() && v[0] == '+') ? 1 : 0;
    uint64_t n;
    bool ovf;
    size_t nd = parse_digits(v, i, &n, &ovf);
    if (nd == 0) return NumErr::Garbage;
    i += nd;
    while (i < v.size() && is_space(v[i])) ++i;
    std::string suf = to_lower(v.substr(i));
    unsigned shift;
    if (suf.empty() || suf == "b") shift = 0;
    else if (suf == "k" || suf == "kb") shift = 10;
    else if (suf == "m" || suf == "mb") shift = 20;
    else if (suf == "g" || suf == "gb") shift = 30;
    else return NumErr::Garbage;
    if (ovf) return NumErr::Overflow;
    if (shift && n > (std::numeric_limits<uint64_t>::max() >> shift)) return NumErr::Overflow;
    n <<= shift;
    if (n > std::numeric_limits<size_t>::max()) return NumErr::Overflow;
    *out = n;
    return NumErr::Ok;
}

// Plain non-negative integer. Overflow saturates (flagged).
NumErr parse_uint(const std::string &v, uint64_t *out) {
    if (!v.empty() && v[0] == '-') {
        uint64_t tmp; bool o;
        return parse_digits(v, 1, &tmp, &o) == v.size() - 1 && v.size() > 1 ? NumErr::Negative
                                                                            : NumErr::Garbage;
    }
    size_t i = (!v.empty() && v[0] == '+') ? 1 : 0;
    bool ovf;
    size_t nd = parse_digits(v, i, out, &ovf);
    if (nd == 0 || i + nd != v.size()) return NumErr::Garbage;
    if (ovf) { *out = std::numeric_limits<uint64_t>::max(); return NumErr::Overflow; }
    return NumErr::Ok;
}

const char *num_err_text(NumErr e) {
    switch (e) {
        case NumErr::Negative: return "negative value";
        case NumErr::Overflow: return "value too large";
        default: return "invalid number";
    }
}

class Parser {
public:
    Parser(Config &c, std::vector<std::string> *w) : cfg_(c), warn_(w) {}

    void line(int no, const std::string &raw) {
        lineno_ = no;
        std::string t = trim(raw);
        if (t.empty() || t[0] == '#') return;
        size_t eq = t.find('=');
        if (eq == std::string::npos) { warn("expected 'key = value'", t); return; }
        std::string key = trim(t.substr(0, eq));
        std::string rest = t.substr(eq + 1);
        if (key.empty()) { warn("missing key", t); return; }
        std::string value;
        if (!extract_value(key, rest, &value)) return;
        apply(key, value, t);
    }

    void finish() {
        if (cfg_.backend == BackendKind::Mmap && cfg_.file.max_size < cfg_.mmap_region_size) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "file.max_size (%llu) is smaller than mmap.region_size (%llu) with "
                          "backend=mmap; raised to %llu",
                          (unsigned long long)cfg_.file.max_size,
                          (unsigned long long)cfg_.mmap_region_size,
                          (unsigned long long)cfg_.mmap_region_size);
            lineno_ = max_size_line_;
            warn(buf, max_size_text_);
            cfg_.file.max_size = cfg_.mmap_region_size;
        }
    }

    int warnings() const { return nwarn_; }

private:
    void warn(const std::string &msg, const std::string &text) {
        ++nwarn_;
        if (!warn_) return;
        std::string s = "config";
        if (lineno_ > 0) s += " line " + std::to_string(lineno_);
        s += ": " + msg;
        if (!text.empty()) s += ": '" + text + "'";
        warn_->push_back(std::move(s));
    }

    // Splits off the value: quotes, inline comment (not for pattern).
    bool extract_value(const std::string &key, const std::string &rest, std::string *out) {
        std::string v = trim(rest);
        if (key == "pattern") {
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            *out = v;
            return true;
        }
        if (!v.empty() && v[0] == '"') {
            size_t close = v.find('"', 1);
            if (close == std::string::npos) { warn("unterminated quoted value", key + " =" + rest); return false; }
            std::string tail = trim(v.substr(close + 1));
            if (!tail.empty() && tail[0] != '#') {
                warn("unexpected text after quoted value", key + " =" + rest);
                return false;
            }
            *out = v.substr(1, close - 1);
            return true;
        }
        if (!v.empty() && v[0] == '#') { *out = ""; return true; }
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i] == '#' && is_space(v[i - 1])) { v = trim(v.substr(0, i)); break; }
        *out = v;
        return true;
    }

    bool get_size(const std::string &v, const std::string &text, uint64_t *out) {
        uint64_t n;
        NumErr e = parse_size(v, &n);
        if (e != NumErr::Ok) { warn(num_err_text(e), text); return false; }
        if (n == 0) { warn("size must be > 0", text); return false; }
        *out = n;
        return true;
    }

    // Interval in ms clamped to [lo, hi]; returns false (keep previous) on garbage.
    bool get_interval(const std::string &v, const std::string &text, uint32_t lo, uint32_t hi,
                      uint32_t *out) {
        uint64_t n;
        NumErr e = parse_uint(v, &n);
        if (e == NumErr::Garbage || e == NumErr::Negative) { warn(num_err_text(e), text); return false; }
        uint64_t c = n < lo ? lo : (n > hi ? hi : n);
        if (c != n || e == NumErr::Overflow)
            warn("value out of range [" + std::to_string(lo) + ", " + std::to_string(hi) +
                     "], clamped to " + std::to_string(c),
                 text);
        *out = uint32_t(c);
        return true;
    }

    void apply(const std::string &key, const std::string &v, const std::string &text) {
        if (key == "level") {
            tblog_level lv;
            if (parse_level_local(v, &lv)) cfg_.level = lv;
            else warn("invalid level", text);
        } else if (key == "pattern") {
            if (v.empty()) warn("empty pattern", text);
            else cfg_.pattern = v;
        } else if (key == "backend") {
            std::string l = to_lower(v);
            if (l == "sync") cfg_.backend = BackendKind::Sync;
            else if (l == "buffer") cfg_.backend = BackendKind::Buffer;
            else if (l == "mmap") cfg_.backend = BackendKind::Mmap;
            else warn("invalid backend (sync|buffer|mmap)", text);
        } else if (key == "file.path") {
            if (v.empty()) warn("empty file.path", text);
            else cfg_.file.path = v;
        } else if (key == "file.max_size") {
            uint64_t n;
            if (get_size(v, text, &n)) {
                cfg_.file.max_size = n;
                max_size_line_ = lineno_;
                max_size_text_ = text;
            }
        } else if (key == "file.max_files") {
            uint64_t n;
            NumErr e = parse_uint(v, &n);
            if (e == NumErr::Garbage || e == NumErr::Overflow) {
                warn(num_err_text(e), text);
            } else if (e == NumErr::Negative || n < 1) {
                warn("file.max_files must be >= 1, set to 1", text);
                cfg_.file.max_files = 1;
            } else if (n > uint64_t(std::numeric_limits<int>::max())) {
                warn(num_err_text(NumErr::Overflow), text);
            } else {
                cfg_.file.max_files = int(n);
            }
        } else if (key == "buffer.size") {
            uint64_t n;
            if (get_size(v, text, &n)) cfg_.buffer_size = size_t(n);
        } else if (key == "mmap.region_size") {
            uint64_t n;
            if (get_size(v, text, &n)) {
                if (n % kPage) {
                    if (n > std::numeric_limits<size_t>::max() - kPage) {
                        warn(num_err_text(NumErr::Overflow), text);
                        return;
                    }
                    uint64_t r = (n + kPage - 1) / kPage * kPage;
                    warn("mmap.region_size must be a multiple of 4096, rounded up to " +
                             std::to_string(r),
                         text);
                    n = r;
                }
                cfg_.mmap_region_size = size_t(n);
            }
        } else if (key == "full_policy") {
            std::string l = to_lower(v);
            if (l == "drop") cfg_.full_policy = FullPolicy::Drop;
            else if (l == "block") cfg_.full_policy = FullPolicy::Block;
            else warn("invalid full_policy (drop|block)", text);
        } else if (key == "format.mode") {
            std::string l = to_lower(v);
            if (l == "eager") cfg_.format_mode = FormatMode::Eager;
            else if (l == "deferred") cfg_.format_mode = FormatMode::Deferred;
            else warn("invalid format.mode (eager|deferred)", text);
        } else if (key == "flush.interval_ms") {
            get_interval(v, text, kFlushMin, kFlushMax, &cfg_.flush_interval_ms);
        } else if (key == "sync.interval_ms") {
            get_interval(v, text, 0, kSyncMax, &cfg_.sync_interval_ms);
        } else if (key == "sync.on_level") {
            tblog_level lv;
            if (parse_level_local(v, &lv)) cfg_.file.urgent_level = lv;
            else warn("invalid level", text);
        } else if (key == "console") {
            set_bool(v, text, &cfg_.console);
        } else if (key == "console.color") {
            set_bool(v, text, &cfg_.console_color);
        } else if (key == "syslog") {
            set_bool(v, text, &cfg_.syslog);
        } else if (key == "reload.watch") {
            set_bool(v, text, &cfg_.reload_watch);
        } else if (key.compare(0, 7, "module.") == 0) {
            std::string name = key.substr(7);
            tblog_level lv;
            if (name.empty()) { warn("empty module name", text); return; }
            if (!parse_level_local(v, &lv)) { warn("invalid level", text); return; }
            for (auto &p : cfg_.module_levels)
                if (p.first == name) { p.second = lv; return; }
            cfg_.module_levels.emplace_back(name, lv);
        } else {
            warn("unknown key '" + key + "'", text);
        }
    }

    void set_bool(const std::string &v, const std::string &text, bool *out) {
        if (!parse_bool(v, out)) warn("invalid boolean", text);
    }

    Config &cfg_;
    std::vector<std::string> *warn_;
    int lineno_ = 0;
    int nwarn_ = 0;
    int max_size_line_ = 0;
    std::string max_size_text_;
};

}  // namespace

// Returns true iff the text produced no warnings. Valid settings are applied
// either way.
bool parse_config_string(const std::string &text, Config &out,
                         std::vector<std::string> *warnings) {
    Parser p(out, warnings);
    int no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        ++no;
        p.line(no, text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    p.finish();
    return p.warnings() == 0;
}

// Returns false only if the file cannot be read; warnings do not fail it.
bool load_config(const char *path, Config &out, std::vector<std::string> *warnings) {
    if (!path) return false;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) return false;
    parse_config_string(text, out, warnings);
    return true;
}

}  // namespace tblog
