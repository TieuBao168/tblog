# Khác biệt giữa các nền tảng

[English](../platforms.md) | **Tiếng Việt**

| | Linux | macOS 12+ | Windows 8+ |
|---|---|---|---|
| fsync | `fdatasync` | `F_FULLFSYNC` (chậm hơn, xem dưới) | `FlushFileBuffers` |
| Thread id (`%t`) | `gettid()` | `pthread_threadid_np` (32 bit) | `GetCurrentThreadId` |
| Call stack khi crash | `backtrace_symbols_fd` (musl: không có) | Định dạng macOS | Chỉ địa chỉ, tự tra symbol |
| Syslog | Có | Có (unified logging) | Không, `syslog = true` bị bỏ qua kèm cảnh báo |
| Makefile | Có | Không, dùng CMake | Không, dùng CMake |

Mọi nền tảng: file mở ở chế độ append, handle không kế thừa sang process con, thư mục cha được tự tạo, và file log là symlink (Windows: mọi reparse point) bị từ chối ([bảo mật](configuration.md#bảo-mật-symlink-quyền-file-escape)). `file.mode` (mặc định `0640`) chỉ áp dụng trên Linux và macOS; trên Windows file mới nhận ACL mặc định của thư mục.

## Linux

- **Tràn stack**: chỉ báo được trên thread có alternate signal stack. tblog cấp cho mỗi thread 64 KB bộ nhớ ảo ở lệnh log đầu tiên của nó, nên mọi thread đã log đều được bảo vệ (đo được 0 dòng mất; trước đây TB 2 065 dòng). Thread chưa từng log thì không. macOS cũng vậy.
- `abort()` của glibc không xả stdio: ở đây `fprintf` mất TB 21 dòng còn tblog mất 0.
- glibc và musl đều được CI chạy toàn bộ test. musl không in call stack khi crash (vẫn xả log). `-DTBLOG_NO_BACKTRACE` tắt call stack ở mọi nơi.
- TSan trên kernel mới cần tắt ASLR (`setarch $(uname -m) -R`); CTest và `make SAN=thread test` tự làm.

### Embedded Linux

- Cần runtime C++ trên máy đích, hoặc link `-static-libstdc++`.
- RAM: 2 × max(`buffer.size`, N × 64 KB). Với 4 CPU và `buffer.size = 128KB`: 512 KB.
- Flash: `sync.interval_ms = 0` giảm số lần ghi thật; dòng ≥ `sync.on_level` vẫn được fsync ngay. Mẫu cấu hình: [configuration.md](configuration.md#cấu-hình-mẫu).
- Vi điều khiển không có OS: dùng [tblog-lite](lite.md).

## macOS

- `fsync` của macOS chỉ đẩy dữ liệu tới ổ đĩa, ổ vẫn có thể giữ trong cache. tblog dùng `F_FULLFSYNC` để "đã sync" có cùng nghĩa như trên Linux. **Nhược**: mỗi lần sync tốn cỡ ms, nên `tblog_flush()` chờ lâu hơn; lệnh log không bị ảnh hưởng (sync chạy trên thread riêng).
- Tràn stack trên thread phụ đến dưới dạng SIGBUS; được báo và xả buffer như Linux (đo được 0 dòng mất). Trên thread không có alternate stack, macOS giết process bằng SIGILL thay vào đó.

## Windows

- MSVC không có chế độ C++11: build thư viện với `TBLOG_CXX_STANDARD=14` hoặc `17`.
- Đường dẫn là UTF-8; `/` và `\` đều được; hỗ trợ UNC và `\\?\`.
- File log mở với `FILE_SHARE_READ | WRITE | DELETE`: đọc, đổi tên, xóa được khi đang ghi (cần cho xoay file).
- `WriteFile` phải chờ khi `FlushFileBuffers` đang chạy trên cùng file: `flush.on_level` + `sync.on_level` cùng level làm dòng ERROR chậm tới 2.69–5.4 ms (Linux / macOS: 37–39 µs) ([configuration.md](configuration.md#flushon_level-sống-sót-qua-kill--9)).
- Console có màu khi bật được ANSI (Windows 10 1511+).
- DLL: gọi `tblog_shutdown()` trước khi thoát; không `FreeLibrary` khi logger đang chạy ([architecture.md](architecture.md#thoát-process-trên-windows)).
- ARM64: chỉ header được CI biên dịch thử; thư viện chưa build hay chạy trên ARM64.
