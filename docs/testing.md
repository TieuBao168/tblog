# Testing

**English** | [Tiếng Việt](vi/testing.md)

```bash
cmake -S . -B build -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON -DTBLOG_BUILD_BENCH=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure      # MSVC: -C Release
ctest --test-dir build -R test_config -V        # a single test
```

Each test has its own scratch directory, so `ctest -j` can run them in parallel. Linux: `make test`, `make SAN=address test`, `make -C lite test`.

## Test suites

No external library needed (`tests/tiny_test.h`). Crash and process-exit tests re-run the test executable itself as a child process (no `fork`), so they also run on Windows.

| Test | Checks |
|---|---|
| `test_config` | Parser, warnings, defaults, hot reload, `file.mode` / `file.max_files` limits |
| `test_core` | Levels, modules, formatter, `message.escape` (one line, within the line size) |
| `test_cpp_api` | `{}` API, `Logger`, user-defined types (built as C++11) |
| `test_queue_file` | Buffer (ordering, drop/block, crash), shard lock (bounded, stops once closed), file rotation, links refused, `file.mode`, `flush.on_level` never hangs |
| `test_crash_sinks` | 4 threads logging, a 5th thread crashes: every line that returned is in the file and console exactly once |
| `test_integration` | End to end through the C API; process hard-killed right after an ERROR line (10 times): 0 lines lost up to the last ERROR line |
| `test_rotation_time` | Daily/hourly rotation (fake clock), gzip compression, recovery after a crash |
| `test_shared_exit` | Shared library: exit without calling `tblog_shutdown`, crash inside the DLL/.so |
| `example_*`, `bench_*_smoke` | Examples and benchmarks run to completion (not measurements) |
| `check_symbols`, `check_exports` | The library exports only public functions |

Skipped tests: `rotating_write_error_counts_loss` on macOS / Windows (no `/dev/full`), `syslog_sink_handles_batches` on Windows, the link and permission tests of `test_queue_file` on Windows (POSIX links and permission bits). The gzip link tests only exist in builds with zlib. The TSan build skips the stack-overflow scenario of `bench_crash_loss_smoke`.

## CI

[ci.yml](../.github/workflows/ci.yml) runs on every push and pull request to `main`, unless only `*.md` / `docs/` changed.

| Platform | Configurations |
|---|---|
| Ubuntu | GCC C++17, GCC C++11, Clang, ASan+UBSan, TSan, no zlib, Makefile, install, `src/platform/` boundary |
| Alpine (musl) | GCC C++17 |
| macOS | arm64 Release, ASan+UBSan, TSan; x86_64 C++11 without zlib |
| Windows | MSVC Release, MSVC Debug C++14 + zlib, MinGW C++11 + zlib; ARM64 headers only |
| tblog-lite | `test`, `test-asan`, `demo`, `size` |

Every job treats warnings as errors. Guards so tests cannot silently drop out of CI:

- `TBLOG_TEST_NO_SKIP=1` on Linux: a skipped test becomes a failure.
- `packaging/check-test-inventory.cmake`: fails if tests are missing or extra compared with the required list.
- `packaging/report-skips.sh`: prints every skipped test into the job summary.

[bench.yml](../.github/workflows/bench.yml) is run manually, see [benchmarks.md](benchmarks.md#rerunning).
