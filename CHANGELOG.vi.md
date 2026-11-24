# Changelog

[English](CHANGELOG.md) | **Tiếng Việt**

## Chưa phát hành

### Không tương thích

- **File log không bao giờ được mở qua symlink hay hard link thứ hai.** Nếu chính đường dẫn log là link thì `tblog_init` giờ thất bại (`Too many levels of symbolic links` / `Too many links`); link ở phần thư mục vẫn dùng được. Cách chuyển: thay link bằng đường dẫn thật.
- **Ký tự điều khiển trong nội dung được escape mặc định** (`message.escape = true`): xuống dòng thành `\n`, ký tự điều khiển khác thành `\xHH`, dấu xuống dòng ở cuối bị bỏ, nên mỗi lệnh một dòng. Hành vi cũ: `message.escape = false`.
- **File log mới được tạo với quyền `0640`** (trước: `0644`): người dùng khác trên máy không còn đọc được. Hành vi cũ: `file.mode = 0644`.
- **`file.max_files` giới hạn tối đa 10000** (vượt thì bị kẹp kèm cảnh báo).

### Thêm

- **`message.escape`**: escape ký tự điều khiển trong nội dung; tốn 6–10 ns mỗi dòng (microbenchmark của formatter). tblog-lite: `TBLOG_LITE_ESCAPE` (mặc định 1, ~270 byte code).
- **`file.mode`**: quyền của các file tblog tạo (bát phân, chủ phải có quyền ghi; Linux, macOS).

### Sửa

- **Link cài sẵn trong thư mục log có thể chuyển hướng việc ghi và đọc.** `app.log → <file bất kỳ>` làm process nối log vào file đích; link giữa các file đã xoay làm bộ nén gzip xóa trắng file đích (`app.N.log.gz.tmp`) hoặc chép nó vào file `.gz` (`app.N.log`). Giờ mở bằng `O_NOFOLLOW` kèm kiểm tra hard link, file tạm bằng `O_EXCL`. Windows: từ chối symlink và junction; các reparse point khác (file giữ chỗ OneDrive, file đã dedup) mở bình thường.
- **Dữ liệu log bằng `%s` / `{}` có thể tạo dòng log giả** (và bản ghi syslog với severity riêng) qua ký tự xuống dòng, và gửi mã điều khiển tới terminal. Giờ được escape (`message.escape`).
- **Khóa shard có thể chờ vô hạn.** Thread real-time (`SCHED_FIFO` / `SCHED_RR`) có thể quay mãi sau một thread giữ khóa ưu tiên thấp hơn bị chen ngang trên cùng CPU, và một thread có thể treo sau thread giữ khóa đã crash giữa lệnh log. Giờ `block` quay, `yield`, rồi ngủ từng 50 µs và thôi chờ khi queue đã đóng; `drop` bỏ cuộc sau một vòng quay ngắn và bỏ dòng (có đếm), nên giờ cũng có thể bỏ dòng khi nhiều thread dùng chung shard trên ít CPU. Dòng không lấy được khóa vì crash được ghi thẳng, như sau crash. Đường không tranh khóa chỉ thêm một lệnh rẽ nhánh.
- **`file.max_files` không có giới hạn trên**: xoay theo kích thước đổi tên mọi file được giữ, nên giá trị quá lớn làm thread nền đứng lại mỗi lần xoay.

## 0.1.0 (2026-11-22)

Bản phát hành đầu tiên.

### Không tương thích

- **`full_policy` mặc định là `block`** (trước: `drop`). Không còn mất dòng khi quá tải (trước: tới 26% ở 16 thread flood); đổi lại lệnh log có lúc chờ vài ms. Hành vi cũ: `full_policy = drop`.
- **Bỏ backend `sync`, `mmap` và chế độ format trì hoãn.** Key `backend`, `mmap.region_size`, `format.mode` bị bỏ qua kèm cảnh báo.
- **ABI:** xóa `tblog_write_site()` và `tblog_site`; chương trình build với header cũ phải build lại. Version vẫn `0.1.0`.
- **Nén gzip cần zlib** (bỏ đường gọi lệnh `gzip` ngoài).
- **Makefile chỉ còn cho Linux**; CMake là cách build chính.

### Thêm

- **`flush.on_level`**: dòng ≥ level chỉ trả về khi đã ghi vào file. Với `ERROR`: sau `kill -9` (4 thread) mất TB 174 dòng INFO thay vì 4 612, 0 dòng tới dòng ERROR cuối; mỗi dòng ERROR tốn 21–148 µs, hoặc ~1 fsync khi kèm `sync.on_level = ERROR`.
- **`tblog_add_sink()`**: sink của người dùng. **`file = false`**: không ghi file.
- **Windows** (MSVC, MinGW-w64; tĩnh và DLL) và **macOS** 12+ (arm64, x86_64).
- **CMake package**: `find_package(tblog)`, `add_subdirectory`, FetchContent; target `tblog::tblog`, `_static`, `_shared`.
- **Benchmark so với `fprintf`**: `bench_vs_printf` (thông lượng, latency) và `bench_crash_loss` (mất log khi crash / `kill -9` / `exit`), workflow `bench.yml`.
- CI trên Linux (glibc, musl), macOS, Windows. Kết quả benchmark của Linux, macOS, Windows MSVC (GitHub Actions) nằm trong `bench/results/`.

### Sửa

- **Tràn stack trên thread phụ, Linux / macOS**: trước đây crash handler không chạy (thread không có alternate signal stack) và mất cả buffer (đo được TB 2 065–2 794 dòng). Giờ mỗi thread được cấp alternate stack 64 KB ở lệnh log đầu tiên của nó (một `mmap`, chưa tốn RAM thật tới khi có signal, gỡ khi thread kết thúc); các lệnh log sau không tốn thêm. Đo lại: 0 dòng mất ở 20/20 lần trên Linux và macOS. Thread chưa từng log thì vẫn chưa được bảo vệ. Windows không đổi.

### Sửa

- **Crash khi các thread khác vẫn log: 0 dòng mất** (trước: hàng trăm dòng mỗi lần). Dòng log sau lúc handler xả buffer được ghi thẳng vào file.
- **Crash không còn ghi lặp chunk đang ghi dở** (trước: thỉnh thoảng lặp cả chunk). Đo lại: 0 dòng lặp ở mọi kịch bản.
- Không còn mất dòng khi crash đúng lúc thread nền đang ghi.
- `make install` cài thiếu `detail/format.hpp`.
- Khi nhúng bằng `add_subdirectory`: không ghi đè `CMAKE_BUILD_TYPE` của project cha, nhận option đặt bằng `set()`.
