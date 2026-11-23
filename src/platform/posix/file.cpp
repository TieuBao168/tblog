// POSIX files, paths and directories (see platform/platform.h).
#include "platform/platform.h"

#include <cerrno>
#include <new>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace tblog {
namespace pf {

namespace {

int64_t mtime_ns_of(const struct stat &st) noexcept {
#if defined(__APPLE__)
    return int64_t(st.st_mtimespec.tv_sec) * 1000000000LL + int64_t(st.st_mtimespec.tv_nsec);
#else
    return int64_t(st.st_mtim.tv_sec) * 1000000000LL + int64_t(st.st_mtim.tv_nsec);
#endif
}

void fill_info(const struct stat &st, FileInfo *out) noexcept {
    out->size = static_cast<uint64_t>(st.st_size);
    out->mtime_ns = mtime_ns_of(st);
    out->id.dev = static_cast<uint64_t>(st.st_dev);
    out->id.ino = static_cast<uint64_t>(st.st_ino);
}

}  // namespace

// ------------------------------------------------------------------ files
FileHandle file_open(const std::string &path, OpenMode mode, unsigned perms) noexcept {
    // O_NOFOLLOW: a symbolic link as the last component fails with ELOOP
    // (see OpenMode in platform.h).
    int flags = O_CLOEXEC;
    switch (mode) {
        case OpenMode::Append: flags |= O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW; break;
        case OpenMode::Read: flags |= O_RDONLY; break;
        case OpenMode::ReadNoFollow: flags |= O_RDONLY | O_NOFOLLOW; break;
        case OpenMode::Truncate: flags |= O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW; break;
        case OpenMode::CreateNew: flags |= O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW; break;
    }
    int fd;
    do {
        fd = ::open(path.c_str(), flags, static_cast<mode_t>(perms & 07777));
    } while (fd < 0 && errno == EINTR);
    return FileHandle{fd};
}

void file_close(FileHandle h) noexcept {
    if (h.valid()) ::close(h.native);
}

FileHandle file_dup(FileHandle h) noexcept {
    if (!h.valid()) {
        errno = EBADF;
        return no_file();
    }
    return FileHandle{::fcntl(h.native, F_DUPFD_CLOEXEC, 0)};
}

bool file_write_all(FileHandle h, const void *data, size_t len, size_t *written) noexcept {
    const char *p = static_cast<const char *>(data);
    size_t done = 0;
    bool ok = true;
    while (done < len) {
        ssize_t n = ::write(h.native, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) {
            errno = EIO;
            ok = false;
            break;
        }
        done += static_cast<size_t>(n);
    }
    if (written) *written = done;
    return ok;
}

bool file_write_crash(FileHandle h, const void *data, size_t len) noexcept {
    const char *p = static_cast<const char *>(data);
    while (len > 0) {
        ssize_t n = ::write(h.native, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

int64_t file_read(FileHandle h, void *buf, size_t len) noexcept {
    for (;;) {
        ssize_t n = ::read(h.native, buf, len);
        if (n < 0 && errno == EINTR) continue;
        return static_cast<int64_t>(n);
    }
}

#if defined(__APPLE__)
// macOS: fsync() only moves the data to the drive, which may keep it in its
// volatile write cache; F_FULLFSYNC also flushes that cache (what fsync /
// fdatasync guarantee on Linux), at a cost of milliseconds per call. Some
// file systems (network, FUSE, FAT) refuse F_FULLFSYNC: fall back to fsync.
// There is no fdatasync, so file_sync and file_datasync are the same call.
static bool full_fsync(int fd) noexcept {
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
    return ::fsync(fd) == 0;
}
bool file_sync(FileHandle h) noexcept { return full_fsync(h.native); }
bool file_datasync(FileHandle h) noexcept { return full_fsync(h.native); }
#else
bool file_sync(FileHandle h) noexcept { return ::fsync(h.native) == 0; }
bool file_datasync(FileHandle h) noexcept { return ::fdatasync(h.native) == 0; }
#endif

bool file_truncate(FileHandle h, uint64_t size) noexcept {
    return ::ftruncate(h.native, static_cast<off_t>(size)) == 0;
}

bool file_info(FileHandle h, FileInfo *out) noexcept {
    struct stat st;
    if (::fstat(h.native, &st) != 0) return false;
    fill_info(st, out);
    return true;
}

bool path_info(const std::string &path, FileInfo *out) noexcept {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    fill_info(st, out);
    return true;
}

// ------------------------------------------------------------------ paths
size_t last_path_separator(const std::string &path) noexcept { return path.rfind('/'); }

bool path_exists(const std::string &path) noexcept {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

bool make_parent_dirs(const std::string &path) noexcept {
    try {
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
    } catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return false;
    }
}

bool rename_replace(const std::string &from, const std::string &to) noexcept {
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool remove_file(const std::string &path) noexcept { return ::unlink(path.c_str()) == 0; }

bool list_dir(const std::string &dir, std::vector<std::string> *names) {
    DIR *d = ::opendir(dir.c_str());
    if (!d) return false;
    try {
        while (dirent *de = ::readdir(d)) {
            const char *n = de->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) continue;
            names->push_back(n);
        }
    } catch (...) {
        ::closedir(d);
        throw;
    }
    ::closedir(d);
    return true;
}

void sync_dir(const std::string &dir) noexcept {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

}  // namespace pf
}  // namespace tblog
