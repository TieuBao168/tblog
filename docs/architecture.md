# Kiến trúc

## Đường đi của một dòng log

```
thread gọi log                       thread nền (worker)                   các sink
--------------                       -------------------                   --------
TBLOG_INFO(...)
  level check (lọc thì dừng ở đây)
  format vào buffer thread_local
  Queue::append  ── memcpy ──> shard của thread
  (dòng >= flush.on_level: chờ tới
   khi thread nền báo "đã ghi")
                                     chờ: shard đầy 75%, dòng >= sync.on_level
                                          hoặc >= flush.on_level, tblog_flush,
                                          hoặc flush.interval_ms
                                     Pipeline::drain:
                                       take: đổi buffer A/B của mọi shard
                                       begin_batch (kiểm tra xoay theo giờ)
                                       ghi từng chunk ──────────────────────> file (xoay, nén)
                                                                              console (stderr)
                                                                              syslog
                                                                              sink riêng (tblog_add_sink)
                                       báo "đã ghi" (thả các lệnh đang chờ
                                         theo flush.on_level), release,
                                         flush các sink
thread sync                          fsync file mỗi sync.interval_ms và khi có dòng >= sync.on_level
thread nén (khi file.compress=gzip)  nén file đã xoay thành .gz
```

- **Thread gọi log** chỉ làm: so level, format bằng `vsnprintf` vào buffer 1024 byte của thread, rồi chép vào shard của nó dưới một spinlock rất ngắn. Trường hợp thường không có system call, không cấp phát, không gọi hàm ảo. Chỉ thread đầu tiên làm shard vượt 75% (hoặc dòng khẩn) trả giá một lần đánh thức worker. Hai trường hợp thread gọi log chờ: shard đầy với `full_policy = block` (mặc định), và dòng ≥ `flush.on_level` ([bên dưới](#chờ-ghi-flushon_level)).
- **Queue** (`src/queue/`): N shard (lũy thừa của 2, ≥ số CPU, tối đa 16), mỗi shard hai buffer. Mỗi thread được gán một shard cố định (round-robin) nên dòng của nó luôn đúng thứ tự. Worker trả các shard theo thứ tự shard, nên dòng của các thread khác nhau trong một lô có thể lệch thứ tự thời gian tối đa khoảng một `flush.interval_ms`.
- **Pipeline** (`src/logger/pipeline.*`): một Queue và danh sách sink theo thứ tự file, console, syslog, sink riêng. Mỗi chunk được ghi vào các sink "crash-safe" (file, console) trước, đánh dấu đã ghi, rồi mới tới các sink còn lại. Worker chỉ ghi vào sink crash-safe khi đang giữ "quyền ghi" của Queue (một biến atomic, không phải khóa), để crash handler biết lần ghi nào đang dở.
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
| Crash: SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL; Windows: SEH exception, `abort()` | Signal handler xả buffer vào file và console, rồi ghi dòng `*** tblog: caught signal 11 (SIGSEGV) ***`. Mọi dòng mà lệnh log đã trả về trước khi process chết đều có trong file và console, nguyên vẹn, đúng một lần, kể cả dòng của các thread khác vẫn đang log trong lúc process chết: sau khi handler bắt đầu xả, lệnh log không đưa dòng vào buffer nữa mà chờ handler xong rồi tự ghi dòng thẳng vào file / console trước khi trả về (nên các dòng này nằm **sau** dòng báo crash). Chỉ dòng mà lệnh log chưa kịp trả về mới có thể mất. Syslog và sink riêng không nhận các dòng này. Chi tiết và các giới hạn: [Crash handling](#crash-handling). |
| `TBLOG_FATAL` | Dòng FATAL được ghi và fsync, rồi `abort()` (như trên) |
| `kill -9`, OOM killer, `TerminateProcess`, `_exit()` | **Mất** phần còn trong buffer: tối đa khoảng một `flush.interval_ms` (hoặc tới khi shard đầy 75%). Dữ liệu đã ghi xuống file thì nằm trong page cache của OS và không mất. Với `flush.on_level`: dòng ≥ level mà lệnh log đã trả về, và mọi dòng trước nó của cùng thread, **không mất** (dòng đã được `write()` trước khi lệnh trả về); chỉ các dòng dưới level log sau dòng đó có thể mất. Số đo: [benchmarks.md](benchmarks.md#kết-quả-mất-log). |
| Dòng ≥ `flush.on_level` trả về | Dòng đó, mọi dòng trước nó của cùng thread và mọi dòng thread nền lấy cùng lượt đã được `write()` vào mọi sink (chưa fsync). Trừ khi: shutdown đang diễn ra, process đang crash (crash handler xả dòng đó), dòng bị bỏ (`full_policy = drop`), lệnh log chạy trên thread nền, hoặc đã chờ quá 2 giây. |
| Mất điện, kernel panic | Mất phần chưa được fsync: tối đa khoảng một `sync.interval_ms` cộng một `flush.interval_ms`. Dòng ≥ `sync.on_level` kích hoạt fsync ngay sau khi được ghi. `sync.interval_ms = 0`: chỉ sync theo `sync.on_level`, `tblog_flush` và lúc shutdown. |

macOS dùng `fcntl(F_FULLFSYNC)` cho fsync (xem [platforms.md](platforms.md#macos)), nên "đã fsync" ở đây nghĩa là đã ra khỏi cache của ổ đĩa, như `fdatasync` trên Linux.

## Chờ ghi (`flush.on_level`)

Một dòng ≥ `flush.on_level` vào buffer như mọi dòng khác; khác ở chỗ lệnh log chờ tới khi thread nền báo đã ghi lô chứa nó (chú thích "Waiting for the write" trong `src/queue/queue.cpp`):

1. **Vé:** trong cùng đoạn spinlock chép dòng vào shard, lệnh log đọc bộ đếm đổi buffer `swap_gen = G` của shard. Lần `take()` kế tiếp của thread nền đổi buffer chứa dòng đó và tăng bộ đếm lên `G + 1` (dưới cùng spinlock), nên dòng thuộc đúng lô mang số `G + 1` của shard đó. Không cần bộ đếm toàn cục: mỗi shard có vé riêng.
2. **Đánh thức:** lệnh log đánh thức thread nền ngay (như shard đầy 75%), không đợi `flush.interval_ms`. Nó không bật cờ "khẩn" của `sync.on_level`, nên không tự kéo theo fsync.
3. **Báo đã ghi:** sau khi ghi **mọi** chunk của lô vào mọi sink, thread nền ghi số lô của từng shard vào `written_gen` của shard đó rồi đánh thức các lệnh đang chờ (condition variable; chỉ khóa mutex khi thật sự có người chờ). Báo sau cả lô, nên khi lệnh trả về thì mọi dòng của lượt đó (mọi shard) đã nằm trong sink.
4. **Chờ:** lệnh log chờ tới khi `written_gen` của shard nó ≥ `G + 1`. Đếm người chờ và `written_gen` dùng thứ tự seq_cst ở cả hai phía, nên không lỡ lần đánh thức nào.

Không bao giờ treo: chờ tối đa 2 giây (đĩa treo); trả về ngay khi queue đóng (shutdown đánh thức người chờ) hoặc crash (signal handler không đánh thức được condition variable, nên người chờ tự kiểm tra lại mỗi 10 ms; handler xả dòng của nó vào file); dòng bị bỏ (`drop`) không chờ; dòng log sau crash được ghi đồng bộ nên không chờ; thread nền (sink riêng tự log) không chờ chính nó. Với `full_policy = block`, dòng chờ có chỗ trong buffer trước, rồi mới lấy vé. Dòng dưới level chỉ tốn một phép so sánh với một biến atomic nằm cùng cache line với cờ `closed_` (vốn đã được đọc ở mỗi lệnh log).

## Crash handling

`tblog_init` cài handler; `tblog_shutdown` gỡ và khôi phục handler cũ. Thứ tự khi crash: dòng báo crash ra stderr (khi không có console sink; có console thì dòng báo đi qua nó, sau các dòng được xả), xả buffer + dòng báo crash vào file và console, call stack ra stderr, rồi chuyển cho handler cũ / hành động mặc định, nên core dump hay Windows Error Reporting vẫn diễn ra. Mọi việc trong handler đều async-signal-safe: không malloc, không khóa, không stdio. Chỉ thread crash đầu tiên làm báo cáo; crash thứ hai (hoặc crash bên trong handler) bỏ qua phần báo cáo.

Xả buffer khi crash (`Queue::crash_drain`, chú thích "Crash safety" trong `src/queue/queue.cpp`):

1. Đánh dấu queue đã crash và đóng nó.
2. Chờ worker ghi xong lần ghi đang dở vào sink crash-safe (nếu có; tối đa khoảng 2 s), rồi giữ quyền ghi. Worker đánh dấu chunk đã ghi trước khi trả quyền ghi, nên handler xả tiếp đúng từ byte worker đã ghi tới: không mất, không lặp. Từ lúc đó worker không ghi gì nữa.
3. Chờ các lệnh log đang ở giữa đoạn chép vào shard chép xong (thử từng spinlock của shard, tổng cộng tối đa khoảng 50 ms, không bao giờ chờ mãi: thread giữ khóa có thể chính là thread crash).
4. Xả phần còn lại của mọi shard, ghi dòng báo crash, rồi mở cho các lệnh log sau crash.

Lệnh log sau crash (bất kỳ thread nào, kể cả thread crash khi log lại từ handler của ứng dụng được gọi tiếp) không động tới spinlock của shard: chờ bước 4 xong, giữ quyền ghi (từng dòng một, không xen nhau), ghi dòng vào file và console bằng đường ghi không khóa, rồi trả về. Chi phí ở đường thường: một lần đọc thêm một cờ atomic (`closed_`, chỉ đọc khi chạy bình thường) trước spinlock. Giới hạn còn lại, đều có chặn thời gian và không bao giờ treo: worker giữ quyền ghi quá khoảng 2 s (sink bị kẹt) hoặc chính worker crash giữa lúc ghi vào sink crash-safe thì phần đầu của chunk đó có thể bị lặp; một spinlock bị giữ suốt khoảng 50 ms thì dòng được chép xong sau lúc handler đọc shard đó có thể mất; lệnh log sau crash chờ handler tối đa khoảng 5 s rồi ghi luôn. Khi thoát process trên Windows (`shutdown_at_exit`, các thread khác đã bị dừng) không có bước chờ nào.

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
