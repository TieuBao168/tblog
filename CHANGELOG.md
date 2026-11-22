# Changelog

## Unreleased

### Thêm

- **So sánh với spdlog (async) và Quill**: option CMake `TBLOG_BENCH_COMPARE` (tải hai thư viện, ghim bản phát hành + SHA-256), chương trình `bench/bench_vs_libs` (mỗi lần đo một process) và các biến thể `spdlog_async`, `quill`, `quill_sighandler` của `bench_crash_loss`; ô `compare` trong workflow `bench.yml`. Kết quả và kết luận: [docs/benchmarks.md](docs/benchmarks.md#so-với-spdlog-và-quill).

## 0.1.0 (2026-11-22)

Bản phát hành đầu tiên.

### Không tương thích

- **`full_policy` mặc định là `block`** (trước: `drop`). Không còn mất dòng khi quá tải (trước: tới 26% ở 16 thread flood); đổi lại lệnh log có lúc chờ vài ms. Hành vi cũ: `full_policy = drop`.
- **Bỏ backend `sync`, `mmap` và chế độ format trì hoãn.** Key `backend`, `mmap.region_size`, `format.mode` bị bỏ qua kèm cảnh báo.
- **ABI:** xóa `tblog_write_site()` và `tblog_site`; chương trình build với header cũ phải build lại. Version vẫn `0.1.0`.
- **Nén gzip cần zlib** (bỏ đường gọi lệnh `gzip` ngoài).
- **Makefile chỉ còn cho Linux**; CMake là cách build chính.

### Thêm

- **`flush.on_level`**: dòng ≥ level chỉ trả về khi đã ghi vào file. Với `ERROR`: sau `kill -9` (4 thread) mất TB 174 dòng INFO thay vì 4 612, 0 dòng tới dòng ERROR cuối; mỗi dòng ERROR tốn 21–148 µs, hoặc ~1 fsync khi kèm `sync.on_level = ERROR`.
- **`tblog_add_sink()`**: sink của người dùng. **`file = false`**: không ghi file.
- **Windows** (MSVC, MinGW-w64; tĩnh và DLL) và **macOS** 12+ (arm64, x86_64).
- **CMake package**: `find_package(tblog)`, `add_subdirectory`, FetchContent; target `tblog::tblog`, `_static`, `_shared`.
- **Benchmark so với `fprintf`**: `bench_vs_printf` (thông lượng, latency) và `bench_crash_loss` (mất log khi crash / `kill -9` / `exit`), workflow `bench.yml`.
- CI trên Linux (glibc, musl), macOS, Windows. Kết quả benchmark của Linux, macOS, Windows MSVC (GitHub Actions) nằm trong `bench/results/`.

### Sửa

- **Tràn stack trên thread phụ, Linux / macOS**: trước đây crash handler không chạy (thread không có alternate signal stack) và mất cả buffer (đo được TB 2 065–2 794 dòng). Giờ mỗi thread được cấp alternate stack 64 KB ở lệnh log đầu tiên của nó (một `mmap`, chưa tốn RAM thật tới khi có signal, gỡ khi thread kết thúc); các lệnh log sau không tốn thêm. Đo lại: 0 dòng mất ở 20/20 lần trên Linux và macOS. Thread chưa từng log thì vẫn chưa được bảo vệ. Windows không đổi.

### Sửa

- **Crash khi các thread khác vẫn log: 0 dòng mất** (trước: hàng trăm dòng mỗi lần). Dòng log sau lúc handler xả buffer được ghi thẳng vào file.
- **Crash không còn ghi lặp chunk đang ghi dở** (trước: thỉnh thoảng lặp cả chunk). Đo lại: 0 dòng lặp ở mọi kịch bản.
- Không còn mất dòng khi crash đúng lúc thread nền đang ghi.
- `make install` cài thiếu `detail/format.hpp`.
- Khi nhúng bằng `add_subdirectory`: không ghi đè `CMAKE_BUILD_TYPE` của project cha, nhận option đặt bằng `set()`.
