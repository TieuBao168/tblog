# Hiệu năng và độ an toàn khi crash: tblog so với printf

Trang này so tblog với cách log đơn giản nhất: `fprintf` vào một file bằng thư viện C chuẩn. Không so với thư viện log nào khác. Có hai chương trình:

- `bench/bench_vs_printf.cpp`: thông lượng, latency của mỗi lệnh log, chi phí của một lệnh bị lọc theo level.
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
| `tblog_drop` / `tblog` | `TBLOG_INFO("iteration %ld value=%d name=%s", ...)`, cấu hình mặc định trừ: chỉ ghi file (`console = false`), `reload.watch = false`, `file.max_size = 4GB` (để không xoay file giữa chừng). `full_policy = drop` là mặc định. |
| `tblog_block` | Như trên, `full_policy = block`. |

Phần timestamp của các biến thể printf làm giống tblog: ngày và giờ lấy từ cache theo thread, chỉ gọi `localtime` khi sang giây mới; micro giây, thread id (cache theo thread) và tên file (cắt sau dấu `/` hoặc `\` ở mỗi lệnh) điền ở mỗi lệnh. Đồng hồ là `GetSystemTimePreciseAsFileTime` / `clock_gettime(CLOCK_REALTIME)`, cùng nguồn với tblog.

## Phương pháp: hiệu năng (`bench_vs_printf`)

- **Flood**: mỗi thread ghi `flood-lines / số thread` dòng nhanh nhất có thể (mặc định 1 triệu dòng mỗi lần chạy), với 1, 2, 4, 8, 16 thread.
  - *End-to-end* (`e2e`): từ lúc các thread được thả ra tới khi mọi dòng đã ghi và fsync: tblog tới khi `tblog_shutdown()` trả về (xả buffer, fsync, đóng file); printf tới khi `fflush` + fsync (`_commit` / `fsync`) + `fclose` trả về. JSON có thêm `e2e_nosync_s` của printf (chỉ tới `fflush`).
  - *Loop*: tới khi thread chậm nhất ra khỏi vòng lặp (thời gian các thread gọi log thật sự bị chiếm).
  - Sau mỗi lần chạy, chương trình đếm số dòng trong file: phải bằng số dòng đã gọi trừ số dòng tblog bỏ (`tblog_dropped_count()`), không thì lần chạy bị đánh dấu hỏng.
- **Paced (open loop)**: mỗi thread gửi 50 000 dòng/giây theo lịch cố định (1, 4, 8 thread; các thread lệch pha nhau để không gửi cùng lúc). Latency tính từ **thời điểm theo lịch** tới khi lệnh trả về, nên một lệnh bị nghẽn cũng làm tăng latency của các lệnh xếp hàng sau nó (tránh *coordinated omission*). JSON có thêm latency của riêng lệnh gọi. Không đo 16 thread: 16 thread chờ bận (busy-wait) sẽ chiếm hết 16 CPU logic, kể cả của thread nền của tblog.
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

N và độ trễ được chọn ngẫu nhiên theo `--seed`, giống nhau cho mọi biến thể trong cùng một lần thử. Biến thể: `fprintf_buffered`, `fprintf_flush`, `tblog` (mặc định, `full_policy = drop`). Ở 20 000 dòng/giây mỗi thread, buffer của tblog không bao giờ đầy, nên `drop` hay `block` không khác gì (JSON ghi `tblog_dropped`, luôn bằng 0 trong các lần chạy dưới đây).

### Kỳ vọng theo lý thuyết

Viết ra trước khi đo, theo cách mỗi biến thể hoạt động:

- **`fprintf_buffered`**: dòng nằm trong buffer stdio của process (4 KB, khoảng 40 dòng) cho tới khi buffer đầy. Process chết mà không qua đường thoát bình thường của thư viện C (crash, `kill -9`) thì mất phần trong buffer: **0–40 dòng**, và dòng cuối file thường bị cắt dở (buffer được ghi ra theo khối 4 KB, không theo dòng). `exit()` xả mọi stream: không mất. `abort()` và `_Exit()` theo chuẩn C không bắt buộc xả stream (tùy thư viện C).
- **`fprintf_flush`**: mỗi lệnh trả về khi dòng đã nằm trong OS (page cache), và page cache vẫn còn khi process chết kiểu gì đi nữa: **0 dòng** ở mọi kịch bản, kể cả `kill -9`. Không bảo vệ khi mất điện (không fsync).
- **tblog**: dòng nằm trong buffer RAM của tblog tới khi thread nền ghi ra (mỗi `flush.interval_ms` = 200 ms, hoặc khi shard của thread đầy 75%).
  - Crash mà handler bắt được (SIGSEGV, `abort`, `assert`, exception không ai bắt, stack overflow trên thread có alternate stack / stack guarantee): handler chờ lần ghi đang dở của thread nền xong rồi xả phần còn lại của buffer: **0 dòng, không lặp**. Thread khác log sau khi handler đã bắt đầu xả thì không đưa vào buffer nữa: lệnh log chờ handler xả xong, rồi tự ghi dòng của nó thẳng vào file (và console) trước khi trả về, sau dòng báo crash. Nên cả *thread khác* đang log trong lúc process chết cũng **0 dòng** (xem [architecture.md](architecture.md#độ-bền-của-log)).
  - `kill -9` / `TerminateProcess`, `_Exit`: không có code nào chạy, mất **toàn bộ phần còn trong buffer**. Ở đây mỗi thread có shard riêng 256 KB (buffer 4 MB chia cho 16 shard, máy có 16 CPU logic), thread nền được đánh thức khi shard đầy 75% = 192 KB ≈ 1 900 dòng 102 byte; ở 20 000 dòng/giây mỗi thread, shard đầy 75% sau khoảng 96 ms, sớm hơn 200 ms. Nên kỳ vọng: **0 đến khoảng 1 900 dòng mỗi thread**, trung bình khoảng một nửa.
  - `exit()` / `return` từ `main`: `tblog_init` đã đăng ký `tblog_shutdown` với `atexit`: không mất.
  - Mất điện: cả ba biến thể đều chưa chắc đã fsync. tblog fsync mỗi `sync.interval_ms` (1 s) và với mỗi dòng ≥ `sync.on_level`; `fprintf` không bao giờ fsync, việc ghi xuống đĩa tùy OS. Không đo được bằng chương trình này.

## Máy đo

| | |
|---|---|
| CPU | AMD Ryzen 7 8845HS (8 nhân, 16 CPU logic) |
| RAM | 31.3 GiB |
| OS | Windows 11 Home 26H2 (10.0.26300), cắm điện, power plan Balanced |
| Ổ đĩa | NVMe SSD (Samsung MZVL81T0HELB), NTFS; thư mục tạm `%TEMP%` trên ổ C: |
| Compiler | MinGW-w64 g++ 15.1.0 (MSYS2 `mingw64`, CRT `msvcrt`), `-O2 -std=c++17 -D__USE_MINGW_ANSI_STDIO=1` |
| tblog | `libtblog_static.a` (`-O2 -fvisibility=hidden`, có zlib); `bench_vs_printf`: commit `334fb5f` + mã nguồn `bench/` chưa commit; `bench_crash_loss`: commit `ca40b53` + bản sửa đường crash (ghi đồng bộ sau crash) chưa commit |
| Ngày | 2026-11-21 (UTC) |
| Thời gian chạy | `bench_vs_printf`: 539 s; `bench_crash_loss`: 121 s (600 process con) |

Đây là **một máy, một lần đo**, trên laptop đang chạy các chương trình khác, không cố định tần số CPU. Khoảng min–max trong bảng cho thấy độ nhiễu; ví dụ end-to-end của tblog dao động ±20–40% giữa các lần chạy. Bộ đếm TSC trên CPU này tăng theo bước khoảng 10 ns, nên latency mỗi lệnh có độ phân giải 10 ns (một cặp lần đọc tốn 10 ns, đã nằm trong số đo).

**Lưu ý quan trọng về `fprintf` trên máy này**: với MinGW-w64 và `__USE_MINGW_ANSI_STDIO=1` (mặc định của g++ trên MinGW, và tblog cũng build như vậy), `fprintf` là bản printf C99 riêng của mingw-w64, và đường ghi vào `FILE*` của nó chậm: biến thể `snprintf_fwrite` (cùng dòng, cùng thư viện, chỉ khác là format vào buffer trên stack rồi `fwrite` một lần) nhanh hơn khoảng 5 lần ở 1 thread. `fprintf` của glibc (Linux) hay UCRT (MSVC) có thể nhanh hơn nhiều; các số `fprintf_*` dưới đây **không đại diện cho mọi nền tảng**. Workflow `bench.yml` đo trên Linux (glibc), macOS và Windows (MSVC), xem [Chạy lại](#chạy-lại).

## Kết quả hiệu năng

Mỗi ô: median [min–max] của 5 lần chạy (sau 1 lần khởi động bị bỏ).

**Flood, end-to-end** (lines/s until everything is written and fsynced; 1 M lines per run split over the threads):

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 394 k [367 k–397 k] | 173 k [173 k–180 k] | 1.17 M [1.14 M–1.18 M] | 1.94 M [1.76 M–2 M] | 1.75 M [1.63 M–1.95 M] |
| 2 | 229 k [220 k–233 k] | 113 k [112 k–118 k] | 1.18 M [954 k–1.23 M] | 2.32 M [1.63 M–2.52 M] | 2.02 M [1.2 M–2.48 M] |
| 4 | 226 k [211 k–390 k] | 129 k [128 k–226 k] | 1.31 M [1.29 M–1.36 M] | 3.48 M [3.1 M–4.08 M] | 2.95 M [2.28 M–4.13 M] |
| 8 | 216 k [213 k–216 k] | 125 k [123 k–149 k] | 1.33 M [1.09 M–1.79 M] | 3.83 M [3.33 M–4.7 M] | 3.77 M [2.03 M–4.59 M] |
| 16 | 202 k [199 k–208 k] | 120 k [117 k–121 k] | 1.18 M [1.12 M–1.2 M] | 4.44 M [4.36 M–5.54 M] | 4.92 M [3.37 M–5.21 M] |

**Flood, logging threads only** (lines/s until the slowest thread left its loop):

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 429 k | 184 k | 1.55 M | 2.63 M | 2.58 M |
| 2 | 239 k | 113 k | 1.54 M | 3.7 M | 4.32 M |
| 4 | 235 k | 130 k | 1.84 M | 7.53 M | 7.1 M |
| 8 | 224 k | 125 k | 1.69 M | 14 M | 13.6 M |
| 16 | 211 k | 121 k | 1.54 M | 20.6 M | 22.4 M |

**Flood, caller latency per call** (p50 / p99 / p99.9 / max, medians across runs):

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 1.99 µs / 7.21 µs / 46.6 µs / 5.61 ms | 4.99 µs / 9.89 µs / 57.2 µs / 10.1 ms | 381 ns / 4.92 µs / 41.5 µs / 1.41 ms | 321 ns / 631 ns / 1.63 µs / 137 µs | 321 ns / 621 ns / 1.15 µs / 104 µs |
| 2 | 4.38 µs / 56.7 µs / 111 µs / 16.6 ms | 15.9 µs / 75.7 µs / 191 µs / 18.4 ms | 641 ns / 8.24 µs / 67.1 µs / 1.04 ms | 581 ns / 641 ns / 1.33 µs / 108 µs | 341 ns / 631 ns / 1.23 µs / 73.4 µs |
| 4 | 7.93 µs / 130 µs / 214 µs / 6.57 ms | 12.8 µs / 351 µs / 763 µs / 30.3 ms | 681 ns / 41.5 µs / 102 µs / 1.61 ms | 491 ns / 641 ns / 1.25 µs / 268 µs | 481 ns / 651 ns / 1.27 µs / 110 µs |
| 8 | 8.71 µs / 313 µs / 531 µs / 18.5 ms | 14.9 µs / 669 µs / 1.25 ms / 21.9 ms | 751 ns / 99.4 µs / 300 µs / 1.24 ms | 571 ns / 691 ns / 1.93 µs / 661 µs | 571 ns / 691 ns / 1.66 µs / 323 µs |
| 16 | 11.1 µs / 701 µs / 1.2 ms / 7.15 ms | 19.1 µs / 1.19 ms / 2.1 ms / 47.9 ms | 862 ns / 200 µs / 466 µs / 17 ms | 571 ns / 721 ns / 2.86 µs / 2.85 ms | 571 ns / 751 ns / 2.51 µs / 1.77 ms |

Dropped lines (full_policy = drop, median per run of 1 M): tblog_drop 2 thr: 14.8 k, tblog_drop 8 thr: 8.79 k, tblog_drop 16 thr: 199 k.

**Paced (open loop), 50 k lines/s per thread**: latency from the scheduled send time (p50 / p99 / p99.9 / max) and the rate achieved:

| threads | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 2 µs / 31.5 µs / 2.29 ms / 3.1 ms (50 k/s) | 4.98 µs / 34.2 µs / 220 µs / 690 µs (50 k/s) | 410 ns / 28.4 µs / 237 µs / 1.06 ms (50 k/s) | 340 ns / 1.89 µs / 17.9 µs / 117 µs (50 k/s) | 342 ns / 1.43 µs / 20.9 µs / 98.9 µs (50 k/s) |
| 4 | 3.27 µs / 554 µs / 1.07 ms / 1.45 ms (200 k/s) | 265 ms / 550 ms / 554 ms / 555 ms (130 k/s) | 597 ns / 1 ms / 6.29 ms / 7.2 ms (200 k/s) | 547 ns / 1.13 µs / 12.6 µs / 159 µs (200 k/s) | 548 ns / 1.22 µs / 15.2 µs / 214 µs (200 k/s) |
| 8 | 395 ms / 803 ms / 815 ms / 819 ms (222 k/s) | 1.07 s / 1.13 s / 1.13 s / 1.13 s (128 k/s) | 605 ns / 283 µs / 3.89 ms / 4.78 ms (400 k/s) | 545 ns / 1.18 µs / 22.8 µs / 583 µs (400 k/s) | 549 ns / 1.56 µs / 20.4 µs / 314 µs (400 k/s) |

**Disabled level** (one call that is filtered out, 20 M calls per run):

| variant | ns per call |
|---|---|
| printf_if_guard | 0.35 [0.34–0.35] |
| tblog_below_level | 0.68 [0.67–0.69] |

Đọc bảng:

- **Một thread**: tblog trả về sau 321 ns (p50) so với 1.99 µs của `fprintf_buffered` và 381 ns của `snprintf_fwrite`; end-to-end (tới khi đã fsync) tblog 1.75–1.94 M dòng/s, `snprintf_fwrite` 1.17 M, `fprintf_buffered` 394 k.
- **Nhiều thread**: `fprintf` dùng chung một `FILE*` nên các thread xếp hàng chờ khóa của stream: thông lượng giảm khi thêm thread (394 k → 202 k dòng/s từ 1 lên 16 thread) và p99 lên tới hàng trăm µs. tblog tăng theo số thread (1.75 M → 4.92 M end-to-end, 2.58 M → 22.4 M ở phía các thread gọi log) và p99 giữ dưới 1 µs.
- **Trần của tblog là thread nền**: các thread gọi log đưa được 13.6–22.4 M dòng/s vào buffer ở 8–16 thread, nhưng end-to-end chỉ 3.8–4.9 M dòng/s: một thread nền ghi ra một file. Khi flood vượt trần đó, `full_policy = drop` **bỏ dòng**: median 199 k trên 1 M dòng ở 16 thread (lần tệ nhất 260 k), 8.8 k ở 8 thread, lần tệ nhất ở 1 thread 7.8 k. `block` không bỏ dòng mà thông lượng tương đương; cái giá là đuôi latency (max 2.85 ms ở 16 thread).
- **Paced 50 000 dòng/giây mỗi thread**: `fprintf_flush` không theo kịp ở 4 thread (130 k/s thay vì 200 k/s), `fprintf_buffered` ở 8 thread (222 k/s thay vì 400 k/s): hàng đợi dồn lên và latency tính từ lịch lên tới hàng trăm ms – 1 s. tblog giữ p50 khoảng 340–550 ns, p99 1.1–1.9 µs ở mọi mức.
- **Lệnh bị lọc**: 0.68 ns (tblog) so với 0.35 ns (`if` với một `atomic<bool>`). Cả hai đều không đáng kể so với một lệnh log thật.

## Kết quả mất log

20 lần thử mỗi ô, 20 000 dòng/giây mỗi thread ghi log. "Lost" = số dòng acknowledged (lệnh log đã trả về) không có trong file log, cộng mọi thread. Ô: trung bình / lớn nhất (tỉ lệ lần thử có mất dòng).

| scenario | threads | fprintf_buffered | fprintf_flush | tblog |
|---|---|---|---|---|
| segv_main | 1 | 20.7 / 41 (100%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| segv_other_thread | 4 | 21.2 / 38 (100%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| abort | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| assert | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| uncaught_exception | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| stack_overflow | 1 | 20.5 / 38 (100%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| stack_overflow_thread | 1 | 22.6 / 39 (100%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| kill_hard | 4 | 23.1 / 39 (100%) | 0.0 / 0 (0%) | 3660.1 / 7432 (100%) |
| exit | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 0.0 / 0 (0%) |
| _Exit | 1 | 0.0 / 0 (0%) | 0.0 / 0 (0%) | 955.3 / 1830 (100%) |

Outcome as expected / tblog crash marker present / duplicates, corrupt, torn lines (sums over trials) / time from the fatal action to the end of the process (mean, max):

| scenario | expected outcome | fprintf_buffered | fprintf_flush | tblog |
|---|---|---|---|---|
| segv_main | exit 0xC0000005 (access violation) | 20/20; 0, 0, 19; 1.9 / 5.1 ms | 20/20; 0, 0, 0; 1.6 / 2.4 ms | 20/20 marker 20/20; 0, 0, 0; 4.6 / 18.6 ms |
| segv_other_thread | exit 0xC0000005 (access violation) | 20/20; 0, 0, 20; 1.8 / 3.3 ms | 20/20; 0, 0, 0; 1.5 / 2.1 ms | 20/20 marker 20/20; 0, 0, 0; 7.3 / 14.2 ms |
| abort | exit 3 (abort) | 20/20; 0, 0, 0; 14.4 / 67.0 ms | 20/20; 0, 0, 0; 14.0 / 71.3 ms | 20/20 marker 20/20; 0, 0, 0; 11.8 / 17.6 ms |
| assert | exit 3 (abort) | 20/20; 0, 0, 0; 14.2 / 17.8 ms | 20/20; 0, 0, 0; 13.7 / 18.8 ms | 20/20 marker 20/20; 0, 0, 0; 13.7 / 25.7 ms |
| uncaught_exception | exit 3 (abort) | 20/20; 0, 0, 0; 15.6 / 55.3 ms | 20/20; 0, 0, 0; 12.0 / 19.1 ms | 20/20 marker 20/20; 0, 0, 0; 15.5 / 24.9 ms |
| stack_overflow | exit 0xC00000FD (stack overflow) | 20/20; 0, 0, 20; 2.8 / 3.7 ms | 20/20; 0, 0, 0; 2.8 / 3.8 ms | 20/20 marker 20/20; 0, 0, 0; 16.5 / 68.0 ms |
| stack_overflow_thread | exit 0xC00000FD (stack overflow) | 20/20; 0, 0, 20; 2.8 / 4.8 ms | 20/20; 0, 0, 0; 2.7 / 3.6 ms | 20/20 marker 20/20; 0, 0, 0; 8.5 / 19.5 ms |
| kill_hard | exit 137 (TerminateProcess) | 20/20; 0, 0, 20; 1.8 / 3.2 ms | 20/20; 0, 0, 0; 2.5 / 17.8 ms | 20/20 marker 0/20; 0, 0, 0; 1.7 / 2.4 ms |
| exit | exit 0 | 20/20; 0, 0, 0; 12.8 / 63.4 ms | 20/20; 0, 0, 0; 11.5 / 18.2 ms | 20/20 marker 0/20; 0, 0, 0; 17.3 / 92.8 ms |
| _Exit | exit 0 | 20/20; 0, 0, 0; 14.5 / 75.0 ms | 20/20; 0, 0, 0; 11.1 / 17.2 ms | 20/20 marker 0/20; 0, 0, 0; 7.6 / 14.7 ms |

(*torn*: dòng cuối file bị cắt dở. Thời gian "fatal action → process end" đo từ ngay trước lệnh gây chết, hoặc lúc process cha gọi kill, tới khi process cha thấy process con đã kết thúc.)

Mọi process con kết thúc đúng như kịch bản (600/600), tblog ghi dòng `*** tblog: caught ... ***` ở mọi lần thử có crash (140/140), không có dòng hỏng, và `tblog_dropped` bằng 0 ở mọi lần thử.

### Giải thích các kết quả bất ngờ

- **`fprintf_buffered` không mất gì với `abort`, `assert`, exception không ai bắt, `_Exit`**, trái với kỳ vọng. Trên MinGW-w64 với CRT `msvcrt.dll`, các đường đó kết thúc bằng `ExitProcess`, và `msvcrt.dll` xả mọi stream khi DLL được gỡ lúc process thoát. Chỉ những cách chết không qua `ExitProcess` mới làm mất buffer stdio: exception phần cứng không ai bắt (`segv_*`, `stack_overflow*`) và `TerminateProcess`. Trên glibc, `abort()` không xả stream (từ glibc 2.27) và `_Exit` cũng không, nên kết quả trên Linux sẽ khác: chưa đo trên máy này.
- **`segv_other_thread`: tblog không mất dòng nào, không lặp dòng nào**, dù 4 thread kia vẫn chạy và vẫn log trong lúc Windows kết thúc process (trung bình 7.3 ms, tối đa 14.2 ms sau lệnh gây crash). Khi handler đã bắt đầu xả, lệnh log của các thread đó không đưa dòng vào buffer nữa mà chờ handler xả xong rồi tự ghi dòng thẳng vào file trước khi trả về, sau dòng báo crash. `fprintf_buffered` mất trung bình 21 dòng (tối đa 38), phần trong buffer 4 KB dùng chung. Tổng 2 dòng *beyond_ack* (ở 2 lần thử): dòng đã ghi xong vào file nhưng process chết trước khi lệnh log của nó kịp trả về; không tính là mất.
- **Không lặp dòng ở mọi kịch bản**: handler chờ thread nền ghi xong chunk đang ghi dở (nếu có) rồi xả tiếp đúng từ byte mà thread nền đã ghi tới; sau đó thread nền không ghi gì thêm (xem [architecture.md](architecture.md#độ-bền-của-log)).
- **`stack_overflow_thread` của tblog không mất gì trên Windows**, dù thread đó không phải thread gọi `tblog_init`: filter của Windows vẫn chạy được trên phần stack còn lại. Trên Linux/macOS, thread không có alternate signal stack thì handler không chạy được khi tràn stack; chưa đo ở đây.
- **`kill_hard` tblog: trung bình 3 660, tối đa 7 432 dòng** (4 thread), tức tối đa 1 858 dòng mỗi thread, gần bằng 75% của một shard 256 KB (khoảng 1 900 dòng) như dự đoán. `_Exit` (1 thread): trung bình 955, tối đa 1 830.

## Khi nào tblog tốt hơn printf / khi nào không

Dựa trên các bảng trên (một máy Windows, MinGW-w64):

- **Thread gọi log nhanh hơn ở mọi số thread**: p50 321–571 ns so với 2–19 µs của `fprintf`, và p99 dưới 1 µs so với tới hàng trăm µs khi nhiều thread tranh nhau một `FILE*`. Nếu điều quan trọng là thread làm việc không bị log làm chậm, tblog tốt hơn rõ.
- **Một thread, cùng cách format**: khoảng cách nhỏ hơn nhiều so với `fprintf` của MinGW. `snprintf` + `fwrite` chỉ chậm hơn tblog khoảng 20% ở p50 (381 so với 321 ns) và đạt 1.17 M dòng/s end-to-end so với 1.75 M. Trên nền tảng có `fprintf` nhanh (glibc, UCRT), một chương trình một thread dùng `fprintf` có buffer có thể ngang hoặc nhanh hơn tblog: chưa đo ở đây, hãy chạy `bench.yml`.
- **`kill -9`, OOM killer, `TerminateProcess`, `_Exit`: tblog mất nhiều hơn hẳn**. Tới gần 2 000 dòng mỗi thread (khoảng 100 ms log) so với tối đa 39 dòng của `fprintf_buffered` và 0 của `fprintf_flush`. Nếu process hay bị giết từ bên ngoài và từng dòng cuối đều quan trọng, `fprintf` + `fflush` mỗi dòng an toàn hơn; với tblog, giảm `flush.interval_ms` hoặc `buffer.size` để thu nhỏ cửa sổ đó, hoặc gọi `tblog_flush()` ở các điểm quan trọng.
- **Crash mà handler bắt được (SIGSEGV, `abort`, `assert`, exception, stack overflow), trên thread đang log hay trên một thread khác trong khi các thread khác vẫn log: tblog không mất dòng nào và không lặp**, như `fprintf_flush`, trong khi `fprintf_buffered` mất tới 41 dòng và để lại một dòng cụt. Các dòng log sau lúc crash nằm sau dòng `*** tblog: caught ... ***`.
- **Flood vượt trần của thread nền** (khoảng 4–5 M dòng/s ở đây): với `full_policy = drop` (mặc định) tblog bỏ dòng (tới 26% ở 16 thread) mà lệnh log vẫn trả về bình thường; số dòng bỏ chỉ thấy qua `tblog_dropped_count()` và dòng `tblog: N lines dropped` trong file. `fprintf` không bao giờ bỏ dòng, chỉ chậm lại. Cần mọi dòng thì dùng `full_policy = block`.
- **Mất điện**: không biến thể nào ở đây an toàn tuyệt đối. tblog fsync định kỳ (1 s) và ngay với dòng ≥ `sync.on_level`; `fprintf` (kể cả `fflush`) không fsync. Không đo.
- **Lệnh bị lọc theo level**: cả hai dưới 1 ns, không có khác biệt thực tế.

## Chạy lại

Local, CMake (mọi nền tảng):

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTBLOG_BUILD_BENCH=ON
cmake --build build-bench --config Release --target bench_vs_printf bench_crash_loss
./build-bench/bench/bench_vs_printf --out-dir bench/results --build-info "Release, <compiler>"   # ~9 phút ở đây
./build-bench/bench/bench_crash_loss --out-dir bench/results --build-info "Release, <compiler>"  # ~2 phút
python3 -I bench/report.py bench/results/*.json            # bảng đầy đủ
python3 -I bench/report.py --compact bench/results/*.json  # bảng ngắn như trong README
```

Windows (MSVC): file chạy nằm trong `build-bench/bin/Release/`. Linux: `make bench` cũng build hai chương trình vào `build/bench/`. Tên file kết quả mặc định là `<os>-<compiler>-<cpu>-<ngày>-perf.json` / `-crash.json`. Các option chính (`--help` để xem đủ):

- `bench_vs_printf`: `--quick`, `--runs N`, `--threads 1,2,4,8,16`, `--paced-threads 1,4,8`, `--variants ...` (thêm `fprintf_unbuffered` nếu muốn), `--flood-lines`, `--rate`, `--scenarios flood,paced,disabled`.
- `bench_crash_loss`: `--quick`, `--trials N`, `--rate N` (`0` = flood), `--scenarios ...`, `--variants ...` (thêm `tblog_block`, `fprintf_unbuffered`), `--seed`, `--keep` (giữ thư mục của từng lần thử để xem file log).

Trên GitHub: tab Actions → **bench** → *Run workflow* ([.github/workflows/bench.yml](../.github/workflows/bench.yml)). Workflow build Release (MSVC trên Windows) và chạy cả hai chương trình trên `ubuntu-latest`, `macos-latest`, `windows-latest`, upload các file JSON làm artifact và ghi bảng của `report.py` vào job summary. Máy ảo CI dùng chung và nhiễu: chỉ dùng để so các biến thể với nhau trên cùng một runner.

CTest `bench_vs_printf_smoke` và `bench_crash_loss_smoke` (khi có `TBLOG_BUILD_TESTS` và `TBLOG_BUILD_BENCH`) chạy `--quick` trên mọi nền tảng trong CI thường: chúng chỉ kiểm tra chương trình chạy hết, không phải phép đo.

Chưa kiểm chứng: mã của hai chương trình trên Linux và macOS (fork/exec, `mmap`, signal) mới chỉ được viết theo các API POSIX, chưa chạy trên máy nào; lần chạy CI đầu tiên sẽ cho biết.
