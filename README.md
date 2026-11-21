# tblog

![CI](https://github.com/TieuBao168/tblog/actions/workflows/ci.yml/badge.svg)

Thư viện log bất đồng bộ cho C và C++ trên Linux, macOS và Windows. Thread gọi log chỉ format dòng log rồi chép vào buffer trong RAM. Mọi việc chạm tới đĩa (ghi file, fsync, xoay file, nén file cũ) đều do thread nền làm. Khi chương trình crash, các dòng còn trong buffer vẫn được ghi ra file trước khi process chết.

- **API C** (`<tblog/log.h>`) kiểu `printf`, dùng được từ C99 và mọi chuẩn C++. Thêm **API C++ dạng `{}`** (`<tblog/tblog.hpp>`, header-only, C++11).
- **Level theo module**: `module.network = DEBUG`. Macro bị lọc không format và không tính đối số. Có thể xóa hẳn khỏi binary lúc biên dịch bằng `TBLOG_COMPILE_LEVEL`.
- **Xoay file** theo dung lượng, theo ngày hoặc theo giờ. Có thể **nén gzip** các file đã xoay trên thread nền (cần zlib).
- **File cấu hình `key = value`**. Level, pattern và chu kỳ flush/sync đổi được lúc đang chạy (hot-reload).
- **Crash handler**: SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL (Windows: SEH exception và `abort()`). Log còn trong buffer được xả vào file và console trước khi process chết, kèm một dòng báo crash ([độ bền](docs/architecture.md#độ-bền-của-log)).
- **Sink riêng** qua `tblog_add_sink()`: nhận mọi dòng log song song với file, console và syslog.
- Có bản **tblog-lite** cho vi điều khiển (C99, không malloc), dùng cùng tên macro: xem [docs/lite.md](docs/lite.md).

## Nền tảng hỗ trợ

| Nền tảng | Compiler | CI |
|---|---|---|
| Linux x86-64, glibc | GCC, Clang | Có: C++11/17, ASan+UBSan, TSan, không zlib |
| Linux x86-64, musl (Alpine) | GCC | Có. Crash handler không in call stack (musl không có `backtrace`). |
| macOS 12+, arm64 và x86_64 | AppleClang | Có: arm64 (Release, ASan+UBSan, TSan), x86_64 (C++11) |
| Windows 8+, x64 | MSVC (C++14 trở lên), MinGW-w64 GCC | Có: MSVC Release/Debug, MinGW C++11 |
| Windows ARM64 | MSVC | Chỉ kiểm tra biên dịch header công khai |

Ứng dụng dùng thư viện: C99 trở lên (MSVC: C11), C++11 trở lên. Bản thân thư viện build được bằng C++11, 14 hoặc 17 (`TBLOG_CXX_STANDARD`). Cần CMake 3.10 trở lên. zlib là tùy chọn. Ghi chú riêng cho từng hệ điều hành nằm ở [docs/platforms.md](docs/platforms.md).

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

**CMake, từ mã nguồn** (không cần cài; CI kiểm tra cách này với `add_subdirectory`):

```cmake
set(TBLOG_BUILD_SHARED OFF)            # tùy chọn: tblog::tblog là bản tĩnh
add_subdirectory(third_party/tblog)
target_link_libraries(app PRIVATE tblog::tblog)
```

Hoặc dùng FetchContent (CMake 3.14 trở lên), cơ chế giống `add_subdirectory`:

```cmake
include(FetchContent)
FetchContent_Declare(tblog GIT_REPOSITORY https://github.com/TieuBao168/tblog.git GIT_TAG main)
FetchContent_MakeAvailable(tblog)
target_link_libraries(app PRIVATE tblog::tblog)
```

Có ba target: `tblog::tblog` (mặc định là bản động), `tblog::tblog_static` và `tblog::tblog_shared`. Trên Windows, nếu project cha đặt `CMAKE_RUNTIME_OUTPUT_DIRECTORY` thì `tblog.dll` được ghi vào cùng thư mục đó.

**pkg-config** (Linux, macOS):

```bash
cc app.c $(pkg-config --cflags --libs tblog) -o app                 # bản động
cc app.c $(pkg-config --static --cflags --libs tblog | sed 's/-ltblog/-l:libtblog.a/') -o app   # bản tĩnh, Linux
```

Cách build, các option CMake, tên file trên từng nền tảng và cách cài: xem [docs/build.md](docs/build.md).

## Cấu hình tối thiểu

```ini
# log.conf
level          = INFO
file.path      = ./logs/app.log
file.max_size  = 64MB
file.max_files = 5
console        = true
module.network = DEBUG
```

Mọi key, giá trị mặc định và quy tắc hot-reload: [docs/configuration.md](docs/configuration.md). File mẫu đầy đủ: [log.conf](log.conf).

## Tài liệu

| File | Nội dung |
|---|---|
| [docs/build.md](docs/build.md) | Option CMake, build trên từng nền tảng, sanitizer, cài đặt, pkg-config, Makefile, cross-compile |
| [docs/api.md](docs/api.md) | API C và C++, sink riêng, `TBLOG_COMPILE_LEVEL`, FATAL, thread-safety |
| [docs/configuration.md](docs/configuration.md) | Mọi key của `log.conf`, hot-reload, token của pattern, xoay file, nén |
| [docs/architecture.md](docs/architecture.md) | Đường đi của một dòng log, các thread, độ bền khi crash / `kill -9` / mất điện |
| [docs/platforms.md](docs/platforms.md) | Ghi chú và giới hạn trên Linux, macOS, Windows, embedded Linux |
| [docs/testing.md](docs/testing.md) | Chạy test, các bộ test, test bị bỏ qua, danh sách job CI |
| [docs/lite.md](docs/lite.md) | tblog-lite cho vi điều khiển |
| [CHANGELOG.md](CHANGELOG.md) | Thay đổi so với các bản trước |

## Giấy phép

MIT, xem [LICENSE](LICENSE).
