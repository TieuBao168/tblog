// Formatter — src/core/formatter.cpp
// Pattern tokens:
//   %D date YYYY-MM-DD   %T time HH:MM:SS (local time)
//   %us microseconds (6 digits)   %ms milliseconds (3 digits)
//   %L level name   %t thread id   %n module name ("" for root -> "-")
//   %f file basename   %l line   %F function   %m message   %% literal '%'
// Unknown tokens are copied verbatim.
#pragma once

#include "tblog/log.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tblog {

struct Record {
    uint64_t ts_ns;
    uint32_t tid;
    tblog_level level;
    const char *module;  // never null; "" = root
    const char *file;
    int line;
    const char *func;
};

// message.escape (escape_message = true): in the %m text, trailing '\n' are
// removed and the other control characters are written as escapes ("\n",
// "\r", "\xHH" for the rest of 0x00-0x1F and 0x7F; tab is kept), so data
// logged with %s / {} cannot start a forged log line or send terminal escape
// sequences. Backslashes are not escaped (Windows paths stay readable), so
// the escaping is not reversible; it only guarantees one record per line.
// Only %m is affected: the pattern's own text and tokens are written as is.
class Formatter {
public:
    explicit Formatter(const std::string &pattern, bool escape_message = true);
    ~Formatter();
    Formatter(const Formatter &) = delete;
    Formatter &operator=(const Formatter &) = delete;

    // Writes ONE complete line ending in '\n' into out (cap bytes, cap >= 64).
    // Returns bytes written. Too long -> message truncated, line ends "...\n".
    // Must not allocate. Thread-safe (const, may use thread_local caches).
    size_t format(char *out, size_t cap, const Record &rec,
                  const char *fmt, va_list ap) const;

private:
    struct Impl;
    Impl *impl_;
};

}  // namespace tblog
