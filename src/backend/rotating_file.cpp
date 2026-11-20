#include "backend/rotating_file.h"

#include "core/compat.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace tblog {

namespace {

// handle() is read by the crash handler.
TBLOG_ASSERT_LOCK_FREE(pf::FileHandle);

// Portable memrchr (a GNU extension that some uClibc configs lack).
const char *last_newline(const char *p, size_t n) noexcept {
    while (n > 0) {
        if (p[--n] == '\n') return p + n;
    }
    return nullptr;
}

}  // namespace

RotatingFile::~RotatingFile() { close(); }

bool RotatingFile::open(const FileOptions &opt) {
    close();
    opt_ = opt;
    if (opt_.max_files < 1) opt_.max_files = 1;
    if (opt_.max_size == 0) opt_.max_size = 1;
    if (!pf::make_parent_dirs(opt_.path)) {
        std::fprintf(stderr, "tblog: cannot create directory for %s: %s\n",
                     opt_.path.c_str(), std::strerror(errno));
        return false;
    }
    arch_.configure(opt_);
    arch_.recover();
    if (!open_fd()) {
        std::fprintf(stderr, "tblog: cannot open %s: %s\n", opt_.path.c_str(),
                     std::strerror(errno));
        return false;
    }
    last_check_ = std::chrono::steady_clock::now();
    const int64_t now = arch_.now();
    start_period(now);
    if (arch_.time_mode() && size_ > 0) {
        // An existing file belongs to the period of its last write.
        pf::FileInfo fi;
        pf::FileHandle h = fd_.load(std::memory_order_relaxed);
        if (h.valid() && pf::file_info(h, &fi)) {
            int64_t m = fi.mtime_ns / 1000000000LL;  // whole seconds (st_mtime)
            if (arch_.period_start(m) != arch_.period_start(now)) {
                period_ = m;
                rotate();  // archives it as app.<its period>.log
            }
        }
    }
    arch_.start();
    return true;
}

void RotatingFile::start_period(int64_t now) noexcept {
    period_ = now;
    next_boundary_ = arch_.next_boundary(now);
}

void RotatingFile::check_time() noexcept {
    if (!arch_.time_mode()) return;
    const int64_t now = arch_.now();
    if (now < next_boundary_) return;
    if (size_ > 0 && fd_.load(std::memory_order_relaxed).valid())
        rotate();  // archives under the old period, then starts the new one
    else
        start_period(now);
}

bool RotatingFile::open_fd() noexcept {
    pf::FileHandle h = pf::file_open(opt_.path, pf::OpenMode::Append);
    if (!h.valid()) return false;
    pf::FileInfo fi;
    size_ = pf::file_info(h, &fi) ? fi.size : 0;
    pf::file_close(fd_.exchange(h, std::memory_order_acq_rel));
    return true;
}

void RotatingFile::close() noexcept {
    pf::file_close(fd_.exchange(pf::no_file(), std::memory_order_acq_rel));
    arch_.finish();  // let queued compressions complete (bounded)
}

std::string RotatingFile::rotated_name(int index) const {
    if (index <= 0) return opt_.path;
    const std::string &p = opt_.path;
    size_t slash = pf::last_path_separator(p);
    size_t base = (slash == std::string::npos) ? 0 : slash + 1;
    size_t dot = p.rfind('.');
    // No extension (or a leading-dot hidden file): append the index.
    if (dot == std::string::npos || dot <= base) return p + "." + std::to_string(index);
    return p.substr(0, dot) + "." + std::to_string(index) + p.substr(dot);
}

void RotatingFile::rotate() noexcept {
    uint64_t handle = 0;
    if (opt_.max_files <= 1) {
        // No history kept: start the file over.
        pf::FileHandle h = fd_.load(std::memory_order_acquire);
        if (h.valid() && pf::file_truncate(h, 0)) {
            size_ = 0;
            if (arch_.time_mode()) start_period(arch_.now());
            return;
        }
        pf::remove_file(opt_.path);
    } else {
        // Shifts app.1.log... (size) or picks app.<period>[.N].log (time);
        // allocation failure -> no rename, keep appending.
        handle = arch_.archive(opt_.path, period_, false);
    }
    if (!open_fd()) {
        const int err = errno;
        pf::file_close(fd_.exchange(pf::no_file(), std::memory_order_acq_rel));
        report_loss(0, err);
    }
    // The old fd is closed: nothing writes to the rotated file any more.
    arch_.mark_ready(handle);
    if (arch_.time_mode()) start_period(arch_.now());
    last_check_ = std::chrono::steady_clock::now();
}

void RotatingFile::datasync() noexcept {
    pf::FileHandle h = fd_.load(std::memory_order_acquire);
    if (h.valid()) pf::file_datasync(h);
}

void RotatingFile::check_reopen() noexcept {
    auto now = std::chrono::steady_clock::now();
    pf::FileHandle h = fd_.load(std::memory_order_acquire);
    if (now - last_check_ < std::chrono::seconds(1)) return;
    last_check_ = now;
    pf::FileInfo by_path, by_fd;
    bool same = h.valid() && pf::file_info(h, &by_fd) && pf::path_info(opt_.path, &by_path) &&
                by_fd.id == by_path.id;
    if (same) return;
    // Deleted / renamed externally (logrotate) or previously failed: reopen.
    pf::make_parent_dirs(opt_.path);
    // On failure open_fd() keeps the old fd (if any): better an unlinked file
    // than lost data. Retried at the next check.
    open_fd();
}

void RotatingFile::report_loss(size_t n, int err) noexcept {
    lost_.fetch_add(n, std::memory_order_relaxed);
    lost_since_warn_ += n;
    auto now = std::chrono::steady_clock::now();
    if (warned_ && now - last_warn_ < std::chrono::seconds(10)) return;
    warned_ = true;
    last_warn_ = now;
    char msg[256];
    int m = std::snprintf(msg, sizeof msg,
                          "tblog: write to %s failed (%s); %llu bytes lost\n",
                          opt_.path.c_str(), std::strerror(err),
                          static_cast<unsigned long long>(lost_since_warn_));
    lost_since_warn_ = 0;
    if (m > 0) pf::stderr_write(msg, static_cast<size_t>(m) < sizeof msg ? m : sizeof msg - 1);
}

void RotatingFile::write_chunk(const char *data, size_t len) noexcept {
    pf::FileHandle h = fd_.load(std::memory_order_acquire);
    if (!h.valid()) { report_loss(len, EBADF); return; }
    size_t done = 0;
    const bool ok = pf::file_write_all(h, data, len, &done);
    const int err = errno;
    size_ += static_cast<uint64_t>(done);
    if (!ok) report_loss(len - done, err);
}

void RotatingFile::write_all(const char *data, size_t len) noexcept {
    check_reopen();
    while (len > 0) {
        uint64_t room = size_ < opt_.max_size ? opt_.max_size - size_ : 0;
        if (len <= room) { write_chunk(data, len); return; }
        // Fill the current file up to the last complete line that fits.
        const char *nl = room ? last_newline(data, static_cast<size_t>(room)) : nullptr;
        if (nl) {
            size_t k = static_cast<size_t>(nl - data) + 1;
            write_chunk(data, k);
            data += k;
            len -= k;
            rotate();
        } else if (size_ > 0) {
            rotate();  // next line doesn't fit: start a fresh file
        } else {
            // A single line longer than max_size into an empty file: write it whole.
            const void *e = ::memchr(data, '\n', len);
            size_t k = e ? static_cast<size_t>(static_cast<const char *>(e) - data) + 1 : len;
            write_chunk(data, k);
            data += k;
            len -= k;
            if (len) rotate();
        }
    }
}

}  // namespace tblog
