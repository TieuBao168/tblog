// Sinks — src/sinks/console.cpp, src/sinks/syslog.cpp
// Sinks receive raw bytes (one or more complete lines) from the worker thread.
#pragma once

#include "platform/platform.h"

#include <cstddef>
#include <memory>

namespace tblog {

class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const char *data, size_t len) = 0;
    virtual void flush() {}
};

std::unique_ptr<Sink> make_console_sink(bool color);  // stderr
// System log (openlog/syslog on POSIX). Returns nullptr, after one warning on
// stderr, where the platform has no supported system log.
std::unique_ptr<Sink> make_syslog_sink(const char *ident);

// Test hook: write `data` to `h` exactly as the console sink would,
// colourised when `color` is true (no terminal check here).
void console_write_to(pf::FileHandle h, const char *data, size_t len, bool color);

}  // namespace tblog
