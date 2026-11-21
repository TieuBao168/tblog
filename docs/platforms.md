# Ghi chú theo nền tảng

| Nền tảng | Compiler | Ghi chú ngắn |
|---|---|---|
| Linux x86-64 (glibc, musl) | GCC, Clang | Nền tảng chính; CI chạy mọi test, không cho phép test nào bị bỏ qua |
| macOS 12+, arm64 và x86_64 | AppleClang | fsync bằng `F_FULLFSYNC` |
| Windows 8+, x64 | MSVC 2019/2022, MinGW-w64 GCC | Không có syslog; lưu ý khi thoát process với DLL |
| Windows ARM64 | MSVC | Chỉ được CI biên dịch thử các header công khai |

Ở mọi nền tảng: file được mở ở chế độ append, handle không được kế thừa sang process con (`O_CLOEXEC` / không inheritable), thư mục cha của `file.path` được tự tạo.

## Linux

- **Thread id** (`%t`) là `gettid()`.
- **Sync:** `fdatasync` cho file log, `fsync` cho thư mục sau khi đổi tên (xoay, nén).
- **Syslog:** `openlog`/`syslog`, ident `tblog`, severity theo token `[LEVEL]` của dòng.
- **Crash:** call stack qua `backtrace_symbols_fd` (glibc). Test và ví dụ được link với `-rdynamic` để call stack có tên hàm.
- **TSan:** trên kernel mới cần tắt ASLR (`setarch $(uname -m) -R`); CTest và `make SAN=thread test` đã tự làm.
- **Bản động** chỉ export các hàm `tblog_*` (version script `packaging/tblog.map`) và được link với `--no-undefined`.

### Embedded Linux

- **libc:** CI chạy toàn bộ test trên glibc (Ubuntu) và musl (Alpine). Trên musl không có `<execinfo.h>`: crash handler vẫn xả log và ghi dòng báo crash, chỉ không in call stack. uClibc-ng chưa được CI kiểm tra; nó có call stack khi được build với `UCLIBC_HAS_BACKTRACE`. `-DTBLOG_NO_BACKTRACE` tắt call stack ở mọi nơi.
- **Máy đích cần runtime C++** (`libstdc++`), hoặc link với `-static-libstdc++`.
- **Log trên flash** (JFFS2, UBIFS): đặt `sync.interval_ms = 0` để giảm số lần ghi; dòng ≥ `sync.on_level` vẫn được sync ngay.
- **RAM:** buffer dùng 2 × max(`buffer.size`, N × 64 KB), N = số shard (lũy thừa của 2 ≥ số CPU, tối đa 16). Với 4 CPU và `buffer.size = 128KB`: 4 shard × 2 × 64 KB = 512 KB.
- Cấu hình gọn gợi ý:

```ini
buffer.size      = 128KB
file.path        = /var/log/app.log
file.max_size    = 1MB
file.max_files   = 3
sync.interval_ms = 0
sync.on_level    = ERROR
console          = true
```

## macOS

- **Phiên bản:** macOS 12 trở lên, arm64 (Apple Silicon) và x86_64; AppleClang.
- **Sync:** `fsync` trên macOS chỉ đẩy dữ liệu xuống ổ đĩa, ổ vẫn có thể giữ trong cache. tblog dùng `fcntl(F_FULLFSYNC)` (quay về `fsync` nếu file system không hỗ trợ, ví dụ ổ mạng, FUSE, FAT), để "đã sync" có cùng nghĩa như `fdatasync` trên Linux. Mỗi lần sync vì vậy tốn cỡ mili giây: `sync.interval_ms`, `sync.on_level` tốn hơn (trên thread sync) và `tblog_flush()` chờ lâu hơn. Các lệnh `TBLOG_*` không bị ảnh hưởng.
- **Thread id** (`%t`) là `pthread_threadid_np` (cắt còn 32 bit).
- **Crash:** như Linux; stack overflow trên thread phụ được báo là `SIGBUS` (macOS báo việc chạm guard page bằng SIGBUS). Call stack theo định dạng của macOS (`0   app   0x... func + 24`).
- **Dylib:** `libtblog.0.1.0.dylib`, install name `@rpath/libtblog.0.dylib`, chỉ export các hàm `tblog_*` (`packaging/tblog.exp`).
- **Syslog:** `syslog(3)`, tức là vào unified logging của macOS.
- **zlib** có sẵn trong SDK.
- **`/dev/full`** không có, nên test `rotating_write_error_counts_loss` bị bỏ qua.
- Makefile không dùng được (nó dùng cờ của GNU ld); dùng CMake.

## Windows

- **Phiên bản:** Windows 8 trở lên, x64. MSVC (Visual Studio 2019/2022) hoặc MinGW-w64 GCC. MSVC không có chế độ C++11: build thư viện với `TBLOG_CXX_STANDARD=14` hoặc `17`; ứng dụng MSVC dùng header ở C++14 trở lên.
- **Tên file:** bản tĩnh `tblog_static.lib` (MinGW: `libtblog_static.a`), DLL `tblog.dll` + import library `tblog.lib` (MinGW: `libtblog.dll` + `libtblog.dll.a`). Dùng DLL thì định nghĩa `TBLOG_DLL` trước khi include (CMake làm sẵn với `tblog::tblog_shared`).
- **Đường dẫn:** UTF-8 (`file.path`, đường dẫn cấu hình), chuyển sang UTF-16 cho API `W`. `/` và `\` đều là dấu phân cách. Đường dẫn UNC và `\\?\` được hỗ trợ.
- **File đang mở:** mọi handle được mở với `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`, nên file log đang ghi vẫn đọc, đổi tên, xóa được (cần cho việc xoay file). Đổi tên dùng `MoveFileExW`; nếu file đích đang bị process khác mở, tblog thử đổi tên kiểu POSIX (`FILE_RENAME_FLAG_POSIX_SEMANTICS`, Windows 10 1607+ trên NTFS).
- **Sync:** `FlushFileBuffers`. Không có fsync thư mục.
- **Thời gian:** `GetSystemTimePreciseAsFileTime`; giờ địa phương qua `localtime_s`.
- **Console:** màu ANSI qua `ENABLE_VIRTUAL_TERMINAL_PROCESSING` khi stderr là console; không bật được thì in không màu.
- **Syslog:** không có (Event Log cần một message source đã đăng ký). `syslog = true` chỉ in cảnh báo và bỏ qua sink này; test `syslog_sink_handles_batches` bị bỏ qua.
- **`/dev/full`** không có tương đương, nên test `rotating_write_error_counts_loss` bị bỏ qua.
- **Crash:** unhandled-exception filter và `SIGABRT`; call stack chỉ có địa chỉ (kèm base address của module), tự tra symbol bằng WinDbg hoặc `addr2line`. Exception được chuyển tiếp nên WER/debugger vẫn thấy; `abort()` kết thúc với exit code 3. Chi tiết: [architecture.md](architecture.md#crash-handling).
- **Thoát process với DLL:** handler `atexit` của `tblog.dll` chạy lúc DLL bị gỡ, khi các thread khác đã bị dừng: tblog không join mà xả buffer vào file và console ngay trên thread đang thoát; sink riêng không nhận các dòng cuối. Nên tự gọi `tblog_shutdown()` trước khi thoát. Không hỗ trợ `FreeLibrary(tblog.dll)` khi logger đang chạy. Chi tiết: [architecture.md](architecture.md#kết-thúc-process-trên-windows).
- **MinGW:** thư viện được build với `__USE_MINGW_ANSI_STDIO=1`; chuỗi format của `TBLOG_*` được kiểm tra theo archetype `gnu_printf`, nên `%zu`, `%lld`, `PRIu64` dùng như trên Linux.
- **ARM64:** chỉ header công khai được CI biên dịch thử (MSVC, cross-compile); thư viện chưa được build hay chạy trên ARM64.
- Makefile không dùng được; dùng CMake.
