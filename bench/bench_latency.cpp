// Latency/throughput benchmark through the public API.
// Usage: build/bench/bench_latency [lines_per_thread]
// Environment:
//   TBLOG_BENCH_RATE=N    pace each thread to N lines/s (default 0 = flood).
//                         Flooding measures a saturated logger: with the Drop
//                         policy a fast caller path shows up as drops.
//   TBLOG_BENCH_POLICY=P  full_policy for the run: drop (default) or block.
// Per-call latency is measured with std::chrono::steady_clock around each call, so every
// number includes ~15-25 ns of timer overhead (printed as "timer").
#include "tblog/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

inline uint64_t ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

struct Result { double p50, p99, p999, max, lines_per_s; };

Result run(int threads, int per_thread, uint64_t rate) {
    const uint64_t period = rate ? 1000000000ull / rate : 0;  // ns between lines
    std::vector<std::vector<uint32_t>> lat(threads, std::vector<uint32_t>(per_thread));
    const uint64_t t0 = ns();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            auto &v = lat[t];
            uint64_t next = ns();
            for (int i = 0; i < per_thread; ++i) {
                if (period) {  // busy-wait pacing: sleeping would add jitter
                    next += period;
                    while (ns() < next) {
                    }
                }
                const uint64_t a = ns();
                TBLOG_INFO("bench thread=%d i=%d value=%f payload=%s", t, i, i * 0.5, "abcdefghijklmnop");
                v[i] = uint32_t(ns() - a);
            }
        });
    for (auto &t : ts) t.join();
    tblog_flush();
    const double secs = double(ns() - t0) / 1e9;
    std::vector<uint32_t> all;
    for (auto &v : lat) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    auto q = [&](double p) { return double(all[size_t(p * double(all.size() - 1))]); };
    return {q(0.50), q(0.99), q(0.999), double(all.back()), double(all.size()) / secs};
}

}  // namespace

int main(int argc, char **argv) {
    const int per_thread = argc > 1 ? std::atoi(argv[1]) : 200000;
    const char *rate_env = std::getenv("TBLOG_BENCH_RATE");
    const uint64_t rate = rate_env ? std::strtoull(rate_env, nullptr, 10) : 0;
    const char *pol_env = std::getenv("TBLOG_BENCH_POLICY");
    const std::string policy = pol_env && *pol_env ? pol_env : "drop";
    uint64_t timer = ~0ull;
    for (int i = 0; i < 1000; ++i) { uint64_t a = ns(); timer = std::min(timer, ns() - a); }
    std::printf("timer overhead ~%llu ns (min); lines/thread=%d; rate/thread=%s; policy=%s\n",
                (unsigned long long)timer, per_thread, rate ? std::to_string(rate).c_str() : "flood",
                policy.c_str());
    std::printf("%3s %9s %9s %9s %10s %12s %8s\n", "thr", "p50(ns)", "p99(ns)", "p999(ns)",
                "max(ns)", "lines/s", "dropped");

    for (int threads : {1, 4, 8}) {
        std::string dir = "build/bench_tmp/buffer" + std::to_string(threads);
        std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir;
        if (std::system(cmd.c_str()) != 0) return 1;
        std::ofstream(dir + "/log.conf")
            << "file.path = " << dir << "/app.log\n"
            << "file.max_size = 4GB\nbuffer.size = 8MB\n"
            << "full_policy = " << policy << "\nreload.watch = false\n";
        const uint64_t dropped_before = tblog_dropped_count();
        if (tblog_init((dir + "/log.conf").c_str()) != 0) return 1;
        Result r = run(threads, per_thread, rate);
        tblog_shutdown();
        const unsigned long long dropped = (unsigned long long)(tblog_dropped_count() - dropped_before);
        std::printf("%3d %9.0f %9.0f %9.0f %10.0f %12.0f %8llu\n", threads, r.p50, r.p99, r.p999,
                    r.max, r.lines_per_s, dropped);
        std::fflush(stdout);
        std::string rm = "rm -rf " + dir;
        if (std::system(rm.c_str()) != 0) return 1;
    }
    return 0;
}
