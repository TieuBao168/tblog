// POSIX time, threads, memory and console (see platform/platform.h).
#include "platform/platform.h"

#include <cerrno>
#include <cstdlib>

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace tblog {
namespace pf {

// ------------------------------------------------------------------ time
bool local_time(int64_t t, std::tm *out) noexcept {
    time_t tt = static_cast<time_t>(t);
    return ::localtime_r(&tt, out) != nullptr;
}

// ------------------------------------------------------------------ threads
uint32_t current_thread_id() noexcept {
#if defined(__linux__)
    return static_cast<uint32_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    uint32_t t = static_cast<uint32_t>(id);
    return t ? t : 1;
#else
#error "tblog: current_thread_id() is not implemented for this platform"
#endif
}

// ------------------------------------------------------------------ memory
void *alloc_aligned(size_t align, size_t size) noexcept {
    void *p = nullptr;
    if (::posix_memalign(&p, align, size) != 0) return nullptr;
    return p;
}

void free_aligned(void *p) noexcept { std::free(p); }

// ------------------------------------------------------------------ console
FileHandle stderr_handle() noexcept { return FileHandle{STDERR_FILENO}; }

bool stderr_is_terminal() noexcept { return ::isatty(STDERR_FILENO) == 1; }

bool enable_ansi_colors() noexcept { return true; }

void stderr_write(const char *p, size_t n) noexcept {
    while (n > 0) {
        ssize_t w = ::write(STDERR_FILENO, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (w == 0) return;
        p += w;
        n -= static_cast<size_t>(w);
    }
}

}  // namespace pf
}  // namespace tblog
