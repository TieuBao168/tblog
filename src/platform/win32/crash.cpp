// Windows crash handling (see "Crash handling" in platform/platform.h; what
// gets reported lives in src/crash/crash_handler.cpp).
//
//   * Hardware faults and other unhandled SEH exceptions (access violation,
//     illegal instruction, integer divide by zero, stack overflow, ...): a
//     top-level filter, SetUnhandledExceptionFilter. It runs on the faulting
//     thread once no __except handler took the exception. C++ exceptions
//     (0xE06D7363) are left alone: an uncaught one ends in std::terminate ->
//     abort(), reported as SIGABRT below.
//   * abort() (and so a failing assert, std::terminate): signal(SIGABRT).
//
// On a crash: the first crashing thread calls the reporter (same marker
// and drain as on POSIX), then writes the return addresses of its stack to
// stderr, then hands on: the previous filter (or EXCEPTION_CONTINUE_SEARCH,
// so Windows Error Reporting / a JIT debugger still sees the exception),
// resp. the previous SIGABRT action re-raised (SIG_DFL: the CRT ends the
// process with exit code 3). A fault inside the reporter, or a crash on a
// second thread while the first is reporting, skips the reporter.
//
// Stack overflow: the filter runs on what is left of the overflowing stack.
// SetThreadStackGuarantee reserves room for it on the installing thread
// (crash_install) and on every thread that calls crash_thread_init().
//
// Everything reachable from the handlers sticks to what is safe while other
// threads may hold arbitrary locks (the heap's included): atomics, WriteFile
// on stderr / the log file, RtlCaptureStackBackTrace. No malloc, no CRT I/O.
#include "platform/win32/win32.h"

#include <atomic>
#include <csignal>
#include <cstring>

// The base address of the module this file is linked into (the EXE, or
// tblog.dll): provided by the MSVC and GNU linkers, no lookup (and no loader
// lock) needed.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace tblog {
namespace pf {

namespace {

constexpr ULONG kStackGuarantee = 64 * 1024;
constexpr DWORD kCppException = 0xE06D7363;  // MSVC "msc" C++ exception code

typedef void (*SignalHandler)(int);

std::atomic<CrashReporter> g_reporter{nullptr};
std::atomic<DWORD> g_owner_tid{0};  // thread currently handling a crash
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;  // written by crash_install
SignalHandler g_prev_abort = SIG_DFL;                   // written by crash_install

const char *exception_name(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
        case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
        default: return "UNKNOWN_EXCEPTION";
    }
}

void put_hex(char *buf, size_t *pos, uint64_t v) noexcept {
    static const char kDigits[] = "0123456789abcdef";
    buf[(*pos)++] = '0';
    buf[(*pos)++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) buf[(*pos)++] = kDigits[(v >> shift) & 0xF];
}

void write_line(const char *label, uint64_t addr) noexcept {
    char line[64];
    size_t n = 0;
    while (*label && n < 24) line[n++] = *label++;
    line[n++] = '[';
    put_hex(line, &n, addr);
    line[n++] = ']';
    line[n++] = '\n';
    stderr_write(line, n);
}

// Return addresses only (no dbghelp: symbolising is not safe in a crashing
// process). Resolve them offline against the module bases printed first,
// e.g. `addr2line -e app.exe <addr - base + image base>` or WinDbg's `ln`.
void write_backtrace(const void *fault_addr) noexcept {
    stderr_write("*** tblog: stack (return addresses):\n", 37);
    write_line("  exe base    ", reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    write_line("  tblog base  ", reinterpret_cast<uintptr_t>(&__ImageBase));  // exe or tblog.dll
    if (fault_addr) write_line("  fault at    ", reinterpret_cast<uintptr_t>(fault_addr));
    void *frames[62];
    const USHORT depth = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < depth; ++i) {
        char label[16] = "  #";
        size_t n = 3;
        if (i >= 10) label[n++] = static_cast<char>('0' + i / 10);
        label[n++] = static_cast<char>('0' + i % 10);
        while (n < 14) label[n++] = ' ';
        label[n] = '\0';
        write_line(label, reinterpret_cast<uintptr_t>(frames[i]));
    }
}

// Claims the crash for this thread. Returns 1 if it did; 0 if this thread
// already owns it (a fault inside our own handler); -1 if another thread is
// reporting (after giving it up to 3 s to finish).
int claim() noexcept {
    const DWORD self = GetCurrentThreadId();
    DWORD expected = 0;
    if (g_owner_tid.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) return 1;
    if (expected == self) return 0;
    for (int i = 0; i < 300 && g_owner_tid.load(std::memory_order_acquire) != 0; ++i) Sleep(10);
    return -1;
}

LONG chain_filter(EXCEPTION_POINTERS *ep) noexcept {
    if (g_prev_filter) return g_prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    if (code == kCppException) return chain_filter(ep);
    const int c = claim();
    if (c <= 0) return EXCEPTION_CONTINUE_SEARCH;  // straight to the default action
    if (CrashReporter r = g_reporter.load(std::memory_order_acquire))
        r(CrashKind::Exception, static_cast<int>(code), exception_name(code));
    write_backtrace(ep->ExceptionRecord->ExceptionAddress);
    return chain_filter(ep);
}

// Hand SIGABRT on, like POSIX pass_on(): the previous action (if chain and
// it was a function) or SIG_DFL, raised again. The CRT resets the action to
// SIG_DFL before calling a handler; raise() with SIG_DFL ends the process
// with exit code 3, SIG_IGN returns (and abort() then ends it anyway).
void pass_on_abort(bool chain) noexcept {
    SignalHandler prev = chain ? g_prev_abort : SIG_DFL;
    if (prev == SIG_ERR) prev = SIG_DFL;
    std::signal(SIGABRT, prev);
    std::raise(SIGABRT);
}

void abort_handler(int sig) {
    const int c = claim();
    if (c <= 0) {
        pass_on_abort(false);
        return;
    }
    if (CrashReporter r = g_reporter.load(std::memory_order_acquire)) r(CrashKind::Signal, sig, "SIGABRT");
    write_backtrace(nullptr);
    pass_on_abort(true);
}

}  // namespace

bool crash_thread_init() noexcept {
    ULONG size = kStackGuarantee;
    // Keeps a larger guarantee if the thread already has one.
    ULONG cur = 0;
    if (SetThreadStackGuarantee(&cur) && cur >= kStackGuarantee) return true;
    return SetThreadStackGuarantee(&size) != FALSE;
}

// The filter runs on what is left of the overflowing stack, which has been
// enough on every thread measured (bench_crash_loss stack_overflow_thread:
// 0 lines lost), so threads that only log keep their default guarantee.
void crash_logging_thread_init() noexcept {}

void crash_install(CrashReporter reporter) noexcept {
    g_reporter.store(reporter, std::memory_order_release);
    g_owner_tid.store(0, std::memory_order_release);
    crash_thread_init();
    g_prev_filter = SetUnhandledExceptionFilter(&crash_filter);
    SignalHandler prev = std::signal(SIGABRT, &abort_handler);
    g_prev_abort = prev == SIG_ERR ? SIG_DFL : prev;
}

void crash_uninstall() noexcept {
    SetUnhandledExceptionFilter(g_prev_filter);
    std::signal(SIGABRT, g_prev_abort);
    g_prev_filter = nullptr;
    g_prev_abort = SIG_DFL;
    g_reporter.store(nullptr, std::memory_order_release);
}

}  // namespace pf
}  // namespace tblog
