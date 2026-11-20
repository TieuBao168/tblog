// tblog config file watcher: one stat() per changed() call.
#include "config/config.h"

namespace tblog {
namespace {

void snapshot(const std::string &path, int64_t *mtime, int64_t *size, pf::FileId *id) {
    pf::FileInfo fi;
    if (!pf::path_info(path, &fi)) {
        *mtime = -1;
        *size = -1;
        *id = pf::FileId{0, 0};
        return;
    }
    *mtime = fi.mtime_ns;
    *size = int64_t(fi.size);
    *id = fi.id;
}

}  // namespace

ConfigWatcher::ConfigWatcher(std::string path) : path_(std::move(path)) {
    snapshot(path_, &mtime_ns_, &size_, &id_);
}

bool ConfigWatcher::changed() {
    int64_t m, s;
    pf::FileId i;
    snapshot(path_, &m, &s, &i);
    if (m == mtime_ns_ && s == size_ && i == id_) return false;
    mtime_ns_ = m;
    size_ = s;
    id_ = i;
    return true;
}

}  // namespace tblog
