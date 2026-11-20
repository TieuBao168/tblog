# tblog

Thư viện log bất đồng bộ cho ứng dụng C/C++ trên Linux. Thread gọi log không bao giờ chạm vào đĩa: việc ghi file, fsync, xoay file và nén file cũ đều nằm ở thread nền.

- **API C thuần** (`<tblog/log.h>`), dùng được từ C99 và mọi chuẩn C++; thêm **API C++ dạng `{}`** (`<tblog/tblog.hpp>`).
- **Ghi qua buffer**: thread gọi log chỉ chép dòng vào buffer của nó; worker gom lại và ghi theo lô.
- **Xoay file** theo dung lượng, theo ngày hoặc theo giờ; **nén gzip** file cũ trên thread nền (cần zlib).
- **Cấu hình bằng file `key = value`**, đổi level/pattern lúc đang chạy mà không cần khởi động lại.
- **Crash handler**: SIGSEGV/SIGABRT vẫn giữ đủ log và in call stack.
- Build được bằng **C++11/14/17**, chạy trên glibc, musl, uClibc-ng; có bản **tblog-lite** cho vi điều khiển.

## Mục lục

- [Bắt đầu nhanh](#bắt-đầu-nhanh)
- [Build](#build)
- [Cài đặt và tích hợp vào project](#cài-đặt-và-tích-hợp-vào-project)
- [API](#api)
- [Cấu hình](#cấu-hình)
- [Độ bền của log](#độ-bền-của-log)
- [Xoay file và nén](#xoay-file-và-nén)
- [Hiệu năng](#hiệu-năng)
- [Embedded Linux](#embedded-linux)
- [tblog-lite cho vi điều khiển](#tblog-lite-cho-vi-điều-khiển)
- [Kiểm thử](#kiểm-thử)
- [Giới hạn đã biết](#giới-hạn-đã-biết)
- [Cấu trúc thư mục](#cấu-trúc-thư-mục)

## Bắt đầu nhanh

```c
#include <tblog/log.h>

int main(void) {
    tblog_init("log.conf");                 /* NULL = cấu hình mặc định */
    TBLOG_INFO("user %d logged in", 42);
    TBLOG_MWARN("network", "retry %d", 3);  /* log theo module "network" */
    tblog_shutdown();                       /* xả hết log, fsync, dừng worker */
    return 0;
}
```

```bash
make
gcc app.c -Iinclude build/libtblog.a -pthread -lstdc++ -o app
```

Hoặc, sau khi `make install`, dùng thư viện động mà không cần nhớ `-lstdc++`:

```bash
gcc app.c $(pkg-config --cflags --libs tblog) -o app
```

Mỗi dòng log mặc định có dạng:

```
2026-11-16 12:47:56.829576 [INFO] [81148] main.c:5 - user 42 logged in
```

## Build

Yêu cầu: Linux, g++ hỗ trợ C++11 trở lên, GNU make. zlib là tùy chọn (cần cho nén gzip; không có zlib thì không nén được).

| Lệnh | Kết quả |
|---|---|
| `make` | `build/libtblog.a`, `build/libtblog.so.0.1.0` cùng các symlink `.so.0`, `.so` |
| `make static` / `make shared` | Chỉ thư viện tĩnh / chỉ thư viện động |
| `make test` | Build và chạy toàn bộ test |
| `make examples` | `build/examples/basic` (C) và `build/examples/cpp_api` (C++) |
| `make bench` | `build/bench/bench_latency` |
| `make check-symbols` | Kiểm tra `.so` chỉ export các hàm `tblog_*` |
| `make check-install` | Cài vào thư mục tạm rồi build và chạy ví dụ với cả bản tĩnh lẫn bản động |
| `make clean` | Xóa mọi thư mục build |

Biến build:

| Biến | Ý nghĩa |
|---|---|
| `STD=c++11` \| `c++14` \| `c++17` | Chuẩn C++ dùng để build thư viện (mặc định `c++17`). Ứng dụng dùng thư viện không bị ràng buộc theo chuẩn này. |
| `SAN=address` \| `thread` | Build với ASan hoặc TSan. Với TSan, `make test` tự chạy test qua `setarch -R`, vì TSan không chạy được khi ASLR bật trên kernel mới. |
| `CXX`, `CC`, `AR` | Đổi compiler, dùng cho cross-compile |
| `HAVE_ZLIB=0` \| `1` | Ép tắt/bật zlib. Mặc định tự dò. |

Mỗi tổ hợp `STD`/`SAN` có thư mục build riêng (`build/`, `build-c++11/`, `build-address/`...), nên không ghi đè object của nhau.

Cross-compile ví dụ:

```bash
make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc AR=aarch64-linux-gnu-ar
```

### CMake

```bash
cmake -S . -B build-cmake -DTBLOG_BUILD_TESTS=ON -DTBLOG_BUILD_EXAMPLES=ON
cmake --build build-cmake -j8
cd build-cmake && ctest --output-on-failure
cmake --install build-cmake --prefix /usr/local
```

| Option | Mặc định | Ý nghĩa |
|---|---|---|
| `TBLOG_BUILD_SHARED` | `ON` | `tblog::tblog` trỏ tới bản động (`OFF`: bản tĩnh) |
| `TBLOG_CXX_STANDARD` | `17` | 11, 14 hoặc 17 |
| `TBLOG_WITH_ZLIB` | `ON` | Dùng zlib nếu tìm thấy |
| `TBLOG_BUILD_TESTS`, `TBLOG_BUILD_EXAMPLES` | `OFF` | Build test (đăng ký với CTest) và ví dụ |

> `CMakeLists.txt` chưa được chạy thử vì máy phát triển không có cmake. Makefile là cách build đã được kiểm chứng.

## Cài đặt và tích hợp vào project

```bash
make install PREFIX=/usr/local      # DESTDIR=, LIBDIR=, INCLUDEDIR=, PKGCONFIGDIR= cũng ghi đè được
sudo ldconfig                       # sau khi cài vào thư mục hệ thống
make uninstall PREFIX=/usr/local
```

Lệnh cài đặt sẽ chép:

- Header: `include/tblog/*.h`, `*.hpp`.
- Thư viện tĩnh: `lib/libtblog.a`.
- Thư viện động: `lib/libtblog.so.0.1.0` (soname `libtblog.so.0`) cùng các symlink.
- File pkg-config: `lib/pkgconfig/tblog.pc`.

**pkg-config**

```bash
# Thư viện động: chỉ cần -ltblog, runtime C++ đã được kéo theo.
cc app.c $(pkg-config --cflags --libs tblog) -o app

# Thư viện tĩnh: --static thêm -lstdc++ -pthread (và -lz nếu build có zlib).
cc app.c $(pkg-config --static --cflags --libs tblog | sed 's/-ltblog/-l:libtblog.a/') -o app
```

Khi cả `.a` lẫn `.so` cùng nằm trong một thư mục, linker sẽ ưu tiên `.so`. Vì vậy muốn link tĩnh thì dùng `-l:libtblog.a`.

**CMake** (từ project khác)

```cmake
find_package(tblog 0.1 REQUIRED)        # set(TBLOG_USE_STATIC ON) trước dòng này để dùng bản tĩnh
target_link_libraries(app PRIVATE tblog::tblog)
```

Thư viện động chỉ export đúng các hàm `tblog_*`: mọi symbol C++ bên trong đều bị ẩn, nên không xung đột với code của ứng dụng.

## API

### C (`<tblog/log.h>`)

```c
int  tblog_init(const char *config_path);  /* 0 = thành công; NULL = mặc định */
void tblog_shutdown(void);                 /* idempotent; cũng tự chạy qua atexit */
void tblog_flush(void);                    /* chờ worker ghi và fdatasync */
void tblog_set_level(const char *module, tblog_level lv);  /* module NULL = gốc */
tblog_level tblog_get_level(const char *module);
uint64_t tblog_dropped_count(void);        /* số dòng bị bỏ do buffer đầy */
```

Macro: `TBLOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL(fmt, ...)` và các biến thể theo module `TBLOG_MTRACE ... TBLOG_MFATAL(module, fmt, ...)`.

- **Kiểu printf:** compiler kiểm tra chuỗi format như với `printf`.
- **Kiểm tra level trước khi format:** lệnh bị lọc theo level gần như không tốn gì, và đối số không bị tính.
- **Loại bỏ lúc biên dịch:** `-DTBLOG_COMPILE_LEVEL=TBLOG_LV_INFO` xóa hẳn các lệnh TRACE/DEBUG khỏi binary.
- **`TBLOG_FATAL`:** ghi log, flush, rồi gọi `abort()`.
- **Log trước `tblog_init`:** được ghi thẳng ra stderr.

### C++ (`<tblog/tblog.hpp>`, header-only, C++11)

```cpp
#include <tblog/tblog.hpp>

TBLOGF_INFO("user {} logged in from {}", user_id, ip);
TBLOGF_MWARN("network", "retry {}/{} after {:.1f}s", n, max, secs);

tblog::Logger net("network");
net.info("connected to {}:{}", host, port);       // tự lấy file:dòng của chính dòng này
TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "dump {}", expensive());  // không tính đối số nếu bị lọc
```

Cú pháp format:

- Cơ bản: `{}`, `{N}` (theo vị trí), `{{`/`}}` (dấu ngoặc nhọn).
- Đặc tả đầy đủ: `{:[fill][<>^][+- ][#][0][width][.precision][type]}`, với type thuộc `d x X o b c e f g s p`.

Kiểu tự định nghĩa: viết một hàm `tblog_format(tblog::Sink&, const T&)` trong namespace của kiểu đó; kiểu nào đã có `operator<<` thì in được luôn.

Khi sai format, lỗi hiện ngay trong dòng log thay vì crash: `{!missing-arg}`, `{!extra-args}`, `{!bad-spec}`.

`TBLOGF_INFO` mất khoảng 150 ns mỗi lệnh, so với khoảng 230 ns của macro printf tương đương. Nếu cần double ở đường nóng, nên dùng `{:.Nf}` thay cho `{}`.

## Cấu hình

File `key = value`; `#` bắt đầu comment. Riêng `pattern` lấy nguyên phần còn lại của dòng. Xem file mẫu [log.conf](log.conf).

| Key | Mặc định | Giá trị |
|---|---|---|
| `level` | `INFO` | `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`, `OFF` |
| `module.<tên>` | (theo gốc) | Level riêng cho một module, ví dụ `module.network = DEBUG` |
| `pattern` | `%D %T.%us [%L] [%t] %f:%l - %m` | Xem bảng token bên dưới |
| `file.path` | `./logs/app.log` | Thư mục cha được tự tạo |
| `file.max_size` | `256MB` | Xoay file khi vượt; nhận hậu tố `B/KB/MB/GB` |
| `file.max_files` | `5` | File đang ghi + tối đa N−1 file đã xoay |
| `file.rotate` | `size` | `size`, `daily`, `hourly` |
| `file.compress` | `none` | `none`, `gzip` (cần build với zlib) |
| `buffer.size` | `4MB` | Tổng dung lượng buffer |
| `full_policy` | `drop` | `drop` (bỏ dòng và đếm, không bao giờ chặn) hoặc `block` (chờ) |
| `flush.interval_ms` | `200` | Chu kỳ worker đẩy log xuống file |
| `sync.interval_ms` | `1000` | Chu kỳ `fdatasync`; `0` = không sync định kỳ |
| `sync.on_level` | `ERROR` | Gặp dòng từ level này trở lên thì fdatasync ngay |
| `console`, `console.color` | `false`, `true` | Ghi thêm ra stderr, có tô màu khi là terminal |
| `syslog` | `false` | Ghi thêm vào syslog |
| `reload.watch` | `true` | Tự đọc lại file config khi file thay đổi |

**Đổi lúc đang chạy** (hot-reload): `level`, `module.*`, `pattern`, `flush.interval_ms`, `sync.interval_ms`. Các key khác cần khởi động lại. Giá trị sai bị bỏ qua (giữ giá trị cũ) và có cảnh báo kèm số dòng ra stderr.

**Key đã bỏ:** `backend`, `mmap.region_size`, `format.mode`. File config cũ vẫn đọc được: các key này bị bỏ qua, kèm cảnh báo `option removed, ignored`.

Token của `pattern`:

| Token | Ý nghĩa | Token | Ý nghĩa |
|---|---|---|---|
| `%D` | Ngày `YYYY-MM-DD` | `%t` | Thread id |
| `%T` | Giờ `HH:MM:SS` (giờ địa phương) | `%n` | Tên module (`-` nếu là gốc) |
| `%us` / `%ms` | Micro- / mili-giây | `%f`, `%l`, `%F` | Tên file, dòng, hàm |
| `%L` | Tên level | `%m` | Nội dung log |
| `%%` | Ký tự `%` | | |

## Độ bền của log

Mỗi thread ghi vào buffer riêng của nó; worker gom các buffer lại và ghi theo lô.

| Tình huống | Kết quả |
|---|---|
| Crash (SIGSEGV, SIGABRT) | Giữ đủ: signal handler xả buffer trước khi process chết |
| `kill -9`, OOM killer | **Mất** phần còn trong buffer (tối đa khoảng một `flush.interval_ms`) |
| Mất điện | Mất tối đa một chu kỳ `sync.interval_ms` |

## Xoay file và nén

| `file.rotate` | Tên các file đã xoay |
|---|---|
| `size` | `app.1.log` (mới nhất), `app.2.log`, ... |
| `daily` | `app.2026-11-13.log`; nếu vượt `max_size` trong cùng ngày thì thêm `app.2026-11-13.1.log`, `.2`, ... |
| `hourly` | `app.2026-11-13T14.log`, ... |

- **Thời điểm đổi kỳ** tính theo giờ địa phương, và chỉ worker kiểm tra đồng hồ, nên không làm chậm thread gọi log. Ranh giới kỳ có thể trễ tối đa khoảng một `flush.interval_ms`; các dòng ghi trong khoảng trễ đó nằm ở file của kỳ trước.
- **`max_size` vẫn giới hạn mỗi file** ở mọi chế độ. Một dòng không bao giờ bị cắt đôi giữa hai file.
- **`file.compress = gzip`:** file đã xoay được nén thành `.gz` trên một thread nền, bằng zlib. Nếu thư viện được build không có zlib, `gzip` bị coi như `none` và có một cảnh báo ra stderr lúc init.
- **Nén an toàn khi crash:** ghi ra `.gz.tmp`, fsync, đổi tên thành `.gz`, rồi mới xóa file gốc. Crash giữa chừng thì lần khởi động sau dọn file `.tmp`, file gốc vẫn còn.
- **Giữ file:** chỉ giữ N file mới nhất, tính cả file đã nén.

## Hiệu năng

Đo bằng `build/bench/bench_latency 200000` trên i5-14600K (20 luồng), NVMe ext4, máy không có tải khác. Dòng log khoảng 90 byte, có `%f`. Mỗi số đã gồm khoảng 27 ns đọc đồng hồ và khoảng 150 ns format. Chế độ đo: bắn log hết tốc lực, chính sách `drop`.

| Thread | p50 | p99 | Dòng/giây | Dòng mất |
|---|---|---|---|---|
| 1 | 260 ns | 391 ns | 2,8 triệu | 0 |
| 8 | 266 ns | 960 ns | 8,0 triệu | 0 |

Biến môi trường của benchmark:

- `TBLOG_BENCH_RATE=N`: giới hạn mỗi thread ở N dòng/giây.
- `TBLOG_BENCH_POLICY=block`: dùng chính sách `block`.

## Embedded Linux

- **libc:** chạy được với glibc, musl và uClibc-ng. Trên musl không có `<execinfo.h>`, nên crash handler vẫn xả log và ghi dòng báo crash, chỉ không in call stack.
- **Máy đích cần `libstdc++`,** hoặc link với `-static-libstdc++`.
- **Log nằm trên flash** (JFFS2, UBIFS): đặt `sync.interval_ms = 0` để đỡ mòn flash.
- **Cấu hình gọn gợi ý:**

```ini
buffer.size      = 128KB
file.path        = /var/log/app.log
file.max_size    = 1MB
file.max_files   = 3
sync.interval_ms = 0
sync.on_level    = ERROR
console          = true
```

## tblog-lite cho vi điều khiển

Thư mục [lite/](lite/) là một thư viện riêng viết bằng C99, dành cho Cortex-M chạy FreeRTOS hoặc bare-metal. Nó dùng **cùng tên macro** (`TBLOG_INFO`, `TBLOG_MWARN`, ...), nên code chuyển được qua lại giữa Linux và MCU.

```c
#include "tblog_lite.h"

tblog_lite_init(&my_port);   /* port: callback ghi UART/RTT, lấy timestamp, khóa ngắt */
TBLOG_INFO("boot %d", 3);
/* trong idle task hoặc vòng lặp chính: */
tblog_lite_process();
```

**Cách hoạt động:**

- Không dùng malloc, không dùng printf của libc, mặc định không dùng float.
- Log ghi vào một ring buffer tĩnh (mặc định 2 KB). Ring đầy thì bỏ dòng và đếm lại, không bao giờ chặn.
- Gọi log từ trong ngắt (ISR) cũng an toàn.

**Kích thước:** khoảng 3,4 KB flash và 2,1 KB RAM (đo trên x86-64 `-Os`, chưa đo trên ARM).

**Port:** có sẵn cho POSIX (đã test) và cho FreeRTOS, Cortex-M bare-metal (chưa chạy trên phần cứng thật).

**Lệnh:** `make -C lite test`, `make -C lite test-asan`, `make -C lite size`. Các tùy chọn cấu hình (`TBLOG_LITE_BUF_SIZE`, `TBLOG_LITE_LINE_MAX`, `TBLOG_LITE_FLOAT`, ...) nằm trong [lite/include/tblog_lite_config.h](lite/include/tblog_lite_config.h).

## Kiểm thử

```bash
make test
make STD=c++11 test
make SAN=address test
make SAN=thread test
make check-install
make -C lite test
```

Bộ test gồm:

- Test riêng cho từng module.
- Test tích hợp qua API công khai, gồm:
  - SIGSEGV: log giữ đủ và dòng báo crash nằm sau cùng;
  - hot-reload.
- Xoay file theo ngày/giờ với đồng hồ giả lập, nhiều thread cùng ghi đúng lúc đổi kỳ.
- Nén gzip file đã xoay (khi build có zlib), kể cả khôi phục sau crash giữa chừng.

## Giới hạn đã biết

- **Nền tảng:** chỉ Linux. Chưa hỗ trợ Windows/macOS.
- **Chưa chạy thử:** `CMakeLists.txt` và pkg-config thật chưa được chạy trên máy phát triển (không cài cmake/pkg-config). Port FreeRTOS/Cortex-M của tblog-lite chưa chạy trên phần cứng.
- **Init một lần:** mỗi cặp `tblog_init`/`tblog_shutdown` giữ lại buffer của lần đó, nên ứng dụng chỉ nên init một lần.
- **Buffer theo thread:** mỗi thread chỉ dùng phần buffer của mình, nên khi một thread bắn log dồn dập, chính sách `drop` sẽ bỏ dòng sớm hơn.
- **`kill -9`:** mất phần log còn trong buffer (xem [Độ bền của log](#độ-bền-của-log)).
- **Nén gzip:** chỉ có khi build với zlib.
- **Độ dài dòng:** một dòng log tối đa 1024 byte; dài hơn thì bị cắt và kết thúc bằng `...`.

## Cấu trúc thư mục

```
include/tblog/        log.h (API C), tblog.hpp + detail/ (API C++)
src/core/             level, module, thread context, formatter
src/backend/          double buffer, rotating file, archiver (xoay + nén)
src/config/           parser và watcher của log.conf
src/logger/           C API, worker thread, sync thread
src/crash/            crash handler
src/sinks/            console, syslog
tests/unit/           test (tests/tiny_test.h, không cần thư viện ngoài)
bench/                benchmark latency/throughput
examples/             basic.c, cpp_api.cpp
packaging/            tblog.pc.in, version script, CMake package config, script kiểm tra
lite/                 tblog-lite cho vi điều khiển
```
