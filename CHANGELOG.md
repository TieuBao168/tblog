# Changelog

## Unreleased

### Thay đổi không tương thích

- **`full_policy` mặc định là `block`** (trước đây `drop`). Khi log nhanh hơn thread nền ghi được, lệnh log giờ chờ tới khi có chỗ trong buffer thay vì bỏ dòng: mặc định không còn mất dòng nào (đo trước đây: tới 26% số dòng bị bỏ ở 16 thread flood), đổi lại đuôi latency của lệnh log lúc quá tải (max vài ms). Muốn hành vi cũ (không bao giờ chờ, đếm và bỏ dòng): `full_policy = drop` ([docs/configuration.md](docs/configuration.md#full_policy)).
- **Bỏ backend `sync` và `mmap`** (v2) và chế độ format trì hoãn: chỉ còn một hàng đợi buffer. Các key `backend`, `mmap.region_size`, `format.mode` vẫn được đọc nhưng bị bỏ qua, kèm cảnh báo `<key>: option removed, ignored`.
- **ABI:** `tblog_write_site()` và kiểu `tblog_site` bị xóa khỏi API công khai; macro `TBLOG_*` giờ gọi `tblog_write()`. Chương trình build với header cũ phải build lại (nó tham chiếu `tblog_write_site`, không còn trong thư viện). Version (`0.1.0`, soname `libtblog.so.0`) chưa đổi.
- **Nén gzip cần zlib.** Bỏ đường dự phòng gọi lệnh `gzip -n` ngoài: build không có zlib thì `file.compress = gzip` thành `none`, kèm cảnh báo lúc init.
- **Makefile chỉ còn cho Linux.** CMake là cách build chính trên mọi nền tảng.

### Thêm

- **Key `flush.on_level`** (mặc định `OFF`, hot-reload được): dòng ở level này trở lên chỉ trả về khi thread nền đã `write()` nó vào file (và mọi sink khác), nên nó còn nguyên sau `kill -9`, `TerminateProcess`, `_exit`, OOM killer. Thread nền được đánh thức ngay; chờ tối đa 2 giây; không fsync (fsync vẫn do `sync.on_level` / `sync.interval_ms`). Dòng dưới level không tốn thêm gì. Đo lại với `flush.on_level = ERROR`: `kill -9` / `TerminateProcess` (20 lần, 4 thread) và `_Exit` (20 lần) không mất dòng nào tính tới dòng ERROR cuối cùng đã trả về, phần mất còn trung bình 173.5 / 56.1 dòng INFO so với 4 612 / 954 khi không bật; mỗi dòng ERROR tốn 17–148 µs (`sync.on_level = OFF`) hoặc khoảng một fsync khi dòng ERROR dày và `sync.on_level = ERROR` ([docs/configuration.md](docs/configuration.md#flushon_level), [docs/benchmarks.md](docs/benchmarks.md#kết-quả-mất-log)). `validate_config` cảnh báo khi đặt dưới `INFO`.
- Benchmark: `bench_crash_loss` có biến thể `tblog_flush_error` (`flush.on_level = ERROR`, mỗi dòng thứ 100 là ERROR) và đếm riêng dòng INFO / ERROR mất và số dòng mất tính tới dòng ERROR cuối cùng đã trả về; `bench_vs_printf` có kịch bản `flush_level` (0%, 1%, 10%, 100% dòng ERROR, `sync.on_level` = `ERROR` / `OFF`). Kết quả trong `bench/results/` được đo lại với mặc định mới.
- `docs/configuration.md` viết lại thành tài liệu tham chiếu cho từng key (mặc định, giá trị, khi nào đổi, đánh đổi, key liên quan, cảnh báo) kèm các cấu hình mẫu đã kiểm tra bằng parser; `log.conf` có đủ mọi key.
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
- **Crash trên một thread trong khi các thread khác vẫn log: không còn mất dòng.** Trước đây các dòng mà thread khác log sau lúc handler xả buffer (trong vài ms process còn đang chết) được báo là đã log nhưng không bao giờ được ghi: hàng trăm dòng mỗi lần. Giờ, khi handler đã bắt đầu xả, lệnh log chờ handler xong rồi tự ghi dòng thẳng vào file và console trước khi trả về (sau dòng `*** tblog: caught ... ***`, từng dòng một, không xen nhau). Đo lại (`bench_crash_loss`, 20 lần thử, 4 thread × 20 000 dòng/giây, thread thứ 5 crash): **0 dòng mất**, 0 dòng lặp ([docs/benchmarks.md](docs/benchmarks.md#kết-quả-mất-log)).
- **Crash không còn ghi lặp chunk đang ghi dở.** Handler chờ thread nền ghi xong lần ghi đang dở (có chặn thời gian) rồi xả tiếp đúng từ byte đã ghi tới; sau đó thread nền không ghi gì thêm. Đo lại: 0 dòng lặp ở mọi kịch bản (trước đây thỉnh thoảng lặp cả một chunk, ở `segv_other_thread` và `uncaught_exception`).
- `make install` cài cả `include/tblog/detail/format.hpp` (trước đó thiếu, nên `<tblog/tblog.hpp>` không dùng được từ bản cài bằng Makefile); `make uninstall` gỡ nó.
- Khi được nhúng bằng `add_subdirectory` / FetchContent: tblog không còn ghi đè `CMAKE_BUILD_TYPE` của project cha, nhận các option đặt bằng `set()` trước đó (CMP0077), và trên Windows ghi `tblog.dll` vào `CMAKE_RUNTIME_OUTPUT_DIRECTORY` của project cha nếu có.
