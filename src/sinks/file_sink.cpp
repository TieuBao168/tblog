// FileSink: see file_sink.h.
#include "sinks/file_sink.h"

#include "sinks/rotating_file.h"

#include <mutex>

namespace tblog {
namespace {

class FileSink final : public LogSink {
public:
    bool open(const FileOptions &opt) { return file_.open(opt); }

    void begin_batch() override {
        std::lock_guard<std::mutex> lk(mu_);
        file_.check_time();
    }

    void write(const char *data, size_t len) override {
        std::lock_guard<std::mutex> lk(mu_);
        file_.write_all(data, len);
    }

    // Don't hold mu_ during fdatasync (it can take ~100 ms): dup the handle
    // so a concurrent rotation cannot close it under us.
    void sync() override {
        pf::FileHandle d = pf::no_file();
        {
            std::lock_guard<std::mutex> lk(mu_);
            pf::FileHandle h = file_.handle();
            if (h.valid()) d = pf::file_dup(h);
        }
        if (d.valid()) {
            pf::file_datasync(d);
            pf::file_close(d);
        }
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(mu_);
        file_.datasync();
        file_.close();
    }

    bool crash_safe() const noexcept override { return true; }

    void crash_write(const char *data, size_t len) noexcept override {
        const pf::FileHandle h = file_.handle();  // lock-free atomic load
        if (h.valid()) pf::file_write_crash(h, data, len);
    }

private:
    std::mutex mu_;  // write path vs sync() (syncer thread)
    RotatingFile file_;
};

}  // namespace

std::unique_ptr<LogSink> make_file_sink(const FileOptions &opt) {
    std::unique_ptr<FileSink> s(new FileSink);
    if (!s->open(opt)) return nullptr;
    return std::unique_ptr<LogSink>(s.release());
}

}  // namespace tblog
