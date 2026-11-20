// Console sink: stderr, optional per-line ANSI colouring by level token.
#include "sinks/sink.h"

#include <cstring>

namespace tblog {

namespace {

void write_all(pf::FileHandle h, const char *p, size_t n) {
    pf::file_write_all(h, p, n);  // nothing sensible to do for stderr errors
}

// Returns the ANSI prefix for the first "[LEVEL]" token in the line, or
// nullptr (no recognised level, or INFO which is printed unchanged).
const char *color_for_line(const char *p, size_t n, bool *found) {
    struct Tok { const char *tok; size_t len; const char *color; };
    static const Tok kToks[] = {
        {"[TRACE]", 7, "\033[2m"},  {"[DEBUG]", 7, "\033[2m"},
        {"[INFO]", 6, nullptr},     {"[WARN]", 6, "\033[33m"},
        {"[ERROR]", 7, "\033[31m"}, {"[FATAL]", 7, "\033[1;31m"},
    };
    *found = false;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != '[') continue;
        for (const Tok &t : kToks) {
            if (n - i >= t.len && std::memcmp(p + i, t.tok, t.len) == 0) {
                *found = true;
                return t.color;
            }
        }
    }
    return nullptr;
}

class OutBuf {
public:
    explicit OutBuf(pf::FileHandle h) : h_(h) {}
    ~OutBuf() { flush(); }
    void put(const char *p, size_t n) {
        if (n > sizeof(buf_) - len_) {
            flush();
            if (n > sizeof(buf_)) {  // huge piece: bypass buffer
                write_all(h_, p, n);
                return;
            }
        }
        std::memcpy(buf_ + len_, p, n);
        len_ += n;
    }
    void flush() {
        if (len_) write_all(h_, buf_, len_);
        len_ = 0;
    }

private:
    pf::FileHandle h_;
    size_t len_ = 0;
    char buf_[8192];
};

constexpr char kReset[] = "\033[0m";

class ConsoleSink final : public Sink {
public:
    explicit ConsoleSink(bool color)
        : color_(color && pf::stderr_is_terminal() && pf::enable_ansi_colors()) {}
    void write(const char *data, size_t len) override {
        console_write_to(pf::stderr_handle(), data, len, color_);
    }

private:
    bool color_;
};

}  // namespace

void console_write_to(pf::FileHandle h, const char *data, size_t len, bool color) {
    if (!data || len == 0) return;
    if (!color) {
        write_all(h, data, len);
        return;
    }
    OutBuf out(h);
    const char *p = data;
    const char *end = data + len;
    while (p < end) {
        const char *nl = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
        size_t body = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);
        bool found = false;
        const char *c = color_for_line(p, body, &found);
        if (c) {
            out.put(c, std::strlen(c));
            out.put(p, body);
            out.put(kReset, sizeof(kReset) - 1);
        } else {
            out.put(p, body);
        }
        if (nl) out.put("\n", 1);
        p += body + (nl ? 1 : 0);
    }
}

std::unique_ptr<Sink> make_console_sink(bool color) {
    return std::unique_ptr<Sink>(new ConsoleSink(color));
}

}  // namespace tblog
