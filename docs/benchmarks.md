# Benchmark: tblog so với `fprintf`

So tblog với cách log đơn giản nhất: `fprintf` vào một file. Không so với thư viện log khác. Mọi số lấy từ [perf JSON](../bench/results/windows-mingw-gcc15-ryzen-7-8845hs-2026-11-21-perf.json) và [crash JSON](../bench/results/windows-mingw-gcc15-ryzen-7-8845hs-2026-11-21-crash.json), bảng dựng bằng `bench/report.py`.

## Kết luận

| | tblog tốt hơn | tblog kém hơn |
|---|---|---|
| Lệnh log | 311 ns p50 so với 2.03 µs (1 thread); p99 671 ns so với 330 µs (8 thread) | So với `snprintf` + `fwrite`: chỉ hơn 20% ở 1 thread (311 so với 381 ns) |
| Thông lượng | 4.34 M dòng/s so với 213 k (8 thread) | Trần ~4–5 M dòng/s (một thread nền) |
| Crash | 0 dòng mất so với TB 24 | Process chết chậm hơn 0.8–9 ms |
| `kill -9` | Với `flush.on_level = ERROR`: 0 dòng mất tới dòng ERROR cuối | Mặc định mất tới 1 939 dòng mỗi thread, so với 41 |
| Quá tải | `block`: không mất dòng | `block`: lệnh log có lúc chờ (max 2.88 ms, 16 thread); `drop`: mất tới 33% |
| Mất điện | fsync mỗi giây và sau ERROR; `fprintf` không fsync | Không biến thể nào an toàn tuyệt đối |

## Máy đo

Ryzen 7 8845HS (16 CPU logic), 31 GiB RAM, Windows 11 26H2, NVMe NTFS, MinGW-w64 g++ 15.1 `-O2`, tblog tĩnh. Một máy, laptop không cố định tần số CPU: end-to-end của tblog dao động ±20–40% giữa các lần chạy.

**Lưu ý**: `fprintf` của MinGW (`__USE_MINGW_ANSI_STDIO=1`) chậm bất thường: cùng dòng đó, `snprintf` + `fwrite` nhanh hơn 5×. Số `fprintf` ở đây **không đại diện** cho glibc hay MSVC; chạy `bench.yml` để đo trên các nền tảng đó.

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

## Kết quả hiệu năng

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
- Với `sync.on_level = ERROR` mỗi dòng ERROR tốn khoảng một fsync, vì trên NTFS `WriteFile` phải chờ `FlushFileBuffers` đang chạy. Dòng ERROR thưa (cách nhau hơn một fsync) và Linux / macOS: chưa đo.
- Kết luận: `flush.on_level` hợp với dòng hiếm (lỗi), không hợp với level của phần lớn các dòng.

## Kết quả mất log

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
- `kill -9` của tblog: tối đa 1 939 dòng một thread, khớp với dự đoán: shard 256 KB, thread nền được đánh thức khi đầy 75% ≈ 1 900 dòng.
- `fprintf_buffered` không mất gì với `abort` và `_Exit`: CRT `msvcrt.dll` của MinGW xả stream khi process thoát. glibc thì không (từ 2.27): trên Linux kết quả sẽ khác, chưa đo.
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
