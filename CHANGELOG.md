# Changelog

## Unreleased

### Thay đổi không tương thích

- **Bỏ backend `sync` và `mmap`** (v2) và chế độ format trì hoãn: chỉ còn một hàng đợi buffer. Các key `backend`, `mmap.region_size`, `format.mode` vẫn được đọc nhưng bị bỏ qua, kèm cảnh báo `<key>: option removed, ignored`.
- **ABI:** `tblog_write_site()` và kiểu `tblog_site` bị xóa khỏi API công khai; macro `TBLOG_*` giờ gọi `tblog_write()`. Chương trình build với header cũ phải build lại (nó tham chiếu `tblog_write_site`, không còn trong thư viện). Version (`0.1.0`, soname `libtblog.so.0`) chưa đổi.
- **Nén gzip cần zlib.** Bỏ đường dự phòng gọi lệnh `gzip -n` ngoài: build không có zlib thì `file.compress = gzip` thành `none`, kèm cảnh báo lúc init.
- **Makefile chỉ còn cho Linux.** CMake là cách build chính trên mọi nền tảng.

### Thêm

- **`tblog_add_sink(write, flush, ctx)`**: sink của người dùng, nhận mọi dòng log song song với file, console và syslog ([docs/api.md](docs/api.md#sink-riêng-tblog_add_sink)).
- **Key `file = true|false`**: `false` để không ghi file (chỉ console / syslog / sink riêng). Không còn sink nào thì console được bật kèm cảnh báo.
- **Windows** (Windows 8+, x64): MSVC và MinGW-w64, bản tĩnh (`tblog_static.lib` / `libtblog_static.a`) và DLL (`tblog.dll`, `TBLOG_DLL` cho dllimport). Đường dẫn UTF-8, xoay file khi file đang mở, crash handler bằng SEH và `SIGABRT`, xả log không join thread khi DLL bị gỡ lúc thoát process. Syslog không được hỗ trợ.
- **macOS** 12+ (arm64, x86_64), AppleClang: bản tĩnh và dylib (`@rpath/libtblog.0.dylib`, chỉ export `tblog_*`), sync bằng `F_FULLFSYNC`.
- **Header công khai portable:** `<tblog/log.h>` và `<tblog/tblog.hpp>` biên dịch sạch với GCC, Clang, AppleClang, MSVC (x86, x64, ARM64; C11 / C++14 trở lên) và MinGW-w64. Macro `TBLOG_API` (export/import), kiểm tra format kiểu printf trên từng compiler.
- **CMake là cách build chính:** `find_package(tblog)` với các target `tblog::tblog`, `tblog::tblog_static`, `tblog::tblog_shared` (`TBLOG_USE_STATIC` để chọn bản tĩnh), dùng được bằng `add_subdirectory` / FetchContent, các option `TBLOG_BUILD_SHARED`, `TBLOG_CXX_STANDARD`, `TBLOG_WITH_ZLIB`, `TBLOG_SANITIZE`, `TBLOG_WERROR`... ([docs/build.md](docs/build.md)).
- CI trên GitHub Actions: Linux (glibc, musl), macOS, Windows ([docs/testing.md](docs/testing.md#ci)).
- **So sánh với `printf`** ([docs/benchmarks.md](docs/benchmarks.md)): `bench/bench_vs_printf` (thông lượng, latency mỗi lệnh, lệnh bị lọc theo level) và `bench/bench_crash_loss` (số dòng mất khi crash, `kill -9`, `exit`, `_Exit`), kết quả JSON + `bench/report.py`, workflow chạy tay `bench.yml`. Thay cho `bench/bench_latency` (test `bench_latency_smoke` thành `bench_vs_printf_smoke` và `bench_crash_loss_smoke`, chạy trên mọi nền tảng).

### Sửa

- Không còn mất dòng khi crash xảy ra đúng lúc thread nền đang ghi một lô.
- `make install` cài cả `include/tblog/detail/format.hpp` (trước đó thiếu, nên `<tblog/tblog.hpp>` không dùng được từ bản cài bằng Makefile); `make uninstall` gỡ nó.
- Khi được nhúng bằng `add_subdirectory` / FetchContent: tblog không còn ghi đè `CMAKE_BUILD_TYPE` của project cha, nhận các option đặt bằng `set()` trước đó (CMP0077), và trên Windows ghi `tblog.dll` vào `CMAKE_RUNTIME_OUTPUT_DIRECTORY` của project cha nếu có.
