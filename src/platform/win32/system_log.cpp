// Windows: no system log is supported (the Event Log needs a registered
// message source, which a library cannot assume). The syslog sink is then
// disabled with a warning (see sinks/syslog.cpp).
#include "platform/platform.h"

namespace tblog {
namespace pf {

bool syslog_available() noexcept { return false; }
void syslog_open(const char *) noexcept {}
void syslog_close() noexcept {}
void syslog_write(SyslogSeverity, const char *, size_t) noexcept {}

}  // namespace pf
}  // namespace tblog
