# Building and installing

**English** | [Tiếng Việt](vi/build.md)

CMake is the main build on every platform. The `Makefile` is only a shortcut for Linux.

Requirements: CMake 3.10+; GCC/Clang (Linux), AppleClang (macOS 12+), MSVC 2019/2022 or MinGW-w64 (Windows 8+). zlib optional (only for `file.compress = gzip`).

## Build

```bash
cmake -S . -B build -DTBLOG_BUILD_TESTS=ON
cmake --build build -j8                     # MSVC: --config Release
ctest --test-dir build --output-on-failure  # MSVC: -C Release
cmake --install build --prefix /usr/local
```

- **MSVC**: add `-A x64`. zlib through vcpkg: `vcpkg install zlib:x64-windows`, then `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`. The DLL, tests and examples all go to `build/bin/<Config>/`.
- **MinGW**: MSYS2 MINGW64 shell, `-G Ninja`. The library is built with `__USE_MINGW_ANSI_STDIO=1`, so `%zu`, `%lld` behave as on Linux.
- **macOS**: zlib ships with the SDK. The dylib's install name is `@rpath/libtblog.0.dylib`; when linking by hand add `-Wl,-rpath,<prefix>/lib`.
- **Sanitizers** (GCC/Clang): `-DTBLOG_SANITIZE=address,undefined` or `thread`, one build directory per configuration.

## CMake options

| Option | Default | Meaning |
|---|---|---|
| `TBLOG_BUILD_SHARED` | `ON` | `tblog::tblog` is the shared library; `OFF` = static. Both are always built |
| `TBLOG_CXX_STANDARD` | `17` | C++ standard used to build the library: `11`, `14`, `17` (MSVC: from `14`). Does not constrain the application |
| `TBLOG_WITH_ZLIB` | `ON` | Use zlib if found |
| `TBLOG_BUILD_TESTS` / `_EXAMPLES` / `_BENCH` | `OFF` | Tests, examples, benchmarks |
| `TBLOG_WERROR` | `OFF` | Warnings are errors |
| `TBLOG_SANITIZE` | empty | `address`, `thread`, `undefined` (comma-separated) |
| `TBLOG_HEADERS_ONLY_CHECK` | `OFF` | Only test-compile the public headers |

When embedded with `add_subdirectory`, tblog does not touch the parent project's `CMAKE_BUILD_TYPE`; set options with `set(...)` beforehand.

## Static or shared

| | Static | Shared |
|---|---|---|
| Pros | One executable; process exit always shuts down normally | Shared by several programs; updates without rebuilding |
| Cons | From C you must also link the C++ runtime, `-pthread`, `-lz` (CMake / `pkg-config --static` do it for you) | Windows: on exit without calling `tblog_shutdown()`, custom sinks do not receive the final lines ([architecture.md](architecture.md#process-exit-on-windows)) |

## Using it from another project

```cmake
find_package(tblog 0.1 REQUIRED)          # set(TBLOG_USE_STATIC ON) before this line to pick the static library
target_link_libraries(app PRIVATE tblog::tblog)
```

- Targets: `tblog::tblog`, `tblog::tblog_static`, `tblog::tblog_shared`. The package pulls in `Threads` and `ZLIB` as needed.
- From source: `add_subdirectory(third_party/tblog)`, or FetchContent (CMake 3.14+) pinned to a tag:

  ```cmake
  include(FetchContent)
  FetchContent_Declare(tblog GIT_REPOSITORY https://github.com/TieuBao168/tblog.git GIT_TAG v0.1.0)
  FetchContent_MakeAvailable(tblog)
  ```

  Same targets as above. Windows: `tblog.dll` is written to the parent project's `CMAKE_RUNTIME_OUTPUT_DIRECTORY` if set.
- Linking the DLL by hand on Windows: define `TBLOG_DLL` before including (`tblog::tblog_shared` does it for you).
- pkg-config (Linux, macOS): `cc app.c $(pkg-config --cflags --libs tblog)`. Static linking with GNU ld when `.a` and `.so` share a directory: replace `-ltblog` with `-l:libtblog.a`.

## Installed files

| Platform | Static | Shared |
|---|---|---|
| Linux | `lib/libtblog.a` | `lib/libtblog.so.0.1.0` (+ symlinks `.so.0`, `.so`) |
| macOS | `lib/libtblog.a` | `lib/libtblog.0.1.0.dylib` (+ symlinks) |
| Windows, MSVC | `lib/tblog_static.lib` | `bin/tblog.dll`, `lib/tblog.lib` |
| Windows, MinGW | `lib/libtblog_static.a` | `bin/libtblog.dll`, `lib/libtblog.dll.a` |

Plus the headers `include/tblog/`, `<libdir>/cmake/tblog/`, `<libdir>/pkgconfig/tblog.pc`. The shared library exports only the `tblog_*` functions.

## Makefile (Linux)

| Command | Result |
|---|---|
| `make` / `make static` / `make shared` | Library into `build/` |
| `make test`, `make examples`, `make bench` | Tests, examples, benchmarks |
| `make install PREFIX=/usr/local` / `uninstall` | Install / uninstall (honours `DESTDIR`, `LIBDIR`...) |
| `make check-symbols check-install` | Check exports and do a trial install |

Variables: `STD=c++11|c++14|c++17`, `SAN=address|thread|undefined`, `HAVE_ZLIB=0|1`, `CXX`, `CC`, `AR`.

## Cross-compiling

```bash
cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=<your-toolchain>.cmake
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar
```

CI does not run cross-compile tests. Embedded notes: [platforms.md](platforms.md#embedded-linux).
