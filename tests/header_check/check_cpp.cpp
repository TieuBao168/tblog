// Compile-only check of the public C++ header: every TBLOGF_* macro, both
// tblog::Logger overload sets, TBLOGF_LOGGER and both customization points
// are instantiated once, so the strict warning flags of each compiler
// (GCC/Clang -Wall -Wextra -Wpedantic, MSVC /W4 /permissive-) see all of it.
// Nothing here is ever linked or run. Built as C++11..20, also with TBLOG_DLL.
#include <tblog/tblog.hpp>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#if TBLOG_CPLUSPLUS_ >= 201703L
#include <string_view>
#endif

namespace hc {

// Customization point 1: allocation-free tblog_format, found by ADL.
struct Endpoint {
    std::string host;
    int port;
};
inline void tblog_format(tblog::Sink &s, const Endpoint &e) { s.format("{}:{}", e.host, e.port); }

// Customization point 1b: tblog_format that also takes the replacement spec.
struct Hex {
    unsigned v;
};
inline void tblog_format(tblog::Sink &s, const Hex &h, const tblog::FormatSpec &sp) {
    if (sp.type == 'X') s.format("{:#X}", h.v);
    else s.format("{:#x}", h.v);
}

// Customization point 2: only operator<<.
struct Streamed {
    int id;
};
inline std::ostream &operator<<(std::ostream &os, const Streamed &v) { return os << "streamed#" << v.id; }

enum class Color : unsigned char { red = 1, green = 2 };
enum Signed { neg = -1, pos = 1 };

inline std::string expensive() { return std::string(16, '*'); }

}  // namespace hc

int tblog_header_check_cpp(const char *conf, std::size_t n);

int tblog_header_check_cpp(const char *conf, std::size_t n) {
    if (tblog_init(conf) != 0) return 1;

    const hc::Endpoint ep{"10.0.0.7", 8443};
    const hc::Hex hx{255};
    const hc::Streamed st{7};
    const std::string str("std::string");
    char buf[16] = "char array";
    const std::int64_t i64 = -5;
    const std::uint64_t u64 = 5;

    TBLOGF_TRACE("trace {}", 1);
    TBLOGF_DEBUG("debug {} {}", ep, st);
    TBLOGF_INFO("info {} {:X} {}", n, hx, str);
    TBLOGF_WARN("warn {} {} {}", i64, u64, buf);
    TBLOGF_ERROR("error {:.3f} {:>8} {}", 1.5, 'c', true);
    TBLOGF_FATAL("fatal, no arguments");

    TBLOGF_MTRACE("net", "trace {}", hc::Color::red);
    TBLOGF_MDEBUG("net", "debug {}", hc::neg);
    TBLOGF_MINFO("net", "info {}", static_cast<const void *>(&ep));
    TBLOGF_MWARN("net", "warn {} {}", nullptr, 2.5f);
    TBLOGF_MERROR("net", "error {}", 3.0L);
    TBLOGF_MFATAL("net", "fatal, no arguments");
#if TBLOG_CPLUSPLUS_ >= 201703L
    TBLOGF_INFO("string_view {}", std::string_view("sv"));
#endif

    // Usable as the body of an unbraced if/else.
    if (n)
        TBLOGF_INFO("then {}", 1);
    else
        TBLOGF_MINFO("net", "else {}", 2);

    // Object style: call-site capturing overloads (FmtLoc) ...
    tblog::Logger root;
    tblog::Logger net("net");
    root.trace("trace {}", 1);
    net.debug("debug {}", ep);
    net.info("info {}", st);
    net.warn("warn, no arguments");
    net.error("error {}", hx);
    net.fatal("fatal {}", str);
    net.log(TBLOG_LV_INFO, "log {}", 1);
    // ... and explicit-location overloads (SourceLoc).
    const tblog::SourceLoc here = TBLOG_HERE;
    net.trace(TBLOG_HERE, "trace {}", 1);
    net.debug(here, "debug {}", 2);
    net.info(TBLOG_HERE, "info, no arguments");
    net.warn(here, "warn {}", ep);
    net.error(TBLOG_HERE, "error {}", st);
    net.fatal(here, "fatal {}", hx);
    net.log(TBLOG_LV_WARN, here, "log {}", 3);

    if (net.enabled(TBLOG_LV_DEBUG)) net.debug("enabled {} {}", net.name(), static_cast<const void *>(net.module()));
    TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "not evaluated when filtered: {}", hc::expensive());

    // The formatting engine on its own.
    char out[64];
    const std::size_t len = tblog::format_to(out, sizeof out, "{:>6}|{:<4}|{}", ep.port, "ab", st);

    tblog_flush();
    const std::uint64_t dropped = tblog_dropped_count();
    TBLOG_INFO("dropped %llu, formatted %zu bytes", static_cast<unsigned long long>(dropped), len);
    tblog_shutdown();
    return 0;
}
