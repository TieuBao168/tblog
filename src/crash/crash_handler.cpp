// tblog crash handler — see crash/crash_handler.h. This file decides WHAT is
// reported; catching the signal, alternate stacks, the backtrace and the
// re-raise are the platform's (pf::crash_install, src/platform/<os>/crash.cpp).
//
// report_crash() runs inside the fatal-signal handler: everything reachable
// from it must be async-signal-safe (atomics, pf::stderr_write,
// Pipeline::crash -> Queue::crash_drain + LogSink::crash_write). No malloc, no
// locks, no stdio.
#include "crash/crash_handler.h"

#include "core/compat.h"
#include "logger/pipeline.h"
#include "platform/platform.h"

#include <atomic>
#include <cstddef>
#include <mutex>

namespace tblog {

namespace {

TBLOG_ASSERT_LOCK_FREE(Pipeline *);

std::atomic<Pipeline *> g_pipeline{nullptr};
std::atomic<bool> g_installed{false};
std::mutex g_mu;  // install/uninstall only (never in the handler)

// ---------------------------------------------------------------- AS-safe utils
size_t append_str(char *buf, size_t pos, size_t cap, const char *s) noexcept {
    while (*s && pos < cap) buf[pos++] = *s++;
    return pos;
}

size_t append_int(char *buf, size_t pos, size_t cap, int v) noexcept {
    char tmp[16];
    int n = 0;
    unsigned u = v < 0 ? 0u - static_cast<unsigned>(v) : static_cast<unsigned>(v);
    do {
        tmp[n++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u && n < 15);
    if (v < 0 && pos < cap) buf[pos++] = '-';
    while (n > 0 && pos < cap) buf[pos++] = tmp[--n];
    return pos;
}

size_t append_hex32(char *buf, size_t pos, size_t cap, unsigned v) noexcept {
    static const char kDigits[] = "0123456789ABCDEF";
    pos = append_str(buf, pos, cap, "0x");
    for (int shift = 28; shift >= 0 && pos < cap; shift -= 4) buf[pos++] = kDigits[(v >> shift) & 0xFu];
    return pos;
}

// "*** tblog: caught signal 11 (SIGSEGV) ***" or, for a Windows exception,
// "*** tblog: caught exception 0xC0000005 (EXCEPTION_ACCESS_VIOLATION) ***".
void report_crash(pf::CrashKind kind, int code, const char *name) {
    char msg[128];
    size_t n;
    if (kind == pf::CrashKind::Exception) {
        n = append_str(msg, 0, sizeof(msg), "*** tblog: caught exception ");
        n = append_hex32(msg, n, sizeof(msg), static_cast<unsigned>(code));
    } else {
        n = append_str(msg, 0, sizeof(msg), "*** tblog: caught signal ");
        n = append_int(msg, n, sizeof(msg), code);
    }
    n = append_str(msg, n, sizeof(msg), " (");
    n = append_str(msg, n, sizeof(msg), name);
    n = append_str(msg, n, sizeof(msg), ") ***\n");

    Pipeline *p = g_pipeline.load(std::memory_order_acquire);
    // With a crash-safe console sink the marker reaches stderr through it,
    // after the drained lines; otherwise print it first.
    if (!p || !p->crash_covers_stderr()) pf::stderr_write(msg, n);
    // Drain buffered lines first so the marker lands after them in each sink.
    if (p) p->crash(msg, n);
}

}  // namespace

bool crash_altstack_for_this_thread() { return pf::crash_thread_init(); }

void install_crash_handler(Pipeline *pipeline) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pipeline.store(pipeline, std::memory_order_release);
    if (g_installed.load(std::memory_order_relaxed)) return;  // just swap pipeline
    pf::crash_install(&report_crash);
    g_installed.store(true, std::memory_order_release);
}

void uninstall_crash_handler() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_installed.load(std::memory_order_relaxed)) {
        g_pipeline.store(nullptr, std::memory_order_release);
        return;
    }
    pf::crash_uninstall();
    g_pipeline.store(nullptr, std::memory_order_release);
    g_installed.store(false, std::memory_order_release);
}

}  // namespace tblog
