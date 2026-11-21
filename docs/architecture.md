# Kiến trúc

## Đường đi của một dòng log

```
thread gọi log                       thread nền (worker)                   các sink
--------------                       -------------------                   --------
TBLOG_INFO(...)
  level check (lọc thì dừng ở đây)
  format vào buffer thread_local
  Queue::append  ── memcpy ──> shard của thread
                                     chờ: shard đầy 75%, dòng >= sync.on_level,
                                          tblog_flush, hoặc flush.interval_ms
                                     Pipeline::drain:
                                       take: đổi buffer A/B của mọi shard
                                       begin_batch (kiểm tra xoay theo giờ)
                                       ghi từng chunk ──────────────────────> file (xoay, nén)
                                                                              console (stderr)
                                                                              syslog
                                                                              sink riêng (tblog_add_sink)
                                       release, flush các sink
thread sync                          fsync file mỗi sync.interval_ms và khi có dòng >= sync.on_level
thread nén (khi file.compress=gzip)  nén file đã xoay thành .gz
```

- **Thread gọi log** chỉ làm: so level, format bằng `vsnprintf` vào buffer 1024 byte của thread, rồi chép vào shard của nó dưới một spinlock rất ngắn. Trường hợp thường không có system call, không cấp phát, không gọi hàm ảo. Chỉ thread đầu tiên làm shard vượt 75% (hoặc dòng khẩn) trả giá một lần đánh thức worker.
- **Queue** (`src/queue/`): N shard (lũy thừa của 2, ≥ số CPU, tối đa 16), mỗi shard hai buffer. Mỗi thread được gán một shard cố định (round-robin) nên dòng của nó luôn đúng thứ tự. Worker trả các shard theo thứ tự shard, nên dòng của các thread khác nhau trong một lô có thể lệch thứ tự thời gian tối đa khoảng một `flush.interval_ms`.
- **Pipeline** (`src/logger/pipeline.*`): một Queue và danh sách sink theo thứ tự file, console, syslog, sink riêng. Mỗi chunk được ghi vào các sink "crash-safe" (file, console) trước, đánh dấu đã ghi, rồi mới tới các sink còn lại.
- **Sink** (`src/sinks/`): `FileSink` (file + `RotatingFile` xoay theo dung lượng/giờ + `Archiver` đặt tên, giữ file, nén), console, syslog, `CallbackSink` cho `tblog_add_sink`.
- **Thread sync** tách riêng để một ổ đĩa chậm không làm worker ngừng đổi buffer.

## Các thread

| Thread | Tạo khi | Việc |
|---|---|---|
| worker | `tblog_init` | Lấy dữ liệu từ Queue, ghi vào mọi sink, flush, kiểm tra hot-reload mỗi giây |
| sync | `tblog_init` | fsync file log theo `sync.interval_ms` và khi có dòng khẩn; `tblog_flush` sync trên worker |
| nén | file đầu tiên cần nén (`file.compress = gzip`) | Nén file đã xoay; không giữ khóa trong lúc nén |

Worker và thread sync có alternate signal stack riêng (POSIX) / `SetThreadStackGuarantee` (Windows), để stack overflow trên chúng vẫn được báo.

## Độ bền của log

| Tình huống | Kết quả |
|---|---|
| `tblog_shutdown()`, `return` từ `main`, `exit()` | Đủ: mọi dòng được ghi vào mọi sink, fsync, đóng file |
| `tblog_flush()` trả về | Mọi dòng đã có trước lời gọi đã nằm trong sink và đã fsync |
| Crash: SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL; Windows: SEH exception, `abort()` | Signal handler xả buffer vào file và console, rồi ghi dòng `*** tblog: caught signal 11 (SIGSEGV) ***`. Có thể mất dòng đang được chép đúng lúc crash và dòng của thread khác ghi sau khi handler đã đọc buffer; một chunk đang ghi dở có thể bị lặp lại. Syslog và sink riêng không nhận phần xả này. |
| `TBLOG_FATAL` | Dòng FATAL được ghi và fsync, rồi `abort()` (như trên) |
| `kill -9`, OOM killer, `TerminateProcess`, `_exit()` | **Mất** phần còn trong buffer: tối đa khoảng một `flush.interval_ms` (hoặc tới khi shard đầy 75%). Dữ liệu đã ghi xuống file thì nằm trong page cache của OS và không mất. Số đo: [benchmarks.md](benchmarks.md#kết-quả-mất-log). |
| Mất điện, kernel panic | Mất phần chưa được fsync: tối đa khoảng một `sync.interval_ms` cộng một `flush.interval_ms`. Dòng ≥ `sync.on_level` kích hoạt fsync ngay sau khi được ghi. `sync.interval_ms = 0`: chỉ sync theo `sync.on_level`, `tblog_flush` và lúc shutdown. |

macOS dùng `fcntl(F_FULLFSYNC)` cho fsync (xem [platforms.md](platforms.md#macos)), nên "đã fsync" ở đây nghĩa là đã ra khỏi cache của ổ đĩa, như `fdatasync` trên Linux.

## Crash handling

`tblog_init` cài handler; `tblog_shutdown` gỡ và khôi phục handler cũ. Thứ tự khi crash: dòng báo crash ra stderr (khi không có console sink; có console thì dòng báo đi qua nó, sau các dòng được xả), xả buffer + dòng báo crash vào file và console, call stack ra stderr, rồi chuyển cho handler cũ / hành động mặc định, nên core dump hay Windows Error Reporting vẫn diễn ra. Mọi việc trong handler đều async-signal-safe: không malloc, không khóa, không stdio. Chỉ thread crash đầu tiên làm báo cáo; crash thứ hai (hoặc crash bên trong handler) bỏ qua phần báo cáo.

| | Linux | macOS | Windows |
|---|---|---|---|
| Bắt | SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL | như Linux | Unhandled-exception filter (access violation, illegal instruction, chia cho 0, stack overflow...) và `signal(SIGABRT)` (`abort`, `assert`, `std::terminate`) |
| Dòng báo | `*** tblog: caught signal 11 (SIGSEGV) ***` | như Linux | `*** tblog: caught exception 0xC0000005 (EXCEPTION_ACCESS_VIOLATION) ***` |
| Call stack ra stderr | `backtrace_symbols_fd` (glibc; musl: không có) | `backtrace_symbols_fd`, định dạng của macOS | Chỉ địa chỉ trả về (kèm base address của module), tự tra symbol |
| Sau đó | Handler cũ, hoặc hành động mặc định (core dump) | như Linux | Filter cũ, hoặc `EXCEPTION_CONTINUE_SEARCH` (WER / debugger vẫn thấy); `abort()` kết thúc với exit code 3 |
| Stack overflow | Báo được trên thread gọi `tblog_init`, worker và thread sync (có alternate stack); thread khác: không chắc | như Linux, nhưng trên thread phụ đến dưới dạng SIGBUS | Báo được trên thread gọi `tblog_init`, worker và thread sync (dự trữ 64 KB stack) |

C++ exception không được bắt thì đi qua `std::terminate` → `abort()` → SIGABRT. Trên Windows, filter bỏ qua mã exception C++ (`0xE06D7363`) để không báo hai lần.

## Kết thúc process trên Windows

`tblog_init` đăng ký `tblog_shutdown` với `atexit()`:

- Bản tĩnh trong file `.exe`: handler chạy trước `ExitProcess`, khi mọi thread còn sống: shutdown bình thường, như Linux và macOS.
- `tblog.dll` (hoặc bản tĩnh nằm trong một DLL khác): handler chạy lúc DLL bị gỡ (`DLL_PROCESS_DETACH`, giữ loader lock), sau khi `ExitProcess` đã dừng các thread khác, có thể đang giữ khóa. Join thread lúc này sẽ treo process. Vì vậy `tblog_shutdown` (qua `pf::process_exiting()`) không join, không chờ, không cấp phát: nó xả buffer ngay trên thread đang thoát qua đường crash không khóa vào file và console. Sink riêng không nhận các dòng cuối này. Dữ liệu đã ghi nằm trong cache của OS và không mất.
- `FreeLibrary` lên `tblog.dll` khi logger đang chạy không được hỗ trợ: hãy gọi `tblog_shutdown()` trước.

## Lớp nền tảng

Mọi lời gọi OS của thư viện đi qua `src/platform/platform.h` (namespace `tblog::pf`): file, đường dẫn, thư mục, thời gian, thread id, bộ nhớ căn lề, console, crash handling, syslog. Hai bản cài đặt:

- `src/platform/posix/`: Linux và macOS 12+ (khác biệt nhỏ nằm trong `#if defined(__APPLE__)` / `__linux__`).
- `src/platform/win32/`: Windows 8+ (MSVC, MinGW-w64). Đường dẫn là UTF-8 và được chuyển sang UTF-16 cho các API `W`.

Hàm lỗi trả `false` / handle không hợp lệ / `-1` và đặt `errno` (bản win32 đổi `GetLastError()` sang `errno` gần nhất). Vài hàm dùng trên đường nóng (`is_path_separator`; `realtime_ns` trên POSIX) nằm trong `inline.h` của từng nền tảng.

**Quy tắc ranh giới:** code ngoài `src/platform/` không được include header của OS (`unistd.h`, `sys/*`, `windows.h`, `pthread.h`, `syslog.h`...) và không được gọi thẳng API của OS (`open`, `fsync`, `CreateFileW`, `localtime_r`...). `packaging/check-platform-boundary.sh` kiểm tra điều đó; CI chạy nó ở job `platform boundary` và báo lỗi kèm dòng vi phạm. Thêm một nền tảng mới nghĩa là thêm một thư mục `src/platform/<os>/` cài đặt `platform.h`, CMake tự chọn theo hệ điều hành đích.

## Hiệu năng

Số đo so với `fprintf` (thông lượng, latency mỗi lệnh, lệnh bị lọc theo level) và số dòng mất khi crash / `kill -9` / `exit`, kèm máy đo, cách đo và cách chạy lại: [benchmarks.md](benchmarks.md). Chương trình đo: `bench/bench_vs_printf.cpp`, `bench/bench_crash_loss.cpp` (`-DTBLOG_BUILD_BENCH=ON`).

## Mã nguồn

```
include/tblog/        log.h (API C), tblog.hpp + detail/format.hpp (API C++)
src/core/             level, module, thread context, formatter
src/queue/            Queue: double buffer theo shard (đường nóng của tblog_write)
src/config/           parser, validate_config, watcher của log.conf
src/logger/           API C, worker, thread sync, Pipeline (Queue -> các sink)
src/crash/            nội dung báo crash (trên pf::crash_install)
src/sinks/            file (rotating file, archiver), console, syslog, sink riêng
src/platform/         platform.h + posix/ + win32/
tests/                unit/, shared/, header_check/, tiny_test.h, test_os.h
packaging/            tblog.pc.in, tblog.map, tblog.exp, CMake package config, script kiểm tra, consumer/
lite/                 tblog-lite cho vi điều khiển
```
