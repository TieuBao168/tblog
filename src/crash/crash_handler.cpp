// tblog crash handler — see "Crash handling" in src/internal.h.
//
// Everything reachable from crash_signal_handler() is async-signal-safe:
// only atomics, write(), sigaction(), raise(), nanosleep(), syscall(gettid)
// and backtrace()/backtrace_symbols_fd() (not formally AS-safe, but safe in
// practice once libgcc has been loaded, which install_crash_handler forces).
// No malloc, no locks, no stdio.
//
// Alternate stack: sigaltstack() is PER THREAD. install_crash_handler() sets
// one up for the calling thread only (from static storage). A stack overflow
// in any other thread can only be reported if that thread also has an
// alternate stack; such threads should call
//     tblog::crash_altstack_for_this_thread();
// (declared below, not in internal.h) early in their life. Faults other than
// stack overflow are handled on the faulting thread's normal stack anyway.
#include "internal.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
// backtrace() lives in glibc (and uClibc-ng built with UCLIBC_HAS_BACKTRACE).
// musl has no <execinfo.h>: crash logging still works, only the stack dump to
// stderr is skipped. -DTBLOG_NO_BACKTRACE forces that path.
#if !defined(TBLOG_NO_BACKTRACE) && defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define TBLOG_HAVE_BACKTRACE 1
#endif
#endif
#ifndef TBLOG_HAVE_BACKTRACE
#define TBLOG_HAVE_BACKTRACE 0
#endif
#include <mutex>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace tblog {

// Internal helper (not in internal.h): give the calling thread its own
// alternate signal stack so stack overflows on it are reported too. Freed at
// thread exit. Returns false if one could not be set up. Idempotent.
bool crash_altstack_for_this_thread();

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
constexpr int kNumSignals = sizeof(kSignals) / sizeof(kSignals[0]);
constexpr size_t kAltStackSize = 64 * 1024;
constexpr size_t kMinUsableAltStack = 16 * 1024;

TBLOG_ASSERT_LOCK_FREE(Backend *);
TBLOG_ASSERT_LOCK_FREE(long);

std::atomic<Backend *> g_backend{nullptr};
std::atomic<long> g_owner_tid{0};    // tid of the thread currently handling a crash
std::atomic<bool> g_installed{false};
struct sigaction g_prev[kNumSignals];  // written under g_mu before handlers go live
std::mutex g_mu;                        // install/uninstall only (never in handler)

alignas(64) char g_altstack[kAltStackSize];
bool g_altstack_ours = false;  // we replaced the installing thread's altstack
stack_t g_prev_altstack;

// ---------------------------------------------------------------- AS-safe utils
void write_all(int fd, const char *p, size_t n) noexcept {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

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

const char *signal_name(int sig) noexcept {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        default:      return "UNKNOWN";
    }
}

int signal_index(int sig) noexcept {
    for (int i = 0; i < kNumSignals; ++i)
        if (kSignals[i] == sig) return i;
    return -1;
}

bool is_real_handler(const struct sigaction &sa) noexcept {
    if (sa.sa_flags & SA_SIGINFO) return sa.sa_sigaction != nullptr;
    return sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN;
}

// Hand the signal on: restore the previous application handler (if chain and
// one existed) or SIG_DFL, then make it fire again. For kernel-generated
// faults (si_code > 0) returning re-executes the faulting instruction, which
// re-raises the signal with the new disposition. For user-sent signals
// (kill/raise/abort: si_code <= 0) we raise() it; it stays pending while we
// are in the handler (the signal is blocked) and is delivered on return.
void pass_on(int sig, siginfo_t *info, bool chain) noexcept {
    const int idx = signal_index(sig);
    struct sigaction sa;
    if (chain && idx >= 0 && is_real_handler(g_prev[idx])) {
        sa = g_prev[idx];
    } else {
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
    }
    sigaction(sig, &sa, nullptr);
    if (info == nullptr || info->si_code <= 0 || sig == SIGABRT) raise(sig);
}

void crash_signal_handler(int sig, siginfo_t *info, void *) {
    const long self = static_cast<long>(syscall(SYS_gettid));
    long expected = 0;
    if (!g_owner_tid.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
        if (expected == self) {
            // Fault inside our own handler (e.g. backend->on_crash crashed):
            // do nothing more, die with this signal.
            pass_on(sig, info, false);
            return;
        }
        // Another thread is already reporting a crash; give it time to finish
        // (it will kill the process), then die ourselves.
        struct timespec ts = {0, 10 * 1000 * 1000};
        for (int i = 0; i < 300 && g_owner_tid.load(std::memory_order_acquire) != 0; ++i)
            nanosleep(&ts, nullptr);
        pass_on(sig, info, false);
        return;
    }

    char msg[96];
    size_t n = append_str(msg, 0, sizeof(msg), "*** tblog: caught signal ");
    n = append_int(msg, n, sizeof(msg), sig);
    n = append_str(msg, n, sizeof(msg), " (");
    n = append_str(msg, n, sizeof(msg), signal_name(sig));
    n = append_str(msg, n, sizeof(msg), ") ***\n");

    write_all(STDERR_FILENO, msg, n);
    if (Backend *b = g_backend.load(std::memory_order_acquire)) {
        // Drain buffered lines first so the marker lands after them in the file.
        b->on_crash();
        b->crash_write(msg, n);
    }

#if TBLOG_HAVE_BACKTRACE
    void *frames[64];
    int depth = backtrace(frames, 64);
    if (depth > 0) backtrace_symbols_fd(frames, depth, STDERR_FILENO);
#endif

    pass_on(sig, info, true);
}

void setup_installing_thread_altstack() {
    stack_t cur;
    if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) &&
        cur.ss_size >= kMinUsableAltStack) {
        return;  // thread already has a usable one (e.g. installed by a sanitizer)
    }
    stack_t ss;
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof(g_altstack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, &g_prev_altstack) == 0) g_altstack_ours = true;
}

void restore_installing_thread_altstack() {
    if (!g_altstack_ours) return;
    stack_t cur;
    // Only touch it if this thread is still using our static stack.
    if (sigaltstack(nullptr, &cur) == 0 && cur.ss_sp == g_altstack) {
        sigaltstack(&g_prev_altstack, nullptr);
        g_altstack_ours = false;
    }
}

struct ThreadAltStack {
    void *mem = nullptr;
    ~ThreadAltStack() {
        if (!mem) return;
        stack_t cur;
        if (sigaltstack(nullptr, &cur) == 0 && cur.ss_sp == mem) {
            stack_t off;
            std::memset(&off, 0, sizeof(off));
            off.ss_flags = SS_DISABLE;
            sigaltstack(&off, nullptr);
        }
        munmap(mem, kAltStackSize);
    }
};

}  // namespace

bool crash_altstack_for_this_thread() {
    stack_t cur;
    if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) &&
        cur.ss_size >= kMinUsableAltStack)
        return true;
    thread_local ThreadAltStack tls;
    if (!tls.mem) {
        void *m = mmap(nullptr, kAltStackSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) return false;
        tls.mem = m;
    }
    stack_t ss;
    ss.ss_sp = tls.mem;
    ss.ss_size = kAltStackSize;
    ss.ss_flags = 0;
    return sigaltstack(&ss, nullptr) == 0;
}

void install_crash_handler(Backend *backend) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_backend.store(backend, std::memory_order_release);
    if (g_installed.load(std::memory_order_relaxed)) return;  // just swap backend

    // Known pitfall: the first backtrace() dlopen()s libgcc_s (malloc + locks).
    // Do it now, outside any signal context.
#if TBLOG_HAVE_BACKTRACE
    void *frames[4];
    (void)backtrace(frames, 4);
#endif

    setup_installing_thread_altstack();

    g_owner_tid.store(0, std::memory_order_release);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < kNumSignals; ++i) {
        if (sigaction(kSignals[i], &sa, &g_prev[i]) != 0) {
            std::memset(&g_prev[i], 0, sizeof(g_prev[i]));
            g_prev[i].sa_handler = SIG_DFL;
        }
    }
    g_installed.store(true, std::memory_order_release);
}

void uninstall_crash_handler() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_installed.load(std::memory_order_relaxed)) {
        g_backend.store(nullptr, std::memory_order_release);
        return;
    }
    for (int i = 0; i < kNumSignals; ++i) sigaction(kSignals[i], &g_prev[i], nullptr);
    restore_installing_thread_altstack();
    g_backend.store(nullptr, std::memory_order_release);
    g_installed.store(false, std::memory_order_release);
}

}  // namespace tblog
