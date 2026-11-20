// Config — src/config/config_parser.cpp, src/config/config_validate.cpp,
// src/config/config_watcher.cpp
#pragma once

#include "backend/backend.h"
#include "platform/platform.h"
#include "tblog/log.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tblog {

// Bounds of flush.interval_ms and sync.interval_ms (sync: 0 = periodic sync off).
constexpr uint32_t kFlushIntervalMinMs = 1, kFlushIntervalMaxMs = 60000;
constexpr uint32_t kSyncIntervalMaxMs = 3600000;

struct Config {
    tblog_level level = TBLOG_LV_INFO;
    std::string pattern = "%D %T.%us [%L] [%t] %f:%l - %m";
    FileOptions file;
    size_t buffer_size = 4u << 20;
    FullPolicy full_policy = FullPolicy::Drop;
    uint32_t flush_interval_ms = 200;
    uint32_t sync_interval_ms = 1000;
    bool console = false;
    bool console_color = true;
    bool syslog = false;
    std::vector<std::pair<std::string, tblog_level>> module_levels;
    bool reload_watch = true;
};

// Keys: level, pattern, file.path, file.max_size, file.max_files,
// file.rotate (size|daily|hourly), file.compress (none|gzip; gzip needs a
// zlib build), buffer.size, full_policy (drop|block),
// flush.interval_ms, sync.interval_ms, sync.on_level, console, console.color,
// syslog, module.<name>, reload.watch.
// Sizes accept B/KB/MB/GB suffixes. Bools: true/false/1/0/yes/no/on/off.
// '#' starts a comment (outside the value of `pattern`, see parser docs).
// Unknown key / bad value -> a warning string, field keeps its previous value.
// Removed keys (backend, mmap.region_size, format.mode) -> "<key>: option
// removed, ignored" warning.
// The parser checks each value on its own; rules that involve the build or
// several fields live in validate_config().
bool parse_config_string(const std::string &text, Config &out,
                         std::vector<std::string> *warnings);
// false if the file cannot be read (out untouched).
bool load_config(const char *path, Config &out, std::vector<std::string> *warnings);

// Normalises a parsed (or default) config so that every field holds the value
// that will actually be used, and appends one warning per adjustment that the
// user should know about. Run after parsing, before the config is used
// (tblog_init and hot-reload). Returns true iff no warning was added.
// Rules:
//   * file.compress = gzip in a build without zlib -> none (warning)
//   * file.max_files < 1 -> 1, file.max_size == 0 -> 1 (programmatic configs;
//     the parser already rejects these)
//   * flush.interval_ms clamped to [kFlushIntervalMinMs, kFlushIntervalMaxMs],
//     sync.interval_ms to [0, kSyncIntervalMaxMs] (programmatic configs; the
//     parser clamps already)
//   * empty file.path / pattern -> the default (programmatic configs)
bool validate_config(Config &cfg, std::vector<std::string> *warnings);

class ConfigWatcher {
public:
    explicit ConfigWatcher(std::string path);
    // true if the file's mtime/size/identity changed since construction or the
    // previous true result. Cheap (one stat()).
    bool changed();

private:
    std::string path_;
    int64_t mtime_ns_ = -1;
    int64_t size_ = -1;
    pf::FileId id_ = {0, 0};
};

}  // namespace tblog
