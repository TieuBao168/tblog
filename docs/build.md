# Build và cài đặt

CMake là cách build chính, được CI kiểm tra trên Linux, macOS và Windows. `Makefile` ở thư mục gốc chỉ là lối tắt cho Linux.

Yêu cầu: CMake 3.10 trở lên; GCC hoặc Clang (Linux), AppleClang (macOS 12+), MSVC 2019/2022 hoặc MinGW-w64 GCC (Windows 8+). zlib là tùy chọn, chỉ cần cho `file.compress = gzip`.

## Option CMake

| Option | Mặc định | Ý nghĩa |
|---|---|---|
| `TBLOG_BUILD_SHARED` | `ON` | `tblog::tblog` trỏ tới bản động. `OFF`: trỏ tới bản tĩnh. Cả hai bản (`tblog_static`, `tblog_shared`) luôn được build và cài. |
| `TBLOG_CXX_STANDARD` | `17` | Chuẩn C++ dùng để build thư viện, test, ví dụ, bench: `11`, `14` hoặc `17`. Không ràng buộc chuẩn của ứng dụng. MSVC không có chế độ C++11, nên dùng `14` hoặc `17`. |
| `TBLOG_WITH_ZLIB` | `ON` | Dùng zlib nếu `find_package(ZLIB)` tìm thấy. Dòng `tblog: gzip compression of rotated files (zlib): ON/OFF` lúc configure cho biết kết quả. |
| `TBLOG_BUILD_TESTS` | `OFF` | Build `tests/unit/*.cpp`, `tests/shared/*.cpp`, `tests/header_check` và đăng ký với CTest (xem [testing.md](testing.md)). |
| `TBLOG_BUILD_EXAMPLES` | `OFF` | Build `examples/*.c`, `examples/*.cpp`. Có thêm `TBLOG_BUILD_TESTS` thì mỗi ví dụ còn là một CTest smoke test. |
| `TBLOG_BUILD_BENCH` | `OFF` | Build `bench/bench_vs_printf` (so với `fprintf`: thông lượng, latency) và `bench/bench_crash_loss` (mất log khi crash/kill/exit), xem [benchmarks.md](benchmarks.md). Có thêm `TBLOG_BUILD_TESTS` thì có test `bench_vs_printf_smoke` và `bench_crash_loss_smoke` (mọi nền tảng). |
| `TBLOG_WERROR` | `OFF` | Coi warning là lỗi (`-Werror` / `/WX`). |
| `TBLOG_SANITIZE` | rỗng | `address`, `thread`, `undefined` hoặc tổ hợp như `address,undefined`. Chỉ GCC/Clang. |
| `TBLOG_HEADERS_ONLY_CHECK` | `OFF` | Chỉ biên dịch thử các header công khai (`tests/header_check`), không build thư viện. Dùng cho MSVC ARM64 trong CI. |

Khi tblog là project gốc và generator là single-config, build type mặc định là `RelWithDebInfo`. Khi được nhúng bằng `add_subdirectory`/FetchContent, tblog không đụng tới `CMAKE_BUILD_TYPE` của project cha. Project cha có thể đặt các option trên bằng `set(TBLOG_BUILD_SHARED OFF)` trước `add_subdirectory` (policy CMP0077).

Mỗi cấu hình nên có thư mục build riêng (`build`, `build-asan`, `build-tsan`...).

## Linux

```bash
cmake -S . -B build -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

## macOS

AppleClang của Xcode hoặc Command Line Tools (`xcode-select --install`). zlib có sẵn trong SDK.

```bash
cmake -S . -B build -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

Dylib có install name `@rpath/libtblog.0.dylib`. Chương trình dùng `find_package(tblog)` được CMake thêm sẵn `LC_RPATH`; khi link tay thì thêm `-Wl,-rpath,<prefix>/lib`. Makefile không dùng được trên macOS.

## Windows: MSVC

Visual Studio 2019/2022 là generator multi-config, nên phải truyền `--config` / `-C`:

```powershell
cmake -S . -B build -A x64 -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix C:/tblog
```

zlib qua vcpkg: `vcpkg install zlib:x64-windows`, rồi thêm `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows` khi configure.

DLL, test, ví dụ và bench được ghi chung vào `build/bin/<Config>/`, nên chạy được ngay mà không cần sửa `PATH`.

## Windows: MinGW-w64

Shell MSYS2 MINGW64 với các gói `mingw-w64-x86_64-gcc`, `mingw-w64-x86_64-cmake`, `mingw-w64-x86_64-ninja`, `mingw-w64-x86_64-zlib`:

```bash
cmake -S . -B build -G Ninja -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Thư viện được build với `__USE_MINGW_ANSI_STDIO=1`, nên `%zu`, `%lld`, `%Lf` trong `TBLOG_*` cho kết quả như trên Linux.

## Sanitizer

```bash
cmake -S . -B build-asan -DTBLOG_BUILD_TESTS=ON -DTBLOG_SANITIZE=address,undefined
cmake -S . -B build-tsan -DTBLOG_BUILD_TESTS=ON -DTBLOG_SANITIZE=thread
```

- Áp dụng cho thư viện, test, ví dụ và bench. `thread` không kết hợp được với `address`.
- Có `undefined` thì thêm `-fno-sanitize-recover=undefined`: lỗi UBSan làm test fail chứ không chỉ in ra.
- Với `thread` trên Linux, CTest chạy test qua `setarch <arch> -R` (tắt ASLR), vì TSan có thể abort khi ASLR bật trên kernel mới. macOS không cần.
- Build có sanitizer không đăng ký test `check_symbols` (instrumentation không thuộc API export).

## Cài đặt: các file được cài

Header (mọi nền tảng): `include/tblog/log.h`, `include/tblog/tblog.hpp`, `include/tblog/detail/format.hpp`. Ngoài ra có `<libdir>/cmake/tblog/` (package config cho `find_package`) và `<libdir>/pkgconfig/tblog.pc`. `<libdir>` là `CMAKE_INSTALL_LIBDIR` (`lib`, hoặc `lib64` / `lib/<multiarch>` tùy distro).

| Nền tảng | Bản tĩnh | Bản động |
|---|---|---|
| Linux | `lib/libtblog.a` | `lib/libtblog.so.0.1.0`, symlink `libtblog.so.0` (soname), `libtblog.so` |
| macOS | `lib/libtblog.a` | `lib/libtblog.0.1.0.dylib`, symlink `libtblog.0.dylib`, `libtblog.dylib` |
| Windows, MSVC | `lib/tblog_static.lib` | `bin/tblog.dll`, import library `lib/tblog.lib` |
| Windows, MinGW | `lib/libtblog_static.a` | `bin/libtblog.dll`, import library `lib/libtblog.dll.a` |

Trên Windows bản tĩnh tên là `tblog_static` vì import library của DLL đã chiếm tên `tblog`. Bản động trên Linux và macOS chỉ export các hàm `tblog_*` (`packaging/tblog.map`, `packaging/tblog.exp`); trên Windows DLL chỉ export các hàm `TBLOG_API`.

## Dùng từ project khác

**`find_package`** (bản đã cài):

```cmake
find_package(tblog 0.1 REQUIRED)
target_link_libraries(app PRIVATE tblog::tblog)
```

- Target: `tblog::tblog` (bản động, trừ khi tblog được build với `TBLOG_BUILD_SHARED=OFF`), `tblog::tblog_static`, `tblog::tblog_shared`.
- `set(TBLOG_USE_STATIC ON)` trước `find_package` để `tblog::tblog` trỏ tới bản tĩnh.
- Package tự kéo theo `Threads` và, nếu tblog được build với zlib, `ZLIB` (bản tĩnh cần nó).
- Trên Windows, `tblog::tblog_shared` mang sẵn định nghĩa `TBLOG_DLL` (dllimport). Khi link DLL bằng tay, định nghĩa `TBLOG_DLL` trước khi include `<tblog/log.h>`; với bản tĩnh thì không định nghĩa.

**`add_subdirectory` / FetchContent** (từ mã nguồn): xem README. Cùng các target như trên. Trên Windows, nếu project cha đặt `CMAKE_RUNTIME_OUTPUT_DIRECTORY` thì `tblog.dll` được ghi vào thư mục đó, cạnh các file `.exe`. CI kiểm tra cách này bằng `packaging/consumer` với `-DTBLOG_SOURCE_DIR=<repo>`.

**pkg-config** (Linux, macOS; trên MinGW `-ltblog` chọn import library của DLL):

```bash
cc app.c $(pkg-config --cflags --libs tblog) -o app
# Bản tĩnh: --static thêm runtime C++ (-lstdc++, macOS: -lc++), -pthread và -lz (nếu có zlib).
cc app.c $(pkg-config --static --cflags --libs tblog | sed 's/-ltblog/-l:libtblog.a/') -o app   # GNU ld
```

Khi `.a` và `.so` cùng nằm trong một thư mục, `-ltblog` chọn `.so`, nên link tĩnh với GNU ld cần `-l:libtblog.a`; ld64 trên macOS không có `-l:`, hãy truyền đường dẫn `<prefix>/lib/libtblog.a`.

Link bản tĩnh từ chương trình C cần thêm runtime C++ (`-lstdc++` hoặc `-lc++`), `-pthread` và `-lz` nếu tblog có zlib: pkg-config `--static` và package CMake đã làm việc đó.

## Makefile (lối tắt trên Linux)

Cần GNU make, GNU ld. Kết quả nằm trong `build/` (hoặc `build-<SAN>`, `build-c++11`... cho từng tổ hợp).

| Lệnh | Kết quả |
|---|---|
| `make` | `libtblog.a`, `libtblog.so.0.1.0` cùng các symlink |
| `make static` / `make shared` | Chỉ bản tĩnh / chỉ bản động |
| `make test` | Build và chạy `tests/unit/*` |
| `make examples`, `make bench` | `examples/basic`, `examples/cpp_api`, `bench/bench_vs_printf`, `bench/bench_crash_loss` |
| `make install PREFIX=/usr/local` | Cài header (cả `detail/`), `.a`, `.so`, `tblog.pc`; nhận `DESTDIR`, `LIBDIR`, `INCLUDEDIR`, `PKGCONFIGDIR` |
| `make uninstall PREFIX=/usr/local` | Gỡ đúng các file trên |
| `make check-symbols` | `.so` chỉ export các hàm `tblog_*` mà `.a` định nghĩa |
| `make check-install` | Cài thử (cả `DESTDIR` + `uninstall`), rồi build và chạy `examples/basic.c` qua pkg-config, tĩnh và động |
| `make clean` | Xóa `build` và `build-*` |

Biến: `STD=c++11|c++14|c++17` (mặc định `c++17`), `SAN=address|thread|undefined`, `HAVE_ZLIB=0|1` (mặc định tự dò), `CXX`, `CC`, `AR`. Sau khi cài vào thư mục hệ thống, chạy `sudo ldconfig`.

## Cross-compile

CMake: dùng toolchain file như mọi project CMake khác.

```bash
cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.cmake
```

Makefile: đổi compiler.

```bash
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar
```

Test cần chạy trên máy đích (hoặc qua emulator); CI không chạy test cross-compile. Riêng MSVC ARM64 được CI biên dịch thử các header công khai (`TBLOG_HEADERS_ONLY_CHECK=ON`). Ghi chú cho embedded Linux: [platforms.md](platforms.md#embedded-linux).
