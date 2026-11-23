# Cấu hình (`log.conf`)

[English](../configuration.md) | **Tiếng Việt**

`tblog_init(path)` đọc file `key = value`. `path` là `NULL`, rỗng hoặc không đọc được thì mọi key lấy mặc định. File mẫu đủ mọi key: [log.conf](../../log.conf).

Số đo trên trang này lấy từ laptop Windows 11 (Ryzen 7 8845HS, NVMe, MinGW g++ 15), trừ khi ghi rõ Linux / macOS / MSVC (GitHub Actions) ([benchmarks.md](benchmarks.md)).

## Cú pháp

- Mỗi dòng một `key = value`; key lặp lại thì dòng sau thắng.
- Tên key phân biệt hoa thường (`Level` là key lạ); giá trị thì không.
- Comment: `#` đầu dòng, hoặc ` #` sau khoảng trắng. Riêng `pattern` lấy hết phần còn lại của dòng, kể cả `#`.
- `"..."` giữ khoảng trắng đầu/cuối hoặc ` #` trong giá trị. Không có escape.
- Kiểu giá trị: level `TRACE` `DEBUG` `INFO` `WARN` `ERROR` `FATAL` `OFF`; boolean `true/false`, `1/0`, `yes/no`, `on/off`; kích thước `64MB`, `512K`, `4 GB` (cơ số 1024, số nguyên); mili giây là số nguyên không hậu tố (`200ms` sai).
- Giá trị sai: cảnh báo ra stderr, key giữ giá trị trước. `tblog_init` không bao giờ thất bại vì cấu hình.

## Mọi key

| Key | Mặc định | Ý nghĩa | Đổi lúc chạy |
|---|---|---|---|
| `level` | `INFO` | Level gốc, áp cho mọi module chưa đặt riêng | Có |
| `module.<tên>` | theo `level` | Level riêng của một module (tên ≤ 45 ký tự, ≤ 256 module) | Có ¹ |
| `pattern` | `%D %T.%us [%L] [%t] %f:%l - %m` | Khuôn dòng, xem [token](#pattern) | Có |
| `file` | `true` | Ghi file | Không ² |
| `file.path` | `./logs/app.log` | File log; tương đối theo thư mục làm việc lúc init | Không ² |
| `file.max_size` | `256MB` | Xoay khi file vượt | Không |
| `file.max_files` | `5` | Số file giữ, kể cả file đang ghi | Không |
| `file.rotate` | `size` | `size`, `daily`, `hourly` | Không |
| `file.compress` | `none` | `none`, `gzip` (cần build có zlib) | Không |
| `buffer.size` | `4MB` | Buffer RAM, chia cho các shard | Không ² |
| `full_policy` | `block` | Buffer đầy: chờ (`block`) hay bỏ dòng (`drop`) | Không |
| `flush.interval_ms` | `200` | Chu kỳ ghi tối đa, 1–60000 | Có |
| `flush.on_level` | `OFF` | Dòng ≥ level chỉ trả về khi đã ghi vào file | Có |
| `sync.interval_ms` | `1000` | Chu kỳ fsync, 0–3600000 (`0` = tắt) | Có |
| `sync.on_level` | `ERROR` | fsync ngay sau dòng ≥ level | Không |
| `console` | `false` | Ghi thêm ra stderr | Không ² |
| `console.color` | `true` | Màu theo level khi stderr là terminal | Không |
| `syslog` | `false` | Ghi thêm vào syslog (Linux, macOS) | Không ² |
| `reload.watch` | `true` | Đọc lại file cấu hình khi đổi (kiểm tra mỗi ≥ 1 s) | Không |

¹ Đọc lại là đọc **chồng lên** cấu hình đang chạy: xóa dòng `module.x` không gỡ level riêng của `x`, phải ghi giá trị mới.
² Đổi lúc chạy thì in `file/buffer/sink changes need a restart` và giữ giá trị cũ. Các key "Không" còn lại: giữ giá trị cũ, không báo.

## Các lựa chọn và cái giá

### `full_policy`: buffer đầy thì chờ hay bỏ dòng

Chỉ có tác dụng khi log nhanh hơn trần của thread nền (khoảng 4–5 M dòng/s ở máy đo). Flood 1 M dòng:

| | `block` (mặc định) | `drop` |
|---|---|---|
| Dòng bị bỏ, 8 / 16 thread | 0 / 0 | 0.9% / 23% (tệ nhất 33%) |
| Lệnh log p99 / max, 16 thread | 671 ns / 2.88 ms | 691 ns / 1.99 ms |
| Thông lượng end-to-end, 16 thread | 4.27 M/s | 5.04 M/s |

- **`block`**: không mất dòng; đổi lại khi quá tải, thread gọi log có lúc chờ vài ms, và đĩa chậm sẽ làm chậm ứng dụng.
- **`drop`**: lệnh log không bao giờ chờ thread nền, chờ đĩa hay chờ thread đang giữ khóa của shard (khóa không trống sau một vòng quay ngắn, có giới hạn, thì dòng cũng bị bỏ; nhờ vậy thread real-time không bị kẹt sau một thread ưu tiên thấp bị chen ngang lúc đang log); đổi lại mất log đúng lúc quá tải (thường là lúc có chuyện). Một thread ồn có thể làm đầy shard của nó trong khi shard khác còn trống. Theo dõi bằng `tblog_dropped_count()` và dòng `tblog: N lines dropped` trong file.
- Chọn `drop` chỉ khi thread gọi log có deadline cứng (vòng lặp thời gian thực, xử lý mạng).

### `flush.on_level`: sống sót qua `kill -9`

Mặc định lệnh log trả về khi dòng còn trong RAM, nên `kill -9`, OOM killer, `TerminateProcess`, `_exit` làm mất phần còn trong buffer. Với `flush.on_level = ERROR`, lệnh ERROR chỉ trả về khi dòng đó (và mọi dòng trước nó của cùng thread) đã `write()` vào file.

| | `OFF` (mặc định) | `ERROR` |
|---|---|---|
| Mất sau `kill -9`, 4 thread (TB / max, 20 lần) | 4 612 / 7 751 dòng | 174 / 396 dòng INFO, **0 tới dòng ERROR cuối** |
| Mất sau `_Exit`, 1 thread | 954 / 1 811 | 56 / 96, **0 tới dòng ERROR cuối** |
| Mỗi dòng ERROR, p50 1 / 8 thread, `sync.on_level = OFF` | 311 / 561 ns | 21 / 148 µs |
| Như trên, `sync.on_level = ERROR`, ERROR dày | | 2.69 / 5.4 ms (≈ một fsync) |
| Dòng dưới level | 311 ns | 311–391 ns: không đổi |

- **Ưu**: dòng lỗi cuối luôn còn khi process bị giết; dòng dưới level không tốn thêm.
- **Nhược**: mỗi dòng ERROR chậm 70–260× (21–148 µs thay vì 311–561 ns). Chỉ trên Windows: kết hợp với `sync.on_level = ERROR` mà dòng ERROR dày thì mỗi dòng chờ cả một fsync (2.69–2.92 ms p50), vì `WriteFile` phải đợi `FlushFileBuffers` đang chạy. Linux và macOS không bị: 39 và 37 µs p50 với cùng cấu hình.
- **Không đặt dưới `INFO`**: khi mọi dòng phải chờ, 1 thread chỉ còn 43.5 k dòng/s (`sync.on_level = OFF`) hoặc 376 dòng/s (`ERROR`). tblog cảnh báo.
- Không fsync (mất điện vẫn cần `sync.*`). Không bao giờ treo: chờ tối đa 2 s; không chờ khi đang shutdown, đang crash, dòng bị `drop`, hoặc khi gọi từ thread nền.

### `sync.on_level`, `sync.interval_ms`: sống sót qua mất điện

fsync đẩy dữ liệu từ cache của OS xuống đĩa; tốn khoảng 2.5 ms trên NVMe ở máy đo. Nó chạy trên thread sync riêng, nên lệnh log không chờ.

| Cấu hình | Ưu | Nhược |
|---|---|---|
| `sync.on_level = ERROR` (mặc định) | Dòng lỗi nằm trên đĩa vài ms sau khi log | Mỗi lượt ghi có dòng ERROR kéo theo một fsync |
| `sync.interval_ms` nhỏ | Mất điện mất ít hơn: tối đa ≈ `sync.interval_ms` + `flush.interval_ms` | Nhiều lần ghi thật hơn (hao flash, pin) |
| `sync.interval_ms = 0`, `sync.on_level = FATAL` | Ít lần ghi thật nhất, hợp với flash | Mất điện có thể mất tới khi kernel tự ghi (Linux: khoảng 30 s) |

macOS dùng `F_FULLFSYNC` (ra khỏi cả cache của ổ đĩa), mỗi lần cỡ ms.

**`flush.on_level` khác `sync.on_level`:**

| | `flush.on_level` | `sync.on_level` |
|---|---|---|
| Lệnh log | Chờ tới khi dòng đã vào file | Trả về ngay |
| Sau lượt ghi | Không fsync | fsync (thread sync) |
| Sống sót `kill -9` khi lệnh trả về | Có | Chưa chắc |
| Sống sót mất điện | Sau fsync kế tiếp | Vài ms sau khi lệnh trả về |

Dùng cả hai ở `ERROR`: dòng lỗi đã vào OS khi lệnh trả về, và lên đĩa vài ms sau.

### `flush.interval_ms`: chu kỳ ghi

Thread nền ghi mỗi `flush.interval_ms`, hoặc sớm hơn khi một shard đầy 75%, có dòng ≥ `flush.on_level` / `sync.on_level`, hoặc `tblog_flush()`.

- **Nhỏ (20–50)**: cửa sổ mất khi `kill -9` hẹp hơn; dòng hiện trong `tail -f` sớm hơn. Nhược: thread nền thức dậy nhiều hơn, mỗi lần ghi ít hơn.
- **Lớn (1000)**: thread nền ít thức hơn (pin, flash).
- Không ảnh hưởng lệnh log. Ở 20 k dòng/s mỗi thread, shard đầy 75% sau khoảng 96 ms, sớm hơn 200 ms mặc định, nên giảm xuống dưới 96 ms mới thu hẹp cửa sổ mất. Chưa đo với giá trị khác.
- Cũng là độ lệch tối đa về thứ tự giữa dòng của các thread khác nhau, và độ trễ của ranh giới `daily` / `hourly`.

### `buffer.size`: RAM

RAM dùng = 2 × max(`buffer.size`, N × 64 KB), N = số shard (lũy thừa của 2 ≥ số CPU, tối đa 16). Mặc định: 8 MB.

- **Lớn (`16MB`, `64MB`)**: hấp thụ đợt log dồn dập mà không chờ (`block`) hay bỏ dòng (`drop`).
- **Nhỏ (`256KB`)**: tiết kiệm RAM (4 CPU: 512 KB).
- Ảnh hưởng tới số dòng mất khi `kill -9`. Mỗi thread mất tối đa phần nhỏ hơn trong hai giới hạn: 75% một shard, hoặc lượng log trong một `flush.interval_ms`. Đo ở 20 k dòng/s mỗi thread:
  - 16 CPU: shard 256 KB, 75% ≈ 1 900 dòng; đo được tối đa 1 939.
  - 4 CPU: shard 1 MB, 75% ≈ 7 500 dòng, nên giới hạn 200 ms ≈ 4 000 dòng thắng; đo được tối đa 3 722 (Linux), 4 460 (Windows), 6 725 (macOS VM, vượt giới hạn, chưa rõ nguyên nhân).

### File: xoay, giữ, nén

- Dung lượng tối đa ≈ `file.max_size` × `file.max_files` (trước nén). File nhỏ thì xoay thường hơn (vài system call mỗi lần, cộng một lần nén nếu bật gzip).
- `daily` / `hourly`: mỗi kỳ một file, dễ tìm và xóa theo ngày. `file.max_size` vẫn áp dụng; một kỳ vượt giới hạn sinh thêm `.1`, `.2`... Với `daily`, `file.max_files` = số ngày muốn giữ + 1.
- `gzip`: nén trên một thread riêng, không làm chậm lệnh log hay thread nền. Nhược: tốn CPU mỗi lần xoay; trên flash phải ghi thêm file `.gz`. An toàn khi crash giữa chừng (`.gz.tmp` → fsync → đổi tên → mới xóa file gốc). Tỉ lệ nén: chưa đo.
- Nên đặt `file.path` tuyệt đối. File bị xóa hoặc đổi tên từ bên ngoài (logrotate): tblog mở lại trong khoảng 1 s.

| `file.rotate` | File đã xoay |
|---|---|
| `size` | `app.1.log` (mới nhất), `app.2.log`... |
| `daily` | `app.2026-11-13.log`, `app.2026-11-13.1.log`... |
| `hourly` | `app.2026-11-13T14.log`... |

### Console, syslog

Mọi sink chạy trên thread nền theo thứ tự file, console, syslog, sink riêng: một sink chậm làm chậm tất cả.

- `console`: dùng cho terminal, container, systemd. Được xả khi crash như file. Nhược: terminal chậm (nhất là console Windows) làm thread nền chậm theo.
- `syslog`: một system call mỗi dòng; không nhận phần xả khi crash; không có trên Windows.
- `file = false` mà không còn sink nào: tblog bật `console` kèm cảnh báo.
- Màu console và severity syslog lấy từ token `[%L]` trong `pattern`.

### `pattern`

| Token | Ý nghĩa | Token | Ý nghĩa |
|---|---|---|---|
| `%D` | `YYYY-MM-DD` (giờ địa phương) | `%n` | Module (`-` = gốc) |
| `%T` | `HH:MM:SS` | `%f` | Tên file nguồn |
| `%us` / `%ms` | Micro / mili giây | `%l` | Số dòng |
| `%L` | Level | `%F` | Tên hàm |
| `%t` | Thread id của OS | `%m` | Nội dung |
| `%%` | `%` | | |

Dòng tối đa 1024 byte, dài hơn bị cắt và kết thúc bằng `...`. Dòng ngắn hơn thì file nhỏ hơn và thread nền ghi được nhiều dòng hơn mỗi giây.

## Sự cố nào, key nào bảo vệ

| Sự cố | Mặc định mất | Bảo vệ |
|---|---|---|
| Crash (SIGSEGV, `abort`, exception, tràn stack; Windows: SEH) | 0 (đo được) | Không cần. Ngoại lệ: tràn stack trên thread **chưa từng log** (Linux / macOS) thì handler không chạy được, mất như `kill -9` |
| `exit()`, `return` từ `main` | 0 | Không cần |
| `kill -9`, OOM killer, `TerminateProcess`, `_exit` | Tới 75% một shard hoặc một `flush.interval_ms` log mỗi thread (đo được 1 900–4 500 dòng, xem [`buffer.size`](#buffersize-ram)) | `flush.on_level`, `tblog_flush()` ở điểm quan trọng |
| Flood quá trần | 0 (`block` chờ) | `buffer.size`; `drop` thì mất dòng nhưng không chờ |
| Mất điện, kernel panic | Tới ≈ `sync.interval_ms` + `flush.interval_ms` | `sync.on_level`, `sync.interval_ms`, `tblog_flush()` |
| Đĩa đầy | Phần không ghi được (cảnh báo ra stderr) | Không có key |

## Cảnh báo

Dạng `tblog: config: config line <N>: <lỗi>` lúc init và mỗi lần đọc lại: `unknown key`, `invalid level`, `invalid boolean`, `invalid number`, `size must be > 0`, `value out of range ... clamped`, `file.max_files must be >= 1`, lỗi ngoặc kép, `option removed, ignored` (các key đã bỏ: `backend`, `mmap.region_size`, `format.mode`).

Sửa tổ hợp không dùng được: `gzip` khi không có zlib → `none`; `file = false` không còn sink → bật `console`; `flush.on_level` dưới `INFO` → chỉ cảnh báo.

## Cấu hình mẫu

Mọi mẫu đã chạy qua parser của tblog, không có cảnh báo. Key không ghi lấy mặc định.

**Dịch vụ thông thường**: mặc định đã đủ (không mất dòng khi buffer đầy hay crash, fsync mỗi giây và sau dòng ERROR).

```ini
file.path         = /var/log/myservice/app.log
file.max_files    = 10
module.http       = WARN
```

**Phải giữ log khi bị giết**: dòng ERROR còn sau `kill -9`; mỗi dòng ERROR tốn 21–148 µs, tới vài ms khi dày.

```ini
file.path         = /var/log/myservice/app.log
flush.interval_ms = 50
flush.on_level    = ERROR
```

**Latency thấp, không bao giờ chờ**: quá tải thì bỏ dòng.

```ini
file.path         = /var/log/trading/app.log
buffer.size       = 64MB
full_policy       = drop
sync.on_level     = FATAL
sync.interval_ms  = 5000
```

**Embedded Linux trên flash**: ít RAM, ít lần ghi thật.

```ini
level             = WARN
file.path         = /data/log/app.log
file.max_size     = 2MB
file.max_files    = 3
buffer.size       = 256KB
flush.interval_ms = 1000
sync.on_level     = FATAL
sync.interval_ms  = 0
reload.watch      = false
```

**Container (chỉ stderr)**:

```ini
file              = false
console           = true
console.color     = false
```

**Mỗi ngày một file, nén gzip, giữ một tháng**:

```ini
file.path         = /var/log/myservice/app.log
file.rotate       = daily
file.compress     = gzip
file.max_size     = 1GB
file.max_files    = 31
```
