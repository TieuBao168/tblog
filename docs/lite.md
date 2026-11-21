# tblog-lite cho vi điều khiển

Thư mục [lite/](../lite/) là một thư viện riêng, viết bằng C99, cho Cortex-M chạy FreeRTOS hoặc bare-metal. Nó dùng **cùng tên macro** với tblog (`TBLOG_INFO`, `TBLOG_MWARN`, `TBLOG_LV_*`, `TBLOG_COMPILE_LEVEL`...), nên code ghi log chuyển được qua lại giữa Linux và MCU: chỉ khác dòng include và lời gọi init. Không include `<tblog/log.h>` và `tblog_lite.h` cùng lúc (báo lỗi biên dịch).

```c
#include "tblog_lite.h"

static void uart_write(const char *data, size_t len) { /* gửi qua UART / RTT */ }
static const tblog_lite_port port = { uart_write, my_tick_ms, my_irq_save, my_irq_restore, NULL };

tblog_lite_init(&port);
TBLOG_INFO("boot, %d sensors", 3);
TBLOG_MWARN("net", "link down, retry in %d ms", 500);

/* trong idle task hoặc vòng lặp chính: */
tblog_lite_process();
```

## Cách hoạt động

- Lệnh log kiểm tra level, format dòng vào buffer trên stack (`TBLOG_LITE_LINE_MAX` byte), rồi chép vào một **ring buffer tĩnh** (`TBLOG_LITE_BUF_SIZE` byte) trong một critical section ngắn.
- **Không bao giờ chặn:** ring đầy thì dòng bị bỏ và đếm; lần `process()` sau ghi thêm dòng `tblog-lite: N line(s) dropped`.
- Byte chỉ tới hàm `write` của port từ `tblog_lite_process()` / `tblog_lite_flush()`, do bạn gọi từ idle task, vòng lặp chính hoặc một task ưu tiên thấp. Lệnh log không bao giờ gọi `write`.
- **Gọi được từ ISR**, miễn là critical section của port che được ISR đó.
- Không malloc, không dùng printf của libc (có formatter riêng `tblog_lite_snprintf`), không dùng số thực trừ khi `TBLOG_LITE_FLOAT=1`.
- Dòng log trước `tblog_lite_init` vẫn nằm trong ring và được in ở lần `process()` đầu tiên.
- Dạng một dòng: `<timestamp> <I|W|E...> [module] file.c:42 nội dung`.

## API

| Hàm | Việc |
|---|---|
| `tblog_lite_init(const tblog_lite_port *port)` | Cài port (gọi lại để thay port). `port` phải sống suốt chương trình (`static const`). |
| `tblog_lite_process()` | Ghi những gì đang có trong ring tại thời điểm gọi; trả số byte đã ghi. Không re-entrant: lời gọi lồng (ví dụ từ ISR) trả 0 ngay. |
| `tblog_lite_flush()` | Như `process()` nhưng lặp tới khi ring rỗng (trước khi reset, trong fault handler). |
| `tblog_lite_set_level(lv)`, `tblog_lite_get_level()` | Level của toàn bộ logger (lite không có level theo module). |
| `tblog_lite_dropped_count()` | Tổng số dòng bị bỏ vì ring đầy. |
| `tblog_lite_snprintf`, `tblog_lite_vsnprintf` | Formatter, dùng riêng được: `%d %i %u %x %X %o %c %s %p %%`, cờ `- 0 + space #`, width/precision (số hoặc `*`), `hh h l ll z`; `%f %F %e %E %g %G` khi `TBLOG_LITE_FLOAT=1`. |

Port (`tblog_lite_port`): chỉ `write` là bắt buộc, thành viên `NULL` được bỏ qua.

- `write(data, len)`: phải dùng xong byte khi trả về.
- `timestamp()`: số in ở đầu dòng (tick, ms, cycle tùy bạn).
- `enter_critical()` / `exit_critical(state)`: loại trừ mọi ngữ cảnh có ghi log (task và ISR); `NULL` = chỉ một ngữ cảnh.
- `fatal()`: gọi sau khi dòng FATAL đã được ghi và flush (tblog đầy đủ thì `abort()`); `NULL` = trả về chỗ gọi.

Với `TBLOG_LITE_COMPAT=1` (mặc định), các tên của tblog đầy đủ cũng dùng được: `tblog_flush()`, `tblog_shutdown()` (đều là `tblog_lite_flush()`), `tblog_set_level(module, lv)` (module khác gốc bị bỏ qua), `tblog_get_level(module)`, `tblog_dropped_count()`.

## Cấu hình lúc biên dịch

Đặt bằng `-DNAME=giá_trị`, hoặc tạo `tblog_lite_user_config.h` và build với `-DTBLOG_LITE_USER_CONFIG`. Thư viện và ứng dụng phải được build với cùng giá trị. Chi tiết: [lite/include/tblog_lite_config.h](../lite/include/tblog_lite_config.h).

| Macro | Mặc định | Ý nghĩa |
|---|---|---|
| `TBLOG_LITE_BUF_SIZE` | `2048` | Kích thước ring (RAM); chứa được `BUF_SIZE − 1` byte |
| `TBLOG_LITE_LINE_MAX` | `128` | Dòng dài nhất (header + nội dung + `\n`), cũng là buffer trên stack của mỗi lệnh log; dài hơn thì bị cắt |
| `TBLOG_LITE_FLOAT` | `0` | `1`: hỗ trợ `%f %e %g` (kéo theo số học double) |
| `TBLOG_LITE_LONG_LONG` | `1` | `0`: `ll` chỉ in 32 bit thấp, tránh link hàm chia 64 bit trên MCU 32 bit |
| `TBLOG_LITE_FILELINE` | `1` | `0`: bỏ `file:line` khỏi header (không có chuỗi `__FILE__` trong flash) |
| `TBLOG_LITE_FILE` | `__FILE__` | Tên file truyền cho macro (ví dụ `__FILE_NAME__` trên GCC 12+/Clang) |
| `TBLOG_LITE_MODULE` | `1` | `0`: bỏ tên module khỏi header (linker bỏ được các chuỗi module) |
| `TBLOG_LITE_DEFAULT_LEVEL` | `TBLOG_LV_INFO` | Level sau khi reset |
| `TBLOG_LITE_COMPAT` | `1` | Định nghĩa các tên hàm của tblog đầy đủ |
| `TBLOG_COMPILE_LEVEL` | `TBLOG_LV_TRACE` | Lệnh dưới level này bị xóa hẳn (cả chuỗi format và đối số); `make test` kiểm tra điều này ở `-O0` và `-Os` |

## Port có sẵn

| Port | Thư mục | Trạng thái |
|---|---|---|
| POSIX (host) | `lite/port/posix/` | Dùng cho test và `make -C lite demo` |
| FreeRTOS (Cortex-M, critical section BASEPRI) | `lite/port/freertos/` | **Chưa chạy trên phần cứng**, chỉ được kiểm tra cú pháp với header giả (`lite/port/stubs`). Có task xả log tùy chọn hoặc idle hook. Chỉ ISR có ưu tiên không cao hơn `configMAX_SYSCALL_INTERRUPT_PRIORITY` được ghi log. |
| Cortex-M bare-metal | `lite/port/cortex_m_baremetal/` | **Chưa chạy trên phần cứng**, chỉ kiểm tra cú pháp. Critical section bằng PRIMASK (không ghi log trong NMI/HardFault). Timestamp: bộ đếm ms (SysTick) hoặc `DWT->CYCCNT`. Đầu ra: UART bạn tự viết hoặc SEGGER RTT. |

## Kích thước

`make -C lite size` in kích thước các object `-Os` cho host, và cho Cortex-M4 (thumb) nếu có `arm-none-eabi-gcc` trong `PATH`. Trên host x86-64 với `TBLOG_LITE_FLOAT=0`: khoảng 3 KB code (core + formatter) và khoảng 2,1 KB RAM tĩnh, gần hết là ring 2 KB (đo bằng MinGW-w64 GCC `-Os -fno-asynchronous-unwind-tables`, 2026-11; bật `TBLOG_LITE_FLOAT` thêm khoảng 2 KB code). Chưa có số đo trên Cortex-M.

## Lệnh

| Lệnh | Việc |
|---|---|
| `make -C lite` | `lite/build/libtblog_lite.a` (host) |
| `make -C lite test` | Test formatter (số nguyên và số thực), ring (thường và ring nhỏ), ISR, cắt bỏ lúc biên dịch (`tests/check_strip.sh`), kiểm tra cú pháp các port. Fail nếu có `tests/test_*.c` không được build và chạy. |
| `make -C lite test-asan` | Như trên, với `-fsanitize=address,undefined` |
| `make -C lite demo` | Build và chạy demo trên host |
| `make -C lite size` | Kích thước, xem trên |

CI chạy cả bốn lệnh `test`, `test-asan`, `demo`, `size` (job `tblog-lite`).
