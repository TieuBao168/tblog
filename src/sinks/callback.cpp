// CallbackSink: a user sink registered with tblog_add_sink() (see sink.h).
#include "sinks/sink.h"

namespace tblog {
namespace {

class CallbackSink final : public LogSink {
public:
    CallbackSink(SinkWriteFn write_fn, SinkFlushFn flush_fn, void *ctx)
        : write_(write_fn), flush_(flush_fn), ctx_(ctx) {}

    void write(const char *data, size_t len) override {
        if (len) write_(ctx_, data, len);
    }
    void flush() override {
        if (flush_) flush_(ctx_);
    }

private:
    SinkWriteFn write_;
    SinkFlushFn flush_;
    void *ctx_;
};

}  // namespace

std::unique_ptr<LogSink> make_callback_sink(SinkWriteFn write, SinkFlushFn flush, void *ctx) {
    if (!write) return nullptr;
    return std::unique_ptr<LogSink>(new CallbackSink(write, flush, ctx));
}

}  // namespace tblog
