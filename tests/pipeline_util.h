// Test helper: a Pipeline set up the way tblog_init does it (Queue + FileSink),
// for the tests that drive the worker side by hand.
#pragma once

#include "logger/pipeline.h"
#include "queue/queue.h"
#include "sinks/file_sink.h"

#include <memory>
#include <utility>

namespace tblog {

struct PipeOptions {
    FileOptions file;
    size_t buffer_size = 4u << 20;
    FullPolicy policy = FullPolicy::Drop;
    tblog_level urgent_level = TBLOG_LV_ERROR;
};

// nullptr if the queue or the file cannot be set up.
inline std::unique_ptr<Pipeline> make_pipe(const PipeOptions &o) {
    QueueOptions qo;
    qo.buffer_size = o.buffer_size;
    qo.policy = o.policy;
    qo.urgent_level = o.urgent_level;
    std::unique_ptr<Queue> q = Queue::create(qo);
    if (!q) return nullptr;
    std::unique_ptr<Sink> f = make_file_sink(o.file);
    if (!f) return nullptr;
    std::unique_ptr<Pipeline> p(new Pipeline(std::move(q)));
    p->add_sink(std::move(f));
    return p;
}

}  // namespace tblog
