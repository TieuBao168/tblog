# API

Hai header công khai:

- `<tblog/log.h>`: API C, dùng được từ C99 (MSVC: C11) và C++11 trở lên.
- `<tblog/tblog.hpp>`: API C++ dạng `{}`, header-only, C++11, xây trên API C. Nó include `<tblog/detail/format.hpp>`.

## API C (`<tblog/log.h>`)

### Vòng đời

```c
int  tblog_init(const char *config_path);
void tblog_shutdown(void);
void tblog_flush(void);
```

- `tblog_init(path)`: đọc file cấu hình (`NULL` hoặc `""` = cấu hình mặc định), tạo buffer, mở file log, khởi động thread nền, cài crash handler. Trả `0` khi thành công.
  - Không đọc được file cấu hình: in `tblog: cannot read <path>, using defaults` ra stderr, dùng mặc định, vẫn trả `0`.
  - Trả `-1` khi không tạo được buffer hoặc không mở được file log (đã in lý do ra stderr).
  - Gọi lại khi logger đang chạy: không làm gì, trả `0` (không đọc lại cấu hình).
  - Lần init đầu tiên đăng ký `tblog_shutdown` với `atexit()`.
- `tblog_shutdown()`: dừng thread nền, ghi nốt mọi dòng còn trong buffer, fsync, đóng file. Gọi nhiều lần không sao. Sau khi shutdown, log lại đi ra stderr. Có thể `tblog_init` lại, nhưng mỗi cặp init/shutdown giữ lại buffer của lần đó (không giải phóng), nên ứng dụng chỉ nên init một lần.
- `tblog_flush()`: nhờ thread nền ghi mọi dòng đã có vào các sink, gọi `flush` của các sink, fsync file, rồi mới trả về (chờ tối đa 2 giây). Không làm gì nếu logger chưa chạy.

### Level và module

```c
void         tblog_set_level(const char *module, tblog_level lv);
tblog_level  tblog_get_level(const char *module);
tblog_module *tblog_module_get(const char *name);
uint64_t     tblog_dropped_count(void);
```

- Level: `TBLOG_LV_TRACE` < `DEBUG` < `INFO` < `WARN` < `ERROR` < `FATAL` < `OFF`. Level gốc mặc định là `INFO`.
- `module` `NULL` hoặc `""` là module gốc. Đặt level gốc thì mọi module chưa có level riêng cũng đổi theo; module đã được đặt level riêng (bằng `tblog_set_level` hoặc `module.<tên>` trong cấu hình) giữ level của nó.
- `tblog_module_get(name)` trả con trỏ ổn định (không bao giờ bị giải phóng). Tên dài hơn 45 ký tự bị cắt. Có tối đa 256 module tính cả gốc; vượt quá thì trả về module gốc.
- `tblog_dropped_count()`: tổng số dòng bị bỏ vì buffer đầy (chính sách `drop`), cộng dồn qua các lần init/shutdown. Khi có dòng bị bỏ, thread nền cũng ghi một dòng `tblog: N lines dropped` vào log.

### Macro ghi log

```c
TBLOG_TRACE(fmt, ...)   TBLOG_DEBUG  TBLOG_INFO  TBLOG_WARN  TBLOG_ERROR  TBLOG_FATAL
TBLOG_MTRACE(module, fmt, ...)  ...  TBLOG_MFATAL(module, fmt, ...)
TBLOG_LOG(level, module, fmt, ...)
```

- Chuỗi format kiểu `printf`. GCC/Clang kiểm tra format lúc biên dịch (MinGW: archetype `gnu_printf`); MSVC đánh dấu bằng SAL `_Printf_format_string_` cho `/analyze`.
- Thứ tự ở mỗi lệnh: cắt theo `TBLOG_COMPILE_LEVEL` lúc biên dịch, tra module của call site (chỉ lần đầu, rồi cache trong biến static), so với level của module. **Lệnh bị lọc không format và không tính đối số.**
- `module` là chuỗi; nó chỉ được đọc ở lần chạy đầu tiên của call site đó, nên phải là hằng (thường là literal).
- Một dòng log đã format tối đa 1024 byte, tính cả `\n`. Dài hơn thì bị cắt và kết thúc bằng `...`.

`tblog_write(m, lv, file, line, func, fmt, ...)` là hàm mà các macro gọi. Nó không tự kiểm tra level; dùng macro thay vì gọi trực tiếp.

### `TBLOG_COMPILE_LEVEL`

```c
#define TBLOG_COMPILE_LEVEL TBLOG_LV_INFO   /* trước khi include, hoặc -DTBLOG_COMPILE_LEVEL=TBLOG_LV_INFO */
#include <tblog/log.h>
```

Mọi lệnh có level thấp hơn bị compiler xóa hẳn (cả chuỗi format lẫn biểu thức đối số). Mặc định `TBLOG_LV_TRACE` (giữ hết). Áp dụng cho từng translation unit; `tblog.hpp` (macro `TBLOGF_*`, `Logger`) dùng cùng macro này.

### `TBLOG_FATAL`

Ghi dòng log, gọi `tblog_flush()` (dòng đã nằm trên đĩa), rồi `abort()`. `abort()` đi qua crash handler như mọi `SIGABRT`. Nếu dòng FATAL bị lọc (level `OFF`, hoặc `TBLOG_COMPILE_LEVEL` cao hơn `FATAL`) thì không có gì xảy ra, kể cả `abort()`.

### Log trước `tblog_init` và sau `tblog_shutdown`

Dòng vẫn được format theo pattern mặc định rồi ghi thẳng ra stderr. Lọc level vẫn áp dụng (level gốc mặc định `INFO`, hoặc level đã đặt bằng `tblog_set_level`).

### Kết thúc process

- `tblog_init` đăng ký `tblog_shutdown` với `atexit()`, nên process chỉ `return` từ `main` hoặc gọi `exit()` vẫn ghi đủ log.
- Windows, khi handler `atexit` đó chạy lúc DLL bị gỡ (`tblog.dll`, hoặc bản tĩnh nằm trong một DLL khác): các thread khác đã bị dừng, nên tblog không join thread mà xả buffer ngay trên thread đang thoát vào file và console. Sink của `tblog_add_sink` không nhận các dòng cuối này. Xem [architecture.md](architecture.md#kết-thúc-process-trên-windows).
- Nên tự gọi `tblog_shutdown()` trước khi thoát (và trước `FreeLibrary` của `tblog.dll`) để mọi sink đều nhận đủ.
- `_exit()`, `quick_exit()`, `kill -9`, `TerminateProcess`: không có shutdown, phần còn trong buffer bị mất ([độ bền](architecture.md#độ-bền-của-log)).

### Sink riêng: `tblog_add_sink`

```c
typedef void (*tblog_sink_write_fn)(void *ctx, const char *data, size_t len);
typedef void (*tblog_sink_flush_fn)(void *ctx);   /* có thể NULL */
int tblog_add_sink(tblog_sink_write_fn write, tblog_sink_flush_fn flush, void *ctx);
```

Gửi mọi dòng log tới code của bạn, song song với file, console và syslog. Với `file = false` trong cấu hình, sink riêng có thể là đầu ra duy nhất.

- Gọi **trước** `tblog_init`. Trả `0` khi thành công; `-1` nếu logger đang chạy hoặc `write == NULL`.
- Sink đã đăng ký giữ nguyên qua các lần `tblog_init`/`tblog_shutdown`; không có hàm gỡ.
- `write(ctx, data, len)` nhận một hoặc nhiều dòng hoàn chỉnh (mỗi dòng kết thúc bằng `\n`, `data` không kết thúc bằng NUL). Dòng của cùng một thread giữ đúng thứ tự.
- `flush(ctx)` được gọi sau mỗi lô ghi, khi `tblog_flush()` và khi `tblog_shutdown()`.
- Cả hai chạy trên thread nền của tblog, không bao giờ đồng thời với nhau, không bao giờ trong signal handler. Chúng không được gọi `tblog_init`, `tblog_shutdown` hay `tblog_flush`. Sink chậm làm chậm việc ghi của mọi sink khác.
- Thứ tự ghi: file, console, syslog, rồi các sink riêng theo thứ tự đăng ký.
- Khi crash, các dòng còn trong buffer chỉ được xả vào file và console, không vào sink riêng.

```c
static void to_stdout(void *ctx, const char *data, size_t len) { fwrite(data, 1, len, (FILE *)ctx); }
static void flush_stdout(void *ctx) { fflush((FILE *)ctx); }

tblog_add_sink(to_stdout, flush_stdout, stdout);
tblog_init("log.conf");
```

## API C++ (`<tblog/tblog.hpp>`)

```cpp
#include <tblog/tblog.hpp>

TBLOGF_INFO("user {} logged in from {}", user_id, ip);
TBLOGF_MWARN("network", "retry {}/{} after {:.1f}s", n, max, secs);

tblog::Logger net("network");
net.info("connected to {}:{}", host, port);              // file:dòng của chính dòng này
net.info(TBLOG_HERE, "explicit location {}", x);          // vị trí ghi rõ
TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "dump {}", expensive()); // không tính đối số nếu bị lọc
```

- Macro `TBLOGF_TRACE` ... `TBLOGF_FATAL`, `TBLOGF_MTRACE` ... `TBLOGF_MFATAL`, `TBLOGF_LOG(level, module, fmt, ...)`: cùng logic call site với `TBLOG_*` (cắt lúc biên dịch, cache module, kiểm tra level trước khi tính đối số).
- Dòng được format vào buffer trên stack `TBLOGF_BUF_SIZE` byte (mặc định 1024, tối thiểu 16; định nghĩa trước khi include để đổi), rồi chuyển cho `tblog_write`. Không cấp phát bộ nhớ. Nội dung dài hơn bị cắt, kết thúc bằng `...`.

### `tblog::Logger`

- `Logger()` (module gốc), `Logger("name")`. Rẻ khi copy (một con trỏ).
- `trace/debug/info/warn/error/fatal(fmt, args...)`, `log(level, fmt, args...)`, mỗi hàm còn có dạng nhận `TBLOG_HERE` (`tblog::SourceLoc`) làm đối số đầu.
- `enabled(level)`, `module()`, `name()`.
- File/dòng/hàm của call site được lấy qua `__builtin_FILE/LINE/FUNCTION` (GCC 4.8+, Clang 9+, MSVC 19.26+); compiler cũ hơn ghi `?:0`.
- Khác với macro: **đối số của `Logger::info(...)` được tính trước khi kiểm tra level** (chỉ phần format bị bỏ qua). Với đối số đắt, dùng `TBLOGF_LOGGER(lg, lv, ...)` hoặc `if (lg.enabled(lv))`.

### Cú pháp format

- `{}` lấy đối số kế tiếp; `{N}` lấy đối số thứ N (từ 0); `{{` và `}}` là dấu ngoặc nhọn.
- Spec: `{[N]:[[fill]align][sign][#][0][width][.precision][type]}`
  - `align`: `<` `>` `^`; `sign`: `+` `-` ` `; `#`: tiền tố `0x`/`0b`/`0`...; `0`: đệm số 0.
  - `type`: `d x X o b B c` (số nguyên, `c` = ký tự), `e E f F g G` (số thực), `s` (chuỗi), `p` (con trỏ).
- Kiểu có sẵn: số nguyên, `bool` (`true`/`false`), `char`, `float`, `double`, `long double`, `const char *`, mảng `char`, `std::string`, `std::string_view` (C++17), con trỏ, `nullptr`, enum (in giá trị số).
- Với `double` ở đường nóng, `{:.Nf}` nhanh hơn `{}` (dạng ngắn nhất cần kiểm tra lại để in đúng như `printf`).

### Kiểu tự định nghĩa

Theo thứ tự ưu tiên, tìm qua ADL trong namespace của kiểu:

1. `void tblog_format(tblog::Sink &s, const T &v, const tblog::FormatSpec &spec)`: tự xử lý spec.
2. `void tblog_format(tblog::Sink &s, const T &v)`: spec chỉ được có fill/align/width (tblog tự đệm).
3. Enum: in giá trị số.
4. `operator<<(std::ostream &, const T &)` (chỉ class): ghi thẳng vào `Sink`, không qua `ostringstream`; spec như (2).

Không có cách nào thì là lỗi biên dịch (`static_assert` với thông báo rõ ràng).

```cpp
namespace geo {
struct Point { int x, y; };
inline void tblog_format(tblog::Sink &s, const Point &p) { s.format("({}, {})", p.x, p.y); }
}
TBLOGF_INFO("at {:>12}", geo::Point{3, 4});
```

`tblog::Sink` (buffer cố định, không cấp phát): `put(c)`, `write(p, n)`, `write(const char *)`, `write(std::string)`, `fill(n, c)`, `format(fmt, args...)`, `size()`, `capacity()`, `truncated()`. Ngoài log còn có `tblog::format_to(buf, cap, fmt, args...)` để format vào buffer của bạn (không ghi NUL, trả độ dài, cắt thành `...` nếu không đủ chỗ).

### Lỗi format

Sai format không crash và không ném exception; lỗi hiện ngay trong dòng log:

| Dấu | Khi nào |
|---|---|
| `{!missing-arg}` | Thiếu đối số cho một `{}` |
| `{!extra-args}` | Thừa đối số (thêm vào cuối dòng) |
| `{!bad-spec}` | Spec sai hoặc không hợp với kiểu |
| `{!bad-format}` | Chuỗi format hỏng (`{` không đóng, chỉ số sai) |

## Thread-safety

- Mọi hàm và macro ghi log gọi được từ bất kỳ thread nào, đồng thời.
- `tblog_init`, `tblog_shutdown`, `tblog_add_sink` được tuần tự hóa bằng một mutex; `tblog_flush` gọi được từ nhiều thread.
- `tblog_set_level` / `tblog_get_level` / `tblog_module_get` an toàn giữa các thread; level đổi có hiệu lực ngay với các lệnh log sau đó.
- Không gọi API tblog từ signal handler (format dùng `vsnprintf` và buffer `thread_local`).
- Mỗi thread gắn với một shard của buffer: dòng của cùng thread luôn đúng thứ tự; dòng của các thread khác nhau có thể lệch thứ tự thời gian tối đa khoảng một `flush.interval_ms` ([architecture.md](architecture.md)).
