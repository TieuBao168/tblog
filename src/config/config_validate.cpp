// validate_config(): build-dependent and cross-field rules, applied after
// parsing. See config.h for the list.
#include "config/config.h"

namespace tblog {

namespace {

void add(std::vector<std::string> *w, int *n, const std::string &msg) {
    ++*n;
    if (w) w->push_back(msg);
}

}  // namespace

bool validate_config(Config &c, std::vector<std::string> *warnings, size_t user_sinks) {
    int n = 0;

#ifndef TBLOG_HAVE_ZLIB
    if (c.file.compress != CompressMode::None) {
        add(warnings, &n, "file.compress = gzip needs a build with zlib; compression disabled");
        c.file.compress = CompressMode::None;
    }
#endif

    if (c.file.path.empty()) {
        c.file.path = FileOptions().path;
        add(warnings, &n, "empty file.path, using " + c.file.path);
    }
    if (c.pattern.empty()) {
        c.pattern = Config().pattern;
        add(warnings, &n, "empty pattern, using the default");
    }
    if (c.file.max_files < 1) {
        c.file.max_files = 1;
        add(warnings, &n, "file.max_files must be >= 1, set to 1");
    }
    if (c.file.max_size == 0) {
        c.file.max_size = 1;
        add(warnings, &n, "file.max_size must be > 0, set to 1 byte");
    }
    if (c.flush_interval_ms < kFlushIntervalMinMs || c.flush_interval_ms > kFlushIntervalMaxMs) {
        c.flush_interval_ms = c.flush_interval_ms < kFlushIntervalMinMs ? kFlushIntervalMinMs
                                                                        : kFlushIntervalMaxMs;
        add(warnings, &n, "flush.interval_ms clamped to " + std::to_string(c.flush_interval_ms));
    }
    if (c.sync_interval_ms > kSyncIntervalMaxMs) {
        c.sync_interval_ms = kSyncIntervalMaxMs;
        add(warnings, &n, "sync.interval_ms clamped to " + std::to_string(c.sync_interval_ms));
    }
    if (!c.file_enabled && !c.console && !c.syslog && user_sinks == 0) {
        c.console = true;
        add(warnings, &n, "file = false and no other sink (console, syslog, tblog_add_sink); "
                          "enabling console");
    }
    return n == 0;
}

}  // namespace tblog
