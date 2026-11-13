#include "backend/rotating_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace tblog {

namespace {

// Portable memrchr (a GNU extension that some uClibc configs lack).
const char *last_newline(const char *p, size_t n) noexcept {
    while (n > 0) {
        if (p[--n] == '\n') return p + n;
    }
    return nullptr;
}

// mkdir -p for the directory part of `path`.
bool make_parent_dirs(const std::string &path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return true;
    std::string dir = path.substr(0, slash);
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            std::string part = dir.substr(0, i);
            if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    struct stat st;
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

RotatingFile::~RotatingFile() { close(); }

bool RotatingFile::open(const FileOptions &opt) {
    close();
    opt_ = opt;
    if (opt_.max_files < 1) opt_.max_files = 1;
    if (opt_.max_size == 0) opt_.max_size = 1;
    if (!make_parent_dirs(opt_.path)) {
        std::fprintf(stderr, "tblog: cannot create directory for %s: %s\n",
                     opt_.path.c_str(), std::strerror(errno));
        return false;
    }
    if (!open_fd()) {
        std::fprintf(stderr, "tblog: cannot open %s: %s\n", opt_.path.c_str(),
                     std::strerror(errno));
        return false;
    }
    last_check_ = std::chrono::steady_clock::now();
    return true;
}

bool RotatingFile::open_fd() noexcept {
    int fd;
    do {
        fd = ::open(opt_.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return false;
    struct stat st;
    size_ = (::fstat(fd, &st) == 0) ? static_cast<uint64_t>(st.st_size) : 0;
    int old = fd_.exchange(fd, std::memory_order_acq_rel);
    if (old >= 0) ::close(old);
    return true;
}

void RotatingFile::close() noexcept {
    int old = fd_.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) ::close(old);
}

std::string RotatingFile::rotated_name(int index) const {
    if (index <= 0) return opt_.path;
    const std::string &p = opt_.path;
    size_t slash = p.rfind('/');
    size_t base = (slash == std::string::npos) ? 0 : slash + 1;
    size_t dot = p.rfind('.');
    // No extension (or a leading-dot hidden file): append the index.
    if (dot == std::string::npos || dot <= base) return p + "." + std::to_string(index);
    return p.substr(0, dot) + "." + std::to_string(index) + p.substr(dot);
}

void RotatingFile::rotate() noexcept {
    try {
        if (opt_.max_files <= 1) {
            // No history kept: start the file over.
            int fd = fd_.load(std::memory_order_acquire);
            if (fd >= 0 && ::ftruncate(fd, 0) == 0) { size_ = 0; return; }
            ::unlink(opt_.path.c_str());
        } else {
            for (int i = opt_.max_files - 2; i >= 1; --i)
                ::rename(rotated_name(i).c_str(), rotated_name(i + 1).c_str());
            ::rename(opt_.path.c_str(), rotated_name(1).c_str());
        }
    } catch (...) {
        // std::string allocation failure: skip renaming, keep appending.
    }
    if (!open_fd()) {
        close();
        report_loss(0, errno);
    }
    last_check_ = std::chrono::steady_clock::now();
}

void RotatingFile::datasync() noexcept {
    int fd = fd_.load(std::memory_order_acquire);
    if (fd >= 0) ::fdatasync(fd);
}

bool RotatingFile::raw_write_all(int fd, const char *data, size_t len) noexcept {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void RotatingFile::check_reopen() noexcept {
    auto now = std::chrono::steady_clock::now();
    int fd = fd_.load(std::memory_order_acquire);
    if (now - last_check_ < std::chrono::seconds(1)) return;
    last_check_ = now;
    struct stat by_path, by_fd;
    bool same = fd >= 0 && ::fstat(fd, &by_fd) == 0 &&
                ::stat(opt_.path.c_str(), &by_path) == 0 &&
                by_fd.st_ino == by_path.st_ino && by_fd.st_dev == by_path.st_dev;
    if (same) return;
    // Deleted / renamed externally (logrotate) or previously failed: reopen.
    try {
        make_parent_dirs(opt_.path);
    } catch (...) {
    }
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
    if (m > 0) raw_write_all(2, msg, static_cast<size_t>(m) < sizeof msg ? m : sizeof msg - 1);
}

void RotatingFile::write_chunk(const char *data, size_t len) noexcept {
    int fd = fd_.load(std::memory_order_acquire);
    if (fd < 0) { report_loss(len, EBADF); return; }
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            report_loss(len, errno);
            return;
        }
        if (n == 0) { report_loss(len, EIO); return; }
        data += n;
        len -= static_cast<size_t>(n);
        size_ += static_cast<uint64_t>(n);
    }
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
