// POSIX fatal-signal handling (see "Crash handling" in platform/platform.h;
// what gets reported lives in src/crash/crash_handler.cpp).
//
// Everything reachable from crash_signal_handler() is async-signal-safe:
// only atomics, write(), sigaction(), raise(), nanosleep(), current_thread_id()
// (syscall(gettid) / pthread_threadid_np), pthread_sigmask() (macOS) and
// backtrace()/backtrace_symbols_fd() (not formally AS-safe, but safe in
// practice once libgcc has been loaded, which crash_install forces; on macOS
// they live in libSystem). No malloc, no locks, no stdio.
//
// macOS differences (the first two are the #if defined(__APPLE__) below):
//   * XNU does not force a blocked synchronous fault signal through as Linux
//     does, and abort() blocks every signal but SIGABRT before raising it, so
//     a fault inside our handler (or the reporter) could spin on the faulting
//     instruction instead of killing the process: the handler unblocks the
//     fatal signals first.
//   * XNU fills si_code of SIGBUS (BUS_ADRERR) and, on arm64, SIGILL for
//     kill()/raise() too, so si_code > 0 does not prove a hardware fault:
//     see pass_on().
//   * A stack overflow on a secondary thread hits its guard page and arrives
//     as SIGBUS, not SIGSEGV (it is reported as such).
//
// Alternate stack: sigaltstack() is PER THREAD. crash_install() sets one up
// for the calling thread only (from static storage). A stack overflow in any
// other thread can only be reported if that thread also has an alternate
// stack: the worker and sync threads call crash_thread_init() when they start,
// and every other thread gets one on its first log call
// (crash_logging_thread_init, from thread_ctx()). A thread that never logs has
// none; an overflow there still kills the process without the report. Faults
// other than stack overflow are handled on the faulting thread's normal stack
// anyway.
#include "platform/platform.h"

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
#if defined(__APPLE__)
#include <pthread.h>  // pthread_sigmask
#endif
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace tblog {
namespace pf {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
constexpr int kNumSignals = sizeof(kSignals) / sizeof(kSignals[0]);
constexpr size_t kAltStackSize = 64 * 1024;
constexpr size_t kMinUsableAltStack = 16 * 1024;

// The signal handler relies on these being lock-free.
static_assert(__atomic_always_lock_free(sizeof(CrashReporter), 0), "atomic CrashReporter must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(long), 0), "atomic long must be lock-free");

std::atomic<CrashReporter> g_reporter{nullptr};
std::atomic<long> g_owner_tid{0};      // tid of the thread currently handling a crash
struct sigaction g_prev[kNumSignals];  // written by crash_install before handlers go live

alignas(64) char g_altstack[kAltStackSize];
bool g_altstack_ours = false;  // we replaced the installing thread's altstack
stack_t g_prev_altstack;

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
    bool resend = info == nullptr || info->si_code <= 0 || sig == SIGABRT;
#if defined(__APPLE__)
    // User-sent signals have si_code SI_USER and up (0x10001...), and XNU
    // reports raise(SIGBUS) / raise(SIGILL) with fault-like codes. Raising is
    // always right for the default action (the signal is unblocked, see
    // unblock_fatal_signals, so raise() terminates the process at once) and
    // is done for SIGBUS / SIGILL too: an application handler may then see
    // one synthetic delivery before a real fault re-faults, instead of a
    // raised signal being lost.
    if (!resend)
        resend = info->si_code >= SI_USER || sig == SIGBUS || sig == SIGILL ||
                 (!(sa.sa_flags & SA_SIGINFO) && sa.sa_handler == SIG_DFL);
#endif
    if (resend) raise(sig);
}

#if defined(__APPLE__)
void unblock_fatal_signals() noexcept {
    sigset_t set;
    sigemptyset(&set);
    for (int s : kSignals) sigaddset(&set, s);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
}
#endif

void crash_signal_handler(int sig, siginfo_t *info, void *) {
#if defined(__APPLE__)
    unblock_fatal_signals();  // a nested fault must re-enter us, not spin
#endif
    const long self = static_cast<long>(current_thread_id());
    long expected = 0;
    if (!g_owner_tid.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
        if (expected == self) {
            // Fault inside our own handler (e.g. the reporter crashed):
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

    if (CrashReporter r = g_reporter.load(std::memory_order_acquire))
        r(CrashKind::Signal, sig, signal_name(sig));

#if TBLOG_HAVE_BACKTRACE
    void *frames[64];
    int depth = backtrace(frames, 64);
    if (depth > 0) backtrace_symbols_fd(frames, depth, STDERR_FILENO);
#endif

    pass_on(sig, info, true);
}

void setup_installing_thread_altstack() noexcept {
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

void restore_installing_thread_altstack() noexcept {
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

bool crash_thread_init() noexcept {
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

void crash_logging_thread_init() noexcept {
    // A crash is being handled: this may be a signal handler logging (the
    // chained application handler, on a thread that never logged before).
    if (g_owner_tid.load(std::memory_order_acquire) != 0) return;
    stack_t cur;
    if (sigaltstack(nullptr, &cur) == 0 && (cur.ss_flags & SS_ONSTACK)) return;  // in a handler
    (void)crash_thread_init();
}

void crash_install(CrashReporter reporter) noexcept {
    g_reporter.store(reporter, std::memory_order_release);

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
}

void crash_uninstall() noexcept {
    for (int i = 0; i < kNumSignals; ++i) sigaction(kSignals[i], &g_prev[i], nullptr);
    restore_installing_thread_altstack();
    g_reporter.store(nullptr, std::memory_order_release);
}

}  // namespace pf
}  // namespace tblog
