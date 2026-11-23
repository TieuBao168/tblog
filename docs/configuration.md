# Configuration (`log.conf`)

**English** | [Tiếng Việt](vi/configuration.md)

`tblog_init(path)` reads a `key = value` file. If `path` is `NULL`, empty or unreadable, every key takes its default. Sample file with every key: [log.conf](../log.conf).

Figures on this page come from a Windows 11 laptop (Ryzen 7 8845HS, NVMe, MinGW g++ 15), unless Linux / macOS / MSVC (GitHub Actions) is stated ([benchmarks.md](benchmarks.md)).

## Syntax

- One `key = value` per line; if a key repeats, the later line wins.
- Key names are case-sensitive (`Level` is an unknown key); values are not.
- Comments: `#` at the start of a line, or ` #` after whitespace. Only `pattern` takes the rest of the line, `#` included.
- `"..."` keeps leading/trailing whitespace or ` #` in a value. No escapes.
- Value types: levels `TRACE` `DEBUG` `INFO` `WARN` `ERROR` `FATAL` `OFF`; booleans `true/false`, `1/0`, `yes/no`, `on/off`; sizes `64MB`, `512K`, `4 GB` (base 1024, integers); milliseconds are integers with no suffix (`200ms` is invalid).
- Invalid value: a warning on stderr, the key keeps its previous value. `tblog_init` never fails because of the configuration.

## Every key

| Key | Default | Meaning | Changeable at run time |
|---|---|---|---|
| `level` | `INFO` | Root level, applies to every module without its own | Yes |
| `module.<name>` | follows `level` | A module's own level (name ≤ 45 characters, ≤ 256 modules) | Yes ¹ |
| `pattern` | `%D %T.%us [%L] [%t] %f:%l - %m` | Line layout, see [tokens](#pattern) | Yes |
| `file` | `true` | Write a file | No ² |
| `file.path` | `./logs/app.log` | Log file; relative to the working directory at init | No ² |
| `file.max_size` | `256MB` | Rotate when the file exceeds this | No |
| `file.max_files` | `5` | Files to keep, the current one included | No |
| `file.rotate` | `size` | `size`, `daily`, `hourly` | No |
| `file.compress` | `none` | `none`, `gzip` (needs a zlib build) | No |
| `buffer.size` | `4MB` | RAM buffer, split across shards | No ² |
| `full_policy` | `block` | Buffer full: wait (`block`) or drop the line (`drop`) | No |
| `flush.interval_ms` | `200` | Maximum write period, 1–60000 | Yes |
| `flush.on_level` | `OFF` | Lines ≥ level return only once written to the file | Yes |
| `sync.interval_ms` | `1000` | fsync period, 0–3600000 (`0` = off) | Yes |
| `sync.on_level` | `ERROR` | fsync right after a line ≥ level | No |
| `console` | `false` | Also write to stderr | No ² |
| `console.color` | `true` | Colour by level when stderr is a terminal | No |
| `syslog` | `false` | Also write to syslog (Linux, macOS) | No ² |
| `reload.watch` | `true` | Re-read the configuration file when it changes (checked every ≥ 1 s) | No |

¹ A reload is applied **on top of** the running configuration: deleting a `module.x` line does not remove `x`'s own level; write a new value instead.
² Changing it at run time prints `file/buffer/sink changes need a restart` and keeps the old value. The other "No" keys: keep the old value, silently.

## Choices and their cost

### `full_policy`: wait or drop when the buffer is full

Only matters when logging is faster than the background thread's ceiling (about 4–5 M lines/s on the test machine). Flood of 1 M lines:

| | `block` (default) | `drop` |
|---|---|---|
| Lines dropped, 8 / 16 threads | 0 / 0 | 0.9% / 23% (worst 33%) |
| Log call p99 / max, 16 threads | 671 ns / 2.88 ms | 691 ns / 1.99 ms |
| End-to-end throughput, 16 threads | 4.27 M/s | 5.04 M/s |

- **`block`**: no lines lost; in exchange, under overload the logging thread sometimes waits a few ms, and a slow disk slows the application.
- **`drop`**: a log call never waits for the background thread, the disk or a thread that holds its shard's lock (if the lock is not free after a short, bounded spin, the line is dropped too, so a real-time thread cannot get stuck behind a lower-priority thread that was preempted while logging); in exchange, logs are lost exactly under overload (usually when something is going on). One noisy thread can fill its shard while others are still empty. Monitor with `tblog_dropped_count()` and the `tblog: N lines dropped` line in the file.
- Choose `drop` only when the logging thread has a hard deadline (real-time loops, network processing).

### `flush.on_level`: surviving `kill -9`

By default a log call returns while the line is still in RAM, so `kill -9`, the OOM killer, `TerminateProcess`, `_exit` lose whatever is in the buffer. With `flush.on_level = ERROR`, an ERROR call returns only once that line (and every earlier line from the same thread) has been `write()`n to the file.

| | `OFF` (default) | `ERROR` |
|---|---|---|
| Lost after `kill -9`, 4 threads (mean / max, 20 runs) | 4 612 / 7 751 lines | 174 / 396 INFO lines, **0 up to the last ERROR line** |
| Lost after `_Exit`, 1 thread | 954 / 1 811 | 56 / 96, **0 up to the last ERROR line** |
| Per ERROR line, p50 1 / 8 threads, `sync.on_level = OFF` | 311 / 561 ns | 21 / 148 µs |
| As above, `sync.on_level = ERROR`, dense ERRORs | | 2.69 / 5.4 ms (≈ one fsync) |
| Lines below the level | 311 ns | 311–391 ns: unchanged |

- **Pros**: the last error line always survives the process being killed; lines below the level cost nothing extra.
- **Cons**: each ERROR line is 70–260× slower (21–148 µs instead of 311–561 ns). Windows only: combined with `sync.on_level = ERROR` and dense ERROR lines, each line waits for a whole fsync (2.69–2.92 ms p50), because `WriteFile` has to wait for a running `FlushFileBuffers`. Linux and macOS are not affected: 39 and 37 µs p50 with the same configuration.
- **Do not set it below `INFO`**: when every line has to wait, 1 thread manages only 43.5 k lines/s (`sync.on_level = OFF`) or 376 lines/s (`ERROR`). tblog warns.
- No fsync (power loss still needs `sync.*`). Never hangs: waits at most 2 s; does not wait during shutdown, during a crash, for a `drop`ped line, or when called from a background thread.

### `sync.on_level`, `sync.interval_ms`: surviving power loss

fsync pushes data from the OS cache to the disk; it costs about 2.5 ms on NVMe on the test machine. It runs on its own sync thread, so log calls do not wait.

| Configuration | Pros | Cons |
|---|---|---|
| `sync.on_level = ERROR` (default) | Error lines are on disk a few ms after being logged | Every write round containing an ERROR line triggers an fsync |
| Small `sync.interval_ms` | Less lost on power loss: at most ≈ `sync.interval_ms` + `flush.interval_ms` | More physical writes (flash wear, battery) |
| `sync.interval_ms = 0`, `sync.on_level = FATAL` | Fewest physical writes, suits flash | Power loss can lose everything until the kernel writes back by itself (Linux: about 30 s) |

macOS uses `F_FULLFSYNC` (out of the drive's cache too), a few ms each time.

**`flush.on_level` versus `sync.on_level`:**

| | `flush.on_level` | `sync.on_level` |
|---|---|---|
| Log call | Waits until the line is in the file | Returns immediately |
| After the write round | No fsync | fsync (sync thread) |
| Survives `kill -9` once the call returns | Yes | Not guaranteed |
| Survives power loss | After the next fsync | A few ms after the call returns |

Use both at `ERROR`: the error line is in the OS when the call returns, and on disk a few ms later.

### `flush.interval_ms`: write period

The background thread writes every `flush.interval_ms`, or earlier when a shard is 75% full, a line ≥ `flush.on_level` / `sync.on_level` arrives, or `tblog_flush()` is called.

- **Small (20–50)**: a narrower loss window on `kill -9`; lines appear sooner in `tail -f`. Con: the background thread wakes more often, writing less each time.
- **Large (1000)**: the background thread wakes less (battery, flash).
- Does not affect log calls. At 20 k lines/s per thread, a shard reaches 75% after about 96 ms, earlier than the 200 ms default, so only values under 96 ms narrow the loss window. Other values not measured.
- It is also the maximum ordering skew between lines from different threads, and the delay of `daily` / `hourly` boundaries.

### `buffer.size`: RAM

RAM used = 2 × max(`buffer.size`, N × 64 KB), N = number of shards (power of 2 ≥ CPU count, at most 16). Default: 8 MB.

- **Large (`16MB`, `64MB`)**: absorbs bursts without waiting (`block`) or dropping lines (`drop`).
- **Small (`256KB`)**: saves RAM (4 CPUs: 512 KB).
- Affects the number of lines lost on `kill -9`. Each thread loses at most the smaller of two limits: 75% of one shard, or one `flush.interval_ms` worth of logging. Measured at 20 k lines/s per thread:
  - 16 CPUs: 256 KB shards, 75% ≈ 1 900 lines; measured maximum 1 939.
  - 4 CPUs: 1 MB shards, 75% ≈ 7 500 lines, so the 200 ms limit ≈ 4 000 lines wins; measured maximum 3 722 (Linux), 4 460 (Windows), 6 725 (macOS VM, over the limit, cause unknown).

### Files: rotation, retention, compression

- Maximum disk use ≈ `file.max_size` × `file.max_files` (before compression). Smaller files rotate more often (a few system calls each time, plus one compression if gzip is on).
- `daily` / `hourly`: one file per period, easy to find and delete by date. `file.max_size` still applies; a period over the limit produces `.1`, `.2`... With `daily`, `file.max_files` = number of days to keep + 1.
- `gzip`: compresses on its own thread, does not slow log calls or the background thread. Cons: CPU on each rotation; on flash an extra `.gz` file is written. Crash-safe midway (`.gz.tmp` → fsync → rename → only then delete the original). Compression ratio: not measured.
- Prefer an absolute `file.path`. If the file is deleted or renamed from outside (logrotate), tblog reopens it within about 1 s.

| `file.rotate` | Rotated files |
|---|---|
| `size` | `app.1.log` (newest), `app.2.log`... |
| `daily` | `app.2026-11-13.log`, `app.2026-11-13.1.log`... |
| `hourly` | `app.2026-11-13T14.log`... |

### Console, syslog

Every sink runs on the background thread in the order file, console, syslog, custom sinks: one slow sink slows them all.

- `console`: for terminals, containers, systemd. Drained on crash like the file. Con: a slow terminal (especially the Windows console) slows the background thread with it.
- `syslog`: one system call per line; does not receive the crash drain; not available on Windows.
- `file = false` with no other sink: tblog enables `console` with a warning.
- Console colour and syslog severity are taken from the `[%L]` token in `pattern`.

### `pattern`

| Token | Meaning | Token | Meaning |
|---|---|---|---|
| `%D` | `YYYY-MM-DD` (local time) | `%n` | Module (`-` = root) |
| `%T` | `HH:MM:SS` | `%f` | Source file name |
| `%us` / `%ms` | Micro / milliseconds | `%l` | Line number |
| `%L` | Level | `%F` | Function name |
| `%t` | OS thread id | `%m` | Message |
| `%%` | `%` | | |

Lines are at most 1024 bytes; longer ones are truncated and end with `...`. Shorter lines mean smaller files and more lines per second through the background thread.

## Which failure, which key protects

| Failure | Lost by default | Protection |
|---|---|---|
| Crash (SIGSEGV, `abort`, exception, stack overflow; Windows: SEH) | 0 (measured) | Not needed. Exception: a stack overflow on a thread that has **never logged** (Linux / macOS) prevents the handler from running, lost as with `kill -9` |
| `exit()`, `return` from `main` | 0 | Not needed |
| `kill -9`, OOM killer, `TerminateProcess`, `_exit` | Up to 75% of a shard or one `flush.interval_ms` of logging per thread (measured 1 900–4 500 lines, see [`buffer.size`](#buffersize-ram)) | `flush.on_level`, `tblog_flush()` at key points |
| Flood over the ceiling | 0 (`block` waits) | `buffer.size`; `drop` loses lines but does not wait |
| Power loss, kernel panic | Up to ≈ `sync.interval_ms` + `flush.interval_ms` | `sync.on_level`, `sync.interval_ms`, `tblog_flush()` |
| Disk full | Whatever could not be written (warning on stderr) | No key |

## Warnings

Format `tblog: config: config line <N>: <error>` at init and on every reload: `unknown key`, `invalid level`, `invalid boolean`, `invalid number`, `size must be > 0`, `value out of range ... clamped`, `file.max_files must be >= 1`, quoting errors, `option removed, ignored` (removed keys: `backend`, `mmap.region_size`, `format.mode`).

Unusable combinations are corrected: `gzip` without zlib → `none`; `file = false` with no sink left → `console` enabled; `flush.on_level` below `INFO` → warning only.

## Sample configurations

Every sample has been run through tblog's parser with no warnings. Keys not listed take their defaults.

**Typical service**: the defaults are enough (no lines lost when the buffer is full or on crash, fsync every second and after ERROR lines).

```ini
file.path         = /var/log/myservice/app.log
file.max_files    = 10
module.http       = WARN
```

**Logs must survive being killed**: ERROR lines survive `kill -9`; each ERROR line costs 21–148 µs, up to a few ms when dense.

```ini
file.path         = /var/log/myservice/app.log
flush.interval_ms = 50
flush.on_level    = ERROR
```

**Low latency, never wait**: drop lines under overload.

```ini
file.path         = /var/log/trading/app.log
buffer.size       = 64MB
full_policy       = drop
sync.on_level     = FATAL
sync.interval_ms  = 5000
```

**Embedded Linux on flash**: little RAM, few physical writes.

```ini
level             = WARN
file.path         = /data/log/app.log
file.max_size     = 2MB
file.max_files    = 3
buffer.size       = 256KB
flush.interval_ms = 1000
sync.on_level     = FATAL
sync.interval_ms  = 0
reload.watch      = false
```

**Container (stderr only)**:

```ini
file              = false
console           = true
console.color     = false
```

**One file per day, gzip-compressed, kept for a month**:

```ini
file.path         = /var/log/myservice/app.log
file.rotate       = daily
file.compress     = gzip
file.max_size     = 1GB
file.max_files    = 31
```
