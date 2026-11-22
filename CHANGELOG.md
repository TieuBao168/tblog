# Changelog

## Unreleased

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

### Sửa (chờ đo lại trên Linux / macOS)

- **Tràn stack trên thread phụ, Linux / macOS**: trước đây crash handler không chạy (thread không có alternate signal stack) và mất cả buffer (đo được TB 2 065–2 794 dòng). Giờ mỗi thread được cấp alternate stack 64 KB ở lệnh log đầu tiên của nó (một `mmap`, chưa tốn RAM thật tới khi có signal, gỡ khi thread kết thúc); các lệnh log sau không tốn thêm. Thread chưa từng log thì vẫn chưa được bảo vệ. Windows không đổi.

### Sửa

- **Crash khi các thread khác vẫn log: 0 dòng mất** (trước: hàng trăm dòng mỗi lần). Dòng log sau lúc handler xả buffer được ghi thẳng vào file.
- **Crash không còn ghi lặp chunk đang ghi dở** (trước: thỉnh thoảng lặp cả chunk). Đo lại: 0 dòng lặp ở mọi kịch bản.
- Không còn mất dòng khi crash đúng lúc thread nền đang ghi.
- `make install` cài thiếu `detail/format.hpp`.
- Khi nhúng bằng `add_subdirectory`: không ghi đè `CMAKE_BUILD_TYPE` của project cha, nhận option đặt bằng `set()`.
