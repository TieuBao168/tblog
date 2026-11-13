// tblog config file watcher: one stat() per changed() call.
#include "internal.h"

#include <sys/stat.h>

namespace tblog {
namespace {

void snapshot(const std::string &path, int64_t *mtime, int64_t *size, uint64_t *ino) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        *mtime = -1;
        *size = -1;
        *ino = 0;
        return;
    }
    *mtime = int64_t(st.st_mtim.tv_sec) * 1000000000LL + int64_t(st.st_mtim.tv_nsec);
    *size = int64_t(st.st_size);
    *ino = uint64_t(st.st_ino);
}

}  // namespace

ConfigWatcher::ConfigWatcher(std::string path) : path_(std::move(path)) {
    snapshot(path_, &mtime_ns_, &size_, &ino_);
}

bool ConfigWatcher::changed() {
    int64_t m, s;
    uint64_t i;
    snapshot(path_, &m, &s, &i);
    if (m == mtime_ns_ && s == size_ && i == ino_) return false;
    mtime_ns_ = m;
    size_ = s;
    ino_ = i;
    return true;
}

}  // namespace tblog
