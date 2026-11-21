# Hiệu năng và độ an toàn khi crash: tblog so với printf

Trang này so tblog với cách log đơn giản nhất: `fprintf` vào một file bằng thư viện C chuẩn. Không so với thư viện log nào khác. Có hai chương trình:

- `bench/bench_vs_printf.cpp`: thông lượng, latency của mỗi lệnh log, chi phí của một lệnh bị lọc theo level, chi phí của `flush.on_level`.
- `bench/bench_crash_loss.cpp`: số dòng log bị mất khi chương trình chết bất thường (crash, `abort`, exception, stack overflow, `kill -9`, `exit`, `_Exit`).

Cả hai ghi kết quả ra JSON (`bench/results/`), `bench/report.py` dựng bảng Markdown từ JSON. Mọi con số trên trang này lấy từ hai file [windows-mingw-gcc15-ryzen-7-8845hs-2026-11-21-perf.json](../bench/results/windows-mingw-gcc15-ryzen-7-8845hs-2026-11-21-perf.json) và [windows-mingw-gcc15-ryzen-7-8845hs-2026-11-21-crash.json](../bench/results/windows-mingw-gcc15-ryzen-7-8845hs-2026-11-21-crash.json); các bảng là output của `python -I bench/report.py` trên hai file đó.

## Các biến thể

Mọi biến thể ghi cùng một dòng, đúng dạng của pattern mặc định của tblog (`%D %T.%us [%L] [%t] %f:%l - %m`):

```
2026-11-22 00:44:25.408369 [INFO] [15704] bench_crash_loss.cpp:236 - iteration 3702 value=914 name=t0
```

(Một dòng thật lấy từ file log của `bench_crash_loss`; khoảng 102 byte kể cả `\n`.)

| Biến thể | Mỗi dòng làm gì |
|---|---|
| `fprintf_buffered` | Một lệnh `fprintf(f, "%s %s.%06u [INFO] [%lu] %s:%d - iteration %ld value=%d name=%s\n", ...)` vào một `FILE*` dùng chung cho mọi thread, mở bằng `"wb"`, buffer mặc định của stdio. Thư viện C tự khóa stream trong mỗi lệnh. |
| `fprintf_flush` | Như trên, thêm `fflush(f)` sau mỗi dòng: khi lệnh trả về, dòng đã nằm trong OS. Bền ngang tblog khi process crash (nhưng không khi mất điện: chưa fsync). |
| `snprintf_fwrite` | `snprintf` cùng dòng đó vào buffer trên stack rồi một lệnh `fwrite` vào `FILE*` dùng chung. Không phải cách "printf thuần", có ở đây để tách chi phí format khỏi chi phí đường stream của thư viện C. |
| `tblog_block` / `tblog` | `TBLOG_INFO("iteration %ld value=%d name=%s", ...)`, cấu hình mặc định trừ: chỉ ghi file (`console = false`), `reload.watch = false`, `file.max_size = 4GB` (để không xoay file giữa chừng). `full_policy = block` là mặc định. |
| `tblog_drop` | Như trên, `full_policy = drop`. |
| `tblog_flush_error` (`bench_crash_loss`) | Như `tblog`, thêm `flush.on_level = ERROR`; mỗi dòng thứ 100 của mỗi thread (K = 100) là `TBLOG_ERROR(...)` cùng nội dung, các dòng khác `TBLOG_INFO`. Ở 20 000 dòng/giây mỗi thread: 200 dòng ERROR mỗi giây mỗi thread, cứ 5 ms một dòng. |
| `tblog_flush_error_<P>pct`, `tblog_flush_error_nosync_<P>pct` (`bench_vs_printf`, kịch bản `flush_level`) | Như `tblog_block`, thêm `flush.on_level = ERROR`; P% số dòng là ERROR, rải đều (P = 1: mỗi dòng thứ 100; 10: mỗi dòng thứ 10; 100: mọi dòng). `nosync`: thêm `sync.on_level = OFF` (chỉ fsync định kỳ); không có `nosync`: `sync.on_level = ERROR` mặc định, tức là mỗi lô có dòng ERROR còn kéo theo một fsync trên thread sync. |

Phần timestamp của các biến thể printf làm giống tblog: ngày và giờ lấy từ cache theo thread, chỉ gọi `localtime` khi sang giây mới; micro giây, thread id (cache theo thread) và tên file (cắt sau dấu `/` hoặc `\` ở mỗi lệnh) điền ở mỗi lệnh. Đồng hồ là `GetSystemTimePreciseAsFileTime` / `clock_gettime(CLOCK_REALTIME)`, cùng nguồn với tblog.

## Phương pháp: hiệu năng (`bench_vs_printf`)

- **Flood**: mỗi thread ghi `flood-lines / số thread` dòng nhanh nhất có thể (mặc định 1 triệu dòng mỗi lần chạy), với 1, 2, 4, 8, 16 thread.
  - *End-to-end* (`e2e`): từ lúc các thread được thả ra tới khi mọi dòng đã ghi và fsync: tblog tới khi `tblog_shutdown()` trả về (xả buffer, fsync, đóng file); printf tới khi `fflush` + fsync (`_commit` / `fsync`) + `fclose` trả về. JSON có thêm `e2e_nosync_s` của printf (chỉ tới `fflush`).
  - *Loop*: tới khi thread chậm nhất ra khỏi vòng lặp (thời gian các thread gọi log thật sự bị chiếm).
  - Sau mỗi lần chạy, chương trình đếm số dòng trong file: phải bằng số dòng đã gọi trừ số dòng tblog bỏ (`tblog_dropped_count()`), không thì lần chạy bị đánh dấu hỏng.
- **Paced (open loop)**: mỗi thread gửi 50 000 dòng/giây theo lịch cố định (1, 4, 8 thread; các thread lệch pha nhau để không gửi cùng lúc). Latency tính từ **thời điểm theo lịch** tới khi lệnh trả về, nên một lệnh bị nghẽn cũng làm tăng latency của các lệnh xếp hàng sau nó (tránh *coordinated omission*). JSON có thêm latency của riêng lệnh gọi. Không đo 16 thread: 16 thread chờ bận (busy-wait) sẽ chiếm hết 16 CPU logic, kể cả của thread nền của tblog.
- **flush_level**: chỉ tblog, `flush.on_level = ERROR`, flood ở 1 và 8 thread với 0%, 1%, 10%, 100% dòng ERROR, mỗi tỉ lệ với `sync.on_level` mặc định (`ERROR`) và `OFF`. Mỗi lần chạy 200 000 dòng chia cho các thread, nhưng tối đa 1 000 dòng ERROR mỗi thread (ở 10% và 100% số dòng ít hơn: một lệnh ERROR có thể tốn cả một fsync). Latency tách riêng lệnh INFO và lệnh ERROR. Lệnh ERROR trả về khi thread nền đã ghi lô chứa nó vào file, nên đây là chi phí thật của `flush.on_level`.
- **Disabled**: một lệnh bị lọc: `TBLOG_DEBUG(...)` khi level là `INFO`, so với `if (debug_enabled) fprintf(...)` với `debug_enabled` là một `std::atomic<bool>` bằng `false` (đọc relaxed). 20 triệu lệnh, tính trung bình.
- **Đồng hồ**: khoảng thời gian dùng `std::chrono::steady_clock`. Latency mỗi lệnh dùng bộ đếm TSC của CPU (`rdtsc`, hiệu chỉnh theo `steady_clock` lúc khởi động) trên x86, vì `steady_clock` của Windows chỉ nhảy mỗi 100 ns; trên ARM dùng `steady_clock`. Chi phí một cặp lần đọc đồng hồ được ghi trong JSON (`timer_overhead_ns`) và **nằm trong** mọi latency.
- **Lặp lại**: mỗi ô (kịch bản × biến thể × số thread) chạy 1 lần khởi động (bỏ đi) rồi `--runs` lần (mặc định 5); trong mỗi vòng các biến thể chạy theo thứ tự xoay vòng. Bảng ghi **median** và khoảng **min–max** giữa các lần chạy. Latency p50/p99/... là percentile của mọi lệnh trong một lần chạy, rồi lấy median giữa các lần chạy.
- Mỗi lần chạy có thư mục tạm riêng (trong thư mục temp của hệ thống), xóa sau khi chạy. tblog được `tblog_init` / `tblog_shutdown` lại ở mỗi lần chạy, trong cùng một process.
- JSON ghi lại môi trường: CPU, số CPU logic, RAM, phiên bản OS, compiler, cờ build (`--build-info`), commit, ngày giờ.

## Phương pháp: mất log khi chết bất thường (`bench_crash_loss`)

Process cha chạy lại chính chương trình đó làm process con, một lần cho mỗi (kịch bản × biến thể × lần thử). Process con ghi các dòng có số thứ tự tăng dần 1, 2, 3... từ một hoặc nhiều thread, mỗi thread 20 000 dòng/giây. **Ngay sau khi lệnh log trả về**, nó ghi số thứ tự đó vào một bộ đếm riêng của thread trong bộ nhớ chia sẻ (file được map: `MapViewOfFile` trên Windows, `mmap(MAP_SHARED)` trên POSIX). Bộ nhớ này vẫn còn sau khi process con chết, nên process cha biết chính xác chương trình đã *tin* rằng dòng nào được log ("acknowledged"). Rồi process con chết theo kịch bản. Process cha đọc file log và tính cho mỗi thread:

- **lost** = số dòng acknowledged − độ dài dãy 1..k liền mạch tìm thấy trong file. Cột "lost" trong các bảng là tổng của mọi thread.
- *missing*: số dòng acknowledged không có trong file (kể cả lỗ ở giữa); *duplicates*: dòng xuất hiện hơn một lần; *corrupt*: dòng không khớp đúng dạng (mỗi dòng được kiểm tra đủ trường và giá trị `value = i*7 % 1000`); *torn*: dòng cuối file bị cắt dở, không có `\n`; *beyond_ack*: dòng đã nằm trong file nhưng lệnh log của nó chưa kịp trả về (không tính là mất).
- Kết quả của process (exit code / signal) có đúng như kịch bản không, và tblog có ghi dòng `*** tblog: caught ... ***` không.

Process con tắt hộp thoại lỗi của Windows cho riêng nó (`SetErrorMode(SEM_NOGPFAULTERRORBOX | ...)`, MSVC thêm `_set_abort_behavior`), không đổi cấu hình hệ thống; trên POSIX nó trả các signal về mặc định và tắt core dump (`RLIMIT_CORE = 1`).

| Kịch bản | Thread ghi log | Cách chết |
|---|---|---|
| `segv_main` | 1 | Ghi qua con trỏ null trên thread đang log, sau N dòng (N ngẫu nhiên 1000–5000, tức 50–250 ms) |
| `segv_other_thread` | 4 | 4 thread log liên tục; thread thứ 5 ghi qua con trỏ null sau 50–250 ms ngẫu nhiên |
| `abort` | 1 | `abort()` sau N dòng |
| `assert` | 1 | `assert` sai (build không có `NDEBUG`) sau N dòng |
| `uncaught_exception` | 1 | `throw` không có ai bắt (`std::terminate`) sau N dòng |
| `stack_overflow` | 1 | Đệ quy vô hạn trên thread đã gọi `tblog_init`, sau N dòng |
| `stack_overflow_thread` | 1 | Đệ quy vô hạn trên một thread khác (không phải thread gọi `tblog_init`), sau N dòng |
| `kill_hard` | 4 | 4 thread log; process cha giết process con sau 50–250 ms ngẫu nhiên (`TerminateProcess` / `SIGKILL`) |
| `exit` | 1 | `exit(0)` sau N dòng, không gọi `tblog_shutdown` / `fclose` (giống `return` từ `main`) |
| `_Exit` | 1 | `std::_Exit(0)` sau N dòng: không chạy handler `atexit`, không xả stdio |

N và độ trễ được chọn ngẫu nhiên theo `--seed`, giống nhau cho mọi biến thể trong cùng một lần thử. Biến thể: `fprintf_buffered`, `fprintf_flush`, `tblog` (mặc định, `full_policy = block`), `tblog_flush_error`. Ở 20 000 dòng/giây mỗi thread, buffer của tblog không bao giờ đầy, nên `drop` hay `block` không khác gì (JSON ghi `tblog_dropped`, luôn bằng 0 trong các lần chạy dưới đây).

Với `tblog_flush_error`, process con còn ghi số thứ tự của **dòng ERROR cuối cùng đã trả về** của mỗi thread vào bộ nhớ chia sẻ, và process cha đếm thêm cho mỗi lần thử: số dòng INFO đã acknowledged bị mất, số dòng ERROR đã acknowledged bị mất, và số dòng bị mất trong khoảng 1..(dòng ERROR cuối cùng đã trả về) của mỗi thread (`flush.on_level` hứa: 0).

### Kỳ vọng theo lý thuyết

Viết ra trước khi đo, theo cách mỗi biến thể hoạt động:

- **`fprintf_buffered`**: dòng nằm trong buffer stdio của process (4 KB, khoảng 40 dòng) cho tới khi buffer đầy. Process chết mà không qua đường thoát bình thường của thư viện C (crash, `kill -9`) thì mất phần trong buffer: **0–40 dòng**, và dòng cuối file thường bị cắt dở (buffer được ghi ra theo khối 4 KB, không theo dòng). `exit()` xả mọi stream: không mất. `abort()` và `_Exit()` theo chuẩn C không bắt buộc xả stream (tùy thư viện C).
- **`fprintf_flush`**: mỗi lệnh trả về khi dòng đã nằm trong OS (page cache), và page cache vẫn còn khi process chết kiểu gì đi nữa: **0 dòng** ở mọi kịch bản, kể cả `kill -9`. Không bảo vệ khi mất điện (không fsync).
- **tblog**: dòng nằm trong buffer RAM của tblog tới khi thread nền ghi ra (mỗi `flush.interval_ms` = 200 ms, hoặc khi shard của thread đầy 75%).
  - Crash mà handler bắt được (SIGSEGV, `abort`, `assert`, exception không ai bắt, stack overflow trên thread có alternate stack / stack guarantee): handler chờ lần ghi đang dở của thread nền xong rồi xả phần còn lại của buffer: **0 dòng, không lặp**. Thread khác log sau khi handler đã bắt đầu xả thì không đưa vào buffer nữa: lệnh log chờ handler xả xong, rồi tự ghi dòng của nó thẳng vào file (và console) trước khi trả về, sau dòng báo crash. Nên cả *thread khác* đang log trong lúc process chết cũng **0 dòng** (xem [architecture.md](architecture.md#độ-bền-của-log)).
  - `kill -9` / `TerminateProcess`, `_Exit`: không có code nào chạy, mất **toàn bộ phần còn trong buffer**. Ở đây mỗi thread có shard riêng 256 KB (buffer 4 MB chia cho 16 shard, máy có 16 CPU logic), thread nền được đánh thức khi shard đầy 75% = 192 KB ≈ 1 900 dòng 102 byte; ở 20 000 dòng/giây mỗi thread, shard đầy 75% sau khoảng 96 ms, sớm hơn 200 ms. Nên kỳ vọng: **0 đến khoảng 1 900 dòng mỗi thread**, trung bình khoảng một nửa.
  - `exit()` / `return` từ `main`: `tblog_init` đã đăng ký `tblog_shutdown` với `atexit`: không mất.
- **`tblog_flush_error`**: như `tblog` ở mọi kịch bản crash. Với `kill -9` / `_Exit`: mọi dòng tới dòng ERROR cuối cùng đã trả về của mỗi thread đã nằm trong file (**0 dòng mất trong khoảng đó**, 0 dòng ERROR mất); chỉ các dòng INFO log sau dòng ERROR đó có thể mất: tối đa 99 dòng mỗi thread (dòng ERROR tiếp theo chưa tới), ít hơn nếu thread nền tình cờ ghi chúng trước khi process chết.
  - Mất điện: cả ba biến thể đều chưa chắc đã fsync. tblog fsync mỗi `sync.interval_ms` (1 s) và với mỗi dòng ≥ `sync.on_level`; `fprintf` không bao giờ fsync, việc ghi xuống đĩa tùy OS. Không đo được bằng chương trình này.

## Máy đo

| | |
|---|---|
| CPU | AMD Ryzen 7 8845HS (8 nhân, 16 CPU logic) |
| RAM | 31.3 GiB |
| OS | Windows 11 Home 26H2 (10.0.26300), cắm điện, power plan Balanced |
| Ổ đĩa | NVMe SSD (Samsung MZVL81T0HELB), NTFS; thư mục tạm `%TEMP%` trên ổ C: |
| Compiler | MinGW-w64 g++ 15.1.0 (MSYS2 `mingw64`, CRT `msvcrt`), `-O2 -std=c++17 -D__USE_MINGW_ANSI_STDIO=1` |
| tblog | `libtblog_static.a` (`-O2 -fvisibility=hidden`, có zlib), commit `38f4109` + thay đổi chưa commit (`full_policy = block` mặc định, `flush.on_level`) |
| Ngày | 2026-11-21 (UTC) |
| Thời gian chạy | `bench_vs_printf`: 690 s; `bench_crash_loss`: 164 s (800 process con) |

Đây là **một máy, một lần đo**, trên laptop đang chạy các chương trình khác, không cố định tần số CPU. Khoảng min–max trong bảng cho thấy độ nhiễu; ví dụ end-to-end của tblog dao động ±20–40% giữa các lần chạy. Bộ đếm TSC trên CPU này tăng theo bước khoảng 10 ns, nên latency mỗi lệnh có độ phân giải 10 ns (một cặp lần đọc tốn 10 ns, đã nằm trong số đo).

**Lưu ý quan trọng về `fprintf` trên máy này**: với MinGW-w64 và `__USE_MINGW_ANSI_STDIO=1` (mặc định của g++ trên MinGW, và tblog cũng build như vậy), `fprintf` là bản printf C99 riêng của mingw-w64, và đường ghi vào `FILE*` của nó chậm: biến thể `snprintf_fwrite` (cùng dòng, cùng thư viện, chỉ khác là format vào buffer trên stack rồi `fwrite` một lần) nhanh hơn khoảng 5 lần ở 1 thread. `fprintf` của glibc (Linux) hay UCRT (MSVC) có thể nhanh hơn nhiều; các số `fprintf_*` dưới đây **không đại diện cho mọi nền tảng**. Workflow `bench.yml` đo trên Linux (glibc), macOS và Windows (MSVC), xem [Chạy lại](#chạy-lại).

## Kết quả hiệu năng

Mỗi ô: median [min–max] của 5 lần chạy (sau 1 lần khởi động bị bỏ).

**Flood, end-to-end** (lines/s until everything is written and fsynced; 1 M lines per run split over the threads):

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 365 k [328 k–381 k] | 179 k [168 k–183 k] | 1.1 M [1 M–1.2 M] | 1.89 M [1.51 M–2.04 M] | 1.82 M [1.24 M–1.99 M] |
| 2 | 221 k [216 k–228 k] | 115 k [114 k–117 k] | 979 k [862 k–1.16 M] | 2.15 M [1.44 M–2.46 M] | 2.76 M [1.72 M–2.97 M] |
| 4 | 213 k [212 k–218 k] | 127 k [125 k–178 k] | 1.16 M [912 k–1.23 M] | 2.72 M [2.28 M–3.49 M] | 3.28 M [1.69 M–3.43 M] |
| 8 | 213 k [203 k–213 k] | 124 k [120 k–125 k] | 1.06 M [869 k–1.13 M] | 4.34 M [2.41 M–4.82 M] | 4.48 M [2.3 M–4.78 M] |
| 16 | 205 k [200 k–220 k] | 120 k [117 k–121 k] | 1.08 M [781 k–1.09 M] | 4.27 M [3.12 M–5.59 M] | 5.04 M [4.18 M–5.75 M] |

**Flood, logging threads only** (lines/s until the slowest thread left its loop):

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 414 k | 188 k | 1.54 M | 2.7 M | 2.72 M |
| 2 | 235 k | 117 k | 1.56 M | 3.7 M | 4.41 M |
| 4 | 227 k | 128 k | 1.67 M | 6.71 M | 6.62 M |
| 8 | 222 k | 125 k | 1.48 M | 14 M | 13.6 M |
| 16 | 213 k | 121 k | 1.38 M | 18.3 M | 23.2 M |

**Flood, caller latency per call** (p50 / p99 / p99.9 / max, medians across runs):

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 2.03 µs / 6.9 µs / 47.6 µs / 10.3 ms | 4.95 µs / 9.64 µs / 55.8 µs / 35.5 ms | 381 ns / 4.52 µs / 44.3 µs / 1.21 ms | 311 ns / 591 ns / 1.17 µs / 72.3 µs | 311 ns / 601 ns / 1.46 µs / 61.5 µs |
| 2 | 5.34 µs / 58.2 µs / 118 µs / 9.67 ms | 16 µs / 64.2 µs / 197 µs / 43.6 ms | 641 ns / 7.31 µs / 66.8 µs / 1.02 ms | 571 ns / 631 ns / 3.23 µs / 71.6 µs | 321 ns / 641 ns / 3.17 µs / 86 µs |
| 4 | 8.05 µs / 139 µs / 232 µs / 15.2 ms | 12.8 µs / 386 µs / 919 µs / 31.6 ms | 671 ns / 52.1 µs / 117 µs / 3.31 ms | 551 ns / 641 ns / 1.24 µs / 193 µs | 561 ns / 651 ns / 2.43 µs / 158 µs |
| 8 | 8.56 µs / 330 µs / 563 µs / 5.28 ms | 14.1 µs / 832 µs / 1.63 ms / 21.1 ms | 781 ns / 114 µs / 346 µs / 2.47 ms | 561 ns / 671 ns / 1.45 µs / 305 µs | 561 ns / 671 ns / 1.91 µs / 151 µs |
| 16 | 9.53 µs / 729 µs / 1.28 ms / 16.7 ms | 16.9 µs / 1.43 ms / 2.66 ms / 25.4 ms | 872 ns / 226 µs / 639 µs / 1.93 ms | 561 ns / 671 ns / 2.84 µs / 2.88 ms | 571 ns / 691 ns / 2.7 µs / 1.99 ms |

Dropped lines (full_policy = drop, median per run of 1 M): tblog_drop 8 thr: 9.4 k, tblog_drop 16 thr: 233 k.

**Paced (open loop), 50 k lines/s per thread**: latency from the scheduled send time (p50 / p99 / p99.9 / max) and the rate achieved:

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 3.62 µs / 54.7 µs / 287 µs / 409 µs (50 k/s) | 7.86 µs / 74.6 µs / 779 µs / 957 µs (50 k/s) | 429 ns / 22 µs / 81 µs / 149 µs (50 k/s) | 598 ns / 3.74 µs / 24.2 µs / 136 µs (50 k/s) | 729 ns / 4.01 µs / 23.6 µs / 129 µs (50 k/s) |
| 4 | 3.32 µs / 1.18 ms / 1.84 ms / 2.62 ms (200 k/s) | 276 ms / 557 ms / 561 ms / 561 ms (129 k/s) | 600 ns / 93 µs / 585 µs / 1.18 ms (200 k/s) | 539 ns / 1.75 µs / 12.9 µs / 174 µs (200 k/s) | 531 ns / 2.18 µs / 25.6 µs / 275 µs (200 k/s) |
| 8 | 406 ms / 801 ms / 812 ms / 813 ms (222 k/s) | 1.06 s / 1.13 s / 1.13 s / 1.13 s (128 k/s) | 607 ns / 298 µs / 1.08 ms / 2.23 ms (400 k/s) | 538 ns / 2.52 µs / 22.9 µs / 447 µs (400 k/s) | 540 ns / 3.15 µs / 71.2 µs / 560 µs (400 k/s) |

**Disabled level** (one call that is filtered out, 20 M calls per run):

| variant | ns per call |
|---|---|
| printf_if_guard | 0.60 [0.32–0.62] |
| tblog_below_level | 1.16 [1.05–1.77] |

Đọc bảng:

- **Một thread**: tblog trả về sau 311 ns (p50) so với 2.03 µs của `fprintf_buffered` và 381 ns của `snprintf_fwrite`; end-to-end (tới khi đã fsync) tblog 1.82–1.89 M dòng/s, `snprintf_fwrite` 1.1 M, `fprintf_buffered` 365 k.
- **Nhiều thread**: `fprintf` dùng chung một `FILE*` nên các thread xếp hàng chờ khóa của stream: thông lượng giảm khi thêm thread (365 k → 205 k dòng/s từ 1 lên 16 thread) và p99 lên tới hàng trăm µs – hơn 1 ms. tblog tăng theo số thread (1.89 M → 4.27 M end-to-end với `block`, 2.7 M → 18.3 M ở phía các thread gọi log) và p99 giữ dưới 1 µs.
- **Trần của tblog là thread nền**: các thread gọi log đưa được 13.6–23.2 M dòng/s vào buffer ở 8–16 thread, nhưng end-to-end chỉ 4.3–5 M dòng/s: một thread nền ghi ra một file. Khi flood vượt trần đó:
  - `block` (**mặc định**): không mất dòng nào; thông lượng end-to-end ngang `drop` (4.34 M so với 4.48 M ở 8 thread, 4.27 M so với 5.04 M ở 16 thread, khoảng min–max chồng lên nhau); cái giá là đuôi latency: max 305 µs ở 8 thread, 2.88 ms ở 16 thread (median của các lần chạy; một lần chạy 1 thread có một lệnh 17 ms).
  - `drop`: **bỏ dòng**: median 233 k trên 1 M dòng ở 16 thread (lần tệ nhất 330 k, tức 33%), 9.4 k ở 8 thread; ở 1, 2, 4 thread median 0 nhưng lần tệ nhất bỏ 126 k, 4.9 k, 142 k.
- **Paced 50 000 dòng/giây mỗi thread**: `fprintf_flush` không theo kịp ở 4 thread (129 k/s thay vì 200 k/s), `fprintf_buffered` ở 8 thread (222 k/s thay vì 400 k/s): hàng đợi dồn lên và latency tính từ lịch lên tới hàng trăm ms – 1 s. tblog giữ p50 khoảng 530–730 ns, p99 1.8–4 µs ở mọi mức, `block` hay `drop` như nhau (buffer không đầy).
- **Lệnh bị lọc**: 1.16 ns (tblog) so với 0.60 ns (`if` với một `atomic<bool>`). Cả hai gần gấp đôi lần đo trước (0.68 / 0.35 ns) trên cùng máy, tức là do trạng thái máy (tần số CPU), không phải do mã; dù sao vẫn không đáng kể so với một lệnh log thật.

### Chi phí của `flush.on_level`

**flush.on_level = ERROR** (flood; 200 k lines per run split over the threads, at most 1 k ERROR lines per thread; the ERROR lines evenly spread). sync.on_level: ERROR = the default (each batch with an ERROR line is also fsynced, on the sync thread), OFF = periodic sync only. Latency p50 / p99 / p99.9 / max:

| threads | ERROR lines | sync.on_level | end-to-end lines/s | logging threads lines/s | INFO call | ERROR call |
|---|---|---|---|---|---|---|
| 1 | 0% | ERROR | 1.45 M [635 k–1.73 M] | 2.65 M | 321 ns / 631 ns / 1.5 µs / 82.6 µs | - |
| 1 | 1% | ERROR | 35.1 k [26.3 k–36.7 k] | 35.2 k | 481 ns / 2.85 µs / 6.96 µs / 68 µs | 2.69 ms / 4.34 ms / 53.6 ms / 76 ms |
| 1 | 10% | ERROR | 3.45 k [3.07 k–3.94 k] | 3.46 k | 561 ns / 4.98 µs / 11 µs / 23 µs | 2.7 ms / 5.04 ms / 18.1 ms / 62.5 ms |
| 1 | 100% | ERROR | 376 [326–398] | 376 | - | 2.54 ms / 4.96 ms / 32.8 ms / 32.9 ms |
| 1 | 0% | OFF | 1.12 M [829 k–1.58 M] | 2.8 M | 311 ns / 591 ns / 1.42 µs / 94.8 µs | - |
| 1 | 1% | OFF | 881 k [317 k–991 k] | 1.54 M | 311 ns / 691 ns / 1.33 µs / 52.2 µs | 21 µs / 88.7 µs / 152 µs / 256 µs |
| 1 | 10% | OFF | 319 k [310 k–329 k] | 405 k | 561 ns / 1.08 µs / 3.44 µs / 7.22 µs | 17.4 µs / 59.9 µs / 131 µs / 226 µs |
| 1 | 100% | OFF | 43.5 k [5.67 k–46.5 k] | 52.1 k | - | 17.2 µs / 49.8 µs / 120 µs / 200 µs |
| 8 | 0% | ERROR | 2.35 M [962 k–3.84 M] | 13.1 M | 391 ns / 661 ns / 2.47 µs / 122 µs | - |
| 8 | 1% | ERROR | 132 k [108 k–151 k] | 133 k | 361 ns / 2.2 µs / 7.4 µs / 137 µs | 5.4 ms / 15.9 ms / 23.5 ms / 23.7 ms |
| 8 | 10% | ERROR | 14.5 k [12.9 k–14.8 k] | 14.5 k | 581 ns / 6.59 µs / 17.8 µs / 84.1 µs | 5.27 ms / 11.6 ms / 146 ms / 207 ms |
| 8 | 100% | ERROR | 1.54 k [1.34 k–1.64 k] | 1.54 k | - | 5.21 ms / 9.04 ms / 14.6 ms / 111 ms |
| 8 | 0% | OFF | 4.29 M [1.62 M–4.54 M] | 13.2 M | 361 ns / 591 ns / 3.04 µs / 119 µs | - |
| 8 | 1% | OFF | 1.92 M [908 k–2.37 M] | 3.54 M | 561 ns / 1.09 µs / 2.45 µs / 114 µs | 148 µs / 428 µs / 526 µs / 552 µs |
| 8 | 10% | OFF | 407 k [309 k–1.04 M] | 711 k | 591 ns / 1.81 µs / 3.75 µs / 17.6 µs | 95.1 µs / 268 µs / 418 µs / 625 µs |
| 8 | 100% | OFF | 69.8 k [60.3 k–130 k] | 74.3 k | - | 103 µs / 233 µs / 345 µs / 458 µs |

Đọc bảng:

- **Dòng dưới level không tốn thêm gì**: dòng 0% (có `flush.on_level = ERROR` nhưng không có dòng ERROR) cho cùng latency INFO (311–391 ns p50) và thông lượng cùng cỡ với `tblog_block` ở trên (lần chạy ngắn hơn, 200 000 dòng, nên end-to-end thấp hơn và nhiễu hơn; khoảng min–max chồng lên nhau).
- **Một lệnh ERROR với `sync.on_level = OFF`**: 17–21 µs p50, 50–89 µs p99 ở 1 thread; 95–148 µs p50, 233–428 µs p99 ở 8 thread (8 thread cùng chờ những lượt ghi lớn hơn). Đây là giá của một lần đánh thức thread nền + một `WriteFile` + đánh thức lại.
- **Với `sync.on_level = ERROR` (mặc định) trên máy này, mỗi lệnh ERROR tốn khoảng một fsync: 2.5–2.7 ms p50 ở 1 thread, 5.2–5.4 ms ở 8 thread**, bất kể tỉ lệ dòng ERROR. Lý do: mỗi lô có dòng ERROR kéo theo một `FlushFileBuffers` trên thread sync, và trên NTFS, `WriteFile` của lô tiếp theo phải chờ `FlushFileBuffers` đang chạy trên cùng file xong. Ở đây dòng ERROR tới dày hơn nhiều so với tốc độ fsync (khoảng 2.5 ms một lần), nên mỗi dòng ERROR chờ fsync của dòng ERROR trước. Dòng ERROR thưa (cách nhau lâu hơn một fsync) chỉ tốn như cột `OFF`: chưa đo riêng. Trên Linux / macOS (`fdatasync` / `F_FULLFSYNC`) chưa đo.
- **Thông lượng khi nhiều dòng ERROR**: với `sync.on_level = OFF`, 10% ERROR còn 319–407 k dòng/s, 100% ERROR 44–70 k dòng/s (mỗi dòng một lượt ghi); với `sync.on_level = ERROR` bị giới hạn bởi fsync: 376 dòng/s ở 1 thread khi mọi dòng là ERROR. `flush.on_level` hợp với dòng hiếm (lỗi), không hợp với level của phần lớn các dòng (đó là lý do có cảnh báo khi đặt dưới `INFO`).
- **Lệnh INFO xen giữa** chậm đi ít (p99 2–7 µs thay vì dưới 1 µs khi có dòng ERROR với sync), vì thread nền bận ghi và fsync thường xuyên hơn.

## Kết quả mất log

20 lần thử mỗi ô, 20 000 dòng/giây mỗi thread ghi log. "Lost" = số dòng acknowledged (lệnh log đã trả về) không có trong file log, cộng mọi thread. Ô: trung bình / lớn nhất (tỉ lệ lần thử có mất dòng).

20 trials per cell, 20 k lines/s per logging thread. Lost = acknowledged lines (the logging call returned) missing from the log file. Cell: mean / max lines lost (share of trials with any loss).

| scenario | threads | fprintf_buffered | fprintf_flush | tblog | tblog_flush_error |
|---|---|---|---|---|---|
| segv_main | 1 | 23.9 / 41 (100%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| segv_other_thread | 4 | 19.2 / 39 (95%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| abort | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| assert | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| uncaught_exception | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| stack_overflow | 1 | 17.3 / 38 (100%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| stack_overflow_thread | 1 | 16.4 / 41 (100%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| kill_hard | 4 | 21.6 / 41 (100%) | 0.0 / 0 (0%) | 4612.4 / 7751 (100%) | 173.5 / 396 (100%) |
| exit | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| _Exit | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 954.4 / 1811 (100%) | 56.1 / 96 (100%) |

Outcome as expected / tblog crash marker present / duplicates, corrupt, torn lines (sums over trials) / time from the fatal action to the end of the process (mean, max):

| scenario | expected outcome | fprintf_buffered | fprintf_flush | tblog | tblog_flush_error |
|---|---|---|---|---|---|
| segv_main | exit 0xC0000005 (access violation) | 20/20; 0, 0, 19; 1.6 / 2.0 ms | 20/20; 0, 0, 0; 1.5 / 2.4 ms | 20/20 marker 20/20; 0, 0, 0; 2.4 / 4.2 ms | 20/20 marker 20/20; 0, 0, 0; 3.1 / 5.8 ms |
| segv_other_thread | exit 0xC0000005 (access violation) | 20/20; 0, 0, 19; 1.5 / 2.2 ms | 20/20; 0, 0, 0; 1.4 / 1.8 ms | 20/20 marker 20/20; 0, 0, 0; 10.3 / 21.8 ms | 20/20 marker 20/20; 0, 0, 0; 19.7 / 165.9 ms |
| abort | exit 3 (abort) | 20/20; 0, 0, 0; 2.7 / 9.0 ms | 20/20; 0, 0, 0; 2.3 / 3.7 ms | 20/20 marker 20/20; 0, 0, 0; 3.6 / 10.9 ms | 20/20 marker 20/20; 0, 0, 0; 3.6 / 5.9 ms |
| assert | exit 3 (abort) | 20/20; 0, 0, 0; 4.8 / 9.3 ms | 20/20; 0, 0, 0; 4.4 / 12.2 ms | 20/20 marker 20/20; 0, 0, 0; 5.2 / 10.0 ms | 20/20 marker 20/20; 0, 0, 0; 5.1 / 9.5 ms |
| uncaught_exception | exit 3 (abort) | 20/20; 0, 0, 0; 4.0 / 17.1 ms | 20/20; 0, 0, 0; 2.9 / 4.6 ms | 20/20 marker 20/20; 0, 0, 0; 9.2 / 21.4 ms | 20/20 marker 20/20; 0, 0, 0; 4.0 / 6.6 ms |
| stack_overflow | exit 0xC00000FD (stack overflow) | 20/20; 0, 0, 20; 3.3 / 5.7 ms | 20/20; 0, 0, 0; 3.4 / 7.6 ms | 20/20 marker 20/20; 0, 0, 0; 7.8 / 12.3 ms | 20/20 marker 20/20; 0, 0, 0; 8.5 / 85.9 ms |
| stack_overflow_thread | exit 0xC00000FD (stack overflow) | 20/20; 0, 0, 19; 3.2 / 5.7 ms | 20/20; 0, 0, 0; 3.3 / 7.8 ms | 20/20 marker 20/20; 0, 0, 0; 6.7 / 11.0 ms | 20/20 marker 20/20; 0, 0, 0; 4.6 / 6.4 ms |
| kill_hard | exit 137 (TerminateProcess) | 20/20; 0, 0, 20; 2.5 / 6.6 ms | 20/20; 0, 0, 0; 2.3 / 6.8 ms | 20/20 marker 0/20; 0, 0, 0; 3.1 / 14.3 ms | 20/20 marker 0/20; 0, 0, 0; 3.6 / 7.1 ms |
| exit | exit 0 | 20/20; 0, 0, 0; 3.2 / 5.1 ms | 20/20; 0, 0, 0; 2.9 / 4.1 ms | 20/20 marker 0/20; 0, 0, 0; 10.7 / 34.8 ms | 20/20 marker 0/20; 0, 0, 0; 6.2 / 16.7 ms |
| _Exit | exit 0 | 20/20; 0, 0, 0; 3.7 / 6.5 ms | 20/20; 0, 0, 0; 4.3 / 12.4 ms | 20/20 marker 0/20; 0, 0, 0; 5.5 / 38.9 ms | 20/20 marker 0/20; 0, 0, 0; 3.7 / 8.7 ms |

(*torn*: dòng cuối file bị cắt dở. Thời gian "fatal action → process end" đo từ ngay trước lệnh gây chết, hoặc lúc process cha gọi kill, tới khi process cha thấy process con đã kết thúc.)

**tblog_flush_error** (every 100th line of each thread is an ERROR line): acknowledged INFO lines lost / acknowledged ERROR lines lost / lines lost up to each thread's last acknowledged ERROR line (mean / max over the trials):

| scenario | threads | INFO lost | ERROR lost | lost up to the last ERROR |
|---|---|---|---|---|
| segv_main | 1 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| segv_other_thread | 4 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| abort | 1 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| assert | 1 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| uncaught_exception | 1 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| stack_overflow | 1 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| stack_overflow_thread | 1 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| kill_hard | 4 | 173.5 / 396 | 0.0 / 0 | 0.0 / 0 |
| exit | 1 | 0.0 / 0 | 0.0 / 0 | 0.0 / 0 |
| _Exit | 1 | 56.1 / 96 | 0.0 / 0 | 0.0 / 0 |

Mọi process con kết thúc đúng như kịch bản (800/800), tblog ghi dòng `*** tblog: caught ... ***` ở mọi lần thử có crash (280/280), không có dòng hỏng, không có dòng lặp, và `tblog_dropped` bằng 0 ở mọi lần thử.

### Giải thích các kết quả bất ngờ

- **`fprintf_buffered` không mất gì với `abort`, `assert`, exception không ai bắt, `_Exit`**, trái với kỳ vọng. Trên MinGW-w64 với CRT `msvcrt.dll`, các đường đó kết thúc bằng `ExitProcess`, và `msvcrt.dll` xả mọi stream khi DLL được gỡ lúc process thoát. Chỉ những cách chết không qua `ExitProcess` mới làm mất buffer stdio: exception phần cứng không ai bắt (`segv_*`, `stack_overflow*`) và `TerminateProcess`. Trên glibc, `abort()` không xả stream (từ glibc 2.27) và `_Exit` cũng không, nên kết quả trên Linux sẽ khác: chưa đo trên máy này.
- **`segv_other_thread`: tblog không mất dòng nào, không lặp dòng nào**, dù 4 thread kia vẫn chạy và vẫn log trong lúc Windows kết thúc process (trung bình 10.3 ms, tối đa 21.8 ms sau lệnh gây crash). Khi handler đã bắt đầu xả, lệnh log của các thread đó không đưa dòng vào buffer nữa mà chờ handler xả xong rồi tự ghi dòng thẳng vào file trước khi trả về, sau dòng báo crash. `fprintf_buffered` mất trung bình 19 dòng (tối đa 39), phần trong buffer 4 KB dùng chung. Vài dòng *beyond_ack* (tblog 1, `tblog_flush_error` 13, `fprintf_flush` 7, tổng của 20 lần thử): dòng đã ghi xong vào file nhưng process chết trước khi lệnh log của nó kịp trả về; không tính là mất.
- **Không lặp dòng ở mọi kịch bản**: handler chờ thread nền ghi xong chunk đang ghi dở (nếu có) rồi xả tiếp đúng từ byte mà thread nền đã ghi tới; sau đó thread nền không ghi gì thêm (xem [architecture.md](architecture.md#độ-bền-của-log)).
- **`stack_overflow_thread` của tblog không mất gì trên Windows**, dù thread đó không phải thread gọi `tblog_init`: filter của Windows vẫn chạy được trên phần stack còn lại. Trên Linux/macOS, thread không có alternate signal stack thì handler không chạy được khi tràn stack; chưa đo ở đây.
- **`kill_hard` tblog: trung bình 4 612, tối đa 7 751 dòng** (4 thread), tối đa 1 939 dòng một thread, gần bằng 75% của một shard 256 KB (khoảng 1 900 dòng) như dự đoán. `_Exit` (1 thread): trung bình 954, tối đa 1 811.
- **`kill_hard` / `_Exit` với `flush.on_level = ERROR`: 0 dòng mất tính tới dòng ERROR cuối cùng đã trả về của mỗi thread, 0 dòng ERROR mất**, trong mọi lần thử. Phần mất chỉ là các dòng INFO log sau dòng ERROR đó: tối đa 99 dòng một thread (đúng 99 ở một số lần thử: dòng ERROR tiếp theo chưa kịp trả về), trung bình 173.5 trên 4 thread với `kill_hard` (tối đa 396 = 4 × 99) và 56.1 với `_Exit` (tối đa 96), so với 4 612 và 954 khi không có `flush.on_level`. Test `flush_on_level_survives_kill_hard` (`tests/unit/test_integration.cpp`) kiểm tra cùng điều đó, mỗi dòng thứ 50 là ERROR, 4 thread flood không giới hạn tốc độ.

## Khi nào tblog tốt hơn printf / khi nào không

Dựa trên các bảng trên (một máy Windows, MinGW-w64):

- **Thread gọi log nhanh hơn ở mọi số thread**: p50 311–571 ns so với 2–17 µs của `fprintf`, và p99 dưới 1 µs so với tới hơn 1 ms khi nhiều thread tranh nhau một `FILE*`. Nếu điều quan trọng là thread làm việc không bị log làm chậm, tblog tốt hơn rõ.
- **Một thread, cùng cách format**: khoảng cách nhỏ hơn nhiều so với `fprintf` của MinGW. `snprintf` + `fwrite` chỉ chậm hơn tblog khoảng 20% ở p50 (381 so với 311 ns) và đạt 1.1 M dòng/s end-to-end so với 1.89 M. Trên nền tảng có `fprintf` nhanh (glibc, UCRT), một chương trình một thread dùng `fprintf` có buffer có thể ngang hoặc nhanh hơn tblog: chưa đo ở đây, hãy chạy `bench.yml`.
- **`kill -9`, OOM killer, `TerminateProcess`, `_Exit`: tblog mặc định mất nhiều hơn hẳn**: tới gần 2 000 dòng mỗi thread (khoảng 100 ms log) so với tối đa 41 dòng của `fprintf_buffered` và 0 của `fprintf_flush`. Với `flush.on_level = ERROR`, dòng ERROR và mọi dòng trước nó không mất, phần mất còn tối đa 99 dòng INFO mỗi thread (tùy mật độ dòng ERROR); cái giá là 17–148 µs mỗi dòng ERROR với `sync.on_level = OFF`, hoặc khoảng một fsync (2.5–5.4 ms ở đây) khi dòng ERROR tới dày với `sync.on_level = ERROR` mặc định. Nếu **từng dòng** cuối đều quan trọng khi bị giết, `fprintf` + `fflush` mỗi dòng (hoặc `flush.on_level = INFO`: mỗi dòng chờ một lượt ghi, như cột 100% ở trên) an toàn hơn; với tblog còn có thể giảm `flush.interval_ms` để thu nhỏ cửa sổ cho mọi dòng, hoặc gọi `tblog_flush()` ở các điểm quan trọng.
- **Crash mà handler bắt được (SIGSEGV, `abort`, `assert`, exception, stack overflow), trên thread đang log hay trên một thread khác trong khi các thread khác vẫn log: tblog không mất dòng nào và không lặp**, như `fprintf_flush`, trong khi `fprintf_buffered` mất tới 41 dòng và để lại một dòng cụt. Các dòng log sau lúc crash nằm sau dòng `*** tblog: caught ... ***`.
- **Flood vượt trần của thread nền** (khoảng 4–5 M dòng/s ở đây): với `full_policy = block` (mặc định) tblog không bỏ dòng, thông lượng ngang `drop`, đổi lại thread gọi log có lúc chờ (max 2.88 ms ở 16 thread). Với `drop`, tblog bỏ dòng (median 23%, lần tệ nhất 33% ở 16 thread) mà lệnh log vẫn trả về bình thường; số dòng bỏ chỉ thấy qua `tblog_dropped_count()` và dòng `tblog: N lines dropped` trong file. `fprintf` không bao giờ bỏ dòng, chỉ chậm lại.
- **Mất điện**: không biến thể nào ở đây an toàn tuyệt đối. tblog fsync định kỳ (1 s) và ngay với dòng ≥ `sync.on_level`; `fprintf` (kể cả `fflush`) không fsync. Không đo.
- **Lệnh bị lọc theo level**: cả hai khoảng 1 ns hoặc ít hơn, không có khác biệt thực tế.

## Chạy lại

Local, CMake (mọi nền tảng):

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTBLOG_BUILD_BENCH=ON
cmake --build build-bench --config Release --target bench_vs_printf bench_crash_loss
./build-bench/bench/bench_vs_printf --out-dir bench/results --build-info "Release, <compiler>"   # ~12 phút ở đây
./build-bench/bench/bench_crash_loss --out-dir bench/results --build-info "Release, <compiler>"  # ~3 phút
python3 -I bench/report.py bench/results/*.json            # bảng đầy đủ
python3 -I bench/report.py --compact bench/results/*.json  # bảng ngắn như trong README
```

Windows (MSVC): file chạy nằm trong `build-bench/bin/Release/`. Linux: `make bench` cũng build hai chương trình vào `build/bench/`. Tên file kết quả mặc định là `<os>-<compiler>-<cpu>-<ngày>-perf.json` / `-crash.json`. Các option chính (`--help` để xem đủ):

- `bench_vs_printf`: `--quick`, `--runs N`, `--threads 1,2,4,8,16`, `--paced-threads 1,4,8`, `--variants ...` (thêm `fprintf_unbuffered` nếu muốn), `--flood-lines`, `--rate`, `--scenarios flood,paced,disabled,flush_level`, `--flush-threads 1,8`, `--flush-lines`, `--flush-max-errors`.
- `bench_crash_loss`: `--quick`, `--trials N`, `--rate N` (`0` = flood), `--scenarios ...`, `--variants ...` (mặc định `fprintf_buffered,fprintf_flush,tblog,tblog_flush_error`; thêm `tblog_drop`, `fprintf_unbuffered`), `--seed`, `--keep` (giữ thư mục của từng lần thử để xem file log).

Trên GitHub: tab Actions → **bench** → *Run workflow* ([.github/workflows/bench.yml](../.github/workflows/bench.yml)). Workflow build Release (MSVC trên Windows) và chạy cả hai chương trình trên `ubuntu-latest`, `macos-latest`, `windows-latest`, upload các file JSON làm artifact và ghi bảng của `report.py` vào job summary. Máy ảo CI dùng chung và nhiễu: chỉ dùng để so các biến thể với nhau trên cùng một runner.

CTest `bench_vs_printf_smoke` và `bench_crash_loss_smoke` (khi có `TBLOG_BUILD_TESTS` và `TBLOG_BUILD_BENCH`) chạy `--quick` trên mọi nền tảng trong CI thường: chúng chỉ kiểm tra chương trình chạy hết, không phải phép đo.

Chưa kiểm chứng: mã của hai chương trình trên Linux và macOS (fork/exec, `mmap`, signal) mới chỉ được viết theo các API POSIX, chưa chạy trên máy nào; lần chạy CI đầu tiên sẽ cho biết.
