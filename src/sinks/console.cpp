// Console sink: stderr, optional per-line ANSI colouring by level token.
#include "internal.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace tblog {

// Test hook (not in internal.h): write `data` to fd exactly as the console
// sink would, colourised when `color` is true (no isatty check here).
void console_write_fd(int fd, const char *data, size_t len, bool color);

namespace {

void write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;  // nothing sensible to do for stderr errors
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
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
    explicit OutBuf(int fd) : fd_(fd) {}
    ~OutBuf() { flush(); }
    void put(const char *p, size_t n) {
        if (n > sizeof(buf_) - len_) {
            flush();
            if (n > sizeof(buf_)) {  // huge piece: bypass buffer
                write_all(fd_, p, n);
                return;
            }
        }
        std::memcpy(buf_ + len_, p, n);
        len_ += n;
    }
    void flush() {
        if (len_) write_all(fd_, buf_, len_);
        len_ = 0;
    }

private:
    int fd_;
    size_t len_ = 0;
    char buf_[8192];
};

constexpr char kReset[] = "\033[0m";

class ConsoleSink final : public Sink {
public:
    explicit ConsoleSink(bool color) : color_(color && ::isatty(STDERR_FILENO) == 1) {}
    void write(const char *data, size_t len) override {
        console_write_fd(STDERR_FILENO, data, len, color_);
    }

private:
    bool color_;
};

}  // namespace

void console_write_fd(int fd, const char *data, size_t len, bool color) {
    if (!data || len == 0) return;
    if (!color) {
        write_all(fd, data, len);
        return;
    }
    OutBuf out(fd);
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
