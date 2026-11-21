# Kiểm thử

## Chạy test

CMake (mọi nền tảng):

```bash
cmake -S . -B build -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON -DTBLOG_BUILD_BENCH=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure          # MSVC: thêm -C Release
ctest --test-dir build -R test_config -V            # một test, in toàn bộ output
cmake -DBUILD_DIR=build -P packaging/check-test-inventory.cmake   # đủ test chưa (MSVC: -DCONFIG=Release)
sh packaging/report-skips.sh build                  # test case nào bị bỏ qua
```

Có thể chạy thẳng một file test (`build/tests/test_config`, Windows: `build/bin/<Config>/test_config.exe`). Mỗi test CTest có thư mục nháp riêng qua biến `TBLOG_TEST_DIR`, nên `ctest -j` chạy song song được.

Sanitizer: thư mục build riêng với `-DTBLOG_SANITIZE=address,undefined` hoặc `-DTBLOG_SANITIZE=thread` ([build.md](build.md#sanitizer)).

Makefile (Linux):

```bash
make test                  # tests/unit/*
make STD=c++11 test
make SAN=address test      # hoặc SAN=thread, SAN=undefined
make check-symbols check-install
make -C lite test test-asan
```

## Các bộ test

Test không cần thư viện ngoài: `tests/tiny_test.h` (khung test), `tests/test_os.h` (thao tác file/process dùng cho test, chạy trên POSIX và Windows). Test cần process con (crash, thoát process) chạy lại chính file test với `--tiny-child=<tên>` thay vì `fork`, nên chạy được cả trên Windows.

| CTest | Nguồn | Link với | Nội dung |
|---|---|---|---|
| `test_config` | `tests/unit/test_config.cpp` | bản tĩnh | Parser, `validate_config`, watcher, key đã bỏ |
| `test_core` | `tests/unit/test_core.cpp` | bản tĩnh | Level, module, thread context, formatter |
| `test_cpp_api` | `tests/unit/test_cpp_api.cpp` | bản tĩnh | API `{}`, `Logger`, kiểu tự định nghĩa, dấu lỗi (phải build được ở C++11) |
| `test_crash_sinks` | `tests/unit/test_crash_sinks.cpp` | bản tĩnh | Crash handler qua Pipeline + Queue thật, console, syslog |
| `test_integration` | `tests/unit/test_integration.cpp` | bản tĩnh | Đầu cuối qua API C: SIGSEGV giữ đủ log, hot-reload, `tblog_add_sink`, `file = false` |
| `test_queue_file` | `tests/unit/test_queue_file.cpp` | bản tĩnh | Queue (thứ tự, drop/block, đánh thức, crash drain), RotatingFile, Pipeline |
| `test_rotation_time` | `tests/unit/test_rotation_time.cpp` | bản tĩnh | Xoay theo ngày/giờ với đồng hồ giả, giữ file, nén gzip và khôi phục sau crash |
| `test_shared_exit` | `tests/shared/test_shared_exit.cpp` | bản động | Thoát process không gọi `tblog_shutdown` (Windows: đường DLL unload), crash trong DLL/.so |
| `example_basic`, `example_cpp_api` | `examples/*` | `tblog::tblog` | Ví dụ chạy được với một `log.conf` trong thư mục nháp |
| `bench_latency_smoke` | `bench/bench_latency.cpp` | bản tĩnh | Chạy ngắn (2000 dòng/thread), không phải phép đo; không có trên Windows |
| `check_symbols` | `packaging/check-symbols.sh` | | `.so`/`.dylib` export đúng các hàm `tblog_*` mà `.a` định nghĩa; không có trong build sanitizer |
| `check_exports` | `packaging/check-exports.ps1` | | Windows: DLL export đúng các hàm `TBLOG_API` |

`tests/header_check/check_c.c` và `check_cpp.cpp` dùng mọi macro và hàm công khai; chúng chỉ được biên dịch (bản thường và bản `TBLOG_DLL`, warning là lỗi), không chạy. Biên dịch được là đạt.

Build không có zlib: các test nén gzip không được biên dịch (`#ifdef TBLOG_HAVE_ZLIB`), không tính là bị bỏ qua.

## Test bị bỏ qua

`TINY_SKIP("lý do")` kết thúc test case với `[ SKIP ] tên (lý do)`, không tính là đạt hay hỏng. Đặt biến môi trường `TBLOG_TEST_NO_SKIP=1` thì skip thành lỗi (`[ FAIL ] tên (skipped: ...)`). CI đặt biến này ở mọi job Linux: trên Linux mọi test phải thật sự chạy.

| Test case | Linux | macOS | Windows | Lý do |
|---|---|---|---|---|
| `test_queue_file` / `rotating_write_error_counts_loss` | chạy | bỏ qua | bỏ qua | Cần `/dev/full` để giả lập đĩa đầy |
| `test_crash_sinks` / `syslog_sink_handles_batches` | chạy | chạy | bỏ qua | Windows không có syslog |

## CI

[.github/workflows/ci.yml](../.github/workflows/ci.yml) chạy với mỗi push và pull request vào `main`, trừ khi chỉ có file `*.md` hoặc `docs/` thay đổi (`log.conf` và chính workflow vẫn kích hoạt CI).

| Job | Nền tảng | Nội dung |
|---|---|---|
| `platform boundary` | Ubuntu | `packaging/check-platform-boundary.sh` |
| `cmake / ...` | Ubuntu | GCC C++17, GCC C++11, Clang C++17, GCC ASan+UBSan, GCC TSan, GCC không zlib; tất cả `-Werror`, test + ví dụ + bench, `TBLOG_TEST_NO_SKIP=1` |
| `alpine (musl)` | Alpine 3.20 | GCC C++17 `-Werror`, toàn bộ CTest, `TBLOG_TEST_NO_SKIP=1` |
| `cmake-macos / ...` | macOS | arm64 Release `-Werror` (+ cài đặt, pkg-config, `find_package`, kiểm tra `@rpath`), arm64 ASan+UBSan, arm64 TSan, x86_64 Release C++11 không zlib |
| `cmake-windows / ...` | Windows | MSVC x64 Release không zlib (+ cài đặt, `find_package`, `add_subdirectory`), MSVC x64 Debug C++14 zlib (vcpkg) `/WX`, MinGW-w64 Release C++11 zlib `-Werror` |
| `headers / msvc arm64` | Windows | Chỉ biên dịch header công khai (ARM64, cross-compile) |
| `install` | Ubuntu | `cmake --install`, rồi `packaging/check-install.sh` (pkg-config, tĩnh + động), `packaging/consumer` với `find_package` và với `add_subdirectory` |
| `make` | Ubuntu | `make test`, `make check-symbols`, `make check-install`, `TBLOG_TEST_NO_SKIP=1` |
| `tblog-lite` | Ubuntu | `make -C lite test`, `test-asan`, `demo`, `size` |

## Các chốt chặn độ phủ

Để một test không thể lặng lẽ rơi khỏi CI:

- **`packaging/check-test-inventory.cmake`** chạy sau `ctest` trong mọi job CMake có test (Linux, Alpine, macOS, Windows). Nó so danh sách test đã đăng ký (`ctest -N`) với danh sách phải có: mỗi `tests/unit/*.cpp` và `tests/shared/*.cpp`, `example_<tên>` cho mỗi file trong `examples/`, `bench_latency_smoke` (có bench, không phải Windows), `check_symbols` (không phải Windows, không sanitizer), `check_exports` (Windows). Thiếu hoặc thừa một test thì job fail với `check-test-inventory: FAIL`, kèm danh sách `not registered:` / `unexpected:`.
- **`TBLOG_TEST_NO_SKIP=1`** trên Linux: một test case bị bỏ qua làm test đó fail.
- **`packaging/report-skips.sh`** chạy ở mọi job CMake (kể cả khi test fail): in mọi test case bị bỏ qua trong lần `ctest` vừa rồi, và ghi vào job summary. Chỉ để thông tin, luôn trả về 0.
- **`lite/Makefile`**: `make -C lite test` fail với `tblog-lite: not in TESTS: ...` nếu có `lite/tests/test_*.c` không được build và chạy.
- **`check_symbols` / `check_exports`**: một hàm công khai thiếu `TBLOG_API`, hoặc một symbol nội bộ lọt ra ngoài, làm test fail.
- **`check-platform-boundary.sh`**: header hay lời gọi OS ngoài `src/platform/` làm job fail.
