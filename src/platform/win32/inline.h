// Windows inline part of the platform layer (included by platform/platform.h
// only). Deliberately free of <windows.h>: every translation unit of the
// library includes this header. realtime_ns() is defined out of line in
// win32/system.cpp for the same reason (one call more per log line).
#pragma once

namespace tblog {
namespace pf {

inline bool FileHandle::valid() const noexcept { return native != nullptr; }
inline FileHandle no_file() noexcept { return FileHandle{nullptr}; }

inline bool is_path_separator(char c) noexcept { return c == '/' || c == '\\'; }

}  // namespace pf
}  // namespace tblog
