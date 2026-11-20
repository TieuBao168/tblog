// Thread context & clock — src/core/thread_context.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tblog {

constexpr size_t kScratchSize = 1024;  // max bytes of one formatted line

struct ThreadContext {
    uint32_t tid;                // OS thread id (pf::current_thread_id()), cached
    char scratch[kScratchSize];  // per-thread formatting buffer
};
ThreadContext &thread_ctx();     // thread_local, lazily initialised
uint64_t now_ns();               // wall clock (CLOCK_REALTIME) in nanoseconds

}  // namespace tblog
