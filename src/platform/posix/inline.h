// POSIX inline part of the platform layer (included by platform/platform.h
// only): the calls on the logging hot path, so they cost nothing more than
// the system call itself.
#pragma once

#include <time.h>

namespace tblog {
namespace pf {

inline bool FileHandle::valid() const noexcept { return native >= 0; }
inline FileHandle no_file() noexcept { return FileHandle{-1}; }

inline bool is_path_separator(char c) noexcept { return c == '/'; }

inline uint64_t realtime_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

}  // namespace pf
}  // namespace tblog
