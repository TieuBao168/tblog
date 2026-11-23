# Benchmark: tblog so với `fprintf`

[English](../benchmarks.md) | **Tiếng Việt**

So tblog với cách log đơn giản nhất: `fprintf` vào một file. Không so với thư viện log khác. Mọi số lấy từ các file JSON trong [bench/results/](../../bench/results/), bảng dựng bằng `bench/report.py`.

## Kết luận

| | tblog tốt hơn | tblog kém hơn |
|---|---|---|
| Lệnh log, 1 thread | p50 nhanh hơn 1.3–1.7× (MinGW: 6.5×), p99 nhanh hơn 8–12× | Trên Linux / macOS, `fprintf` một thread đã đủ nhanh (321–375 ns p50) |
| Lệnh log, nhiều thread | p99 0.48–1.8 µs so với 34–139 µs (4 thread) | |
| Thông lượng | 2.5–13× ở 4 thread | Trần của một thread nền: 1.5 M (Windows CI) tới ~6 M dòng/s (Linux) |
| Crash, kể cả tràn stack | 0 dòng mất ở mọi nền tảng, so với TB 16–28 | Process chết chậm hơn 0.5–9 ms |
| `kill -9` | `flush.on_level = ERROR`: 0 dòng mất tới dòng ERROR cuối | Mặc định mất 1 900–4 500 dòng mỗi thread (macOS VM: 6 725), so với 41 |
| Quá tải | `block`: không mất dòng | `block`: lệnh log có lúc chờ (max 2.9–36 ms); `drop`: mất tới 47% |
| Mất điện | fsync mỗi giây và sau ERROR; `fprintf` không fsync | Không biến thể nào an toàn tuyệt đối |

## Máy đo

| Tên | Máy | Ghi chú |
|---|---|---|
| Laptop | Ryzen 7 8845HS, 16 CPU, Windows 11, NVMe, MinGW-w64 g++ 15.1 `-O2` | Phần chi tiết bên dưới. `fprintf` của MinGW chậm bất thường: `snprintf` + `fwrite` nhanh hơn 5× |
| Linux | GitHub Actions, EPYC 7763, 4 CPU, Ubuntu 26.04, g++ 15.2 | VM dùng chung |
| macOS | GitHub Actions, Apple M1 (VM), 3 CPU, macOS 26, clang 21 | VM dùng chung; đồng hồ bước 41 ns |
| Windows CI | GitHub Actions, EPYC 9V45, 4 CPU, Windows Server 2025, MSVC 19.52 | VM dùng chung; đĩa chậm (fsync) |

Số hiệu năng của CI lấy từ lần chạy bench đầu (commit `47e4c97`, máy như bảng trên). Số mất log lấy từ lần chạy sau bản sửa tràn stack (commit `12fb37d`); lần đó Linux chạy trên Xeon Platinum 8370C và Windows trên EPYC 7763 (vẫn 4 CPU). Máy CI nhiễu: chỉ nên so các biến thể với nhau trên cùng một máy, không so giữa các máy. Laptop không cố định tần số CPU: end-to-end của tblog dao động ±20–40% giữa các lần chạy. Hai thay đổi sau này chưa có trong các số này: `drop` cũng bỏ dòng khi khóa shard vẫn bận sau một vòng quay ngắn (commit `dd7791a`), và `message.escape` (bật mặc định) thêm 6–10 ns mỗi dòng ([configuration.md](configuration.md#messageescape-mỗi-lệnh-một-dòng)).

## So sánh giữa các nền tảng

**Lệnh log** p50 / p99 (flood):

| | Linux | macOS | Windows CI | Laptop |
|---|---|---|---|---|
| `fprintf`, 1 thread | 321 ns / 3.73 µs | 375 ns / 5.33 µs | 381 ns / 2.84 µs | 2.03 µs / 6.9 µs |
| tblog, 1 thread | 251 ns / 310 ns | 292 ns / 667 ns | 230 ns / 371 ns | 311 ns / 591 ns |
| `fprintf`, 4 thread | 741 ns / 59 µs | 459 ns / 96.2 µs | 1.23 µs / 34.3 µs | 8.05 µs / 139 µs |
| tblog, 4 thread | 441 ns / 501 ns | 417 ns / 1.79 µs | 361 ns / 481 ns | 551 ns / 641 ns |
| Lệnh bị lọc (tblog / `if`) | 0.63 / 0.32 ns | 0.57 / 0.32 ns | 0.50 / 0.23 ns | 1.16 / 0.60 ns |

**Thông lượng end-to-end** (tới khi đã fsync), `fprintf` → tblog:

| Thread | Linux | macOS | Windows CI | Laptop |
|---|---|---|---|---|
| 1 | 2.19 M → 3.39 M | 1.74 M → 2.77 M | 600 k → 1.18 M | 365 k → 1.89 M |
| 4 | 734 k → 6.44 M | 562 k → 3.62 M | 600 k → 1.52 M | 213 k → 2.72 M |

**Mất log** (TB / max dòng, 20 lần; cột tblog / tblog + `flush.on_level = ERROR`):

| Kịch bản | Nền tảng | `fprintf` | tblog | + `flush.on_level` |
|---|---|---|---|---|
| SIGSEGV, 4 thread khác đang log | Mọi nền tảng | 15.8–19.4 / 40 | 0 | 0 |
| `abort`, `assert`, exception | Linux | 18.8–21.1 / 40 | 0 | 0 |
| | macOS, Windows | 0 (libc xả stdio) | 0 | 0 |
| Tràn stack, thread phụ | Mọi nền tảng | 20.2–20.6 / 41 | 0 | 0 |
| `kill -9`, 4 thread | Linux | 25.1 / 41 | 7 000 / 14 886 | 203 / 396 ¹ |
| | macOS | 20.4 / 37 | 15 072 / 23 457 | 198 / 382 ¹ |
| | Windows CI | 16.5 / 39 | 9 344 / 17 825 | 320 / 396 ¹ |
| `_Exit` | Linux | 17.6 / 35 | 2 206 / 3 771 | 56 / 96 ¹ |

¹ 0 dòng mất tới dòng ERROR cuối, ở mọi nền tảng.

- **Tràn stack trên thread phụ, Linux / macOS**: trước commit `12fb37d` mất cả buffer (TB 2 065 dòng trên Linux, 2 794 trên macOS; macOS còn chết bằng SIGILL), vì chỉ thread gọi `tblog_init`, worker và thread sync có alternate signal stack. Giờ mỗi thread được cấp một stack ở lệnh log đầu tiên: 0 dòng mất, có dòng báo crash ở 20/20 lần, macOS chết bằng SIGBUS như dự kiến.
- **`kill -9` trên máy 4 CPU mất nhiều hơn laptop**: shard 1 MB nên giới hạn là 200 ms log (~4 000 dòng ở 20 k dòng/s), không phải 75% shard. Tối đa mỗi thread: 3 722 (Linux), 4 460 (Windows CI), 6 725 (macOS VM, vượt giới hạn: chưa rõ nguyên nhân).
- **`flush.on_level = ERROR` + `sync.on_level = ERROR`**, 1% dòng ERROR, 1 thread: lệnh ERROR 39 µs p50 (Linux), 37 µs (macOS), 2.92 ms (Windows CI), 2.69 ms (laptop). Việc phải chờ fsync chỉ xảy ra trên Windows.
- `drop` ở 8 thread trên Windows CI bỏ 47% số dòng (466 k / 1 M): đĩa chậm làm thread nền ghi chậm.
- Kịch bản paced với 8 thread không có ý nghĩa trên máy CI 3–4 CPU: 8 thread chờ bận chiếm hết CPU của cả thread nền.

## Cách đo

| Biến thể | Mỗi dòng |
|---|---|
| `fprintf_buffered` | Một `fprintf` vào `FILE*` dùng chung, buffer stdio mặc định |
| `fprintf_flush` | Như trên + `fflush` mỗi dòng |
| `snprintf_fwrite` | `snprintf` vào stack rồi một `fwrite` (tách chi phí format khỏi chi phí stream) |
| `tblog_block` | `TBLOG_INFO(...)`, mặc định, chỉ ghi file |
| `tblog_drop` | Như trên, `full_policy = drop` |
| `tblog_flush_error` | Như trên + `flush.on_level = ERROR`, mỗi dòng thứ 100 là ERROR |

Cùng một dòng ~102 byte theo pattern mặc định; timestamp của `fprintf` được làm nhanh giống tblog (cache theo giây).

- **Flood**: 1 M dòng chia cho 1–16 thread, nhanh nhất có thể. *End-to-end* tính tới khi mọi dòng đã ghi và fsync. Số dòng trong file được kiểm tra lại.
- **Paced**: 50 k dòng/s mỗi thread theo lịch cố định; latency tính từ thời điểm theo lịch (tránh *coordinated omission*).
- **Lệnh bị lọc**: `TBLOG_DEBUG` khi level `INFO`, so với `if (atomic_flag) fprintf(...)`.
- **Mất log**: process con log các dòng đánh số, ghi số dòng đã trả về vào bộ nhớ chia sẻ, rồi chết theo kịch bản. Process cha so với file: **mất** = số dòng đã trả về mà không có trong file.
- 5 lần chạy (perf) hoặc 20 lần thử (crash) mỗi ô, sau 1 lần khởi động. Bảng ghi median [min–max].

## Hiệu năng chi tiết (laptop)

Bảng đầy đủ của các máy CI: `python3 -I bench/report.py bench/results/<máy>-*.json`.

**Thông lượng end-to-end** (dòng/s, tới khi đã fsync):

| Thread | fprintf_buffered | fprintf_flush | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|---|
| 1 | 365 k | 179 k | 1.1 M | 1.89 M [1.51–2.04 M] | 1.82 M |
| 4 | 213 k | 127 k | 1.16 M | 2.72 M [2.28–3.49 M] | 3.28 M |
| 8 | 213 k | 124 k | 1.06 M | 4.34 M [2.41–4.82 M] | 4.48 M |
| 16 | 205 k | 120 k | 1.08 M | 4.27 M [3.12–5.59 M] | 5.04 M |

`fprintf` giảm khi thêm thread (tranh khóa của `FILE*`); tblog tăng. Phía thread gọi log, tblog đưa được 2.7 M (1 thread) tới 18.3 M dòng/s (16 thread) vào buffer: phần chênh với end-to-end là trần của thread nền.

**Latency lệnh log** (p50 / p99 / max):

| Thread | fprintf_buffered | snprintf_fwrite | tblog_block | tblog_drop |
|---|---|---|---|---|
| 1 | 2.03 µs / 6.9 µs / 10.3 ms | 381 ns / 4.52 µs / 1.21 ms | 311 ns / 591 ns / 72 µs | 311 ns / 601 ns / 62 µs |
| 4 | 8.05 µs / 139 µs / 15.2 ms | 671 ns / 52 µs / 3.31 ms | 551 ns / 641 ns / 193 µs | 561 ns / 651 ns / 158 µs |
| 8 | 8.56 µs / 330 µs / 5.28 ms | 781 ns / 114 µs / 2.47 ms | 561 ns / 671 ns / 305 µs | 561 ns / 671 ns / 151 µs |
| 16 | 9.53 µs / 729 µs / 16.7 ms | 872 ns / 226 µs / 1.93 ms | 561 ns / 671 ns / 2.88 ms | 571 ns / 691 ns / 1.99 ms |

`fprintf_flush`: 4.95 µs p50 (1 thread), 14.1 µs (8 thread). `drop` bỏ 9.4 k dòng / 1 M ở 8 thread, 233 k ở 16 thread (lần tệ nhất 330 k).

**Paced, 50 k dòng/s mỗi thread** (p50 / p99, tốc độ đạt được):

| Thread | fprintf_buffered | fprintf_flush | tblog_block |
|---|---|---|---|
| 1 | 3.62 µs / 54.7 µs | 7.86 µs / 74.6 µs | 598 ns / 3.74 µs |
| 4 | 3.32 µs / 1.18 ms | 276 ms / 557 ms (chỉ 129 k/200 k) | 539 ns / 1.75 µs |
| 8 | 406 ms / 801 ms (chỉ 222 k/400 k) | 1.06 s / 1.13 s (chỉ 128 k/400 k) | 538 ns / 2.52 µs |

Khi `fprintf` không theo kịp tốc độ, hàng đợi dồn lên và latency lên tới hàng trăm ms. tblog giữ dưới 4 µs p99 ở mọi mức.

**Lệnh bị lọc**: tblog 1.16 ns, `if` + `fprintf` 0.60 ns. Không khác biệt thực tế.

### Chi phí `flush.on_level = ERROR`

Flood 200 k dòng, p50 / p99 của lệnh ERROR:

| Thread | % ERROR | `sync.on_level = OFF` | `sync.on_level = ERROR` (mặc định) |
|---|---|---|---|
| 1 | 1% | 21 µs / 89 µs; 881 k dòng/s | 2.69 ms / 4.34 ms; 35 k dòng/s |
| 1 | 100% | 17 µs / 50 µs; 43.5 k dòng/s | 2.54 ms / 4.96 ms; 376 dòng/s |
| 8 | 1% | 148 µs / 428 µs; 1.92 M dòng/s | 5.4 ms / 15.9 ms; 132 k dòng/s |
| 8 | 100% | 103 µs / 233 µs; 69.8 k dòng/s | 5.21 ms / 9.04 ms; 1.54 k dòng/s |

- Dòng INFO xen giữa vẫn 311–591 ns p50: dòng dưới level không tốn thêm.
- Với `sync.on_level = ERROR` mỗi dòng ERROR tốn khoảng một fsync, vì trên NTFS `WriteFile` phải chờ `FlushFileBuffers` đang chạy. Linux và macOS không bị ([xem trên](#so-sánh-giữa-các-nền-tảng)). Dòng ERROR thưa (cách nhau hơn một fsync): chưa đo.
- Kết luận: `flush.on_level` hợp với dòng hiếm (lỗi), không hợp với level của phần lớn các dòng.

## Mất log chi tiết (laptop)

20 lần thử mỗi ô, 20 k dòng/s mỗi thread. Ô: dòng mất trung bình / lớn nhất.

| Kịch bản | Thread | fprintf_buffered | fprintf_flush | tblog | tblog_flush_error |
|---|---|---|---|---|---|
| SIGSEGV trên thread đang log | 1 | 23.9 / 41 | 0 | 0 | 0 |
| SIGSEGV trên thread khác | 4 | 19.2 / 39 | 0 | 0 | 0 |
| `abort`, `assert`, exception không ai bắt | 1 | 0 | 0 | 0 | 0 |
| Stack overflow (thread init / thread khác) | 1 | 17.3 / 38, 16.4 / 41 | 0 | 0 | 0 |
| `kill -9` / `TerminateProcess` | 4 | 21.6 / 41 | 0 | 4 612 / 7 751 | 174 / 396 ¹ |
| `exit` | 1 | 0 | 0 | 0 | 0 |
| `_Exit` | 1 | 0 | 0 | 954 / 1 811 | 56 / 96 ¹ |

¹ Toàn bộ là dòng INFO log sau dòng ERROR cuối: 0 dòng ERROR mất, 0 dòng mất tới dòng ERROR cuối. Tối đa 99 dòng mỗi thread (khoảng cách giữa hai dòng ERROR).

- 800/800 process con chết đúng kịch bản; tblog ghi dòng `*** tblog: caught ... ***` ở 280/280 lần crash; 0 dòng hỏng, 0 dòng lặp.
- `fprintf_buffered` để lại dòng cụt ở cuối file ở 19–20/20 lần với SIGSEGV, stack overflow và `kill -9`.
- `kill -9` của tblog: tối đa 1 939 dòng một thread, khớp với dự đoán: 16 CPU → shard 256 KB, thread nền được đánh thức khi đầy 75% ≈ 1 900 dòng.
- `fprintf_buffered` không mất gì với `abort` và `_Exit`: CRT `msvcrt.dll` của MinGW xả stream khi process thoát. glibc thì không: trên Linux `fprintf` mất TB 21 dòng với `abort` ([xem trên](#so-sánh-giữa-các-nền-tảng)).
- Tràn stack trên thread phụ không mất gì trên Windows; Linux / macOS: xem phần so sánh ở trên.
- Thời gian từ lúc gây crash tới khi process kết thúc: tblog 2.4 ms so với 1.6 ms của `fprintf` (1 thread), 10.3 ms so với 1.5 ms (4 thread khác đang log).

## Chạy lại

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DTBLOG_BUILD_BENCH=ON
cmake --build build-bench --config Release --target bench_vs_printf bench_crash_loss
./build-bench/bench/bench_vs_printf  --out-dir bench/results   # ~12 phút
./build-bench/bench/bench_crash_loss --out-dir bench/results   # ~3 phút
python3 -I bench/report.py bench/results/*.json                 # bảng đầy đủ
```

- MSVC: file chạy ở `build-bench/bin/Release/`. Linux: `make bench`.
- `--quick` để chạy nhanh; `--help` xem mọi option (số thread, biến thể, kịch bản, `--keep` giữ file log của từng lần thử).
- GitHub: Actions → **bench** → *Run workflow*: chạy trên Linux, macOS, Windows (MSVC) và ghi bảng vào job summary. Máy CI nhiễu: chỉ so các biến thể với nhau trên cùng runner.
- CTest `bench_vs_printf_smoke`, `bench_crash_loss_smoke` chỉ kiểm tra chương trình chạy hết, không phải phép đo.
