// C++ API tour + micro-benchmark: printf macro vs {} macro.
// Build: g++ -std=c++11 -O2 -Iinclude examples/cpp_api.cpp build/libtblog.a -pthread -o cpp_api
// Run:   ./cpp_api [log.conf] [iterations]
#include <tblog/tblog.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace app {
struct Endpoint {
    std::string host;
    int port;
};
// Allocation-free customization point (found by ADL): width/fill/align of
// "{:>20}" are applied to whatever this writes.
void tblog_format(tblog::Sink &s, const Endpoint &e) { s.format("{}:{}", e.host, e.port); }
}  // namespace app

namespace {
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

template <class Fn> double per_call(int n, Fn fn) {
    const uint64_t t0 = now_ns();
    for (int i = 0; i < n; ++i) fn(i);
    return double(now_ns() - t0) / n;
}
}  // namespace

int main(int argc, char **argv) {
    if (tblog_init(argc > 1 ? argv[1] : nullptr) != 0) {
        std::fprintf(stderr, "tblog_init failed\n");
        return 1;
    }
    const int iters = argc > 2 ? std::atoi(argv[2]) : 200000;

    // --- macros: call site captured, arguments untouched when filtered ---
    const app::Endpoint ep{"10.0.0.7", 8443};
    TBLOGF_INFO("user {} logged in from {}", 42, ep);
    TBLOGF_MWARN("network", "retry {}/{} after {:.1f}s", 2, 5, 0.25);
    TBLOGF_DEBUG("filtered at INFO: {} is never formatted", ep);
    TBLOGF_INFO("hex {:#x} bin {:#b} padded [{:>8}] [{:<8}] [{:^8}] {:+.3e}", 255, 5, "r", "l", "c", 12345.678);
    TBLOGF_INFO("positional {1} {0} {1}, escapes {{}}, pointer {2}", "a", "b", static_cast<const void *>(&ep));

    // --- object style ---
    tblog::Logger net("network");
    net.info("connected to {:>20}", ep);           // file/line of this line
    net.warn(TBLOG_HERE, "explicit location {}", 1);  // same, explicit
    if (net.enabled(TBLOG_LV_DEBUG)) net.debug("expensive dump {}", std::string(10, '*'));
    TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "not evaluated when filtered: {}", std::string(10, '*'));

    // --- threads ---
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([t] {
            for (int i = 0; i < 1000; ++i) TBLOGF_INFO("worker {} step {}", t, i);
        });
    for (auto &t : ts) t.join();
    tblog_flush();

    // --- benchmark (single thread, includes enqueue into the backend) ---
    const char *payload = "abcdefghijklmnop";
    const double c_ns = per_call(iters, [&](int i) {
        TBLOG_INFO("bench i=%d value=%.3f payload=%s", i, i * 0.5, payload);
    });
    const double f_ns = per_call(iters, [&](int i) {
        TBLOGF_INFO("bench i={} value={:.3f} payload={}", i, i * 0.5, payload);
    });
    const double fd_ns = per_call(iters, [&](int i) {
        TBLOGF_INFO("bench i={} value={} payload={}", i, i * 0.5, payload);
    });
    const double l_ns = per_call(iters, [&](int i) {
        net.info("bench i={} value={:.3f} payload={}", i, i * 0.5, payload);
    });
    const double off_ns = per_call(iters, [&](int i) {
        TBLOGF_DEBUG("bench i={} value={:.3f} payload={}", i, i * 0.5, payload);
    });
    tblog_shutdown();

    std::printf("ns/call over %d calls (log level INFO):\n", iters);
    std::printf("  TBLOG_INFO  printf-style        %7.1f\n", c_ns);
    std::printf("  TBLOGF_INFO {} with {:.3f}      %7.1f\n", f_ns);
    std::printf("  TBLOGF_INFO {} shortest double  %7.1f\n", fd_ns);
    std::printf("  Logger::info                    %7.1f\n", l_ns);
    std::printf("  TBLOGF_DEBUG (filtered)         %7.1f\n", off_ns);
    std::printf("dropped=%llu\n", static_cast<unsigned long long>(tblog_dropped_count()));
    return 0;
}
