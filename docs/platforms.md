# Platform differences

**English** | [Tiếng Việt](vi/platforms.md)

| | Linux | macOS 12+ | Windows 8+ |
|---|---|---|---|
| fsync | `fdatasync` | `F_FULLFSYNC` (slower, see below) | `FlushFileBuffers` |
| Thread id (`%t`) | `gettid()` | `pthread_threadid_np` (32 bits) | `GetCurrentThreadId` |
| Call stack on crash | `backtrace_symbols_fd` (musl: none) | macOS format | Addresses only, symbolise yourself |
| Syslog | Yes | Yes (unified logging) | No, `syslog = true` is ignored with a warning |
| Makefile | Yes | No, use CMake | No, use CMake |

All platforms: files are opened in append mode, handles are not inherited by child processes, parent directories are created automatically.

## Linux

- **Stack overflow**: can only be reported on a thread with an alternate signal stack. tblog gives each thread 64 KB of virtual memory on its first log call, so every thread that has logged is protected (measured 0 lines lost; previously a mean of 2 065). A thread that has never logged is not. Same on macOS.
- glibc's `abort()` does not flush stdio: here `fprintf` loses a mean of 21 lines while tblog loses 0.
- CI runs the full test suite on both glibc and musl. musl prints no call stack on crash (the log is still drained). `-DTBLOG_NO_BACKTRACE` disables the call stack everywhere.
- TSan on recent kernels needs ASLR off (`setarch $(uname -m) -R`); CTest and `make SAN=thread test` do this automatically.

### Embedded Linux

- Needs the C++ runtime on the target, or link with `-static-libstdc++`.
- RAM: 2 × max(`buffer.size`, N × 64 KB). With 4 CPUs and `buffer.size = 128KB`: 512 KB.
- Flash: `sync.interval_ms = 0` reduces physical writes; lines ≥ `sync.on_level` are still fsynced immediately. Sample configuration: [configuration.md](configuration.md#sample-configurations).
- Microcontrollers without an OS: use [tblog-lite](lite.md).

## macOS

- macOS's `fsync` only pushes data to the drive, which may still hold it in its cache. tblog uses `F_FULLFSYNC` so that "synced" means the same as on Linux. **Con**: each sync takes a few ms, so `tblog_flush()` waits longer; log calls are unaffected (sync runs on its own thread).
- A stack overflow on a secondary thread arrives as SIGBUS; it is reported and the buffer drained as on Linux (measured 0 lines lost). On a thread without an alternate stack, macOS kills the process with SIGILL instead.

## Windows

- MSVC has no C++11 mode: build the library with `TBLOG_CXX_STANDARD=14` or `17`.
- Paths are UTF-8; both `/` and `\` work; UNC and `\\?\` are supported.
- The log file is opened with `FILE_SHARE_READ | WRITE | DELETE`: it can be read, renamed and deleted while being written (needed for rotation).
- `WriteFile` has to wait while `FlushFileBuffers` runs on the same file: `flush.on_level` + `sync.on_level` at the same level slows ERROR lines to 2.69–5.4 ms (Linux / macOS: 37–39 µs) ([configuration.md](configuration.md#flushon_level-surviving-kill--9)).
- The console is coloured when ANSI can be enabled (Windows 10 1511+).
- DLL: call `tblog_shutdown()` before exiting; do not `FreeLibrary` while the logger is running ([architecture.md](architecture.md#process-exit-on-windows)).
- ARM64: CI only test-compiles the headers; the library has not been built or run on ARM64.
