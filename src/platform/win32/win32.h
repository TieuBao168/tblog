// Shared helpers of the win32 platform implementation (src/platform/win32/
// only): <windows.h> with the usual trimming, UTF-8 <-> UTF-16 conversion and
// the GetLastError() -> errno mapping the pf:: contract asks for.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "platform/platform.h"

#include <string>

namespace tblog {
namespace pf {
namespace win {

inline HANDLE handle_of(FileHandle f) noexcept { return static_cast<HANDLE>(f.native); }
inline FileHandle file_of(HANDLE h) noexcept {
    return FileHandle{h == INVALID_HANDLE_VALUE ? nullptr : static_cast<void *>(h)};
}

// Closest errno value for a Win32 error code.
int errno_from(DWORD err) noexcept;
// errno = errno_from(GetLastError()).
void set_errno_from_last_error() noexcept;

// UTF-8 -> UTF-16. false (errno EINVAL for invalid UTF-8 or an embedded NUL,
// ENOMEM) on failure. Never throws.
bool to_wide(const std::string &s, std::wstring *out) noexcept;
// UTF-16 -> UTF-8 (invalid sequences become U+FFFD). May throw std::bad_alloc.
std::string to_utf8(const wchar_t *s, size_t n);

}  // namespace win
}  // namespace pf
}  // namespace tblog
