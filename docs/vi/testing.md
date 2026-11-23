# Kiểm thử

[English](../testing.md) | **Tiếng Việt**

```bash
cmake -S . -B build -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON -DTBLOG_BUILD_BENCH=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure      # MSVC: -C Release
ctest --test-dir build -R test_config -V        # một test
```

Mỗi test có thư mục nháp riêng, nên `ctest -j` chạy song song được. Linux: `make test`, `make SAN=address test`, `make -C lite test`.

## Các bộ test

Không cần thư viện ngoài (`tests/tiny_test.h`). Test crash và thoát process chạy lại chính file test làm process con (không dùng `fork`), nên chạy được trên cả Windows.

| Test | Kiểm tra |
|---|---|
| `test_config` | Parser, cảnh báo, giá trị mặc định, hot-reload, giới hạn `file.mode` / `file.max_files` |
| `test_core` | Level, module, formatter, `message.escape` (một dòng, trong giới hạn dòng) |
| `test_cpp_api` | API `{}`, `Logger`, kiểu tự định nghĩa (build ở C++11) |
| `test_queue_file` | Buffer (thứ tự, drop/block, crash), khóa shard (có giới hạn, dừng khi đã đóng), xoay file, từ chối link, `file.mode`, `flush.on_level` không treo |
| `test_crash_sinks` | 4 thread log, thread thứ 5 crash: mọi dòng đã trả về có trong file và console đúng một lần |
| `test_integration` | Đầu cuối qua API C; process bị giết cứng ngay sau dòng ERROR (10 lần): 0 dòng mất tới dòng ERROR cuối |
| `test_rotation_time` | Xoay theo ngày/giờ (đồng hồ giả), nén gzip, khôi phục sau crash |
| `test_shared_exit` | Bản động: thoát không gọi `tblog_shutdown`, crash trong DLL/.so |
| `example_*`, `bench_*_smoke` | Ví dụ và benchmark chạy hết (không phải phép đo) |
| `check_symbols`, `check_exports` | Thư viện chỉ export các hàm công khai |

Test bị bỏ qua: `rotating_write_error_counts_loss` trên macOS / Windows (không có `/dev/full`), `syslog_sink_handles_batches` trên Windows, các test link và quyền file của `test_queue_file` trên Windows (link và quyền kiểu POSIX). Test link với gzip chỉ có trong bản build có zlib. Bản TSan bỏ kịch bản stack overflow của `bench_crash_loss_smoke`.

## CI

[ci.yml](../../.github/workflows/ci.yml) chạy với mỗi push và pull request vào `main`, trừ khi chỉ đổi `*.md` / `docs/`.

| Nền tảng | Cấu hình |
|---|---|
| Ubuntu | GCC C++17, GCC C++11, Clang, ASan+UBSan, TSan, không zlib, Makefile, cài đặt, ranh giới `src/platform/` |
| Alpine (musl) | GCC C++17 |
| macOS | arm64 Release, ASan+UBSan, TSan; x86_64 C++11 không zlib |
| Windows | MSVC Release, MSVC Debug C++14 + zlib, MinGW C++11 + zlib; ARM64 chỉ header |
| tblog-lite | `test`, `test-asan`, `demo`, `size` |

Mọi job dùng warning-là-lỗi. Chốt chặn để test không lặng lẽ rơi khỏi CI:

- `TBLOG_TEST_NO_SKIP=1` trên Linux: test bị bỏ qua thành fail.
- `packaging/check-test-inventory.cmake`: thiếu hoặc thừa test so với danh sách phải có thì fail.
- `packaging/report-skips.sh`: in mọi test bị bỏ qua vào job summary.

[bench.yml](../../.github/workflows/bench.yml) chạy tay, xem [benchmarks.md](benchmarks.md#chạy-lại).
