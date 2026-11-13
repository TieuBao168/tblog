// Latency/throughput benchmark through the public API.
// Usage: build/bench/bench_latency [lines_per_thread]
// Per-call latency is measured with clock_gettime around each call, so every
// number includes ~15-25 ns of timer overhead (printed as "timer").
#include "tblog/log.h"

#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

inline uint64_t ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

struct Result { double p50, p99, p999, max, lines_per_s; };

Result run(int threads, int per_thread) {
    std::vector<std::vector<uint32_t>> lat(threads, std::vector<uint32_t>(per_thread));
    const uint64_t t0 = ns();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            auto &v = lat[t];
            for (int i = 0; i < per_thread; ++i) {
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
    uint64_t timer = ~0ull;
    for (int i = 0; i < 1000; ++i) { uint64_t a = ns(); timer = std::min(timer, ns() - a); }
    std::printf("timer overhead ~%llu ns (min); lines/thread=%d\n", (unsigned long long)timer, per_thread);
    std::printf("%-7s %3s %9s %9s %9s %10s %12s %8s\n", "backend", "thr", "p50(ns)", "p99(ns)",
                "p999(ns)", "max(ns)", "lines/s", "dropped");

    const char *backends[] = {"sync", "buffer", "mmap"};
    for (const char *b : backends) {
        for (int threads : {1, 4, 8}) {
            std::string dir = "build/bench_tmp/" + std::string(b) + std::to_string(threads);
            std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir;
            if (std::system(cmd.c_str()) != 0) return 1;
            std::ofstream(dir + "/log.conf")
                << "backend = " << b << "\nfile.path = " << dir << "/app.log\n"
                << "file.max_size = 4GB\nbuffer.size = 8MB\nmmap.region_size = 16MB\n"
                << "full_policy = drop\nreload.watch = false\n";
            const uint64_t dropped_before = tblog_dropped_count();
            if (tblog_init((dir + "/log.conf").c_str()) != 0) return 1;
            Result r = run(threads, per_thread);
            tblog_shutdown();
            const unsigned long long dropped = (unsigned long long)(tblog_dropped_count() - dropped_before);
            std::printf("%-7s %3d %9.0f %9.0f %9.0f %10.0f %12.0f %8llu\n", b, threads, r.p50, r.p99,
                        r.p999, r.max, r.lines_per_s, dropped);
            std::fflush(stdout);
            std::string rm = "rm -rf " + dir;
            if (std::system(rm.c_str()) != 0) return 1;
        }
    }
    return 0;
}
