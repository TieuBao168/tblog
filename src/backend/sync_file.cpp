// Baseline backend: every append() write()s directly under a mutex.
#include "backend/rotating_file.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace tblog {

namespace {

class SyncFileBackend final : public Backend {
public:
    explicit SyncFileBackend(const FileOptions &opt) : urgent_level_(opt.urgent_level) {}
    bool open(const FileOptions &opt) { return file_.open(opt); }
    ~SyncFileBackend() override { shutdown(); }

    bool append(const char *data, uint32_t len, tblog_level lv) noexcept override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (closed_) return false;
            file_.write_all(data, len);
            for (Sink *s : sinks_) s->write(data, len);
        }
        if (lv >= urgent_level_) {
            urgent_.store(true, std::memory_order_release);
            wake();
        }
        return true;
    }

    void wait_for_work(std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lk(wait_mu_);
        wait_cv_.wait_for(lk, timeout, [&] { return wake_req_; });
        wake_req_ = false;
    }

    void flush() override {
        std::lock_guard<std::mutex> lk(mu_);
        for (Sink *s : sinks_) s->flush();
    }

    void sync() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!closed_) file_.datasync();
    }

    bool consume_urgent() noexcept override {
        return urgent_.exchange(false, std::memory_order_acq_rel);
    }

    void wake() noexcept override {
        try {
            std::lock_guard<std::mutex> lk(wait_mu_);
            wake_req_ = true;
        } catch (...) {
        }
        wait_cv_.notify_all();
    }

    void shutdown() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (closed_) return;
            closed_ = true;
            for (Sink *s : sinks_) s->flush();
            file_.datasync();
            file_.close();
        }
        wake();
    }

    // Nothing is buffered in user space.
    void on_crash() noexcept override {}

    void crash_write(const char *data, size_t len) noexcept override {
        int fd = file_.fd();
        if (fd >= 0) RotatingFile::raw_write_all(fd, data, len);
    }

    void add_sink(Sink *s) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (s) sinks_.push_back(s);
    }

    uint64_t dropped() const noexcept override { return 0; }

private:
    const tblog_level urgent_level_;
    std::mutex mu_;
    RotatingFile file_;
    std::vector<Sink *> sinks_;
    bool closed_ = false;
    std::atomic<bool> urgent_{false};

    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    bool wake_req_ = false;
};

}  // namespace

std::unique_ptr<Backend> make_sync_file_backend(const FileOptions &opt) {
    std::unique_ptr<SyncFileBackend> b(new SyncFileBackend(opt));
    if (!b->open(opt)) return nullptr;
    return b;
}

}  // namespace tblog
