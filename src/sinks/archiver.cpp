// Rotated-file set: naming, retention, crash-safe background gzip.
// See archiver.h for the protocol.
#include "sinks/archiver.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>

#ifdef TBLOG_HAVE_ZLIB
#include <zlib.h>
#endif

namespace tblog {

namespace {

bool exists(const std::string &p) { return pf::path_exists(p); }

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

}  // namespace

Archiver::~Archiver() { finish(std::chrono::milliseconds(0)); }

void Archiver::configure(const FileOptions &opt) {
    path_ = opt.path;
    max_files_ = opt.max_files < 1 ? 1 : opt.max_files;
    rotate_ = opt.rotate;
    compress_ = opt.compress;
    mode_ = opt.mode;
#ifndef TBLOG_HAVE_ZLIB
    compress_ = CompressMode::None;  // validate_config() already warned
#endif
    now_fn_ = opt.now_fn;
    size_t slash = pf::last_path_separator(path_);
    size_t base = slash == std::string::npos ? 0 : slash + 1;
    sep_ = slash == std::string::npos ? '/' : path_[slash];
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
    return static_cast<int64_t>(pf::realtime_ns() / 1000000000ull);
}

int64_t Archiver::period_start(int64_t t) const noexcept {
    time_t tt = static_cast<time_t>(t);
    std::tm lt;
    if (!pf::local_time(t, &lt)) return t;
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
        std::tm lt;
        nb = ps + 86400;
        if (pf::local_time(ps, &lt)) {
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
    std::tm lt;
    char buf[32];
    if (!pf::local_time(t, &lt)) return "0000-00-00";
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
    std::vector<std::string> names;
    if (!pf::list_dir(dir_, &names)) return out;
    const std::string prefix = dir_ == "/" ? "/" : dir_ + sep_;
    for (std::string &n : names) {
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
            if (e.tmp) pf::remove_file(e.name + ".gz.tmp");
            // Crash between rename(.gz) and unlink(original): .gz is complete.
            if (e.plain && e.gz) pf::remove_file(e.name);
        }
    } catch (...) {
    }
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
        pf::remove_file(n);
        pf::remove_file(n + ".gz");
        pf::remove_file(n + ".gz.tmp");
        cancel_locked(n);
    }
}

uint64_t Archiver::archive(const std::string &live, int64_t period, bool ready) noexcept {
    try {
        std::lock_guard<std::mutex> lk(mu_);
        if (max_files_ <= 1) {
            pf::remove_file(live);
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
                    pf::remove_file(p);
                    pf::remove_file(p + ".gz");
                    pf::remove_file(p + ".gz.tmp");
                    cancel_locked(p);
                    continue;
                }
                const std::string q = size_name(i + 1);
                pf::rename_replace(p, q);
                if (compress() || exists(p + ".gz")) {
                    pf::rename_replace(p + ".gz", q + ".gz");
                    pf::rename_replace(p + ".gz.tmp", q + ".gz.tmp");
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
        if (!pf::rename_replace(live, dst)) {
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
        // Neither file may be a symbolic link planted by someone who can
        // write the log directory: reading through one would copy its target
        // into the .gz, truncating through one would destroy its target. The
        // temporary file is created fresh (O_EXCL): a stale one is removed
        // first, and anything that reappears in between makes the open fail.
        pf::FileHandle in = pf::file_open(j->src, pf::OpenMode::ReadNoFollow);
        pf::FileHandle out = pf::no_file();
        if (in.valid()) {
            pf::remove_file(tmp);
            out = pf::file_open(tmp, pf::OpenMode::CreateNew, mode_);
        }
        if (!in.valid() || !out.valid()) {
            if (!j->cancelled)
                std::fprintf(stderr, "tblog: compress %s: %s\n", j->src.c_str(),
                             std::strerror(errno));
            pf::file_close(in);
            jobs_.erase(j);
            idle_cv_.notify_all();
            continue;
        }
        lk.unlock();
        bool ok = compress_file(in, out);
        ok = ok && pf::file_sync(out);
        pf::file_close(in);
        pf::file_close(out);
        lk.lock();
        // j stays valid: cancel_locked() only flags active jobs.
        if (j->cancelled) {  // deleted by retention / shifted out meanwhile
            jobs_.erase(j);
            idle_cv_.notify_all();
            continue;
        }
        const std::string cur_tmp = j->src + ".gz.tmp";
        if (!ok || !pf::rename_replace(cur_tmp, j->src + ".gz")) {
            if (!abort_.load())
                std::fprintf(stderr, "tblog: compression of %s failed; kept uncompressed\n",
                             j->src.c_str());
            pf::remove_file(cur_tmp);
            jobs_.erase(j);
            idle_cv_.notify_all();
            continue;
        }
        // The .gz must be durable before the original disappears.
        lk.unlock();
        pf::sync_dir(dir_);
        lk.lock();
        if (!j->cancelled) pf::remove_file(j->src);
        jobs_.erase(j);
        idle_cv_.notify_all();
    }
}

#ifdef TBLOG_HAVE_ZLIB
// gzip stream (same format and level as gzdopen(fd, "wb6"): deflate with a
// gzip wrapper, level 6, default memLevel/strategy) written through pf::.
bool Archiver::compress_file(pf::FileHandle in, pf::FileHandle out) noexcept {
    constexpr size_t kChunk = 64 * 1024;
    std::unique_ptr<unsigned char[]> inbuf(new (std::nothrow) unsigned char[kChunk]);
    std::unique_ptr<unsigned char[]> outbuf(new (std::nothrow) unsigned char[kChunk]);
    if (!inbuf || !outbuf) return false;
    z_stream zs;
    std::memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, 6, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    bool ok = true;
    for (bool done = false; !done;) {
        if (abort_.load(std::memory_order_relaxed)) { ok = false; break; }
        const int64_t n = pf::file_read(in, inbuf.get(), kChunk);
        if (n < 0) { ok = false; break; }
        const int flush = n == 0 ? Z_FINISH : Z_NO_FLUSH;
        zs.next_in = inbuf.get();
        zs.avail_in = static_cast<uInt>(n);
        do {  // drain the input (and, at the end, the trailer)
            zs.next_out = outbuf.get();
            zs.avail_out = static_cast<uInt>(kChunk);
            const int r = deflate(&zs, flush);
            if (r == Z_STREAM_ERROR) { ok = false; break; }
            const size_t have = kChunk - zs.avail_out;
            if (have && !pf::file_write_all(out, outbuf.get(), have)) { ok = false; break; }
            if (r == Z_STREAM_END) { done = true; break; }
        } while (zs.avail_out == 0);
        if (!ok) break;
        if (n == 0 && !done) { ok = false; break; }  // Z_FINISH must end the stream
    }
    return deflateEnd(&zs) == Z_OK && ok;
}
#else
// Without zlib configure() turns compression off, so no job is ever queued.
bool Archiver::compress_file(pf::FileHandle, pf::FileHandle) noexcept { return false; }
#endif

}  // namespace tblog
