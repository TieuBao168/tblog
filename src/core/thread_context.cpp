// Per-thread context and wall clock.
#include "core/thread_context.h"

#include "core/compat.h"
#include "platform/platform.h"

namespace tblog {

ThreadContext &thread_ctx() {
    static thread_local ThreadContext ctx;  // POD: zero-initialised, no TLS ctor
    if (TBLOG_UNLIKELY(ctx.tid == 0)) ctx.tid = pf::current_thread_id();
    return ctx;
}

uint64_t now_ns() { return pf::realtime_ns(); }

}  // namespace tblog
