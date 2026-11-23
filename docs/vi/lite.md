# tblog-lite cho vi điều khiển

[English](../lite.md) | **Tiếng Việt**

[lite/](../../lite/) là thư viện C99 riêng cho Cortex-M (FreeRTOS hoặc bare-metal), dùng **cùng tên macro** với tblog (`TBLOG_INFO`, `TBLOG_MWARN`, `TBLOG_COMPILE_LEVEL`...): code log chuyển qua lại giữa Linux và MCU chỉ khác dòng include và init.

```c
#include "tblog_lite.h"

static void uart_write(const char *data, size_t len) { /* UART / RTT */ }
static const tblog_lite_port port = { uart_write, my_tick_ms, my_irq_save, my_irq_restore, NULL };

tblog_lite_init(&port);
TBLOG_INFO("boot, %d sensors", 3);

/* trong idle task hoặc vòng lặp chính: */
tblog_lite_process();
```

## So với tblog đầy đủ

| | tblog-lite | tblog |
|---|---|---|
| Bộ nhớ | ~3.7 KB code (3.4 KB với `TBLOG_LITE_ESCAPE=0`), ~2.1 KB RAM (ring 2 KB) | 8 MB buffer mặc định |
| Đầu ra | Hàm `write` của bạn, gọi từ `process()` | File, console, syslog, sink riêng |
| Buffer đầy | Luôn bỏ dòng (đếm được) | Chờ hoặc bỏ |
| Level | Một level toàn cục | Theo module |
| Gọi từ ISR | Được | Không |
| malloc, printf của libc | Không dùng | Có |

Số kích thước đo trên host x86-64 (`-Os`, `TBLOG_LITE_FLOAT=0`); bật số thực thêm ~2 KB code. **Chưa đo và chưa chạy trên phần cứng Cortex-M**: port FreeRTOS và bare-metal mới chỉ được kiểm tra cú pháp.

## Cách hoạt động

- Lệnh log format vào stack (`TBLOG_LITE_LINE_MAX` byte) rồi chép vào ring tĩnh trong một critical section ngắn. **Không bao giờ chặn**, không bao giờ gọi `write`.
- `tblog_lite_process()` / `tblog_lite_flush()` (do bạn gọi) đưa byte tới `write`. Ring đầy thì dòng bị bỏ, lần `process()` sau in `tblog-lite: N line(s) dropped`.
- Dòng log trước `tblog_lite_init` vẫn nằm trong ring.

## API

| Hàm | Việc |
|---|---|
| `tblog_lite_init(port)` | Cài port; `port` phải sống suốt chương trình |
| `tblog_lite_process()` | Ghi phần đang có trong ring, trả số byte; gọi lồng trả 0 |
| `tblog_lite_flush()` | Ghi tới khi ring rỗng (trước reset, trong fault handler) |
| `tblog_lite_set_level`, `tblog_lite_get_level` | Level toàn cục |
| `tblog_lite_dropped_count()` | Số dòng bị bỏ |
| `tblog_lite_snprintf` | Formatter riêng: `%d %u %x %c %s %p`, cờ, width, `hh h l ll z`; `%f %e %g` khi `TBLOG_LITE_FLOAT=1` |

Port: chỉ `write` bắt buộc. `timestamp()` cho số đầu dòng; `enter_critical` / `exit_critical` phải che mọi ngữ cảnh có log (task và ISR); `fatal()` gọi sau dòng FATAL. `TBLOG_LITE_COMPAT=1` (mặc định) thêm các tên `tblog_flush`, `tblog_set_level`... của tblog đầy đủ.

## Cấu hình lúc biên dịch

Đặt bằng `-D`, hoặc `tblog_lite_user_config.h` + `-DTBLOG_LITE_USER_CONFIG`. Thư viện và ứng dụng phải cùng giá trị.

| Macro | Mặc định | Đổi khi |
|---|---|---|
| `TBLOG_LITE_BUF_SIZE` | `2048` | Ít RAM (nhỏ hơn) / hay bị bỏ dòng (lớn hơn) |
| `TBLOG_LITE_LINE_MAX` | `128` | Cần dòng dài hơn (cũng là stack mỗi lệnh log) |
| `TBLOG_LITE_FLOAT` | `0` | Cần `%f` (+~2 KB code, kéo số học double) |
| `TBLOG_LITE_LONG_LONG` | `1` | `0`: tránh hàm chia 64 bit trên MCU 32 bit |
| `TBLOG_LITE_FILELINE` | `1` | `0`: bỏ `file:line`, không có chuỗi `__FILE__` trong flash |
| `TBLOG_LITE_MODULE` | `1` | `0`: bỏ tên module khỏi dòng |
| `TBLOG_LITE_ESCAPE` | `1` | `0`: ghi nguyên ký tự điều khiển trong nội dung (bớt ~270 byte code); `1` escape chúng như `message.escape` của tblog đầy đủ |
| `TBLOG_LITE_DEFAULT_LEVEL` | `TBLOG_LV_INFO` | Level sau reset |
| `TBLOG_COMPILE_LEVEL` | `TBLOG_LV_TRACE` | Xóa hẳn lệnh dưới level khỏi flash |

## Port có sẵn

| Port | Trạng thái |
|---|---|
| `lite/port/posix/` | Dùng cho test và demo |
| `lite/port/freertos/` | Critical section BASEPRI; chưa chạy trên phần cứng |
| `lite/port/cortex_m_baremetal/` | PRIMASK, SysTick hoặc `DWT->CYCCNT`, UART hoặc RTT; chưa chạy trên phần cứng |

Lệnh: `make -C lite` (thư viện), `test`, `test-asan`, `demo`, `size`. CI chạy cả bốn.
