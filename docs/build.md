# Build và cài đặt

CMake là cách build chính trên mọi nền tảng. `Makefile` chỉ là lối tắt cho Linux.

Yêu cầu: CMake 3.10+; GCC/Clang (Linux), AppleClang (macOS 12+), MSVC 2019/2022 hoặc MinGW-w64 (Windows 8+). zlib tùy chọn (chỉ cho `file.compress = gzip`).

## Build

```bash
cmake -S . -B build -DTBLOG_BUILD_TESTS=ON
cmake --build build -j8                     # MSVC: --config Release
ctest --test-dir build --output-on-failure  # MSVC: -C Release
cmake --install build --prefix /usr/local
```

- **MSVC**: thêm `-A x64`. zlib qua vcpkg: `vcpkg install zlib:x64-windows` rồi `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`. DLL, test, ví dụ nằm chung `build/bin/<Config>/`.
- **MinGW**: shell MSYS2 MINGW64, `-G Ninja`. Thư viện build với `__USE_MINGW_ANSI_STDIO=1`, nên `%zu`, `%lld` chạy như trên Linux.
- **macOS**: zlib có sẵn trong SDK. Dylib có install name `@rpath/libtblog.0.dylib`; link tay thì thêm `-Wl,-rpath,<prefix>/lib`.
- **Sanitizer** (GCC/Clang): `-DTBLOG_SANITIZE=address,undefined` hoặc `thread`, mỗi cấu hình một thư mục build riêng.

## Option CMake

| Option | Mặc định | Ý nghĩa |
|---|---|---|
| `TBLOG_BUILD_SHARED` | `ON` | `tblog::tblog` là bản động; `OFF` = bản tĩnh. Cả hai luôn được build |
| `TBLOG_CXX_STANDARD` | `17` | Chuẩn C++ để build thư viện: `11`, `14`, `17` (MSVC: từ `14`). Không ràng buộc ứng dụng |
| `TBLOG_WITH_ZLIB` | `ON` | Dùng zlib nếu tìm thấy |
| `TBLOG_BUILD_TESTS` / `_EXAMPLES` / `_BENCH` | `OFF` | Test, ví dụ, benchmark |
| `TBLOG_WERROR` | `OFF` | Warning là lỗi |
| `TBLOG_SANITIZE` | rỗng | `address`, `thread`, `undefined` (kết hợp bằng dấu phẩy) |
| `TBLOG_HEADERS_ONLY_CHECK` | `OFF` | Chỉ biên dịch thử header công khai |

Khi được nhúng bằng `add_subdirectory`, tblog không đụng `CMAKE_BUILD_TYPE` của project cha; đặt option bằng `set(...)` trước đó.

## Tĩnh hay động

| | Tĩnh | Động |
|---|---|---|
| Ưu | Một file chạy; thoát process luôn shutdown bình thường | Nhiều chương trình dùng chung; cập nhật không cần build lại |
| Nhược | Từ C phải link thêm runtime C++, `-pthread`, `-lz` (CMake / `pkg-config --static` làm sẵn) | Windows: khi thoát mà chưa gọi `tblog_shutdown()`, sink riêng không nhận các dòng cuối ([architecture.md](architecture.md#thoát-process-trên-windows)) |

## Dùng từ project khác

```cmake
find_package(tblog 0.1 REQUIRED)          # set(TBLOG_USE_STATIC ON) trước dòng này để chọn bản tĩnh
target_link_libraries(app PRIVATE tblog::tblog)
```

- Target: `tblog::tblog`, `tblog::tblog_static`, `tblog::tblog_shared`. Package tự kéo `Threads` và `ZLIB` khi cần.
- Từ mã nguồn: `add_subdirectory(third_party/tblog)` hoặc FetchContent, cùng các target. Windows: `tblog.dll` được ghi vào `CMAKE_RUNTIME_OUTPUT_DIRECTORY` của project cha nếu có.
- Link DLL bằng tay trên Windows: định nghĩa `TBLOG_DLL` trước khi include (`tblog::tblog_shared` làm sẵn).
- pkg-config (Linux, macOS): `cc app.c $(pkg-config --cflags --libs tblog)`. Link tĩnh với GNU ld khi `.a` và `.so` cùng thư mục: thay `-ltblog` bằng `-l:libtblog.a`.

## File được cài

| Nền tảng | Tĩnh | Động |
|---|---|---|
| Linux | `lib/libtblog.a` | `lib/libtblog.so.0.1.0` (+ symlink `.so.0`, `.so`) |
| macOS | `lib/libtblog.a` | `lib/libtblog.0.1.0.dylib` (+ symlink) |
| Windows, MSVC | `lib/tblog_static.lib` | `bin/tblog.dll`, `lib/tblog.lib` |
| Windows, MinGW | `lib/libtblog_static.a` | `bin/libtblog.dll`, `lib/libtblog.dll.a` |

Cộng header `include/tblog/`, `<libdir>/cmake/tblog/`, `<libdir>/pkgconfig/tblog.pc`. Bản động chỉ export các hàm `tblog_*`.

## Makefile (Linux)

| Lệnh | Kết quả |
|---|---|
| `make` / `make static` / `make shared` | Thư viện vào `build/` |
| `make test`, `make examples`, `make bench` | Test, ví dụ, benchmark |
| `make install PREFIX=/usr/local` / `uninstall` | Cài / gỡ (nhận `DESTDIR`, `LIBDIR`...) |
| `make check-symbols check-install` | Kiểm tra export và cài thử |

Biến: `STD=c++11|c++14|c++17`, `SAN=address|thread|undefined`, `HAVE_ZLIB=0|1`, `CXX`, `CC`, `AR`.

## Cross-compile

```bash
cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=<toolchain-của-bạn>.cmake
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar
```

CI không chạy test cross-compile. Ghi chú embedded: [platforms.md](platforms.md#embedded-linux).
