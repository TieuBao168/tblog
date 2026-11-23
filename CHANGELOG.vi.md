# Changelog

[English](CHANGELOG.md) | **Tiếng Việt**

## Chưa phát hành

### Bảo mật

- **File log không còn được mở qua symlink.** Trước đây một symlink cài sẵn ở đường dẫn log (`app.log → <file bất kỳ>`) làm process nối dòng log vào file đích, và symlink cài giữa các file đã xoay làm bộ nén gzip xóa trắng file đích (`app.N.log.gz.tmp`) hoặc chép file đích vào một file `.gz` đọc được (`app.N.log`). Giờ file log, file đã xoay và file tạm khi nén đều mở với `O_NOFOLLOW` (Windows: từ chối symlink và junction; các reparse point khác như file giữ chỗ OneDrive vẫn mở bình thường), file có thêm hard link bị từ chối (`Too many links`), và file tạm được tạo bằng `O_EXCL`. Nếu chính đường dẫn log là symlink thì `tblog_init` thất bại (`Too many levels of symbolic links`); symlink ở phần thư mục vẫn dùng được.
- **Ký tự điều khiển trong nội dung được escape** (`message.escape = true`, mới): xuống dòng thành `\n`, ký tự điều khiển khác thành `\xHH`, dấu xuống dòng ở cuối bị bỏ. Trước đây một giá trị chứa ký tự xuống dòng có thể tạo dòng log giả (và bản ghi syslog với severity riêng), còn mã điều khiển đi thẳng ra terminal. Dấu `\` không bị escape. Chi phí: 6–10 ns mỗi dòng (microbenchmark của formatter). tblog-lite escape giống vậy (`TBLOG_LITE_ESCAPE`, mặc định 1, ~270 byte code).
- **File log được tạo với quyền `0640`** thay vì `0644` (`file.mode`, mới): người dùng khác trên máy không còn đọc được. File đã có giữ nguyên quyền. Quyền không có ghi cho chủ (ví dụ `0440`) bị từ chối vì file sẽ không mở lại được.
- **`file.max_files` giới hạn tối đa 10000** (vượt thì bị kẹp kèm cảnh báo): xoay theo kích thước đổi tên mọi file được giữ, nên giá trị quá lớn làm thread nền đứng lại mỗi lần xoay.

### Sửa

- **Khóa của shard không còn chờ vô hạn.** Trước đây lệnh log quay và `yield` mãi khi thread khác giữ khóa shard của nó; thread real-time (`SCHED_FIFO` / `SCHED_RR`) có thể treo sau một thread ưu tiên thấp hơn bị chen ngang trên cùng CPU, vì `yield` chỉ nhường cho thread cùng độ ưu tiên. Giờ `full_policy = drop` bỏ cuộc sau một vòng quay ngắn và bỏ dòng (tính vào `tblog_dropped_count()`); `block` sau khi quay và `yield` thì ngủ từng 50 µs, để thread giữ khóa được chạy, và bỏ cuộc khi queue đã đóng, nên thread giữ khóa bị crash giữa lệnh log không làm treo nó. Khi không lấy được khóa vì queue vừa đóng hoặc crash, dòng giờ đi theo đường sau crash (ghi thẳng) thay vì bị bỏ. Với `drop`, dòng giờ cũng có thể bị bỏ khi nhiều thread dùng chung shard trên ít CPU và thread giữ khóa bị chen ngang. Đường không tranh khóa chỉ thêm một lệnh rẽ nhánh theo policy.

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
