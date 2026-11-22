# API

| Header | Dùng từ | Kiểu format |
|---|---|---|
| `<tblog/log.h>` | C99 (MSVC: C11), C++11 | `printf`, compiler kiểm tra format |
| `<tblog/tblog.hpp>` | C++11, header-only, xây trên API C | `{}`, nhận kiểu tự định nghĩa |

## Vòng đời

```c
int  tblog_init(const char *config_path);   /* 0 = OK, -1 = không mở được file log / buffer */
void tblog_shutdown(void);                  /* ghi nốt, fsync, dừng thread; gọi nhiều lần không sao */
void tblog_flush(void);                     /* ghi + fsync mọi dòng đã có, chờ tối đa 2 s */
```

- `tblog_init`: file cấu hình không đọc được thì dùng mặc định và vẫn trả `0`. Gọi lại khi đang chạy: không làm gì. Đăng ký `tblog_shutdown` với `atexit()`.
- Chỉ nên init một lần: mỗi cặp init/shutdown giữ lại buffer của lần đó.
- Trước `tblog_init` và sau `tblog_shutdown`, dòng log ra thẳng stderr (vẫn lọc level).
- `_exit`, `quick_exit`, `kill -9`: không có shutdown, phần trong buffer bị mất ([độ bền](architecture.md#độ-bền-theo-tình-huống)).

## Ghi log (C)

```c
TBLOG_TRACE(fmt, ...)  TBLOG_DEBUG  TBLOG_INFO  TBLOG_WARN  TBLOG_ERROR  TBLOG_FATAL
TBLOG_MINFO(module, fmt, ...)  ...                /* theo module */
TBLOG_LOG(level, module, fmt, ...)
```

- **Lệnh bị lọc không format, không tính đối số**: 1.16 ns, so với 0.60 ns của `if (flag) fprintf(...)`.
- `module` phải là hằng (thường là literal): nó chỉ được đọc ở lần chạy đầu của call site, rồi được cache.
- Dòng tối đa 1024 byte, dài hơn bị cắt thành `...`.
- Lệnh log **chờ** trong hai trường hợp: buffer đầy với `full_policy = block` (mặc định), và dòng ≥ `flush.on_level` ([configuration.md](configuration.md#các-lựa-chọn-và-cái-giá)).
- `TBLOG_FATAL`: ghi, `tblog_flush()`, rồi `abort()`. Bị lọc thì không `abort()`.

### Xóa lệnh log lúc biên dịch

```c
#define TBLOG_COMPILE_LEVEL TBLOG_LV_INFO   /* hoặc -DTBLOG_COMPILE_LEVEL=TBLOG_LV_INFO */
#include <tblog/log.h>
```

Lệnh dưới level bị xóa hẳn (cả chuỗi format). Ưu: 0 ns và binary nhỏ hơn. Nhược: không bật lại được lúc chạy. Áp dụng cho từng file nguồn, cả API C++.

## Level và module

```c
void          tblog_set_level(const char *module, tblog_level lv);   /* NULL = gốc */
tblog_level   tblog_get_level(const char *module);
tblog_module *tblog_module_get(const char *name);
uint64_t      tblog_dropped_count(void);   /* số dòng bị bỏ (chỉ full_policy = drop) */
```

- `TBLOG_LV_TRACE` < `DEBUG` < `INFO` < `WARN` < `ERROR` < `FATAL` < `OFF`.
- Đổi level gốc thì mọi module chưa đặt riêng đổi theo. Có hiệu lực ngay.
- Tối đa 256 module, tên ≤ 45 ký tự; vượt quá thì trả module gốc.

## Sink riêng

```c
typedef void (*tblog_sink_write_fn)(void *ctx, const char *data, size_t len);
typedef void (*tblog_sink_flush_fn)(void *ctx);   /* có thể NULL */
int tblog_add_sink(tblog_sink_write_fn write, tblog_sink_flush_fn flush, void *ctx);

static void to_stdout(void *ctx, const char *d, size_t n) { fwrite(d, 1, n, (FILE *)ctx); }
tblog_add_sink(to_stdout, NULL, stdout);          /* trước tblog_init */
tblog_init("log.conf");
```

- Gọi **trước** `tblog_init`; không gỡ được. `-1` nếu logger đang chạy hoặc `write == NULL`.
- `write` nhận một hoặc nhiều dòng hoàn chỉnh (kết thúc `\n`, không có NUL). `flush` được gọi sau mỗi lô.
- Chạy trên thread nền, không bao giờ đồng thời, không bao giờ trong signal handler. Không gọi `tblog_init` / `shutdown` / `flush` từ trong sink.
- **Nhược**: sink chậm làm chậm mọi sink; không nhận phần xả khi crash (chỉ file và console nhận).

## API C++

```cpp
#include <tblog/tblog.hpp>

TBLOGF_INFO("user {} from {}", id, ip);
TBLOGF_MWARN("network", "retry {}/{} after {:.1f}s", n, max, secs);

tblog::Logger net("network");
net.info("connected to {}:{}", host, port);
TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "dump {}", expensive());   // không gọi expensive() nếu bị lọc
```

**Macro hay `Logger`?**

| | `TBLOGF_*` | `tblog::Logger` |
|---|---|---|
| Đối số khi bị lọc | Không tính | **Vẫn tính** (chỉ bỏ format) |
| Cú pháp | Macro | Hàm thành viên, copy rẻ (một con trỏ) |
| Dùng khi | Đối số đắt | Đối số rẻ, muốn gắn module vào object |

Với `Logger` và đối số đắt: dùng `TBLOGF_LOGGER(lg, lv, ...)` hoặc `if (lg.enabled(lv))`.

- Format vào buffer trên stack `TBLOGF_BUF_SIZE` byte (mặc định 1024), không cấp phát.
- `file:line` của `Logger` lấy qua `__builtin_FILE/LINE` (GCC 4.8+, Clang 9+, MSVC 19.26+); compiler cũ ghi `?:0`. Dùng `TBLOG_HERE` để ghi rõ.

### Cú pháp format

- `{}` đối số kế tiếp, `{N}` đối số thứ N, `{{` `}}` là ngoặc nhọn.
- Spec: `{[N]:[[fill]align][sign][#][0][width][.precision][type]}`; `type`: `d x X o b B c e E f F g G s p`.
- Kiểu có sẵn: số nguyên, `bool`, `char`, số thực, chuỗi C, `std::string`, `std::string_view` (C++17), con trỏ, `nullptr`, enum (in số).
- `double` trên đường nóng: `{:.Nf}` nhanh hơn `{}`.
- Sai format không crash, không ném exception; dòng log chứa `{!missing-arg}`, `{!extra-args}`, `{!bad-spec}` hoặc `{!bad-format}`.

### Kiểu tự định nghĩa

Định nghĩa `tblog_format` trong namespace của kiểu (tìm qua ADL):

```cpp
namespace geo {
struct Point { int x, y; };
inline void tblog_format(tblog::Sink &s, const Point &p) { s.format("({}, {})", p.x, p.y); }
}
TBLOGF_INFO("at {:>12}", geo::Point{3, 4});
```

Thứ tự tìm: `tblog_format(Sink&, const T&, const FormatSpec&)` (tự xử lý spec) → `tblog_format(Sink&, const T&)` (tblog tự đệm width) → enum → `operator<<` (ghi thẳng vào `Sink`, không qua `ostringstream`). Không có cách nào: lỗi biên dịch có thông báo rõ.

`tblog::format_to(buf, cap, fmt, args...)` format vào buffer của bạn (trả độ dài, không ghi NUL).

## Thread-safety

- Mọi lệnh log gọi được từ mọi thread, đồng thời. Dòng của cùng thread luôn đúng thứ tự.
- `tblog_init`, `tblog_shutdown`, `tblog_add_sink` được tuần tự hóa bằng mutex; các hàm level an toàn giữa các thread.
- Không gọi API tblog từ signal handler của bạn.
