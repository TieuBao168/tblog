// Syslog sink: one syslog() call per line, priority from the "[LEVEL]" token.
#include "internal.h"

#include <cstring>
#include <mutex>
#include <syslog.h>

namespace tblog {
namespace {

// openlog() keeps the ident pointer, so it must live in static storage.
// The log connection is process-global: opened by the first live sink,
// closed by the last one. A later sink with a different ident shares the
// already-open connection (and its ident).
std::mutex g_syslog_mu;
int g_syslog_refs = 0;
char g_ident[64];

int priority_for_line(const char *p, size_t n) {
    struct Tok { const char *tok; size_t len; int prio; };
    static const Tok kToks[] = {
        {"[TRACE]", 7, LOG_DEBUG},  {"[DEBUG]", 7, LOG_DEBUG},
        {"[INFO]", 6, LOG_INFO},    {"[WARN]", 6, LOG_WARNING},
        {"[ERROR]", 7, LOG_ERR},    {"[FATAL]", 7, LOG_CRIT},
    };
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != '[') continue;
        for (const Tok &t : kToks)
            if (n - i >= t.len && std::memcmp(p + i, t.tok, t.len) == 0) return t.prio;
    }
    return LOG_INFO;
}

class SyslogSink final : public Sink {
public:
    explicit SyslogSink(const char *ident) {
        std::lock_guard<std::mutex> lk(g_syslog_mu);
        if (g_syslog_refs++ == 0) {
            const char *id = nullptr;
            if (ident && *ident) {
                std::strncpy(g_ident, ident, sizeof(g_ident) - 1);
                g_ident[sizeof(g_ident) - 1] = '\0';
                id = g_ident;
            }
            ::openlog(id, LOG_PID | LOG_NDELAY, LOG_USER);
        }
    }
    ~SyslogSink() override {
        std::lock_guard<std::mutex> lk(g_syslog_mu);
        if (--g_syslog_refs == 0) ::closelog();
    }

    void write(const char *data, size_t len) override {
        if (!data) return;
        const char *p = data;
        const char *end = data + len;
        while (p < end) {
            const char *nl = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
            size_t body = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);
            if (body > 0) {
                int n = body > 0x7fffffff ? 0x7fffffff : static_cast<int>(body);
                ::syslog(priority_for_line(p, body), "%.*s", n, p);
            }
            p += body + (nl ? 1 : 0);
        }
    }
};

}  // namespace

std::unique_ptr<Sink> make_syslog_sink(const char *ident) {
    return std::unique_ptr<Sink>(new SyslogSink(ident));
}

}  // namespace tblog
