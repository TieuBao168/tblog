// Syslog sink: one system-log message per line, severity from the "[LEVEL]"
// token. The connection itself is the platform's (pf::syslog_*).
#include "sinks/sink.h"

#include <cstdio>
#include <cstring>

namespace tblog {
namespace {

pf::SyslogSeverity severity_for_line(const char *p, size_t n) {
    struct Tok { const char *tok; size_t len; pf::SyslogSeverity sev; };
    static const Tok kToks[] = {
        {"[TRACE]", 7, pf::SyslogSeverity::Debug},  {"[DEBUG]", 7, pf::SyslogSeverity::Debug},
        {"[INFO]", 6, pf::SyslogSeverity::Info},    {"[WARN]", 6, pf::SyslogSeverity::Warning},
        {"[ERROR]", 7, pf::SyslogSeverity::Error},  {"[FATAL]", 7, pf::SyslogSeverity::Critical},
    };
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != '[') continue;
        for (const Tok &t : kToks)
            if (n - i >= t.len && std::memcmp(p + i, t.tok, t.len) == 0) return t.sev;
    }
    return pf::SyslogSeverity::Info;
}

class SyslogSink final : public Sink {
public:
    explicit SyslogSink(const char *ident) { pf::syslog_open(ident); }
    ~SyslogSink() override { pf::syslog_close(); }

    void write(const char *data, size_t len) override {
        if (!data) return;
        const char *p = data;
        const char *end = data + len;
        while (p < end) {
            const char *nl = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
            size_t body = nl ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);
            if (body > 0) pf::syslog_write(severity_for_line(p, body), p, body);
            p += body + (nl ? 1 : 0);
        }
    }
};

}  // namespace

std::unique_ptr<Sink> make_syslog_sink(const char *ident) {
    if (!pf::syslog_available()) {
        std::fprintf(stderr, "tblog: syslog is not supported on this platform, sink disabled\n");
        return nullptr;
    }
    return std::unique_ptr<Sink>(new SyslogSink(ident));
}

}  // namespace tblog
