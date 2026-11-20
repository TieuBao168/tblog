// POSIX system log: openlog/syslog/closelog (see platform/platform.h).
#include "platform/platform.h"

#include <cstring>
#include <mutex>

#include <syslog.h>

namespace tblog {
namespace pf {

namespace {

// openlog() keeps the ident pointer, so it must live in static storage.
// The log connection is process-global: opened by the first open(), closed
// by the last close(). A later open() with a different ident shares the
// already-open connection (and its ident).
std::mutex g_syslog_mu;
int g_syslog_refs = 0;
char g_ident[64];

int priority(SyslogSeverity s) noexcept {
    switch (s) {
        case SyslogSeverity::Debug: return LOG_DEBUG;
        case SyslogSeverity::Info: return LOG_INFO;
        case SyslogSeverity::Warning: return LOG_WARNING;
        case SyslogSeverity::Error: return LOG_ERR;
        case SyslogSeverity::Critical: return LOG_CRIT;
    }
    return LOG_INFO;
}

}  // namespace

bool syslog_available() noexcept { return true; }

void syslog_open(const char *ident) noexcept {
    try {
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
    } catch (...) {
    }
}

void syslog_close() noexcept {
    try {
        std::lock_guard<std::mutex> lk(g_syslog_mu);
        if (--g_syslog_refs == 0) ::closelog();
    } catch (...) {
    }
}

void syslog_write(SyslogSeverity sev, const char *msg, size_t len) noexcept {
    int n = len > 0x7fffffff ? 0x7fffffff : static_cast<int>(len);
    ::syslog(priority(sev), "%.*s", n, msg);
}

}  // namespace pf
}  // namespace tblog
