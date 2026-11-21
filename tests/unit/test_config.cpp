// Unit tests for src/config (parser + validate_config + watcher).
#include "config/config.h"
#include "tiny_test.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

using namespace tblog;

namespace {

std::string test_dir() {
    const char *d = std::getenv("TBLOG_TEST_DIR");
    std::string dir = (d && *d) ? d : "build/tmp";
    // mkdir -p
    for (size_t i = 1; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0755);
    return dir;
}

void write_file(const std::string &p, const std::string &content) {
    FILE *f = std::fopen(p.c_str(), "wb");
    if (!f) return;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

bool any_contains(const std::vector<std::string> &w, const char *needle) {
    for (auto &s : w)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

void set_mtime(const std::string &p, time_t sec) {
    struct timespec ts[2];
    ts[0].tv_sec = sec; ts[0].tv_nsec = 0;
    ts[1].tv_sec = sec; ts[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, p.c_str(), ts, 0);
}

const char *kExample =
    "level = INFO\n"
    "pattern = %D %T.%us [%L] [%t] %f:%l - %m\n"
    "file.path = ./logs/app.log\n"
    "file.max_size = 256MB\n"
    "file.max_files = 5\n"
    "buffer.size = 4MB\n"
    "full_policy = drop\n"
    "flush.interval_ms = 200\n"
    "sync.interval_ms = 1000\n"
    "sync.on_level = ERROR\n"
    "console = false\n"
    "module.network = DEBUG\n"
    "reload.watch = true\n";

}  // namespace

TEST(full_example) {
    Config c;
    c.level = TBLOG_LV_TRACE;  // make sure values are actually assigned
    c.console = true;
    c.reload_watch = false;
    c.urgent_level = TBLOG_LV_FATAL;
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string(kExample, c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_EQ(int(c.level), int(TBLOG_LV_INFO));
    EXPECT_EQ(c.pattern, std::string("%D %T.%us [%L] [%t] %f:%l - %m"));
    EXPECT_EQ(c.file.path, std::string("./logs/app.log"));
    EXPECT_EQ(c.file.max_size, uint64_t(256) << 20);
    EXPECT_EQ(c.file.max_files, 5);
    EXPECT_EQ(c.buffer_size, size_t(4) << 20);
    EXPECT_TRUE(c.full_policy == FullPolicy::Drop);
    EXPECT_EQ(c.flush_interval_ms, 200u);
    EXPECT_EQ(c.sync_interval_ms, 1000u);
    EXPECT_EQ(int(c.urgent_level), int(TBLOG_LV_ERROR));
    EXPECT_FALSE(c.console);
    ASSERT_TRUE(c.module_levels.size() == 1);
    EXPECT_EQ(c.module_levels[0].first, std::string("network"));
    EXPECT_EQ(int(c.module_levels[0].second), int(TBLOG_LV_DEBUG));
    EXPECT_TRUE(c.reload_watch);
}

TEST(rotate_and_compress) {
    Config c;
    EXPECT_TRUE(c.file.rotate == RotateMode::Size);  // defaults
    EXPECT_TRUE(c.file.compress == CompressMode::None);
    EXPECT_TRUE(c.file.now_fn == nullptr);
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string("file.rotate = daily\nfile.compress = gzip  # comment\n", c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_TRUE(c.file.rotate == RotateMode::Daily);
    EXPECT_TRUE(c.file.compress == CompressMode::Gzip);
    EXPECT_TRUE(parse_config_string("file.rotate = HOURLY\nfile.compress = None\n", c, nullptr));
    EXPECT_TRUE(c.file.rotate == RotateMode::Hourly);
    EXPECT_TRUE(c.file.compress == CompressMode::None);
    EXPECT_TRUE(parse_config_string("file.rotate = Size", c, nullptr));
    EXPECT_TRUE(c.file.rotate == RotateMode::Size);

    // Invalid values warn (with the line) and keep the previous value.
    c.file.rotate = RotateMode::Daily;
    c.file.compress = CompressMode::Gzip;
    w.clear();
    EXPECT_FALSE(parse_config_string("level = INFO\nfile.rotate = weekly\nfile.compress = zstd\n"
                                     "file.rotate =\n",
                                     c, &w));
    ASSERT_TRUE(w.size() == 3);
    EXPECT_TRUE(w[0].find("line 2") != std::string::npos);
    EXPECT_TRUE(w[0].find("size|daily|hourly") != std::string::npos);
    EXPECT_TRUE(w[1].find("line 3") != std::string::npos);
    EXPECT_TRUE(w[1].find("none|gzip") != std::string::npos);
    EXPECT_TRUE(c.file.rotate == RotateMode::Daily);
    EXPECT_TRUE(c.file.compress == CompressMode::Gzip);
}

TEST(other_keys_and_values) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string("full_policy = Block\nconsole.color = off\n"
                                    "syslog = yes\nlevel = warning\nconsole = 1\n",
                                    c, &w));
    EXPECT_TRUE(c.full_policy == FullPolicy::Block);
    EXPECT_FALSE(c.console_color);
    EXPECT_TRUE(c.syslog);
    EXPECT_TRUE(c.console);
    EXPECT_EQ(int(c.level), int(TBLOG_LV_WARN));
    for (const char *lv : {"trace", "Debug", "INFO", "Warn", "error", "fatal", "OFF"}) {
        Config d;
        EXPECT_TRUE(parse_config_string(std::string("level = ") + lv, d, nullptr));
    }
}

// Keys of removed features warn ("option removed") and change nothing, so old
// config files keep loading.
TEST(removed_options_warn_and_are_ignored) {
    Config c;
    const Config before = c;
    std::vector<std::string> w;
    EXPECT_FALSE(parse_config_string("backend = mmap\nmmap.region_size = 16MB\nlevel = DEBUG\n"
                                     "format.mode = deferred\nbackend = bogus\n",
                                     c, &w));
    ASSERT_TRUE(w.size() == 4);
    EXPECT_TRUE(w[0].find("line 1") != std::string::npos);
    EXPECT_TRUE(w[0].find("backend: option removed, ignored") != std::string::npos);
    EXPECT_TRUE(w[1].find("line 2") != std::string::npos);
    EXPECT_TRUE(w[1].find("mmap.region_size: option removed, ignored") != std::string::npos);
    EXPECT_TRUE(w[2].find("line 4") != std::string::npos);
    EXPECT_TRUE(w[2].find("format.mode: option removed, ignored") != std::string::npos);
    EXPECT_TRUE(w[3].find("line 5") != std::string::npos);
    EXPECT_TRUE(w[3].find("backend: option removed, ignored") != std::string::npos);
    for (const auto &s : w) EXPECT_TRUE(s.find("unknown key") == std::string::npos);
    // The valid line in between is still applied; nothing else changed.
    EXPECT_EQ(int(c.level), int(TBLOG_LV_DEBUG));
    EXPECT_EQ(c.file.max_size, before.file.max_size);
    EXPECT_EQ(c.buffer_size, before.buffer_size);
    EXPECT_TRUE(c.full_policy == before.full_policy);
}

TEST(comments_and_whitespace) {
    Config c;
    std::vector<std::string> w;
    const char *t =
        "# full comment\n"
        "   # indented comment\n"
        "\n"
        "   \t  \n"
        "  level   =   DEBUG   # inline comment\n"
        "file.path=/tmp/a#b.log\n"       // '#' not preceded by space -> part of value
        "console = true\t# tab comment\r\n"
        "module.db = ERROR #x\n";
    EXPECT_TRUE(parse_config_string(t, c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_EQ(int(c.level), int(TBLOG_LV_DEBUG));
    EXPECT_EQ(c.file.path, std::string("/tmp/a#b.log"));
    EXPECT_TRUE(c.console);
    ASSERT_TRUE(c.module_levels.size() == 1);
    EXPECT_EQ(c.module_levels[0].first, std::string("db"));
}

TEST(pattern_with_hash) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string("pattern = #%l [%L] # %m  \n", c, &w));
    EXPECT_EQ(c.pattern, std::string("#%l [%L] # %m"));
    EXPECT_TRUE(parse_config_string("pattern = \"  > %m <  \"\n", c, &w));
    EXPECT_EQ(c.pattern, std::string("  > %m <  "));
}

TEST(quoted_values) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string("file.path = \"  my dir/x #1.log \"  # comment\n", c, &w));
    EXPECT_EQ(c.file.path, std::string("  my dir/x #1.log "));
    EXPECT_TRUE(parse_config_string("level = \"ERROR\"", c, &w));
    EXPECT_EQ(int(c.level), int(TBLOG_LV_ERROR));
    // malformed quotes -> warning, value kept
    w.clear();
    EXPECT_FALSE(parse_config_string("file.path = \"unterminated\nfile.path = \"a\" junk\n", c, &w));
    EXPECT_EQ(w.size(), size_t(2));
    EXPECT_EQ(c.file.path, std::string("  my dir/x #1.log "));
}

TEST(invalid_values_keep_previous) {
    Config c;
    EXPECT_TRUE(parse_config_string(kExample, c, nullptr));
    Config before = c;
    const char *bad[] = {
        "level = LOUD", "pattern =", "file.path =", "file.max_size = 12XB",
        "file.max_size = -5MB", "file.max_files = abc", "buffer.size = 0",
        "buffer.size = MB", "full_policy = maybe",
        "flush.interval_ms = fast", "flush.interval_ms = -3", "sync.interval_ms = 1e3",
        "sync.on_level = NOPE", "console = maybe", "console.color = 2", "syslog = ",
        "reload.watch = yesno", "module.net = LOUD", "module. = INFO", "no equals sign",
        "= value", "file.max_files = 99999999999999999999",
    };
    for (const char *line : bad) {
        std::vector<std::string> w;
        EXPECT_FALSE(parse_config_string(std::string(line) + "\n", c, &w));
        EXPECT_EQ(w.size(), size_t(1));
        if (!w.empty()) {
            EXPECT_TRUE(w[0].find("line 1") != std::string::npos);
            // offending text (trimmed line) is quoted in the warning
            std::string t(line);
            while (!t.empty() && t.back() == ' ') t.pop_back();
            EXPECT_TRUE(w[0].find("'" + t + "'") != std::string::npos);
        }
    }
    EXPECT_EQ(int(c.level), int(before.level));
    EXPECT_EQ(c.pattern, before.pattern);
    EXPECT_EQ(c.file.path, before.file.path);
    EXPECT_EQ(c.file.max_size, before.file.max_size);
    EXPECT_EQ(c.file.max_files, before.file.max_files);
    EXPECT_EQ(c.buffer_size, before.buffer_size);
    EXPECT_TRUE(c.full_policy == before.full_policy);
    EXPECT_EQ(c.flush_interval_ms, before.flush_interval_ms);
    EXPECT_EQ(c.sync_interval_ms, before.sync_interval_ms);
    EXPECT_EQ(int(c.urgent_level), int(before.urgent_level));
    EXPECT_EQ(c.console, before.console);
    EXPECT_EQ(c.console_color, before.console_color);
    EXPECT_EQ(c.syslog, before.syslog);
    EXPECT_EQ(c.reload_watch, before.reload_watch);
    EXPECT_TRUE(c.module_levels == before.module_levels);
}

TEST(unknown_key_and_line_numbers) {
    Config c;
    std::vector<std::string> w;
    EXPECT_FALSE(parse_config_string("level = INFO\n# c\n\nfoo.bar = 1\nlevel = nope\n", c, &w));
    ASSERT_TRUE(w.size() == 2);
    EXPECT_TRUE(w[0].find("line 4") != std::string::npos);
    EXPECT_TRUE(w[0].find("foo.bar") != std::string::npos);
    EXPECT_TRUE(w[1].find("line 5") != std::string::npos);
    EXPECT_TRUE(w[1].find("nope") != std::string::npos);
    // a null warnings vector is fine
    EXPECT_FALSE(parse_config_string("zzz = 1", c, nullptr));
}

TEST(duplicates_and_modules) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string("level = DEBUG\nlevel = ERROR\nmodule.a = INFO\n"
                                    "module.b = WARN\nmodule.a = TRACE\n",
                                    c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_EQ(int(c.level), int(TBLOG_LV_ERROR));
    ASSERT_TRUE(c.module_levels.size() == 2);
    EXPECT_EQ(c.module_levels[0].first, std::string("a"));
    EXPECT_EQ(int(c.module_levels[0].second), int(TBLOG_LV_TRACE));
    EXPECT_EQ(c.module_levels[1].first, std::string("b"));
    // second parse on the same Config updates/appends
    parse_config_string("module.b = OFF\nmodule.c = FATAL\n", c, nullptr);
    ASSERT_TRUE(c.module_levels.size() == 3);
    EXPECT_EQ(int(c.module_levels[1].second), int(TBLOG_LV_OFF));
    EXPECT_EQ(c.module_levels[2].first, std::string("c"));
}

TEST(size_suffixes_and_overflow) {
    struct { const char *v; uint64_t expect; } ok[] = {
        {"123", 123}, {"123B", 123}, {"2kb", 2048}, {"2 KB", 2048}, {"3Mb", 3ull << 20},
        {"1GB", 1ull << 30}, {"4g", 4ull << 30}, {"5k", 5120}, {"7m", 7ull << 20},
        {"16000GB", 16000ull << 30},
    };
    for (auto &e : ok) {
        Config c;
        std::vector<std::string> w;
        EXPECT_TRUE(parse_config_string(std::string("file.max_size = ") + e.v, c, &w));
        EXPECT_EQ(c.file.max_size, e.expect);
    }
    const char *bad[] = {"99999999999999999999", "18446744073709551615KB", "17179869184GB",
                         "-1", "1TB", "MB", "1.5GB", "12 34"};
    for (const char *b : bad) {
        Config c;
        std::vector<std::string> w;
        EXPECT_FALSE(parse_config_string(std::string("buffer.size = ") + b, c, &w));
        EXPECT_EQ(w.size(), size_t(1));
        EXPECT_EQ(c.buffer_size, size_t(4) << 20);
    }
}

TEST(interval_clamping) {
    Config c;
    std::vector<std::string> w;
    EXPECT_FALSE(parse_config_string("flush.interval_ms = 0", c, &w));
    EXPECT_EQ(c.flush_interval_ms, 1u);
    EXPECT_EQ(w.size(), size_t(1));
    EXPECT_TRUE(any_contains(w, "clamped"));
    w.clear();
    EXPECT_FALSE(parse_config_string("flush.interval_ms = 70000", c, &w));
    EXPECT_EQ(c.flush_interval_ms, 60000u);
    EXPECT_EQ(w.size(), size_t(1));
    EXPECT_TRUE(parse_config_string("flush.interval_ms = 60000\nsync.interval_ms = 0", c, nullptr));
    EXPECT_EQ(c.sync_interval_ms, 0u);
    w.clear();
    EXPECT_FALSE(parse_config_string("sync.interval_ms = 3600001", c, &w));
    EXPECT_EQ(c.sync_interval_ms, 3600000u);
    EXPECT_EQ(w.size(), size_t(1));
    w.clear();
    EXPECT_FALSE(parse_config_string("sync.interval_ms = 99999999999999999999999", c, &w));
    EXPECT_EQ(c.sync_interval_ms, 3600000u);
    EXPECT_EQ(w.size(), size_t(1));
}

TEST(sanity_rules) {
    Config c;
    std::vector<std::string> w;
    EXPECT_FALSE(parse_config_string("file.max_files = 0", c, &w));
    EXPECT_EQ(c.file.max_files, 1);
    EXPECT_EQ(w.size(), size_t(1));
    w.clear();
    c.file.max_files = 4;
    EXPECT_FALSE(parse_config_string("file.max_files = -2", c, &w));
    EXPECT_EQ(c.file.max_files, 1);

    // A small file.max_size is taken as is.
    Config b;
    EXPECT_TRUE(parse_config_string("file.max_size = 1MB\n", b, nullptr));
    EXPECT_EQ(b.file.max_size, uint64_t(1) << 20);
}

TEST(load_config_file) {
    std::string dir = test_dir();
    std::string p = dir + "/test_config_load.conf";
    write_file(p, kExample);
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(load_config(p.c_str(), c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_EQ(c.file.max_size, uint64_t(256) << 20);
    // warnings do not make load_config fail
    write_file(p, "bogus = 1\nlevel = ERROR\n");
    w.clear();
    EXPECT_TRUE(load_config(p.c_str(), c, &w));
    EXPECT_EQ(w.size(), size_t(1));
    EXPECT_EQ(int(c.level), int(TBLOG_LV_ERROR));
    ::unlink(p.c_str());
}

TEST(load_config_missing) {
    std::string p = test_dir() + "/definitely_missing_tblog.conf";
    ::unlink(p.c_str());
    Config c;
    c.level = TBLOG_LV_FATAL;
    c.pattern = "keep";
    std::vector<std::string> w;
    EXPECT_FALSE(load_config(p.c_str(), c, &w));
    EXPECT_EQ(int(c.level), int(TBLOG_LV_FATAL));
    EXPECT_EQ(c.pattern, std::string("keep"));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_FALSE(load_config(nullptr, c, &w));
    EXPECT_FALSE(load_config(test_dir().c_str(), c, &w));  // a directory
    EXPECT_EQ(c.pattern, std::string("keep"));
}

TEST(watcher) {
    std::string p = test_dir() + "/test_config_watch.conf";
    ::unlink(p.c_str());

    ConfigWatcher missing(p);
    EXPECT_FALSE(missing.changed());

    write_file(p, "level = INFO\n");
    set_mtime(p, 1000000);
    EXPECT_TRUE(missing.changed());   // appeared
    EXPECT_FALSE(missing.changed());  // reported once

    ConfigWatcher w(p);
    EXPECT_FALSE(w.changed());

    // content + size change (mtime pinned to the same value -> size alone detects it)
    write_file(p, "level = DEBUG\n");
    set_mtime(p, 1000000);
    EXPECT_TRUE(w.changed());
    EXPECT_FALSE(w.changed());

    // same-size rewrite, only mtime differs
    write_file(p, "level = TRACE\n");
    set_mtime(p, 1000000);
    EXPECT_FALSE(w.changed());
    set_mtime(p, 1000005);
    EXPECT_TRUE(w.changed());
    EXPECT_FALSE(w.changed());

    // deletion
    ::unlink(p.c_str());
    EXPECT_TRUE(w.changed());
    EXPECT_FALSE(w.changed());

    // re-creation with identical size and mtime still detected (missing -> present)
    write_file(p, "level = TRACE\n");
    set_mtime(p, 1000005);
    EXPECT_TRUE(w.changed());
    EXPECT_FALSE(w.changed());

    // atomic replace via rename (new inode), same size and mtime
    std::string tmp = p + ".tmp";
    write_file(tmp, "level = TRACE\n");
    set_mtime(tmp, 1000005);
    struct stat a, b;
    ::stat(p.c_str(), &a);
    ::stat(tmp.c_str(), &b);
    ::rename(tmp.c_str(), p.c_str());
    if (a.st_ino != b.st_ino) EXPECT_TRUE(w.changed());
    EXPECT_FALSE(w.changed());
    ::unlink(p.c_str());
}

// ------------------------------------------------------------ validate_config
TEST(validate_defaults_unchanged) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(validate_config(c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    const Config d;
    EXPECT_EQ(int(c.level), int(d.level));
    EXPECT_EQ(c.pattern, d.pattern);
    EXPECT_EQ(c.file.path, d.file.path);
    EXPECT_EQ(c.file.max_size, d.file.max_size);
    EXPECT_EQ(c.file.max_files, d.file.max_files);
    EXPECT_TRUE(c.file.rotate == d.file.rotate);
    EXPECT_TRUE(c.file.compress == d.file.compress);
    EXPECT_EQ(c.buffer_size, d.buffer_size);
    EXPECT_EQ(c.flush_interval_ms, d.flush_interval_ms);
    EXPECT_EQ(c.sync_interval_ms, d.sync_interval_ms);
    EXPECT_TRUE(validate_config(c, nullptr));  // null warnings vector is fine
}

TEST(validate_parsed_config_unchanged) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string(kExample, c, &w));
    Config before = c;
    EXPECT_TRUE(validate_config(c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_EQ(c.file.path, before.file.path);
    EXPECT_EQ(c.file.max_files, before.file.max_files);
    EXPECT_EQ(c.flush_interval_ms, before.flush_interval_ms);
    ASSERT_TRUE(c.module_levels.size() == 1);
}

TEST(validate_gzip_needs_zlib) {
    Config c;
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string("file.compress = gzip\n", c, &w));
    EXPECT_TRUE(c.file.compress == CompressMode::Gzip);  // the parser does not judge the build
#ifdef TBLOG_HAVE_ZLIB
    EXPECT_TRUE(validate_config(c, &w));
    EXPECT_EQ(w.size(), size_t(0));
    EXPECT_TRUE(c.file.compress == CompressMode::Gzip);
#else
    EXPECT_FALSE(validate_config(c, &w));
    ASSERT_TRUE(w.size() == 1);
    EXPECT_TRUE(any_contains(w, "file.compress"));
    EXPECT_TRUE(any_contains(w, "zlib"));
    EXPECT_TRUE(c.file.compress == CompressMode::None);
    // Idempotent: a validated config validates cleanly.
    w.clear();
    EXPECT_TRUE(validate_config(c, &w));
    EXPECT_EQ(w.size(), size_t(0));
#endif
}

// Programmatic configs (not from the parser) are normalised too.
TEST(validate_normalises_out_of_range_fields) {
    Config c;
    c.file.path.clear();
    c.pattern.clear();
    c.file.max_files = 0;
    c.file.max_size = 0;
    c.flush_interval_ms = 0;
    c.sync_interval_ms = 5000000;
    std::vector<std::string> w;
    EXPECT_FALSE(validate_config(c, &w));
    EXPECT_EQ(w.size(), size_t(6));
    EXPECT_EQ(c.file.path, FileOptions().path);
    EXPECT_EQ(c.pattern, Config().pattern);
    EXPECT_EQ(c.file.max_files, 1);
    EXPECT_EQ(c.file.max_size, uint64_t(1));
    EXPECT_EQ(c.flush_interval_ms, kFlushIntervalMinMs);
    EXPECT_EQ(c.sync_interval_ms, kSyncIntervalMaxMs);
    EXPECT_TRUE(any_contains(w, "file.path"));
    EXPECT_TRUE(any_contains(w, "pattern"));
    EXPECT_TRUE(any_contains(w, "file.max_files"));
    EXPECT_TRUE(any_contains(w, "file.max_size"));
    EXPECT_TRUE(any_contains(w, "flush.interval_ms"));
    EXPECT_TRUE(any_contains(w, "sync.interval_ms"));

    c.flush_interval_ms = 100000;
    c.file.max_files = -3;
    w.clear();
    EXPECT_FALSE(validate_config(c, &w));
    EXPECT_EQ(w.size(), size_t(2));
    EXPECT_EQ(c.flush_interval_ms, kFlushIntervalMaxMs);
    EXPECT_EQ(c.file.max_files, 1);

    // sync.interval_ms = 0 means "no periodic sync": valid.
    c.sync_interval_ms = 0;
    w.clear();
    EXPECT_TRUE(validate_config(c, &w));
    EXPECT_EQ(c.sync_interval_ms, 0u);
}

TEST(file_key) {
    Config c;
    EXPECT_TRUE(c.file_enabled);  // default: log file on
    std::vector<std::string> w;
    EXPECT_TRUE(parse_config_string("file = false\n", c, &w));
    EXPECT_FALSE(c.file_enabled);
    EXPECT_TRUE(parse_config_string("file = on\n", c, &w));
    EXPECT_TRUE(c.file_enabled);
    EXPECT_FALSE(parse_config_string("file = maybe\n", c, &w));  // keeps previous
    EXPECT_TRUE(c.file_enabled);
    EXPECT_TRUE(any_contains(w, "invalid boolean"));
}

TEST(validate_file_false_needs_another_sink) {
    std::vector<std::string> w;
    Config c;
    c.file_enabled = false;  // and no console / syslog / user sink
    EXPECT_FALSE(validate_config(c, &w));
    EXPECT_TRUE(c.console);
    EXPECT_EQ(w.size(), size_t(1));
    EXPECT_TRUE(any_contains(w, "enabling console"));

    for (int k = 0; k < 3; ++k) {  // console, syslog or a user sink: fine
        Config d;
        d.file_enabled = false;
        if (k == 0) d.console = true;
        if (k == 1) d.syslog = true;
        w.clear();
        EXPECT_TRUE(validate_config(d, &w, k == 2 ? 1 : 0));
        EXPECT_EQ(w.size(), size_t(0));
        EXPECT_EQ(d.console, k == 0);
    }
}

TINY_TEST_MAIN()
