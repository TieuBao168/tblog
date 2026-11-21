# Cấu hình (`log.conf`)

`tblog_init(path)` đọc một file `key = value`. Không có file (hoặc `path` là `NULL`) thì mọi key lấy giá trị mặc định. File mẫu: [log.conf](../log.conf).

## Cú pháp

- Mỗi dòng một `key = value`. Khoảng trắng đầu/cuối bị bỏ; dòng trống bị bỏ qua.
- `#` ở đầu dòng là comment. Comment cuối dòng bắt đầu từ một `#` đứng sau khoảng trắng: `console = true  # bật console`.
- Riêng `pattern`: giá trị là toàn bộ phần còn lại của dòng, kể cả `#`.
- Bọc giá trị trong `"..."` để giữ khoảng trắng đầu/cuối, hoặc để giá trị chứa ` #`.
- Kích thước: số nguyên kèm hậu tố `B`, `K`/`KB`, `M`/`MB`, `G`/`GB` (không phân biệt hoa thường, cơ số 1024): `64MB`, `512K`.
- Boolean: `true`/`false`, `1`/`0`, `yes`/`no`, `on`/`off` (không phân biệt hoa thường).
- Level: `TRACE`, `DEBUG`, `INFO`, `WARN` (hoặc `WARNING`), `ERROR`, `FATAL`, `OFF` (không phân biệt hoa thường).
- Key lạ, giá trị sai: một cảnh báo ra stderr (`tblog: config: config line N: ...`), key đó giữ giá trị trước. Các key hợp lệ khác vẫn được áp dụng.

## Các key

| Key | Mặc định | Giá trị | Hot-reload |
|---|---|---|---|
| `level` | `INFO` | Level của module gốc | Có |
| `module.<tên>` | (theo gốc) | Level riêng cho một module, ví dụ `module.network = DEBUG` | Có |
| `pattern` | `%D %T.%us [%L] [%t] %f:%l - %m` | Xem [token](#token-của-pattern); không được rỗng | Có |
| `file` | `true` | `false`: không ghi file (chỉ console / syslog / `tblog_add_sink`) | Không (cảnh báo) |
| `file.path` | `./logs/app.log` | Đường dẫn file log; thư mục cha được tự tạo. Không được rỗng. | Không (cảnh báo) |
| `file.max_size` | `256MB` | Kích thước > 0. Xoay file khi dòng tiếp theo làm file vượt giới hạn này. | Không |
| `file.max_files` | `5` | Số nguyên ≥ 1: file đang ghi + tối đa N−1 file đã xoay. `0` hoặc số âm bị đặt thành `1`. | Không |
| `file.rotate` | `size` | `size`, `daily`, `hourly` | Không |
| `file.compress` | `none` | `none`, `gzip` (cần build có zlib; không có thì thành `none` kèm cảnh báo) | Không |
| `buffer.size` | `4MB` | Kích thước > 0, xem [buffer](#buffer) | Không (cảnh báo) |
| `full_policy` | `drop` | `drop`: buffer đầy thì bỏ dòng và đếm, không bao giờ chặn. `block`: thread gọi log chờ tới khi có chỗ. | Không |
| `flush.interval_ms` | `200` | 1 – 60000. Chu kỳ tối đa giữa hai lần thread nền ghi xuống sink. | Có |
| `sync.interval_ms` | `1000` | 0 – 3600000. Chu kỳ fsync file log; `0` = không sync định kỳ. | Có |
| `sync.on_level` | `ERROR` | Dòng từ level này trở lên đánh thức thread nền ngay và kích hoạt fsync. | Không |
| `console` | `false` | Ghi thêm ra stderr | Không (cảnh báo) |
| `console.color` | `true` | Tô màu theo level khi stderr là terminal | Không |
| `syslog` | `false` | Ghi thêm vào syslog (Linux, macOS; Windows: bỏ qua kèm cảnh báo) | Không (cảnh báo) |
| `reload.watch` | `true` | Tự đọc lại file cấu hình khi nó thay đổi | Không |

Giá trị ngoài khoảng của `flush.interval_ms` / `sync.interval_ms` bị kẹp về biên, kèm cảnh báo.

Nếu `file = false` mà không có console, syslog hay sink riêng nào, tblog bật `console` kèm cảnh báo, để log vẫn đi đâu đó.

### Buffer

`buffer.size` là dung lượng của **một** bộ buffer, chia đều cho N shard, N = lũy thừa của 2 nhỏ nhất ≥ số CPU, tối đa 16. Mỗi shard có hai buffer (một cái đang nhận dòng mới, một cái đang được ghi), tối thiểu 64 KB mỗi buffer. Bộ nhớ dùng: 2 × max(`buffer.size`, N × 64 KB). Mặc định trên máy 16 CPU: 16 shard × 2 × 256 KB = 8 MB.

Mỗi thread luôn ghi vào cùng một shard, nên với `drop`, một thread bắn log dồn dập có thể làm đầy shard của nó (và bị bỏ dòng) trong khi shard khác còn chỗ. Thread nền được đánh thức khi một shard đầy 75%.

## Hot-reload

Khi `reload.watch = true` và `tblog_init` nhận một đường dẫn, thread nền kiểm tra file (mtime, kích thước, inode) tối đa mỗi giây một lần, ở mỗi chu kỳ làm việc (tức là sau tối đa khoảng `max(1 s, flush.interval_ms)`). Khi file đổi:

- Áp dụng ngay: `level`, `module.*`, `pattern`, `flush.interval_ms`, `sync.interval_ms`.
- Đổi `file`, `file.path`, `buffer.size`, `console`, `syslog`: in `tblog: config: file/buffer/sink changes need a restart` và giữ giá trị đang chạy.
- Các key còn lại (`file.max_size`, `file.max_files`, `file.rotate`, `file.compress`, `full_policy`, `sync.on_level`, `console.color`, `reload.watch`): giữ giá trị đang chạy, cần khởi động lại.
- File được đọc chồng lên cấu hình đang chạy: **xóa một dòng không đưa key về mặc định**. Xóa `module.x` không bỏ level riêng của module `x`; muốn đổi thì ghi giá trị mới.
- Không mở được file (ví dụ bị xóa): giữ nguyên cấu hình. Cảnh báo về giá trị sai được in lại ở mỗi lần đọc.

## Key đã bỏ

`backend`, `mmap.region_size`, `format.mode` (các backend sync/mmap và chế độ format trì hoãn đã bị xóa). File cấu hình cũ vẫn đọc được: các key này bị bỏ qua, kèm cảnh báo `<key>: option removed, ignored`.

## Token của pattern

| Token | Ý nghĩa |
|---|---|
| `%D` | Ngày `YYYY-MM-DD` (giờ địa phương) |
| `%T` | Giờ `HH:MM:SS` (giờ địa phương) |
| `%us` | Micro giây trong giây, 6 chữ số |
| `%ms` | Mili giây trong giây, 3 chữ số |
| `%L` | Tên level: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL` |
| `%t` | Thread id của OS (Linux: `gettid`, macOS: `pthread_threadid_np`, Windows: `GetCurrentThreadId`, cắt còn 32 bit) |
| `%n` | Tên module (`-` nếu là module gốc) |
| `%f` | Tên file nguồn (chỉ phần sau dấu `/`, trên Windows cả `\`) |
| `%l` | Số dòng |
| `%F` | Tên hàm |
| `%m` | Nội dung log |
| `%%` | Ký tự `%` |

Token lạ được giữ nguyên. Màu của console và severity của syslog được chọn theo token `[LEVEL]` đầu tiên trong dòng (`[ERROR]`, `[WARN]`...): giữ `[%L]` trong pattern nếu dùng hai tính năng đó (không có thì: không màu, severity `INFO`).

## Xoay file

| `file.rotate` | File đang ghi | Các file đã xoay |
|---|---|---|
| `size` | `app.log` | `app.1.log` (mới nhất), `app.2.log`, ... `app.(N-1).log`; mỗi lần xoay các số dịch lên một |
| `daily` | `app.log` | `app.2026-11-13.log`; vượt `max_size` trong cùng ngày thì `app.2026-11-13.1.log`, `.2`, ... |
| `hourly` | `app.log` | `app.2026-11-13T14.log`, `app.2026-11-13T14.1.log`, ... |

- Tên được tách thành phần gốc + phần mở rộng của `file.path` (`app` + `.log`).
- `max_size` giới hạn mỗi file ở mọi chế độ. Một dòng không bao giờ bị cắt đôi giữa hai file.
- `daily`/`hourly` đổi kỳ theo giờ địa phương. Chỉ thread nền xem đồng hồ (sau khi lấy dữ liệu từ buffer), nên ranh giới kỳ có thể trễ tối đa khoảng một `flush.interval_ms`; một dòng không bao giờ nằm trong file của kỳ trước kỳ mà nó được ghi.
- Ở chế độ theo thời gian, tên không bao giờ bị dùng lại hay dịch số, nên thứ tự (kỳ, số) là thứ tự thời gian.
- Giữ file: chỉ giữ `max_files − 1` file đã xoay mới nhất, tính cả file đã nén. `max_files = 1`: file cũ bị xóa ngay khi xoay.
- File đang ghi bị xóa hoặc đổi tên từ bên ngoài (logrotate...): tblog phát hiện trong vòng khoảng một giây và mở lại `file.path`.

## Nén

`file.compress = gzip` (build có zlib): mỗi file đã xoay được nén thành `<tên>.gz` trên một thread nền riêng.

- An toàn khi crash: ghi `<tên>.gz.tmp`, fsync, đổi tên thành `<tên>.gz`, fsync thư mục, rồi mới xóa file gốc. Lần khởi động sau dọn mọi `*.gz.tmp` (file gốc vẫn còn) và xóa file gốc nếu `.gz` đã có. Crash giữa chừng chỉ để lại file gốc, hoặc một `.gz` hoàn chỉnh.
- Lúc khởi động, các file đã xoay mà chưa nén được đưa vào hàng đợi nén.
- `tblog_shutdown` chờ việc nén đang dở tối đa 10 giây; phần còn lại được nén ở lần chạy sau.
- Build không có zlib: `gzip` bị coi như `none`, kèm cảnh báo lúc init.
