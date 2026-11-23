# Benchmarks: tblog versus `fprintf`

**English** | [Tiếng Việt](vi/benchmarks.md)

Compares tblog with the simplest way to log: `fprintf` into a file. No comparison with other logging libraries. Every figure comes from the JSON files in [bench/results/](../bench/results/); tables are built with `bench/report.py`.

## Conclusions

| | tblog is better | tblog is worse |
|---|---|---|
| Log call, 1 thread | p50 1.3–1.7× faster (MinGW: 6.5×), p99 8–12× faster | On Linux / macOS, single-threaded `fprintf` is already fast enough (321–375 ns p50) |
| Log call, many threads | p99 0.48–1.8 µs versus 34–139 µs (4 threads) | |
| Throughput | 2.5–13× at 4 threads | Ceiling of one background thread: 1.5 M (Windows CI) to ~6 M lines/s (Linux) |
| Crash, stack overflow included | 0 lines lost on every platform, versus a mean of 16–28 | Process dies 0.5–9 ms later |
| `kill -9` | `flush.on_level = ERROR`: 0 lines lost up to the last ERROR line | By default 1 900–4 500 lines lost per thread (macOS VM: 6 725), versus 41 |
| Overload | `block`: no lines lost | `block`: log calls sometimes wait (max 2.9–36 ms); `drop`: up to 47% lost |
| Power loss | fsync every second and after ERROR; `fprintf` does not fsync | No variant is fully safe |

## Machines

| Name | Machine | Notes |
|---|---|---|
| Laptop | Ryzen 7 8845HS, 16 CPUs, Windows 11, NVMe, MinGW-w64 g++ 15.1 `-O2` | The detailed sections below. MinGW's `fprintf` is unusually slow: `snprintf` + `fwrite` is 5× faster |
| Linux | GitHub Actions, EPYC 7763, 4 CPUs, Ubuntu 26.04, g++ 15.2 | Shared VM |
| macOS | GitHub Actions, Apple M1 (VM), 3 CPUs, macOS 26, clang 21 | Shared VM; clock step 41 ns |
| Windows CI | GitHub Actions, EPYC 9V45, 4 CPUs, Windows Server 2025, MSVC 19.52 | Shared VM; slow disk (fsync) |

CI performance figures come from the first bench run (commit `47e4c97`, machines as above). Log-loss figures come from the run after the stack-overflow fix (commit `12fb37d`); in that run Linux ran on a Xeon Platinum 8370C and Windows on an EPYC 7763 (still 4 CPUs). CI machines are noisy: only compare variants with each other on the same machine, not across machines. The laptop does not pin its CPU frequency: tblog's end-to-end figures vary ±20–40% between runs. Two later changes are not in these figures: `drop` also drops a line whose shard lock stays busy after a short spin (commit `dd7791a`), and `message.escape` (on by default) adds 6–10 ns per line ([configuration.md](configuration.md#messageescape-one-call-one-line)).

## Cross-platform comparison

**Log call** p50 / p99 (flood):

| | Linux | macOS | Windows CI | Laptop |
|---|---|---|---|---|
| `fprintf`, 1 thread | 321 ns / 3.73 µs | 375 ns / 5.33 µs | 381 ns / 2.84 µs | 2.03 µs / 6.9 µs |
| tblog, 1 thread | 251 ns / 310 ns | 292 ns / 667 ns | 230 ns / 371 ns | 311 ns / 591 ns |
| `fprintf`, 4 threads | 741 ns / 59 µs | 459 ns / 96.2 µs | 1.23 µs / 34.3 µs | 8.05 µs / 139 µs |
| tblog, 4 threads | 441 ns / 501 ns | 417 ns / 1.79 µs | 361 ns / 481 ns | 551 ns / 641 ns |
| Filtered call (tblog / `if`) | 0.63 / 0.32 ns | 0.57 / 0.32 ns | 0.50 / 0.23 ns | 1.16 / 0.60 ns |

**End-to-end throughput** (until fsynced), `fprintf` → tblog:

| Threads | Linux | macOS | Windows CI | Laptop |
|---|---|---|---|---|
| 1 | 2.19 M → 3.39 M | 1.74 M → 2.77 M | 600 k → 1.18 M | 365 k → 1.89 M |
| 4 | 734 k → 6.44 M | 562 k → 3.62 M | 600 k → 1.52 M | 213 k → 2.72 M |

**Log loss** (mean / max lines, 20 runs; columns tblog / tblog + `flush.on_level = ERROR`):

| Scenario | Platform | `fprintf` | tblog | + `flush.on_level` |
|---|---|---|---|---|
| SIGSEGV, 4 other threads logging | All platforms | 15.8–19.4 / 40 | 0 | 0 |
| `abort`, `assert`, exception | Linux | 18.8–21.1 / 40 | 0 | 0 |
| | macOS, Windows | 0 (libc flushes stdio) | 0 | 0 |
| Stack overflow, secondary thread | All platforms | 20.2–20.6 / 41 | 0 | 0 |
| `kill -9`, 4 threads | Linux | 25.1 / 41 | 7 000 / 14 886 | 203 / 396 ¹ |
| | macOS | 20.4 / 37 | 15 072 / 23 457 | 198 / 382 ¹ |
| | Windows CI | 16.5 / 39 | 9 344 / 17 825 | 320 / 396 ¹ |
| `_Exit` | Linux | 17.6 / 35 | 2 206 / 3 771 | 56 / 96 ¹ |

¹ 0 lines lost up to the last ERROR line, on every platform.

- **Stack overflow on a secondary thread, Linux / macOS**: before commit `12fb37d` the whole buffer was lost (mean 2 065 lines on Linux, 2 794 on macOS; macOS also died with SIGILL), because only the thread that called `tblog_init`, the worker and the sync thread had an alternate signal stack. Now every thread gets a stack on its first log call: 0 lines lost, a crash report line in 20/20 runs, and macOS dies with SIGBUS as expected.
- **`kill -9` on 4-CPU machines loses more than on the laptop**: shards are 1 MB, so the limit is 200 ms of logging (~4 000 lines at 20 k lines/s), not 75% of a shard. Maximum per thread: 3 722 (Linux), 4 460 (Windows CI), 6 725 (macOS VM, over the limit: cause unknown).
- **`flush.on_level = ERROR` + `sync.on_level = ERROR`**, 1% ERROR lines, 1 thread: an ERROR call costs 39 µs p50 (Linux), 37 µs (macOS), 2.92 ms (Windows CI), 2.69 ms (laptop). Waiting for fsync only happens on Windows.
- `drop` with 8 threads on Windows CI drops 47% of lines (466 k / 1 M): the slow disk slows the background thread.
- The paced scenario with 8 threads is meaningless on 3–4-CPU CI machines: 8 busy-waiting threads take all the CPU, the background thread's included.

## Method

| Variant | Per line |
|---|---|
| `fprintf_buffered` | One `fprintf` into a shared `FILE*`, default stdio buffer |
| `fprintf_flush` | As above + `fflush` per line |
| `snprintf_fwrite` | `snprintf` onto the stack, then one `fwrite` (separates formatting cost from stream cost) |
| `tblog_block` | `TBLOG_INFO(...)`, defaults, file output only |
| `tblog_drop` | As above, `full_policy = drop` |
| `tblog_flush_error` | As above + `flush.on_level = ERROR`, every 100th line is ERROR |

The same ~102-byte line in the default pattern; `fprintf`'s timestamp is made as fast as tblog's (cached per second).

- **Flood**: 1 M lines split across 1–16 threads, as fast as possible. *End-to-end* runs until every line is written and fsynced. The line count in the file is verified.
- **Paced**: 50 k lines/s per thread on a fixed schedule; latency is measured from the scheduled time (avoiding *coordinated omission*).
- **Filtered call**: `TBLOG_DEBUG` at level `INFO`, versus `if (atomic_flag) fprintf(...)`.
- **Log loss**: a child process logs numbered lines, records how many have returned in shared memory, then dies according to the scenario. The parent compares with the file: **lost** = lines that returned but are not in the file.
- 5 runs (perf) or 20 trials (crash) per cell, after 1 warm-up. Tables show median [min–max].

## Detailed performance (laptop)

Full tables for the CI machines: `python3 -I bench/report.py bench/results/<machine>-*.json`.

**End-to-end throughput** (lines/s, until fsynced):

| Threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 365 k | 179 k | 1.1 M | 1.89 M [1.51–2.04 M] | 1.82 M |
| 4 | 213 k | 127 k | 1.16 M | 2.72 M [2.28–3.49 M] | 3.28 M |
| 8 | 213 k | 124 k | 1.06 M | 4.34 M [2.41–4.82 M] | 4.48 M |
| 16 | 205 k | 120 k | 1.08 M | 4.27 M [3.12–5.59 M] | 5.04 M |

`fprintf` drops as threads are added (contention on the `FILE*` lock); tblog rises. On the logging side, tblog puts 2.7 M (1 thread) to 18.3 M lines/s (16 threads) into the buffer: the gap to end-to-end is the background thread's ceiling.

**Log call latency** (p50 / p99 / max):

| Threads | fprintf_buffered | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|
| 1 | 2.03 µs / 6.9 µs / 10.3 ms | 381 ns / 4.52 µs / 1.21 ms | 311 ns / 591 ns / 72 µs | 311 ns / 601 ns / 62 µs |
| 4 | 8.05 µs / 139 µs / 15.2 ms | 671 ns / 52 µs / 3.31 ms | 551 ns / 641 ns / 193 µs | 561 ns / 651 ns / 158 µs |
| 8 | 8.56 µs / 330 µs / 5.28 ms | 781 ns / 114 µs / 2.47 ms | 561 ns / 671 ns / 305 µs | 561 ns / 671 ns / 151 µs |
| 16 | 9.53 µs / 729 µs / 16.7 ms | 872 ns / 226 µs / 1.93 ms | 561 ns / 671 ns / 2.88 ms | 571 ns / 691 ns / 1.99 ms |

`fprintf_flush`: 4.95 µs p50 (1 thread), 14.1 µs (8 threads). `drop` drops 9.4 k lines / 1 M at 8 threads, 233 k at 16 threads (worst run 330 k).

**Paced, 50 k lines/s per thread** (p50 / p99, rate achieved):

| Threads | fprintf_buffered | fprintf_flush | tblog_block |
|---|---|---|---|
| 1 | 3.62 µs / 54.7 µs | 7.86 µs / 74.6 µs | 598 ns / 3.74 µs |
| 4 | 3.32 µs / 1.18 ms | 276 ms / 557 ms (only 129 k/200 k) | 539 ns / 1.75 µs |
| 8 | 406 ms / 801 ms (only 222 k/400 k) | 1.06 s / 1.13 s (only 128 k/400 k) | 538 ns / 2.52 µs |

When `fprintf` cannot keep up with the rate, the backlog grows and latency reaches hundreds of ms. tblog stays under 4 µs p99 at every level.

**Filtered call**: tblog 1.16 ns, `if` + `fprintf` 0.60 ns. No practical difference.

### Cost of `flush.on_level = ERROR`

Flood of 200 k lines, p50 / p99 of the ERROR calls:

| Threads | % ERROR | `sync.on_level = OFF` | `sync.on_level = ERROR` (default) |
|---|---|---|---|
| 1 | 1% | 21 µs / 89 µs; 881 k lines/s | 2.69 ms / 4.34 ms; 35 k lines/s |
| 1 | 100% | 17 µs / 50 µs; 43.5 k lines/s | 2.54 ms / 4.96 ms; 376 lines/s |
| 8 | 1% | 148 µs / 428 µs; 1.92 M lines/s | 5.4 ms / 15.9 ms; 132 k lines/s |
| 8 | 100% | 103 µs / 233 µs; 69.8 k lines/s | 5.21 ms / 9.04 ms; 1.54 k lines/s |

- INFO lines in between stay at 311–591 ns p50: lines below the level cost nothing extra.
- With `sync.on_level = ERROR` each ERROR line costs about one fsync, because on NTFS `WriteFile` has to wait for a running `FlushFileBuffers`. Linux and macOS are not affected ([see above](#cross-platform-comparison)). Sparse ERROR lines (further apart than one fsync): not measured.
- Conclusion: `flush.on_level` suits rare lines (errors), not the level of most lines.

## Detailed log loss (laptop)

20 trials per cell, 20 k lines/s per thread. Cell: mean / max lines lost.

| Scenario | Threads | fprintf_buffered | fprintf_flush | tblog | tblog_flush_error |
|---|---|---|---|---|---|
| SIGSEGV on the logging thread | 1 | 23.9 / 41 | 0 | 0 | 0 |
| SIGSEGV on another thread | 4 | 19.2 / 39 | 0 | 0 | 0 |
| `abort`, `assert`, uncaught exception | 1 | 0 | 0 | 0 | 0 |
| Stack overflow (init thread / other thread) | 1 | 17.3 / 38, 16.4 / 41 | 0 | 0 | 0 |
| `kill -9` / `TerminateProcess` | 4 | 21.6 / 41 | 0 | 4 612 / 7 751 | 174 / 396 ¹ |
| `exit` | 1 | 0 | 0 | 0 | 0 |
| `_Exit` | 1 | 0 | 0 | 954 / 1 811 | 56 / 96 ¹ |

¹ All of them are INFO lines logged after the last ERROR line: 0 ERROR lines lost, 0 lines lost up to the last ERROR line. At most 99 lines per thread (the gap between two ERROR lines).

- 800/800 child processes died according to the scenario; tblog wrote the `*** tblog: caught ... ***` line in 280/280 crashes; 0 corrupted lines, 0 duplicated lines.
- `fprintf_buffered` left a truncated line at the end of the file in 19–20/20 runs with SIGSEGV, stack overflow and `kill -9`.
- tblog's `kill -9`: at most 1 939 lines per thread, matching the prediction: 16 CPUs → 256 KB shards, the background thread is woken at 75% full ≈ 1 900 lines.
- `fprintf_buffered` loses nothing on `abort` and `_Exit`: MinGW's `msvcrt.dll` CRT flushes streams when the process exits. glibc does not: on Linux `fprintf` loses a mean of 21 lines on `abort` ([see above](#cross-platform-comparison)).
- Stack overflow on a secondary thread loses nothing on Windows; Linux / macOS: see the comparison above.
- Time from triggering the crash to process end: tblog 2.4 ms versus 1.6 ms for `fprintf` (1 thread), 10.3 ms versus 1.5 ms (4 other threads logging).

## Rerunning

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTBLOG_BUILD_BENCH=ON
cmake --build build-bench --config Release --target bench_vs_printf bench_crash_loss
./build-bench/bench/bench_vs_printf  --out-dir bench/results   # ~12 minutes
./build-bench/bench/bench_crash_loss --out-dir bench/results   # ~3 minutes
python3 -I bench/report.py bench/results/*.json                 # full tables
```

- MSVC: the executables are in `build-bench/bin/Release/`. Linux: `make bench`.
- `--quick` for a fast run; `--help` lists every option (thread counts, variants, scenarios, `--keep` keeps each trial's log file).
- GitHub: Actions → **bench** → *Run workflow*: runs on Linux, macOS, Windows (MSVC) and writes the tables into the job summary. CI machines are noisy: only compare variants with each other on the same runner.
- The CTest tests `bench_vs_printf_smoke`, `bench_crash_loss_smoke` only check that the programs run to completion; they are not measurements.
