// Pipeline: see pipeline.h.
#include "logger/pipeline.h"

#include <cstdio>
#include <utility>

namespace tblog {

Pipeline::Pipeline(std::unique_ptr<Queue> q) : queue_(std::move(q)) {}

Pipeline::~Pipeline() { shutdown(); }

void Pipeline::add_sink(std::unique_ptr<LogSink> s) {
    if (!s) return;
    LogSink *p = s.get();
    owned_.push_back(std::move(s));
    sinks_.push_back(p);
    if (p->crash_safe()) {
        crash_sinks_.push_back(p);
        if (p->writes_stderr()) crash_stderr_ = true;
    } else {
        other_sinks_.push_back(p);
    }
}

void Pipeline::drain(bool force_flush) {
    std::lock_guard<std::mutex> lk(mu_);
    if (shut_) return;
    Queue &q = *queue_;
    // After a crash, never take again: a new swap would hand a buffer the
    // crash handler may still be reading back to the producers.
    if (q.crashed()) return;
    Queue::Batch b;
    q.take(b);

    bool wrote = false;
    // The crash-safe sinks are written only between q.begin_write() and
    // q.end_write(): a crash handler waits for the write in flight and then
    // drains exactly what is left; once the queue crashed begin_write() fails
    // and nothing more is written here (no duplicates).
    if (q.begin_write()) {
        // Time rotation (FileSink) is decided here, AFTER the take: every
        // line taken above was appended before this clock read, so it never
        // lands in a file of an earlier period than the one in effect when
        // it was appended.
        for (LogSink *s : sinks_) s->begin_batch();
        q.end_write();
        size_t i = 0;
        for (; i < b.size; ++i) {
            if (!q.begin_write()) break;  // crashed: the handler drains the rest
            const Queue::Chunk &c = b.chunks[i];
            for (LogSink *s : crash_sinks_) s->write(c.data, c.len);
            q.mark_written(b, i);
            q.end_write();
            for (LogSink *s : other_sinks_) s->write(c.data, c.len);
            wrote = true;
        }
        // flush.on_level: the whole batch is in every sink (write(), before
        // the flush() / fsync); release its waiting callers. Cut short by a
        // crash: they return on crashed() instead.
        if (i == b.size) q.publish_written(b);
        const uint64_t d = q.dropped();
        if (d != reported_dropped_ && q.begin_write()) {
            char line[64];
            int m = std::snprintf(line, sizeof line, "tblog: %llu lines dropped\n",
                                  static_cast<unsigned long long>(d - reported_dropped_));
            reported_dropped_ = d;
            if (m > 0)
                for (LogSink *s : crash_sinks_) s->write(line, static_cast<size_t>(m));
            q.end_write();
            if (m > 0) {
                for (LogSink *s : other_sinks_) s->write(line, static_cast<size_t>(m));
                wrote = true;
            }
        }
        for (LogSink *s : sinks_) s->end_batch();
    }
    q.release(b);
    if (wrote || force_flush)
        for (LogSink *s : sinks_) s->flush();
}

void Pipeline::sync() {
    for (LogSink *s : sinks_) s->sync();
}

void Pipeline::shutdown() {
    queue_->close();  // appends that got in before this are taken below
    drain(true);
    std::lock_guard<std::mutex> lk(mu_);
    if (shut_) return;
    shut_ = true;
    for (LogSink *s : sinks_) s->shutdown();
}

// ------------------------------------------------------------------ crash
void Pipeline::crash_visit(void *ctx, const char *data, size_t len) noexcept {
    const Pipeline *p = static_cast<const Pipeline *>(ctx);
    for (LogSink *s : p->crash_sinks_) s->crash_write(data, len);
}

void Pipeline::crash(const char *marker, size_t len, bool others_running) noexcept {
    // Drained lines, then the marker; later appends write synchronously
    // through crash_visit (after the marker). A second call: its marker only.
    queue_->crash_drain(&crash_visit, this, marker, len, others_running);
}

}  // namespace tblog
