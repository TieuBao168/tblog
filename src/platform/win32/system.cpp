// Windows time, threads, process state, memory and console (see
// platform/platform.h).
#include "platform/win32/win32.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <malloc.h>

namespace tblog {
namespace pf {

namespace win {
// Set by DllMain (win32/dllmain.cpp, DLL builds only) on DLL_PROCESS_DETACH.
extern std::atomic<bool> g_dll_detaching;
}  // namespace win

// ------------------------------------------------------------------ time
uint64_t realtime_ns() noexcept {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100u;  // 100 ns ticks since 1601 -> ns since 1970
}

bool local_time(int64_t t, std::tm *out) noexcept {
    const time_t tt = static_cast<time_t>(t);
    return localtime_s(out, &tt) == 0;
}

// ------------------------------------------------------------------ threads
uint32_t current_thread_id() noexcept { return static_cast<uint32_t>(GetCurrentThreadId()); }

bool process_exiting() noexcept {
    if (win::g_dll_detaching.load(std::memory_order_acquire)) return true;
    // ntdll export (Windows XP and later): TRUE once ExitProcess started the
    // DLL_PROCESS_DETACH sequence. That is when the atexit handlers of a DLL
    // (and of a static tblog linked into one) run, after every other thread
    // was terminated. An EXE runs its atexit handlers before ExitProcess,
    // with its threads alive: there this is FALSE and shutdown joins normally.
    typedef BOOLEAN(NTAPI * ShutdownInProgressFn)(void);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    FARPROC p = GetProcAddress(ntdll, "RtlDllShutdownInProgress");
    if (!p) return false;
    ShutdownInProgressFn fn;
    static_assert(sizeof fn == sizeof p, "function pointer size");
    std::memcpy(&fn, &p, sizeof fn);  // FARPROC -> typed pointer without a cast warning
    return fn() != FALSE;
}

// ------------------------------------------------------------------ memory
void *alloc_aligned(size_t align, size_t size) noexcept { return _aligned_malloc(size, align); }

void free_aligned(void *p) noexcept { _aligned_free(p); }

// ------------------------------------------------------------------ console
// Looked up on every call (cheap: a field of the PEB), so SetStdHandle()
// redirections are honoured.
FileHandle stderr_handle() noexcept { return win::file_of(GetStdHandle(STD_ERROR_HANDLE)); }

bool stderr_is_terminal() noexcept {
    DWORD mode = 0;
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != FALSE;
}

bool enable_ansi_colors() noexcept {
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (!h || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
    // Fails on consoles older than Windows 10 1511: no colours then.
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
}

void stderr_write(const char *p, size_t n) noexcept {
    const FileHandle h = stderr_handle();
    if (h.valid()) file_write_crash(h, p, n);
}

}  // namespace pf
}  // namespace tblog
