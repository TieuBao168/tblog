// Per-thread context and wall clock.
#include "internal.h"

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace tblog {

ThreadContext &thread_ctx() {
    static thread_local ThreadContext ctx;  // POD: zero-initialised, no TLS ctor
    if (__builtin_expect(ctx.tid == 0, 0)) ctx.tid = (uint32_t)syscall(SYS_gettid);
    return ctx;
}

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

}  // namespace tblog
