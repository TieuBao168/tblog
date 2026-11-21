# Cấu hình (`log.conf`)

`tblog_init(path)` đọc một file `key = value`. `path` là `NULL`, chuỗi rỗng, hay file không đọc được (in `tblog: cannot read <path>, using defaults`) thì mọi key lấy giá trị mặc định. File mẫu có đủ mọi key: [log.conf](../log.conf).

Trang này là tài liệu tham chiếu cho từng key: làm gì, mặc định, giá trị hợp lệ, khi nào nên đổi, cái giá phải trả, và key nào liên quan. Cuối trang có [cấu hình mẫu](#cấu-hình-mẫu) cho các trường hợp hay gặp.

- [Cú pháp](#cú-pháp)
- [Bảng tóm tắt](#bảng-tóm-tắt)
- [Level và module](#level-và-module): `level`, `module.<tên>`
- [Định dạng dòng log](#định-dạng-dòng-log): `pattern`
- [File, xoay file, nén](#file-xoay-file-nén): `file`, `file.path`, `file.max_size`, `file.max_files`, `file.rotate`, `file.compress`
- [Buffer và khi buffer đầy](#buffer-và-khi-buffer-đầy): `buffer.size`, `full_policy`
- [Ghi xuống đĩa và độ bền](#ghi-xuống-đĩa-và-độ-bền): `flush.interval_ms`, `flush.on_level`, `sync.interval_ms`, `sync.on_level`
- [Console, syslog, sink riêng](#console-syslog-sink-riêng): `console`, `console.color`, `syslog`
- [Hot-reload](#hot-reload): `reload.watch`
- [Kiểm tra giá trị và cảnh báo](#kiểm-tra-giá-trị-và-cảnh-báo)
- [Key đã bỏ](#key-đã-bỏ)
- [Cấu hình mẫu](#cấu-hình-mẫu)

## Cú pháp

- Mỗi dòng một `key = value`. Khoảng trắng đầu/cuối của key và value bị bỏ. Dòng trống bị bỏ qua.
- **Tên key phân biệt hoa thường** (`Level = INFO` là key lạ). Giá trị kiểu level, boolean, `file.rotate`, `file.compress`, `full_policy` và hậu tố kích thước thì không phân biệt hoa thường.
- `#` ở đầu dòng (sau khoảng trắng) là comment. Comment cuối dòng bắt đầu từ một `#` đứng ngay sau khoảng trắng: `console = true  # bật console`. `#` dính liền chữ (`a#b`) là một phần của giá trị.
- Riêng `pattern`: giá trị là **toàn bộ phần còn lại của dòng**, kể cả `#` (không có comment cuối dòng).
- Bọc giá trị trong `"..."` để giữ khoảng trắng đầu/cuối, hoặc để giá trị chứa ` #`: `file.path = "./my logs/app #1.log"`. Sau dấu `"` đóng chỉ được có khoảng trắng hoặc comment. Không có ký tự escape.
- Một key xuất hiện nhiều lần: dòng sau cùng thắng.
- Kiểu giá trị:

| Kiểu | Cách viết | Ví dụ |
|---|---|---|
| Level | `TRACE`, `DEBUG`, `INFO`, `WARN` (hoặc `WARNING`), `ERROR`, `FATAL`, `OFF` | `ERROR`, `warn` |
| Boolean | `true`/`false`, `1`/`0`, `yes`/`no`, `on`/`off` | `on` |
| Kích thước | Số nguyên ≥ 1, có thể kèm hậu tố `B`, `K`/`KB`, `M`/`MB`, `G`/`GB` (cơ số 1024; được có khoảng trắng trước hậu tố). Không có số thập phân. | `64MB`, `512K`, `4 GB`, `1048576` |
| Số mili giây | Số nguyên không dấu, **không** có hậu tố (`200ms` là sai) | `200` |

- Key lạ hoặc giá trị sai: một cảnh báo ra stderr, key đó giữ giá trị trước (mặc định, hoặc giá trị đang chạy khi hot-reload); các key hợp lệ khác vẫn được áp dụng. Xem [cảnh báo](#kiểm-tra-giá-trị-và-cảnh-báo).

## Bảng tóm tắt

| Key | Mặc định | Giá trị | Hot-reload |
|---|---|---|---|
| [`level`](#level) | `INFO` | level | Có |
| [`module.<tên>`](#moduletên) | theo `level` | level | Có (đặt mới / đổi; xóa dòng không gỡ) |
| [`pattern`](#pattern) | `%D %T.%us [%L] [%t] %f:%l - %m` | chuỗi, không rỗng | Có |
| [`file`](#file) | `true` | boolean | Không (cảnh báo) |
| [`file.path`](#filepath) | `./logs/app.log` | đường dẫn, không rỗng | Không (cảnh báo) |
| [`file.max_size`](#filemax_size) | `256MB` | kích thước | Không |
| [`file.max_files`](#filemax_files) | `5` | số nguyên ≥ 1 | Không |
| [`file.rotate`](#filerotate) | `size` | `size`, `daily`, `hourly` | Không |
| [`file.compress`](#filecompress) | `none` | `none`, `gzip` | Không |
| [`buffer.size`](#buffersize) | `4MB` | kích thước | Không (cảnh báo) |
| [`full_policy`](#full_policy) | `block` | `block`, `drop` | Không |
| [`flush.interval_ms`](#flushinterval_ms) | `200` | 1 – 60000 | Có |
| [`flush.on_level`](#flushon_level) | `OFF` | level | Có |
| [`sync.interval_ms`](#syncinterval_ms) | `1000` | 0 – 3600000 (`0` = tắt sync định kỳ) | Có |
| [`sync.on_level`](#syncon_level) | `ERROR` | level | Không |
| [`console`](#console) | `false` | boolean | Không (cảnh báo) |
| [`console.color`](#consolecolor) | `true` | boolean | Không |
| [`syslog`](#syslog) | `false` | boolean | Không (cảnh báo) |
| [`reload.watch`](#reloadwatch) | `true` | boolean | Không |

"Không (cảnh báo)": đổi key này trong file khi đang chạy thì in `tblog: config: file/buffer/sink changes need a restart` và giữ giá trị cũ. "Không": giữ giá trị cũ, không báo gì. Chi tiết: [Hot-reload](#hot-reload).

## Level và module

Một dòng log được ghi khi level của nó ≥ level của module nó thuộc về. Thứ tự: `TRACE` < `DEBUG` < `INFO` < `WARN` < `ERROR` < `FATAL` < `OFF`. Lệnh log bị lọc chỉ tốn một phép so sánh (dưới 1 ns, [benchmarks.md](benchmarks.md#kết-quả-hiệu-năng)): không format, không tính đối số.

### `level`

- **Làm gì:** level của module gốc (các macro không có module: `TBLOG_INFO`, `TBLOGF_INFO`...), và của mọi module chưa có level riêng.
- **Mặc định:** `INFO`. **Giá trị:** một level. `OFF` tắt mọi dòng, kể cả `FATAL` (khi đó `TBLOG_FATAL` cũng không `abort()`).
- **Khi nào đổi:** `DEBUG`/`TRACE` khi điều tra lỗi (đổi lúc đang chạy bằng hot-reload, rồi trả lại), `WARN` cho dịch vụ rất nhiều traffic.
- **Đánh đổi:** mỗi dòng được ghi tốn khoảng 0.3–0.6 µs ở thread gọi log và băng thông của thread nền; `DEBUG` trên một dịch vụ bận dễ làm buffer đầy (xem [`full_policy`](#full_policy)).
- **Liên quan:** `module.<tên>`; `TBLOG_COMPILE_LEVEL` (xóa hẳn macro dưới một level lúc biên dịch, [api.md](api.md#tblog_compile_level)); `tblog_set_level(NULL, lv)` trong code.
- **Ví dụ:** `level = WARN`

### `module.<tên>`

- **Làm gì:** level riêng cho một module, dùng với `TBLOG_MINFO("network", ...)`, `tblog::Logger net("network")`.
- **Mặc định:** không có; module theo `level`. **Giá trị:** một level. Tên module phân biệt hoa thường, tối đa 45 ký tự (dài hơn bị cắt), tối đa 256 module tính cả gốc.
- **Khi nào đổi:** bật `DEBUG` cho đúng phần đang điều tra mà không làm ngập log của phần còn lại; hạ một module ồn ào xuống `WARN`.
- **Đánh đổi:** không có chi phí thêm ở đường nóng (mỗi call site cache con trỏ module).
- **Liên quan:** module đã có level riêng không đổi theo khi `level` đổi. Hot-reload: xóa dòng `module.x` **không** đưa module `x` về theo `level`; muốn vậy, ghi `module.x = <cùng giá trị với level>`.
- **Ví dụ:** `module.network = DEBUG`

## Định dạng dòng log

### `pattern`

- **Làm gì:** khuôn của mỗi dòng log, gồm chữ thường và các token bên dưới.
- **Mặc định:** `%D %T.%us [%L] [%t] %f:%l - %m`, cho dòng dạng `2026-11-16 12:47:56.829576 [INFO] [81148] main.c:5 - user 42 logged in`.
- **Giá trị:** toàn bộ phần sau `=` tới cuối dòng, kể cả `#`. Bọc trong `"..."` để giữ khoảng trắng đầu/cuối (cặp ngoặc kép ngoài cùng bị bỏ). Không được rỗng.
- **Khi nào đổi:** thêm tên module (`%n`), tên hàm (`%F`); bỏ phần không cần cho file nhỏ hơn; mili giây thay micro giây.
- **Đánh đổi:** token nào cũng rẻ; dòng ngắn hơn thì file nhỏ hơn và thread nền ghi được nhiều dòng hơn mỗi giây. Một dòng (sau khi format) dài tối đa 1024 byte; dài hơn thì nội dung bị cắt và dòng kết thúc bằng `...`.
- **Liên quan:** màu của console và severity của syslog lấy theo token `[LEVEL]` đầu tiên trong dòng (`[ERROR]`, `[WARN]`...): giữ `[%L]` nếu dùng hai tính năng đó (không có thì: không màu, severity `INFO`). Hot-reload: dòng mới dùng pattern mới ngay.
- **Ví dụ:** `pattern = %D %T.%ms [%L] [%n] %F() %f:%l - %m`

| Token | Ý nghĩa |
|---|---|
| `%D` | Ngày `YYYY-MM-DD` (giờ địa phương) |
| `%T` | Giờ `HH:MM:SS` (giờ địa phương) |
| `%us` | Micro giây trong giây, 6 chữ số |
| `%ms` | Mili giây trong giây, 3 chữ số |
| `%L` | Tên level: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL` |
| `%t` | Thread id của OS (Linux: `gettid`, macOS: `pthread_threadid_np`, Windows: `GetCurrentThreadId`; cắt còn 32 bit) |
| `%n` | Tên module (`-` nếu là module gốc) |
| `%f` | Tên file nguồn (chỉ phần sau `/`, trên Windows cả `\`) |
| `%l` | Số dòng |
| `%F` | Tên hàm |
| `%m` | Nội dung log |
| `%%` | Ký tự `%` |

Token lạ (ví dụ `%x`) được giữ nguyên trong dòng.

## File, xoay file, nén

### `file`

- **Làm gì:** bật/tắt file log.
- **Mặc định:** `true`. **Giá trị:** boolean.
- **Khi nào đổi:** `false` khi log chỉ cần ra console (container, chạy dưới systemd / launchd), syslog, hoặc sink riêng (`tblog_add_sink`). Không file thì không tạo thư mục, không xoay, không nén.
- **Liên quan:** `file = false` mà không có `console`, `syslog` hay sink riêng nào thì tblog bật `console` kèm cảnh báo, để log vẫn đi đâu đó. Khi crash, phần còn trong buffer chỉ được xả vào file và console (không vào syslog, sink riêng). Restart-only.
- **Ví dụ:** `file = false`

### `file.path`

- **Làm gì:** đường dẫn file log đang ghi. Đường dẫn tương đối tính từ thư mục làm việc (current directory) của process lúc `tblog_init`. Thư mục cha được tạo nếu chưa có. Trên Windows đường dẫn là UTF-8.
- **Mặc định:** `./logs/app.log`. **Giá trị:** đường dẫn không rỗng; bọc `"..."` nếu có khoảng trắng đầu/cuối hoặc ` #`.
- **Khi nào đổi:** luôn đặt đường dẫn tuyệt đối cho dịch vụ (thư mục làm việc của dịch vụ hay thay đổi theo cách chạy); dùng thư mục người dùng có quyền ghi cho ứng dụng desktop.
- **Liên quan:** tên các file đã xoay được dựng từ phần gốc + phần mở rộng (`app` + `.log`), xem [xoay file](#cách-đặt-tên-khi-xoay). Không mở được file lúc init: `tblog_init` trả `-1`. File bị xóa / đổi tên từ bên ngoài (logrotate...) khi đang chạy: tblog phát hiện trong khoảng một giây và mở lại `file.path`. Restart-only (cảnh báo).
- **Ví dụ:** `file.path = /var/log/myapp/app.log`

### `file.max_size`

- **Làm gì:** kích thước tối đa của một file. Khi dòng tiếp theo làm file vượt giới hạn này, file được xoay trước khi ghi. Một dòng không bao giờ bị cắt đôi giữa hai file.
- **Mặc định:** `256MB`. **Giá trị:** kích thước > 0.
- **Khi nào đổi:** nhỏ hơn (`4MB`–`32MB`) trên thiết bị ít dung lượng hoặc khi cần mở file bằng trình soạn thảo; lớn hơn khi log nhiều và muốn ít file.
- **Đánh đổi:** dung lượng tối đa trên đĩa ≈ `file.max_size × file.max_files` (trước nén). File nhỏ thì xoay thường hơn (mỗi lần xoay vài system call, và một lần nén nếu bật gzip).
- **Liên quan:** áp dụng ở mọi chế độ `file.rotate`, kể cả `daily`/`hourly`. Restart-only.
- **Ví dụ:** `file.max_size = 64MB`

### `file.max_files`

- **Làm gì:** số file giữ lại, tính cả file đang ghi: file đang ghi + tối đa `N − 1` file đã xoay mới nhất (tính cả file đã nén). File cũ hơn bị xóa khi xoay.
- **Mặc định:** `5`. **Giá trị:** số nguyên ≥ 1. `0` hoặc số âm thành `1` (kèm cảnh báo). `1`: file cũ bị xóa ngay khi xoay.
- **Khi nào đổi:** với `daily`, đặt bằng số ngày muốn giữ + 1 (ví dụ `31` cho khoảng một tháng, nếu mỗi ngày không vượt `file.max_size`).
- **Đánh đổi:** dung lượng đĩa, xem `file.max_size`.
- **Liên quan:** với `daily`/`hourly`, một ngày/giờ vượt `max_size` sinh nhiều file (`.1`, `.2`...), mỗi file tính một suất. Restart-only.
- **Ví dụ:** `file.max_files = 10`

### `file.rotate`

- **Làm gì:** khi nào xoay file ngoài giới hạn dung lượng.
- **Mặc định:** `size`. **Giá trị:** `size` (chỉ theo `file.max_size`), `daily` (thêm: mỗi khi sang ngày mới theo giờ địa phương), `hourly` (mỗi khi sang giờ mới).
- **Khi nào đổi:** `daily` khi muốn mỗi file là một ngày (dễ tra, dễ xóa theo ngày, hợp với `file.compress = gzip`); `hourly` cho dịch vụ log rất nhiều.
- **Đánh đổi:** chỉ thread nền xem đồng hồ (sau khi lấy dữ liệu từ buffer), nên ranh giới kỳ có thể trễ tối đa khoảng một `flush.interval_ms`; một dòng không bao giờ nằm trong file của kỳ trước kỳ mà nó được ghi. File của một kỳ không có dòng nào thì không được tạo.
- **Liên quan:** `file.max_size` vẫn giới hạn mỗi file; `file.max_files`. Restart-only.
- **Ví dụ:** `file.rotate = daily`

#### Cách đặt tên khi xoay

| `file.rotate` | File đang ghi | Các file đã xoay |
|---|---|---|
| `size` | `app.log` | `app.1.log` (mới nhất), `app.2.log`, ... `app.(N-1).log`; mỗi lần xoay các số dịch lên một |
| `daily` | `app.log` | `app.2026-11-13.log`; vượt `max_size` trong cùng ngày thì `app.2026-11-13.1.log`, `.2`, ... |
| `hourly` | `app.log` | `app.2026-11-13T14.log`, `app.2026-11-13T14.1.log`, ... |

Ở chế độ theo thời gian, tên không bao giờ bị dùng lại hay dịch số, nên thứ tự (kỳ, số) là thứ tự thời gian.

### `file.compress`

- **Làm gì:** nén mỗi file đã xoay thành `<tên>.gz` trên một thread nền riêng. File đang ghi không bao giờ bị nén.
- **Mặc định:** `none`. **Giá trị:** `none`, `gzip`. Cần thư viện được build có zlib; không có thì `gzip` thành `none` kèm cảnh báo lúc init.
- **Khi nào đổi:** giữ được nhiều ngày log trên ít dung lượng (log văn bản thường nén được nhiều lần).
- **Đánh đổi:** CPU của một thread nền mỗi lần xoay (không làm chậm thread gọi log hay worker); trên flash, ghi thêm một lần (file `.gz`) rồi xóa file gốc.
- **Liên quan:** `file.max_files` tính cả file `.gz`. An toàn khi crash: ghi `<tên>.gz.tmp`, fsync, đổi tên thành `<tên>.gz`, fsync thư mục, rồi mới xóa file gốc; lần khởi động sau dọn mọi `*.gz.tmp` (file gốc vẫn còn) và xóa file gốc nếu `.gz` đã có. Lúc khởi động, các file đã xoay mà chưa nén được đưa vào hàng đợi nén. `tblog_shutdown` chờ việc nén đang dở tối đa 10 giây; phần còn lại được nén ở lần chạy sau. Restart-only.
- **Ví dụ:** `file.compress = gzip`

## Buffer và khi buffer đầy

Thread gọi log chép dòng đã format vào buffer trong RAM; thread nền (worker) lấy buffer và ghi ra các sink. Buffer chia thành N shard, N = lũy thừa của 2 nhỏ nhất ≥ số CPU, tối đa 16. Mỗi thread luôn ghi vào cùng một shard (gán vòng tròn lúc thread log lần đầu), nên dòng của một thread luôn đúng thứ tự.

### `buffer.size`

- **Làm gì:** dung lượng của **một** bộ buffer, chia đều cho N shard. Mỗi shard có hai buffer (một cái nhận dòng mới, một cái đang được ghi), tối thiểu 64 KB mỗi buffer.
- **Mặc định:** `4MB`. **Giá trị:** kích thước > 0.
- **Bộ nhớ dùng:** 2 × max(`buffer.size`, N × 64 KB). Mặc định trên máy 16 CPU: 16 shard × 2 × 256 KB = 8 MB; trên máy 4 CPU: 4 × 2 × 1 MB = 8 MB.
- **Khi nào đổi:** lớn hơn (`16MB`, `64MB`) nếu có đợt log dồn dập ngắn mà không muốn thread gọi log phải chờ (`block`) hoặc mất dòng (`drop`); nhỏ hơn trên máy ít RAM (`256KB` trên 4 CPU = 4 × 2 × 64 KB = 512 KB).
- **Đánh đổi:** RAM; và với `kill -9` / `_exit`, phần còn trong buffer bị mất: buffer lớn hơn không làm mất nhiều hơn trong điều kiện thường (worker được đánh thức khi một shard đầy 75% hoặc sau mỗi `flush.interval_ms`), nhưng trong một đợt dồn dập thì có thể.
- **Liên quan:** `full_policy`; thread nền được đánh thức khi một shard đầy 75%. Restart-only (cảnh báo).
- **Ví dụ:** `buffer.size = 16MB`

### `full_policy`

- **Làm gì:** việc thread gọi log làm khi shard của nó đầy (log nhanh hơn thread nền ghi được: trần khoảng 4–5 triệu dòng/giây ở máy đo, [benchmarks.md](benchmarks.md#kết-quả-hiệu-năng)).
- **Mặc định:** `block`. **Giá trị:**
  - `block`: thread gọi log chờ tới khi thread nền đổi buffer, rồi chép dòng vào. Không mất dòng nào; thông lượng end-to-end ngang `drop`; cái giá là đuôi latency của lệnh log lúc buffer đầy (đo được: max vài ms ở 8–16 thread flood).
  - `drop`: dòng bị bỏ ngay, lệnh log trả về như thường (không bao giờ chờ). Số dòng bị bỏ có trong `tblog_dropped_count()` và một dòng `tblog: N lines dropped` trong log ở lượt ghi sau.
- **Khi nào đổi:** `drop` khi thread gọi log không bao giờ được phép chờ (vòng lặp thời gian thực, thread xử lý mạng có deadline) và chấp nhận mất log lúc quá tải.
- **Đánh đổi:** `block` bảo toàn mọi dòng nhưng truyền áp lực ngược về ứng dụng khi đĩa chậm hoặc log quá nhiều; `drop` giữ latency ổn định nhưng mất dòng đúng lúc có chuyện (lúc quá tải). Với `drop`, một thread bắn log dồn dập có thể làm đầy shard của nó và bị bỏ dòng trong khi shard khác còn chỗ.
- **Liên quan:** `buffer.size`; `flush.on_level` (với `drop`, dòng ≥ `flush.on_level` bị bỏ thì lệnh trả về ngay, không chờ). Restart-only.
- **Ví dụ:** `full_policy = drop`

## Ghi xuống đĩa và độ bền

Một dòng log đi qua ba chặng: (1) buffer RAM của tblog → (2) `write()` / `WriteFile` vào file, dữ liệu nằm trong page cache của OS: **sống sót khi process chết kiểu gì cũng được** (`kill -9`, `TerminateProcess`, `_exit`, OOM killer) → (3) fsync (`fdatasync`, macOS `F_FULLFSYNC`, Windows `FlushFileBuffers`): **sống sót khi mất điện / kernel panic**. Bốn key dưới đây quyết định khi nào một dòng qua chặng 2 và chặng 3.

### `flush.interval_ms`

- **Làm gì:** chu kỳ tối đa giữa hai lượt ghi của thread nền. Thread nền còn được đánh thức sớm khi một shard đầy 75%, khi có dòng ≥ `sync.on_level` hoặc ≥ `flush.on_level`, và khi `tblog_flush()`.
- **Mặc định:** `200`. **Giá trị:** 1 – 60000 (ms); ngoài khoảng bị kẹp về biên, kèm cảnh báo.
- **Khi nào đổi:** nhỏ hơn (`20`–`50`) để thu hẹp cửa sổ mất log khi `kill -9` và để dòng xuất hiện trong file / console sớm hơn (xem log trực tiếp bằng `tail -f`); lớn hơn (`1000`) trên flash hoặc thiết bị pin để thread nền thức dậy ít hơn.
- **Đánh đổi:** chu kỳ ngắn: thread nền thức dậy nhiều hơn, mỗi lượt ghi ít hơn (nhiều `write()` nhỏ). Không ảnh hưởng thread gọi log.
- **Liên quan:** với `kill -9`, dòng dưới `flush.on_level` mất tối đa khoảng một `flush.interval_ms` log (hoặc tới 75% một shard nếu shard đầy trước). Hot-reload: có hiệu lực từ chu kỳ sau. Cũng quyết định độ trễ của hot-reload và ranh giới `daily`/`hourly`.
- **Ví dụ:** `flush.interval_ms = 50`

### `flush.on_level`

- **Làm gì:** một dòng ở level này trở lên được đưa vào buffer như mọi dòng khác, rồi **thread gọi log chờ tới khi thread nền đã `write()` dòng đó vào file** (và vào mọi sink khác) mới trả về. Thread nền được đánh thức ngay, không đợi `flush.interval_ms`. Không fsync: dòng nằm trong page cache của OS, nên còn nguyên khi process bị `kill -9`, `TerminateProcess`, `_exit`, OOM killer. Thứ tự giữ nguyên, vì chỉ thread nền ghi file.
- **Mặc định:** `OFF` (không lệnh log nào chờ). **Giá trị:** một level.
- **Đảm bảo khi lệnh log trả về:** dòng đó, mọi dòng trước nó của cùng thread, và mọi dòng khác mà thread nền lấy cùng lượt (dòng của các thread khác đã vào buffer trước lúc thread nền lấy) đã được ghi vào mọi sink (file, console, syslog, sink riêng; với `file = false`: các sink đang có). Lệnh log trả về sớm, không có đảm bảo đó, khi: logger đang shutdown; process đang crash (crash handler sẽ xả dòng đó vào file, nên nó vẫn không mất); dòng bị bỏ vì `full_policy = drop`; lệnh log chạy trên chính thread nền (sink riêng tự log); hoặc chờ quá **2 giây** (đĩa bị treo: lệnh log không bao giờ treo mãi; dòng vẫn nằm trong buffer và được ghi khi đĩa chạy lại).
- **Khi nào đổi:** `ERROR` cho dịch vụ có thể bị giết từ bên ngoài (OOM killer, `kill -9` của watchdog / orchestrator, `TerminateProcess` của trình quản lý dịch vụ) mà dòng lỗi cuối cùng phải còn. Đo được: với `flush.on_level = ERROR`, sau `kill -9` / `_Exit` không mất dòng nào tính tới dòng ERROR cuối cùng đã trả về (20 lần `kill_hard` với 4 thread và 20 lần `_Exit`, mỗi dòng thứ 100 là ERROR: 0 dòng mất tới dòng ERROR cuối, phần mất còn trung bình 173.5 / 56.1 dòng INFO, so với 4 612 / 954 dòng khi không bật, [benchmarks.md](benchmarks.md#kết-quả-mất-log)).
- **Đánh đổi:** mỗi dòng ≥ level tốn một lượt ghi của thread nền thay vì vài trăm ns: đo được 17–21 µs p50 ở 1 thread, 95–148 µs ở 8 thread khi `sync.on_level = OFF`; với `sync.on_level = ERROR` (mặc định) mà dòng ERROR tới dày hơn tốc độ fsync thì mỗi dòng tốn khoảng một fsync: 2.5–2.7 ms ở 1 thread, 5.2–5.4 ms ở 8 thread (Windows, NVMe, [benchmarks.md](benchmarks.md#chi-phí-của-flushon_level)). Dòng dưới level không tốn thêm gì (một phép so sánh). Đặt dưới `INFO` thì gần như mọi dòng phải chờ: tblog cảnh báo.
- **Liên quan:** khác hẳn `sync.on_level`, xem bảng dưới. `FATAL` luôn được ghi và fsync trước `abort()`, bất kể key này. `full_policy = block`: dòng chờ có chỗ trong buffer trước, rồi chờ được ghi. Hot-reload: có hiệu lực ngay cho các lệnh log sau.
- **Ví dụ:** `flush.on_level = ERROR`

### `sync.interval_ms`

- **Làm gì:** chu kỳ fsync file log (trên thread sync riêng, không chặn thread gọi log).
- **Mặc định:** `1000`. **Giá trị:** 0 – 3600000 (ms); `0` = không sync định kỳ (chỉ sync theo `sync.on_level`, `tblog_flush()` và lúc shutdown); trên 3600000 bị kẹp, kèm cảnh báo.
- **Khi nào đổi:** lớn hơn hoặc `0` trên flash / thẻ SD để giảm số lần ghi thật xuống thiết bị (OS vẫn tự ghi page cache xuống đĩa theo chu kỳ của nó, trên Linux mặc định khoảng 30 giây); nhỏ hơn khi mất điện là rủi ro thật và log phải đầy đủ tới gần thời điểm đó.
- **Đánh đổi:** mỗi fsync là một lần ghi xuống thiết bị (hao mòn flash, pin). Trên Windows, `WriteFile` của thread nền phải chờ khi một `FlushFileBuffers` đang chạy trên cùng file, nên fsync dày làm chậm thread nền (và các lệnh đang chờ theo `flush.on_level`).
- **Liên quan:** mất điện: mất tối đa khoảng một `sync.interval_ms` cộng một `flush.interval_ms` log. Hot-reload: có hiệu lực từ lần chờ sau.
- **Ví dụ:** `sync.interval_ms = 5000`

### `sync.on_level`

- **Làm gì:** một dòng ở level này trở lên đánh thức thread nền ngay (dòng được ghi vào file trong vòng một lượt ghi), rồi yêu cầu thread sync fsync file ngay sau lượt ghi đó. **Thread gọi log không chờ gì cả:** lệnh trả về ngay như mọi dòng khác.
- **Mặc định:** `ERROR`. **Giá trị:** một level; `OFF` = chỉ sync định kỳ.
- **Khi nào đổi:** `WARN` khi cảnh báo cũng phải sống sót qua mất điện; `FATAL`/`OFF` trên flash (giảm số lần ghi thật) hoặc khi dòng ERROR rất nhiều.
- **Đánh đổi:** mỗi lượt ghi có dòng ≥ level kéo theo một fsync (khoảng 2–3 ms trên NVMe ở máy đo). Kết hợp với `flush.on_level` cùng level trên Windows: dòng ERROR tới dày hơn tốc độ fsync thì mỗi dòng chờ cả fsync trước đó (đo được 2.5–5.4 ms mỗi dòng ERROR, so với 17–148 µs khi `sync.on_level = OFF`, [benchmarks.md](benchmarks.md#chi-phí-của-flushon_level)).
- **Liên quan:** `sync.interval_ms`; `flush.on_level`. Restart-only.
- **Ví dụ:** `sync.on_level = WARN`

### `flush.on_level` và `sync.on_level` khác nhau thế nào

| | `flush.on_level` | `sync.on_level` |
|---|---|---|
| Thread gọi log | **Chờ** tới khi dòng đã được `write()` vào file (tối đa 2 s) | Không chờ, trả về ngay |
| Thread nền | Đánh thức ngay | Đánh thức ngay |
| Sau lượt ghi | Không fsync | Yêu cầu fsync (bất đồng bộ, thread sync) |
| Khi lệnh log trả về, dòng sống sót qua `kill -9`, `TerminateProcess`, `_exit`, OOM | **Có** | Chưa chắc (dòng có thể còn trong buffer) |
| Sống sót qua mất điện | Chỉ sau fsync kế tiếp (`sync.on_level`, `sync.interval_ms`) | Sau fsync của chính lượt đó (vài ms sau khi lệnh trả về) |
| Chi phí mỗi dòng ≥ level | Một lượt ghi của thread nền (17–148 µs đo được; tới vài ms khi dòng ERROR dày và kèm fsync) | Gần như không, ở thread gọi log |
| Mặc định | `OFF` | `ERROR` |

Hai key bổ sung cho nhau: `flush.on_level = ERROR` + `sync.on_level = ERROR` nghĩa là dòng ERROR đã ở trong OS khi lệnh trả về và trên đĩa vài ms sau đó.

### Loại sự cố → key nào bảo vệ

| Sự cố | Mặc định mất gì | Bảo vệ bằng |
|---|---|---|
| Crash mà handler bắt được: SIGSEGV, SIGABRT (`abort`, `assert`, exception không ai bắt), SIGBUS, SIGFPE, SIGILL; Windows: SEH exception, `abort()` | Không mất (đo được: 0) | Không cần key: crash handler xả buffer ([architecture.md](architecture.md#crash-handling)) |
| `exit()`, `return` từ `main`, `tblog_shutdown()` | Không mất | Không cần key |
| `kill -9`, `TerminateProcess`, OOM killer, `_exit()` / `_Exit()`, crash mà handler không chạy được (stack overflow trên thread không có alternate stack, POSIX) | Phần còn trong buffer: tối đa khoảng một `flush.interval_ms` log (đo được: tới khoảng 1 900 dòng mỗi thread) | `flush.on_level` (dòng ≥ level và mọi dòng trước nó của cùng thread: không mất); `flush.interval_ms` nhỏ hơn (thu hẹp cửa sổ cho mọi dòng); `tblog_flush()` ở điểm quan trọng |
| Log nhanh hơn thread nền ghi được (flood) | Không mất (`block`: thread gọi log chờ) | `full_policy` (`drop` thì mất dòng, nhưng không chờ); `buffer.size` |
| Mất điện, kernel panic, máy treo cứng | Phần chưa fsync: tối đa khoảng `sync.interval_ms` + `flush.interval_ms` | `sync.on_level` (fsync ngay sau dòng ≥ level), `sync.interval_ms`, `tblog_flush()` (ghi + fsync rồi mới trả về) |
| Đĩa đầy, lỗi ghi | Phần không ghi được (đếm, cảnh báo rate-limit ra stderr) | Không có key; theo dõi stderr |
| `TBLOG_FATAL` | Không mất: ghi + fsync rồi `abort()` | Không cần key |

## Console, syslog, sink riêng

Mọi sink nhận cùng các dòng, theo thứ tự: file, console, syslog, rồi các sink riêng theo thứ tự đăng ký. Một sink chậm làm chậm thread nền, tức là mọi sink.

### `console`

- **Làm gì:** ghi thêm mọi dòng ra stderr.
- **Mặc định:** `false`. **Giá trị:** boolean.
- **Khi nào đổi:** `true` cho chương trình chạy trong terminal, container (log thu từ stderr), hoặc dưới systemd/journald.
- **Đánh đổi:** terminal chậm (đặc biệt console của Windows) làm thread nền chậm theo; dưới `flush.on_level`, lệnh log chờ cả phần ghi ra console.
- **Liên quan:** console là sink "crash-safe" như file: khi crash, phần còn trong buffer được xả ra stderr và dòng báo crash đi qua nó. `file = false` không còn sink nào thì `console` tự bật. Restart-only (cảnh báo).
- **Ví dụ:** `console = true`

### `console.color`

- **Làm gì:** tô màu theo level (`WARN` vàng, `ERROR` đỏ, `FATAL` đỏ đậm, `TRACE`/`DEBUG` mờ) khi stderr là terminal. Ghi vào file / pipe thì không bao giờ có mã màu. Windows: bật chế độ ANSI của console (Windows 10 1511 trở lên; console cũ hơn thì không màu).
- **Mặc định:** `true`. **Giá trị:** boolean.
- **Khi nào đổi:** `false` nếu terminal không hiểu mã ANSI hoặc output được chụp lại làm tài liệu.
- **Liên quan:** cần token `[%L]` trong `pattern`. Dòng xả khi crash không có màu. Restart-only.
- **Ví dụ:** `console.color = false`

### `syslog`

- **Làm gì:** ghi thêm mỗi dòng thành một bản tin syslog (ident `tblog`), severity theo token `[LEVEL]`: `TRACE`/`DEBUG` → debug, `INFO` → info, `WARN` → warning, `ERROR` → err, `FATAL` → crit.
- **Mặc định:** `false`. **Giá trị:** boolean. Linux, macOS. Windows: không hỗ trợ, bỏ qua kèm cảnh báo (nếu đó là sink duy nhất thì log ra console).
- **Khi nào đổi:** máy đã gom log qua syslog / journald; thiết bị embedded có syslog gửi đi xa.
- **Đánh đổi:** một system call mỗi dòng (chậm hơn file nhiều); syslog không nhận phần xả khi crash.
- **Liên quan:** cần `[%L]` trong `pattern` để có severity đúng. Restart-only (cảnh báo).
- **Ví dụ:** `syslog = true`

### Sink riêng (`tblog_add_sink`)

Không phải key: đăng ký bằng code, trước `tblog_init` ([api.md](api.md#sink-riêng-tblog_add_sink)). Mỗi sink riêng tính là một sink cho quy tắc `file = false` (không bật console tự động). Sink riêng không nhận phần xả khi crash.

## Hot-reload

### `reload.watch`

- **Làm gì:** thread nền kiểm tra file cấu hình (mtime, kích thước, inode / file id) tối đa mỗi giây một lần, ở mỗi chu kỳ làm việc (tức là sau tối đa khoảng `max(1 s, flush.interval_ms)`), và đọc lại khi file đổi.
- **Mặc định:** `true`. **Giá trị:** boolean. Chỉ có tác dụng khi `tblog_init` nhận một đường dẫn.
- **Khi nào đổi:** `false` khi file cấu hình nằm trên thư mục chỉ đọc / không bao giờ đổi, hoặc không muốn ai đổi hành vi log lúc đang chạy.
- **Liên quan:** Restart-only.
- **Ví dụ:** `reload.watch = false`

Khi file đổi:

- **Áp dụng ngay:** `level`, `module.*`, `pattern`, `flush.interval_ms`, `flush.on_level`, `sync.interval_ms`.
- **Cần khởi động lại, có cảnh báo** (`tblog: config: file/buffer/sink changes need a restart`): `file`, `file.path`, `buffer.size`, `console`, `syslog`. Giá trị đang chạy được giữ.
- **Cần khởi động lại, không cảnh báo:** `file.max_size`, `file.max_files`, `file.rotate`, `file.compress`, `full_policy`, `sync.on_level`, `console.color`, `reload.watch`.
- File được đọc **chồng lên** cấu hình đang chạy: xóa một dòng không đưa key về mặc định (xóa `module.x` không bỏ level riêng của `x`; muốn đổi thì ghi giá trị mới).
- Không mở được file (ví dụ bị xóa): giữ nguyên cấu hình. Cảnh báo về giá trị sai được in lại ở mỗi lần đọc.

## Kiểm tra giá trị và cảnh báo

Cảnh báo in ra stderr lúc `tblog_init` và ở mỗi lần hot-reload. Giá trị sai không bao giờ làm `tblog_init` thất bại.

Từ parser, dạng `tblog: config: config line <N>: <lỗi>: '<dòng>'`; key đó giữ giá trị trước:

| Lỗi | Khi nào |
|---|---|
| `expected 'key = value'` / `missing key` | Dòng không có `=` / không có tên key |
| `unknown key '<key>'` | Tên key lạ (nhớ: tên key phân biệt hoa thường) |
| `invalid level` | `level`, `module.*`, `flush.on_level`, `sync.on_level` không phải tên level |
| `invalid boolean` | Giá trị boolean sai |
| `invalid number` / `negative value` / `value too large` | Số / kích thước viết sai, âm, hoặc tràn |
| `size must be > 0` | `file.max_size`, `buffer.size` = 0 |
| `file.max_files must be >= 1, set to 1` | `file.max_files` ≤ 0 (giá trị được đặt thành 1) |
| `value out of range [lo, hi], clamped to X` | `flush.interval_ms`, `sync.interval_ms` ngoài khoảng (giá trị được kẹp) |
| `invalid file.rotate (size\|daily\|hourly)`, `invalid file.compress (none\|gzip)`, `invalid full_policy (drop\|block)` | Giá trị không thuộc danh sách |
| `empty pattern`, `empty file.path`, `empty module name` | Giá trị rỗng / `module. = ...` |
| `unterminated quoted value`, `unexpected text after quoted value` | Lỗi ngoặc kép |
| `<key>: option removed, ignored` | [Key đã bỏ](#key-đã-bỏ) |

Sau parser, `validate_config` sửa các tổ hợp không dùng được, dạng `tblog: config: <nội dung>`:

| Cảnh báo | Kết quả |
|---|---|
| `file.compress = gzip needs a build with zlib; compression disabled` | `file.compress = none` |
| `file = false and no other sink (console, syslog, tblog_add_sink); enabling console` | `console = true` |
| `flush.on_level below INFO: nearly every line waits for the worker's write (slow); ERROR is the usual choice` | Giữ nguyên giá trị (chỉ cảnh báo) |

Lúc chạy: `tblog: cannot read <path>, using defaults` (không đọc được file lúc init), `tblog: no usable sink, logging to the console` (`file = false` + `syslog` trên Windows).

## Key đã bỏ

`backend`, `mmap.region_size`, `format.mode` (các backend sync/mmap và chế độ format trì hoãn đã bị xóa). File cấu hình cũ vẫn đọc được: các key này bị bỏ qua, kèm cảnh báo `<key>: option removed, ignored`.

## Cấu hình mẫu

Mọi đoạn dưới đây đã được chạy qua parser và `validate_config` thật của tblog: không có cảnh báo nào (đoạn "chỉ sink riêng" với một sink đã đăng ký bằng `tblog_add_sink`). Key không ghi thì lấy mặc định.

### Dịch vụ thông thường

Mặc định đã hợp cho phần lớn dịch vụ: không mất dòng khi buffer đầy (`block`), không mất dòng khi crash, fsync mỗi giây và ngay sau dòng ERROR. Chỉ cần đường dẫn tuyệt đối.

```ini
level             = INFO
file.path         = /var/log/myservice/app.log
file.max_size     = 256MB
file.max_files    = 10
module.http       = WARN
```

### Ưu tiên latency (không bao giờ chờ)

Thread gọi log không bao giờ bị chặn: buffer lớn để hấp thụ đợt dồn dập, quá tải thì bỏ dòng (theo dõi `tblog_dropped_count()`), không có lệnh nào chờ ghi file.

```ini
level             = INFO
file.path         = /var/log/trading/app.log
buffer.size       = 64MB
full_policy       = drop
flush.on_level    = OFF
sync.on_level     = FATAL
sync.interval_ms  = 5000
```

### Dịch vụ phải giữ log khi crash và khi bị giết

Dòng ERROR (và mọi dòng trước nó của cùng thread) đã nằm trong OS khi lệnh log trả về, nên còn sau `kill -9` / OOM killer; fsync ngay sau đó cho trường hợp mất điện. Chu kỳ ghi ngắn để các dòng INFO sau dòng ERROR cuối cũng ít mất. Chi phí: mỗi dòng ERROR chờ một lượt ghi (17–148 µs đo được; tới vài ms khi dòng ERROR dày và kèm fsync).

```ini
level             = INFO
file.path         = /var/log/myservice/app.log
full_policy       = block
flush.interval_ms = 50
flush.on_level    = ERROR
sync.on_level     = ERROR
sync.interval_ms  = 1000
```

### Embedded Linux trên flash

Ít RAM, ít dung lượng, hạn chế số lần ghi thật xuống flash: buffer nhỏ, file nhỏ và ít, thread nền thức dậy mỗi giây, chỉ fsync với FATAL (để kernel tự ghi page cache theo chu kỳ của nó), không fsync định kỳ.

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

### Ứng dụng desktop có console

Log ra cả terminal (có màu) và file trong thư mục của người dùng, mỗi ngày một file, giữ khoảng một tuần.

```ini
level             = INFO
file.path         = ./logs/desktop.log
file.rotate       = daily
file.max_files    = 8
file.max_size     = 64MB
console           = true
console.color     = true
pattern           = %T.%ms [%L] [%n] %m
```

### Chỉ console (container)

Không file; mọi dòng ra stderr để runtime của container gom lại. Không màu vì stderr là pipe.

```ini
level             = INFO
file              = false
console           = true
console.color     = false
```

### Chỉ sink riêng

Không file, không console: mọi dòng đi tới sink đăng ký bằng `tblog_add_sink()` **trước** `tblog_init` (không có sink nào thì tblog bật console kèm cảnh báo).

```ini
level             = DEBUG
file              = false
flush.interval_ms = 100
```

### Xoay theo ngày, nén gzip

Mỗi ngày một file, file cũ được nén `.gz`, giữ khoảng một tháng (cần build có zlib).

```ini
level             = INFO
file.path         = /var/log/myservice/app.log
file.rotate       = daily
file.compress     = gzip
file.max_size     = 1GB
file.max_files    = 31
```
