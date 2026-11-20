// tblog crash handler — see crash/crash_handler.h. This file decides WHAT is
// reported; catching the signal, alternate stacks, the backtrace and the
// re-raise are the platform's (pf::crash_install, src/platform/<os>/crash.cpp).
//
// report_crash() runs inside the fatal-signal handler: everything reachable
// from it must be async-signal-safe (atomics, pf::stderr_write, the
// backend's on_crash/crash_write). No malloc, no locks, no stdio.
#include "crash/crash_handler.h"

#include "backend/backend.h"
#include "core/compat.h"
#include "platform/platform.h"

#include <atomic>
#include <cstddef>
#include <mutex>

namespace tblog {

namespace {

TBLOG_ASSERT_LOCK_FREE(Backend *);

std::atomic<Backend *> g_backend{nullptr};
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

void report_crash(int sig, const char *name) {
    char msg[96];
    size_t n = append_str(msg, 0, sizeof(msg), "*** tblog: caught signal ");
    n = append_int(msg, n, sizeof(msg), sig);
    n = append_str(msg, n, sizeof(msg), " (");
    n = append_str(msg, n, sizeof(msg), name);
    n = append_str(msg, n, sizeof(msg), ") ***\n");

    pf::stderr_write(msg, n);
    if (Backend *b = g_backend.load(std::memory_order_acquire)) {
        // Drain buffered lines first so the marker lands after them in the file.
        b->on_crash();
        b->crash_write(msg, n);
    }
}

}  // namespace

bool crash_altstack_for_this_thread() { return pf::crash_thread_init(); }

void install_crash_handler(Backend *backend) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_backend.store(backend, std::memory_order_release);
    if (g_installed.load(std::memory_order_relaxed)) return;  // just swap backend
    pf::crash_install(&report_crash);
    g_installed.store(true, std::memory_order_release);
}

void uninstall_crash_handler() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_installed.load(std::memory_order_relaxed)) {
        g_backend.store(nullptr, std::memory_order_release);
        return;
    }
    pf::crash_uninstall();
    g_backend.store(nullptr, std::memory_order_release);
    g_installed.store(false, std::memory_order_release);
}

}  // namespace tblog
