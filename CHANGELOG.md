# Changelog

**English** | [Tiếng Việt](CHANGELOG.vi.md)

## 0.1.0 (2026-11-22)

First release.

### Breaking

- **`full_policy` defaults to `block`** (previously `drop`). Lines are no longer lost under overload (previously up to 26% with 16 flooding threads); in exchange a log call sometimes waits a few ms. Old behaviour: `full_policy = drop`.
- **Removed the `sync` and `mmap` backends and deferred formatting.** The keys `backend`, `mmap.region_size`, `format.mode` are ignored with a warning.
- **ABI:** removed `tblog_write_site()` and `tblog_site`; programs built against the old headers must be rebuilt. The version stays `0.1.0`.
- **gzip compression requires zlib** (the path that ran an external `gzip` command is gone).
- **The Makefile is Linux-only**; CMake is the main build.

### Added

- **`flush.on_level`**: lines ≥ the level return only once written to the file. With `ERROR`: after `kill -9` (4 threads) a mean of 174 INFO lines is lost instead of 4 612, and 0 lines up to the last ERROR line; each ERROR line costs 21–148 µs, or ~1 fsync together with `sync.on_level = ERROR`.
- **`tblog_add_sink()`**: user sinks. **`file = false`**: no file output.
- **Windows** (MSVC, MinGW-w64; static and DLL) and **macOS** 12+ (arm64, x86_64).
- **CMake package**: `find_package(tblog)`, `add_subdirectory`, FetchContent; targets `tblog::tblog`, `_static`, `_shared`.
- **Benchmarks against `fprintf`**: `bench_vs_printf` (throughput, latency) and `bench_crash_loss` (log loss on crash / `kill -9` / `exit`), workflow `bench.yml`.
- CI on Linux (glibc, musl), macOS, Windows. Benchmark results for Linux, macOS, Windows MSVC (GitHub Actions) are in `bench/results/`.

### Fixed

- **Stack overflow on a secondary thread, Linux / macOS**: previously the crash handler did not run (the thread had no alternate signal stack) and the whole buffer was lost (measured mean of 2 065–2 794 lines). Every thread now gets a 64 KB alternate stack on its first log call (one `mmap`, no real RAM used until a signal arrives, released when the thread exits); later log calls cost nothing extra. Remeasured: 0 lines lost in 20/20 runs on Linux and macOS. A thread that has never logged is still unprotected. Windows unchanged.

### Fixed

- **Crash while other threads keep logging: 0 lines lost** (previously hundreds of lines each time). Log lines issued after the handler has drained the buffer are written straight to the file.
- **A crash no longer writes the chunk being written twice** (previously a whole chunk was occasionally duplicated). Remeasured: 0 duplicated lines in every scenario.
- No more lost lines when a crash hits exactly while the background thread is writing.
- `make install` did not install `detail/format.hpp`.
- When embedded via `add_subdirectory`: no longer overrides the parent project's `CMAKE_BUILD_TYPE`, and honours options set with `set()`.
