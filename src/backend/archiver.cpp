// Rotated-file set: naming, retention, crash-safe background gzip.
// See archiver.h for the protocol.
#include "backend/archiver.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef TBLOG_HAVE_ZLIB
#include <zlib.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace tblog {

namespace {

bool exists(const std::string &p) {
    struct stat st;
    return ::lstat(p.c_str(), &st) == 0;
}

bool ends_with(const std::string &s, const std::string &suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool all_digits(const std::string &s) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// "YYYY-MM-DD" or "YYYY-MM-DDTHH".
bool is_label(const std::string &s) {
    if (s.size() != 10 && s.size() != 13) return false;
    static const char kMask[] = "dddd-dd-ddTdd";
    for (size_t i = 0; i < s.size(); ++i) {
        if (kMask[i] == 'd') {
            if (s[i] < '0' || s[i] > '9') return false;
        } else if (s[i] != kMask[i]) {
            return false;
        }
    }
    return true;
}

std::string pad(unsigned long v, int width) {
    char b[32];
    std::snprintf(b, sizeof b, "%0*lu", width, v);
    return b;
}

void fsync_dir(const std::string &dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

// Keep fds away from 0..2 so dup2 onto stdin/stdout in a child is never a no-op.
int high_fd(int fd) {
    if (fd < 0 || fd > 2) return fd;
    int d = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    ::close(fd);
    return d;
}

}  // namespace

Archiver::~Archiver() { finish(std::chrono::milliseconds(0)); }

void Archiver::configure(const FileOptions &opt) {
    path_ = opt.path;
    max_files_ = opt.max_files < 1 ? 1 : opt.max_files;
    rotate_ = opt.rotate;
    compress_ = opt.compress;
    now_fn_ = opt.now_fn;
    size_t slash = path_.rfind('/');
    size_t base = slash == std::string::npos ? 0 : slash + 1;
    dir_ = slash == std::string::npos ? "." : (slash == 0 ? "/" : path_.substr(0, slash));
    size_t dot = path_.rfind('.');
    // No extension (or a leading-dot hidden file): suffixes go at the end.
    if (dot == std::string::npos || dot <= base) {
        stem_ = path_;
        ext_.clear();
    } else {
        stem_ = path_.substr(0, dot);
        ext_ = path_.substr(dot);
    }
    base_prefix_ = stem_.substr(base) + ".";
    std::lock_guard<std::mutex> lk(mu_);
    stopping_ = false;
    abort_.store(false);
}

// ------------------------------------------------------------------ clock
int64_t Archiver::now() const noexcept {
    if (now_fn_) return now_fn_();
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec);
}

int64_t Archiver::period_start(int64_t t) const noexcept {
    time_t tt = static_cast<time_t>(t);
    struct tm lt;
    if (!::localtime_r(&tt, &lt)) return t;
    const int64_t into_hour = lt.tm_min * 60 + lt.tm_sec;
    if (rotate_ == RotateMode::Hourly) return t - into_hour;
    struct tm d;
    std::memset(&d, 0, sizeof d);
    d.tm_year = lt.tm_year;
    d.tm_mon = lt.tm_mon;
    d.tm_mday = lt.tm_mday;
    d.tm_isdst = -1;
    time_t s = ::mktime(&d);
    if (s == static_cast<time_t>(-1) || s > tt) return t - lt.tm_hour * 3600 - into_hour;
    return static_cast<int64_t>(s);
}

int64_t Archiver::next_boundary(int64_t t) const noexcept {
    if (!time_mode()) return INT64_MAX;
    const int64_t ps = period_start(t);
    int64_t nb;
    if (rotate_ == RotateMode::Hourly) {
        nb = ps + 3600;
    } else {
        time_t tt = static_cast<time_t>(ps);
        struct tm lt;
        nb = ps + 86400;
        if (::localtime_r(&tt, &lt)) {
            struct tm d;
            std::memset(&d, 0, sizeof d);
            d.tm_year = lt.tm_year;
            d.tm_mon = lt.tm_mon;
            d.tm_mday = lt.tm_mday + 1;  // mktime normalises month/year
            d.tm_isdst = -1;
            time_t s = ::mktime(&d);
            if (s != static_cast<time_t>(-1)) nb = static_cast<int64_t>(s);
        }
    }
    return nb > t ? nb : t + 1;  // never a boundary in the past (DST oddities)
}

std::string Archiver::period_label(int64_t t) const {
    time_t tt = static_cast<time_t>(t);
    struct tm lt;
    char buf[32];
    if (!::localtime_r(&tt, &lt)) return "0000-00-00";
    std::strftime(buf, sizeof buf, rotate_ == RotateMode::Hourly ? "%Y-%m-%dT%H" : "%Y-%m-%d",
                  &lt);
    return buf;
}

// ------------------------------------------------------------------ names
std::string Archiver::size_name(int index) const {
    if (index <= 0) return path_;
    return stem_ + "." + std::to_string(index) + ext_;
}

std::string Archiver::time_name(const std::string &label, int index) const {
    std::string s = stem_ + "." + label;
    if (index > 0) s += "." + std::to_string(index);
    return s + ext_;
}

std::vector<Archiver::Entry> Archiver::scan() const {
    std::vector<Entry> out;
    DIR *d = ::opendir(dir_.c_str());
    if (!d) return out;
    const std::string prefix = dir_ == "/" ? "/" : dir_ + "/";
    while (dirent *de = ::readdir(d)) {
        std::string n = de->d_name;
        int kind = 0;  // 0 plain, 1 gz, 2 tmp
        if (ends_with(n, ".gz.tmp")) {
            kind = 2;
            n.resize(n.size() - 7);
        } else if (ends_with(n, ".gz")) {
            kind = 1;
            n.resize(n.size() - 3);
        }
        if (n.size() <= base_prefix_.size() + ext_.size()) continue;
        if (n.compare(0, base_prefix_.size(), base_prefix_) != 0 || !ends_with(n, ext_)) continue;
        std::string mid =
            n.substr(base_prefix_.size(), n.size() - base_prefix_.size() - ext_.size());
        Entry e;
        if (all_digits(mid)) {
            unsigned long idx = std::strtoul(mid.c_str(), nullptr, 10);
            if (idx == 0 || std::to_string(idx) != mid) continue;
            e.key = pad(idx, 10);
        } else {
            size_t dot = mid.find('.');
            std::string label = mid.substr(0, dot);
            unsigned long idx = 0;
            if (!is_label(label)) continue;
            if (dot != std::string::npos) {
                std::string is = mid.substr(dot + 1);
                if (!all_digits(is)) continue;
                idx = std::strtoul(is.c_str(), nullptr, 10);
                if (idx == 0 || std::to_string(idx) != is) continue;
            }
            label.resize(13, ' ');  // daily sorts before hourly of the same day
            e.key = label + pad(idx, 10);
            e.is_time = true;
        }
        e.name = prefix + n;
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const Entry &x) { return x.name == e.name; });
        Entry &dst = it == out.end() ? (out.push_back(e), out.back()) : *it;
        (kind == 0 ? dst.plain : kind == 1 ? dst.gz : dst.tmp) = true;
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(),
              [](const Entry &a, const Entry &b) { return a.key < b.key; });
    return out;
}

// ------------------------------------------------------------------ startup
void Archiver::recover() noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        for (const Entry &e : scan()) {
            // Interrupted compression: the original is intact, drop the partial.
            if (e.tmp) ::unlink((e.name + ".gz.tmp").c_str());
            // Crash between rename(.gz) and unlink(original): .gz is complete.
            if (e.plain && e.gz) ::unlink(e.name.c_str());
        }
    } catch (...) {
    }
}

std::string Archiver::newest_plain_archive() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Entry> es = scan();
    const Entry *best = nullptr;
    for (const Entry &e : es) {
        if (e.is_time != time_mode()) continue;
        // Size mode: index 1 is the newest; time mode: the largest key.
        if (!best || (time_mode() ? e.key > best->key : e.key < best->key)) best = &e;
    }
    return best && best->plain && !best->gz ? best->name : std::string();
}

void Archiver::start() noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        if (time_mode()) retention_locked();
        if (!compress()) return;
        for (const Entry &e : scan()) {
            if (!e.plain || e.gz) continue;
            bool queued = false;
            for (const Job &j : jobs_) queued = queued || j.src == e.name;
            if (!queued) add_job_locked(next_id_++, e.name, true);
        }
    } catch (...) {
    }
}

// ------------------------------------------------------------------ rotation
void Archiver::cancel_locked(const std::string &src) {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->src != src) {
            ++it;
        } else if (it->active) {
            it->cancelled = true;  // compressor drops its result
            ++it;
        } else {
            it = jobs_.erase(it);
        }
    }
    idle_cv_.notify_all();
}

void Archiver::retention_locked() {
    std::vector<Entry> es = scan();
    std::vector<const Entry *> mine;
    for (const Entry &e : es)
        if (e.is_time && (e.plain || e.gz)) mine.push_back(&e);  // sorted oldest first
    const size_t keep = static_cast<size_t>(max_files_ - 1);
    for (size_t i = 0; i + keep < mine.size(); ++i) {
        const std::string &n = mine[i]->name;
        ::unlink(n.c_str());
        ::unlink((n + ".gz").c_str());
        ::unlink((n + ".gz.tmp").c_str());
        cancel_locked(n);
    }
}

uint64_t Archiver::archive(const std::string &live, int64_t period, bool ready) noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        if (max_files_ <= 1) {
            ::unlink(live.c_str());
            return 0;
        }
        std::string dst;
        if (!time_mode()) {
            // app.(N-2) -> app.(N-1) ... app.1 -> app.2, carrying .gz / .gz.tmp
            // and queued jobs along; the oldest is dropped.
            const int top = max_files_ - 1;
            for (int i = top; i >= 1; --i) {
                const std::string p = size_name(i);
                if (i == top) {
                    ::unlink(p.c_str());
                    ::unlink((p + ".gz").c_str());
                    ::unlink((p + ".gz.tmp").c_str());
                    cancel_locked(p);
                    continue;
                }
                const std::string q = size_name(i + 1);
                ::rename(p.c_str(), q.c_str());
                if (compress() || exists(p + ".gz")) {
                    ::rename((p + ".gz").c_str(), (q + ".gz").c_str());
                    ::rename((p + ".gz.tmp").c_str(), (q + ".gz.tmp").c_str());
                }
                for (Job &j : jobs_)
                    if (j.src == p) j.src = q;
            }
            dst = size_name(1);
        } else {
            const std::string label = period_label(period);
            for (int idx = 0;; ++idx) {
                dst = time_name(label, idx);
                if (!exists(dst) && !exists(dst + ".gz") && !exists(dst + ".gz.tmp")) break;
            }
        }
        if (::rename(live.c_str(), dst.c_str()) != 0) {
            std::fprintf(stderr, "tblog: rotate %s -> %s: %s\n", live.c_str(), dst.c_str(),
                         std::strerror(errno));
            return 0;
        }
        uint64_t id = 0;
        if (compress()) {
            id = next_id_++;
            add_job_locked(id, dst, ready);
        }
        if (time_mode()) retention_locked();
        return id;
    } catch (...) {
        return 0;
    }
}

void Archiver::mark_ready(uint64_t handle) noexcept {
    if (handle == 0) return;
    try {
        std::lock_guard<std::mutex> lk(mu_);
        for (Job &j : jobs_)
            if (j.id == handle) {
                j.ready = true;
                ensure_thread_locked();
                cv_.notify_one();
            }
    } catch (...) {
    }
}

void Archiver::add_job_locked(uint64_t id, const std::string &src, bool ready) {
    Job j;
    j.id = id;
    j.src = src;
    j.ready = ready;
    jobs_.push_back(j);
    if (ready) {
        ensure_thread_locked();
        cv_.notify_one();
    }
}

void Archiver::ensure_thread_locked() {
    if (thread_started_ || stopping_) return;
    try {
        thread_ = std::thread(&Archiver::compressor_main, this);
        thread_started_ = true;
    } catch (...) {
        std::fprintf(stderr, "tblog: cannot start compression thread\n");
    }
}

void Archiver::finish(std::chrono::milliseconds budget) noexcept {
    try {
        std::unique_lock<std::mutex> lk(mu_);
        stopping_ = true;
        cv_.notify_all();
        if (thread_started_)
            idle_cv_.wait_for(lk, budget, [&] {
                for (const Job &j : jobs_)
                    if (j.active || (j.ready && !j.cancelled)) return false;
                return true;
            });
        abort_.store(true);
        cv_.notify_all();
    } catch (...) {
        abort_.store(true);
    }
    if (thread_.joinable()) thread_.join();
    try {
        std::lock_guard<std::mutex> lk(mu_);
        thread_started_ = false;
        jobs_.clear();  // leftovers stay uncompressed; the next start() queues them
    } catch (...) {
    }
}

// ------------------------------------------------------------------ compressor
void Archiver::compressor_main() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        std::list<Job>::iterator j = jobs_.end();
        cv_.wait(lk, [&] {
            if (abort_.load()) return true;
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
                if (it->ready && !it->cancelled && !it->active) {
                    j = it;
                    return true;
                }
            return stopping_;
        });
        if (abort_.load() || j == jobs_.end()) break;
        j->active = true;
        const std::string tmp = j->src + ".gz.tmp";
        int in = high_fd(::open(j->src.c_str(), O_RDONLY | O_CLOEXEC));
        int out = in < 0 ? -1
                         : high_fd(::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
                                          0644));
        if (in < 0 || out < 0) {
            if (!j->cancelled)
                std::fprintf(stderr, "tblog: compress %s: %s\n", j->src.c_str(),
                             std::strerror(errno));
            if (in >= 0) ::close(in);
            jobs_.erase(j);
            idle_cv_.notify_all();
            continue;
        }
        lk.unlock();
        bool ok = compress_fds(in, out);
        ok = ok && ::fsync(out) == 0;
        ::close(in);
        ::close(out);
        lk.lock();
        // j stays valid: cancel_locked() only flags active jobs.
        if (j->cancelled) {  // deleted by retention / shifted out meanwhile
            jobs_.erase(j);
            idle_cv_.notify_all();
            continue;
        }
        const std::string cur_tmp = j->src + ".gz.tmp";
        if (!ok || ::rename(cur_tmp.c_str(), (j->src + ".gz").c_str()) != 0) {
            if (!abort_.load())
                std::fprintf(stderr, "tblog: compression of %s failed; kept uncompressed\n",
                             j->src.c_str());
            ::unlink(cur_tmp.c_str());
            jobs_.erase(j);
            idle_cv_.notify_all();
            continue;
        }
        // The .gz must be durable before the original disappears.
        lk.unlock();
        fsync_dir(dir_);
        lk.lock();
        if (!j->cancelled) ::unlink(j->src.c_str());
        jobs_.erase(j);
        idle_cv_.notify_all();
    }
}

#ifdef TBLOG_HAVE_ZLIB
bool Archiver::compress_fds(int in, int out) noexcept {
    int d = ::dup(out);
    if (d < 0) return false;
    gzFile gz = ::gzdopen(d, "wb6");
    if (!gz) {
        ::close(d);
        return false;
    }
    char buf[64 * 1024];
    bool ok = true;
    for (;;) {
        if (abort_.load(std::memory_order_relaxed)) { ok = false; break; }
        ssize_t n = ::read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (::gzwrite(gz, buf, static_cast<unsigned>(n)) != static_cast<int>(n)) { ok = false; break; }
    }
    return ::gzclose(gz) == Z_OK && ok;  // gzclose closes d
}
#else
// No zlib at build time: `gzip -n -c < in > out` via posix_spawn. The result is
// checked against the gzip trailer (ISIZE = input length mod 2^32), which also
// covers an application that set SIGCHLD to SIG_IGN (waitpid -> ECHILD).
bool Archiver::compress_fds(int in, int out) noexcept {
    posix_spawn_file_actions_t fa;
    if (::posix_spawn_file_actions_init(&fa) != 0) return false;
    ::posix_spawn_file_actions_adddup2(&fa, in, 0);
    ::posix_spawn_file_actions_adddup2(&fa, out, 1);
    static char a0[] = "gzip", a1[] = "-n", a2[] = "-c";
    char *argv[] = {a0, a1, a2, nullptr};
    pid_t pid;
    int e = ::posix_spawnp(&pid, "gzip", &fa, nullptr, argv, environ);
    ::posix_spawn_file_actions_destroy(&fa);
    if (e != 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            std::fprintf(stderr, "tblog: cannot run gzip: %s\n", std::strerror(e));
        return false;
    }
    int st = 0;
    bool status_known = true;
    for (;;) {
        pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { status_known = false; break; }  // ECHILD: SIGCHLD ignored
        if (abort_.load(std::memory_order_relaxed)) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        struct timespec ts = {0, 2 * 1000 * 1000};
        ::nanosleep(&ts, nullptr);
    }
    if (status_known && !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) return false;
    struct stat si, so;
    if (::fstat(in, &si) != 0 || ::fstat(out, &so) != 0 || so.st_size < 18) return false;
    unsigned char t[4];
    if (::pread(out, t, 4, so.st_size - 4) != 4) return false;
    uint32_t isize = uint32_t(t[0]) | uint32_t(t[1]) << 8 | uint32_t(t[2]) << 16 |
                     uint32_t(t[3]) << 24;
    return isize == static_cast<uint32_t>(si.st_size);
}
#endif

}  // namespace tblog
