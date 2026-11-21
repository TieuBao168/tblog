# tblog

![CI](https://github.com/TieuBao168/tblog/actions/workflows/ci.yml/badge.svg)

Thư viện log bất đồng bộ cho C và C++ trên Linux, macOS và Windows. Thread gọi log chỉ so level, format dòng và chép vào buffer trong RAM (khoảng 0.3–0.6 µs, không system call). Một thread nền ghi file, xoay file, nén file cũ; một thread khác fsync. Khi chương trình crash, các dòng còn trong buffer vẫn được ghi ra file trước khi process chết.

- **API C** (`<tblog/log.h>`) kiểu `printf`, dùng được từ C99 và mọi chuẩn C++; **API C++** dạng `{}` (`<tblog/tblog.hpp>`, header-only, C++11) với `tblog::Logger` và kiểu tự định nghĩa ([docs/api.md](docs/api.md)).
- **Level theo module** (`module.network = DEBUG`). Lệnh bị lọc không format, không tính đối số (dưới 1 ns); có thể xóa hẳn khỏi binary bằng `TBLOG_COMPILE_LEVEL`.
- **Sink:** file (mặc định), console (stderr, có màu), syslog (Linux, macOS), và **sink riêng** của bạn qua `tblog_add_sink()`; `file = false` để không ghi file.
- **Xoay file** theo dung lượng, theo ngày hoặc theo giờ, giữ N file; **nén gzip** file đã xoay trên thread nền (cần zlib), an toàn khi crash giữa chừng.
- **Crash handler**: SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL (Windows: SEH exception, `abort()`): buffer được xả vào file và console, kèm dòng `*** tblog: caught ... ***` và call stack; mọi dòng mà lệnh log đã trả về đều có trong file, kể cả của các thread khác đang log lúc crash.
- **Độ bền chỉnh được:** buffer đầy thì chờ (`full_policy = block`, mặc định) hoặc bỏ dòng (`drop`); `flush.on_level = ERROR` để dòng lỗi đã nằm trong file khi lệnh log trả về (sống sót qua `kill -9`); fsync định kỳ và ngay sau dòng lỗi (`sync.*`); `tblog_flush()`.
- **File cấu hình `key = value`**, đọc lại khi file đổi (hot-reload): level, module, pattern, chu kỳ ghi / sync, `flush.on_level`.
- **tblog-lite** cho vi điều khiển (C99, không malloc), cùng tên macro: [docs/lite.md](docs/lite.md).

## Nền tảng hỗ trợ

| Nền tảng | Compiler | CI |
|---|---|---|
| Linux x86-64, glibc | GCC, Clang | Có: C++11/17, ASan+UBSan, TSan, không zlib |
| Linux x86-64, musl (Alpine) | GCC | Có. Crash handler không in call stack (musl không có `backtrace`). |
| macOS 12+, arm64 và x86_64 | AppleClang | Có: arm64 (Release, ASan+UBSan, TSan), x86_64 (C++11) |
| Windows 8+, x64 | MSVC (C++14 trở lên), MinGW-w64 GCC | Có: MSVC Release/Debug, MinGW C++11 |
| Windows ARM64 | MSVC | Chỉ kiểm tra biên dịch header công khai |

Ứng dụng dùng thư viện: C99 trở lên (MSVC: C11), C++11 trở lên. Bản thân thư viện build được bằng C++11, 14 hoặc 17 (`TBLOG_CXX_STANDARD`). Cần CMake 3.10 trở lên; zlib là tùy chọn. Ghi chú riêng cho từng hệ điều hành: [docs/platforms.md](docs/platforms.md).

## Bắt đầu nhanh

C:

```c
#include <tblog/log.h>

int main(void) {
    tblog_init("log.conf");                 /* NULL = cấu hình mặc định */
    TBLOG_INFO("user %d logged in", 42);
    TBLOG_MWARN("network", "retry %d", 3);  /* log theo module "network" */
    tblog_shutdown();                       /* xả hết buffer, fsync, dừng thread nền */
    return 0;
}
```

C++:

```cpp
#include <tblog/tblog.hpp>

int main() {
    tblog_init("log.conf");
    TBLOGF_INFO("user {} logged in from {}", 42, "10.0.0.7");
    TBLOGF_MWARN("network", "retry {}/{} after {:.1f}s", 2, 5, 0.25);

    tblog::Logger net("network");          // gắn với một module
    net.info("connected to {}:{}", "example.org", 443);
    tblog_shutdown();
}
```

Với pattern mặc định, mỗi dòng log có dạng:

```
2026-11-16 12:47:56.829576 [INFO] [81148] main.c:5 - user 42 logged in
```

## Tích hợp vào project

**CMake, bản đã cài** (`cmake --install`):

```cmake
find_package(tblog 0.1 REQUIRED)       # set(TBLOG_USE_STATIC ON) trước dòng này để dùng bản tĩnh
target_link_libraries(app PRIVATE tblog::tblog)
```

**CMake, từ mã nguồn** (`add_subdirectory`, hoặc FetchContent từ CMake 3.14):

```cmake
set(TBLOG_BUILD_SHARED OFF)            # tùy chọn: tblog::tblog là bản tĩnh
add_subdirectory(third_party/tblog)    # hoặc FetchContent_Declare(tblog GIT_REPOSITORY https://github.com/TieuBao168/tblog.git GIT_TAG main)
target_link_libraries(app PRIVATE tblog::tblog)
```

Ba target: `tblog::tblog` (mặc định là bản động), `tblog::tblog_static`, `tblog::tblog_shared`. Trên Windows, nếu project cha đặt `CMAKE_RUNTIME_OUTPUT_DIRECTORY` thì `tblog.dll` được ghi vào đó. **pkg-config** (Linux, macOS): `cc app.c $(pkg-config --cflags --libs tblog)`. Option CMake, tên file trên từng nền tảng, cài đặt, Makefile: [docs/build.md](docs/build.md).

## Cấu hình

Không có file cấu hình thì mọi key lấy mặc định: log `INFO` trở lên vào `./logs/app.log`, xoay ở 256 MB, giữ 5 file. Các key hay dùng nhất:

| Key | Mặc định | Dùng để |
|---|---|---|
| `level`, `module.<tên>` | `INFO` | Level gốc / level riêng của một module |
| `file.path` | `./logs/app.log` | File log (nên đặt đường dẫn tuyệt đối) |
| `file.max_size`, `file.max_files` | `256MB`, `5` | Dung lượng mỗi file, số file giữ lại |
| `file.rotate`, `file.compress` | `size`, `none` | Xoay theo `daily` / `hourly`; nén `gzip` |
| `full_policy` | `block` | Buffer đầy: chờ (`block`) hay bỏ dòng (`drop`) |
| `flush.on_level` | `OFF` | Dòng ≥ level này chỉ trả về khi đã ghi vào file: sống sót qua `kill -9` |
| `sync.on_level`, `sync.interval_ms` | `ERROR`, `1000` | fsync ngay sau dòng ≥ level / định kỳ: sống sót qua mất điện |
| `console`, `syslog` | `false` | Ghi thêm ra stderr / syslog |

```ini
# log.conf: dịch vụ phải giữ dòng lỗi cả khi bị kill -9
level          = INFO
file.path      = /var/log/myservice/app.log
file.rotate    = daily
file.max_files = 31
flush.on_level = ERROR
module.network = DEBUG
```

Mọi key (mặc định, giá trị hợp lệ, khi nào đổi, đánh đổi, hot-reload, cảnh báo) và các cấu hình mẫu (latency thấp, embedded trên flash, desktop, chỉ console, xoay theo ngày + gzip): [docs/configuration.md](docs/configuration.md). File mẫu có đủ mọi key: [log.conf](log.conf).

## Hiệu năng và độ an toàn khi crash

So với `fprintf` vào một file (cùng dòng log, có timestamp). Máy: AMD Ryzen 7 8845HS (16 CPU logic), Windows 11, NVMe, MinGW-w64 g++ 15.1 `-O2`, tblog tĩnh, cấu hình mặc định chỉ ghi file; median của 5 lần chạy, 1 triệu dòng mỗi lần ([kết quả JSON](bench/results/)). End-to-end tính tới khi mọi dòng đã ghi và fsync.

| Thread | Biến thể | Thread gọi log p50 / p99 | End-to-end (dòng/s) |
|---|---|---|---|
| 1 | `fprintf` (buffer stdio) | 2.03 µs / 6.9 µs | 365 k |
| 1 | `fprintf` + `fflush` mỗi dòng | 4.95 µs / 9.64 µs | 179 k |
| 1 | tblog (`full_policy = block`, mặc định) | 311 ns / 591 ns | 1.89 M |
| 1 | tblog, `full_policy = drop` | 311 ns / 601 ns | 1.82 M |
| 8 | `fprintf` (buffer stdio) | 8.56 µs / 330 µs | 213 k |
| 8 | `fprintf` + `fflush` mỗi dòng | 14.1 µs / 832 µs | 124 k |
| 8 | tblog (`block`, mặc định) | 561 ns / 671 ns | 4.34 M |
| 8 | tblog, `drop` | 561 ns / 671 ns | 4.48 M (bỏ 9.4 k dòng / 1 M) |

Chi phí của một dòng ERROR với `flush.on_level = ERROR` (lệnh chỉ trả về khi dòng đã nằm trong file), 1% số dòng là ERROR: 21 µs / 89 µs (p50 / p99) ở 1 thread, 148 µs / 428 µs ở 8 thread khi `sync.on_level = OFF`; với `sync.on_level = ERROR` mặc định thì khoảng một fsync: 2.69 ms / 4.34 ms và 5.4 ms / 15.9 ms. Dòng INFO xen giữa vẫn 311–561 ns p50.

Số dòng mất khi process chết (trung bình / lớn nhất của 20 lần thử, 4 thread hoặc 1 thread, 20 000 dòng/giây mỗi thread; cột cuối: cùng cấu hình thêm `flush.on_level = ERROR`, mỗi dòng thứ 100 là ERROR):

| Kịch bản | `fprintf` | `fprintf` + `fflush` | tblog | tblog + `flush.on_level = ERROR` |
|---|---|---|---|---|
| SIGSEGV trên thread đang log | 24 / 41 | 0 | 0 | 0 |
| SIGSEGV trên thread khác, 4 thread đang log | 19 / 39 | 0 | 0 | 0 |
| `abort`, `assert`, exception không ai bắt, `exit` | 0 | 0 | 0 | 0 |
| Stack overflow | 17 / 38 | 0 | 0 | 0 |
| `kill -9` / `TerminateProcess`, 4 thread | 22 / 41 | 0 | 4 612 / 7 751 | 174 / 396, **0 tới dòng ERROR cuối** |
| `_Exit` (không chạy `atexit`) | 0 | 0 | 954 / 1 811 | 56 / 96, **0 tới dòng ERROR cuối** |

Khi crash, không dòng nào bị ghi lặp. Sau khi handler đã xả buffer, thread khác log tiếp thì dòng được ghi thẳng vào file (và console) trước khi lệnh trả về, nằm sau dòng `*** tblog: caught ... ***`.

Điểm yếu, đo được ở trên:

- `kill -9`, `_Exit` với cấu hình mặc định: tblog mất phần còn trong buffer, tới 1 939 dòng mỗi thread (khoảng 100 ms log), so với tối đa 41 của `fprintf`. `flush.on_level = ERROR` giữ được dòng lỗi và mọi dòng trước nó, nhưng các dòng INFO sau dòng lỗi cuối vẫn có thể mất (tới 99 mỗi thread ở đây).
- `flush.on_level` + `sync.on_level` cùng level trên Windows: dòng ERROR tới dày thì mỗi dòng chờ cả một fsync (2.5–5.4 ms), vì `WriteFile` phải chờ `FlushFileBuffers` đang chạy. Trên Linux / macOS chưa đo.
- `fprintf` của MinGW chậm bất thường; `snprintf` + `fwrite` một thread chỉ chậm hơn tblog khoảng 20% (381 so với 311 ns). Trên glibc / MSVC chưa đo ở máy này.
- Trần khoảng 4–5 M dòng/s (một thread nền ghi file). Vượt trần: `block` (mặc định) làm thread gọi log chờ (max 2.88 ms ở 16 thread); `drop` bỏ tới 33% số dòng ở 16 thread.
- Mất điện: không biến thể nào an toàn tuyệt đối (tblog fsync mỗi giây và sau dòng ERROR; `fprintf` không fsync).

Cách đo, bảng đầy đủ (16 thread, latency open-loop, lệnh bị lọc, chi phí `flush.on_level` theo tỉ lệ dòng ERROR), giải thích và cách chạy lại: [docs/benchmarks.md](docs/benchmarks.md).

## Tài liệu

| File | Nội dung |
|---|---|
| [docs/configuration.md](docs/configuration.md) | Tham chiếu mọi key của `log.conf`, độ bền theo loại sự cố, hot-reload, cảnh báo, cấu hình mẫu |
| [docs/api.md](docs/api.md) | API C và C++, sink riêng, `TBLOG_COMPILE_LEVEL`, FATAL, thread-safety |
| [docs/architecture.md](docs/architecture.md) | Đường đi của một dòng log, các thread, `flush.on_level`, độ bền khi crash / `kill -9` / mất điện |
| [docs/benchmarks.md](docs/benchmarks.md) | So với `fprintf`: thông lượng, latency, số dòng mất khi crash / `kill -9` / `exit`; cách đo, cách chạy lại |
| [docs/build.md](docs/build.md) | Option CMake, build trên từng nền tảng, sanitizer, cài đặt, pkg-config, Makefile, cross-compile |
| [docs/platforms.md](docs/platforms.md) | Ghi chú và giới hạn trên Linux, macOS, Windows, embedded Linux |
| [docs/testing.md](docs/testing.md) | Chạy test, các bộ test, test bị bỏ qua, danh sách job CI |
| [docs/lite.md](docs/lite.md) | tblog-lite cho vi điều khiển |
| [CHANGELOG.md](CHANGELOG.md) | Thay đổi so với các bản trước |

## Giấy phép

MIT, xem [LICENSE](LICENSE).
