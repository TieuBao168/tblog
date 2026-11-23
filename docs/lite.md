# tblog-lite for microcontrollers

**English** | [Tiếng Việt](vi/lite.md)

[lite/](../lite/) is a separate C99 library for Cortex-M (FreeRTOS or bare-metal) that uses **the same macro names** as tblog (`TBLOG_INFO`, `TBLOG_MWARN`, `TBLOG_COMPILE_LEVEL`...): logging code moves between Linux and an MCU with only the include line and init changing.

```c
#include "tblog_lite.h"

static void uart_write(const char *data, size_t len) { /* UART / RTT */ }
static const tblog_lite_port port = { uart_write, my_tick_ms, my_irq_save, my_irq_restore, NULL };

tblog_lite_init(&port);
TBLOG_INFO("boot, %d sensors", 3);

/* in the idle task or main loop: */
tblog_lite_process();
```

## Compared with full tblog

| | tblog-lite | tblog |
|---|---|---|
| Memory | ~3 KB code, ~2.1 KB RAM (2 KB ring) | 8 MB buffer by default |
| Output | Your `write` function, called from `process()` | File, console, syslog, custom sinks |
| Buffer full | Always drops the line (counted) | Wait or drop |
| Levels | One global level | Per module |
| Callable from an ISR | Yes | No |
| malloc, libc printf | Not used | Used |

Sizes measured on an x86-64 host (`-Os`, `TBLOG_LITE_FLOAT=0`); enabling floating point adds ~2 KB of code. **Not yet measured or run on Cortex-M hardware**: the FreeRTOS and bare-metal ports have only been syntax-checked.

## How it works

- A log call formats onto the stack (`TBLOG_LITE_LINE_MAX` bytes), then copies into a static ring inside a short critical section. It **never blocks** and never calls `write`.
- `tblog_lite_process()` / `tblog_lite_flush()` (called by you) hand the bytes to `write`. When the ring is full the line is dropped, and the next `process()` prints `tblog-lite: N line(s) dropped`.
- Lines logged before `tblog_lite_init` still stay in the ring.

## API

| Function | Purpose |
|---|---|
| `tblog_lite_init(port)` | Install the port; `port` must live for the whole program |
| `tblog_lite_process()` | Write what is in the ring, returns the byte count; a nested call returns 0 |
| `tblog_lite_flush()` | Write until the ring is empty (before a reset, in a fault handler) |
| `tblog_lite_set_level`, `tblog_lite_get_level` | Global level |
| `tblog_lite_dropped_count()` | Number of dropped lines |
| `tblog_lite_snprintf` | Its own formatter: `%d %u %x %c %s %p`, flags, width, `hh h l ll z`; `%f %e %g` when `TBLOG_LITE_FLOAT=1` |

Port: only `write` is required. `timestamp()` provides the number at the start of the line; `enter_critical` / `exit_critical` must cover every context that logs (tasks and ISRs); `fatal()` is called after a FATAL line. `TBLOG_LITE_COMPAT=1` (the default) adds full tblog's names `tblog_flush`, `tblog_set_level`...

## Compile-time configuration

Set with `-D`, or `tblog_lite_user_config.h` + `-DTBLOG_LITE_USER_CONFIG`. The library and the application must use the same values.

| Macro | Default | Change when |
|---|---|---|
| `TBLOG_LITE_BUF_SIZE` | `2048` | Short of RAM (smaller) / lines often dropped (larger) |
| `TBLOG_LITE_LINE_MAX` | `128` | You need longer lines (also the stack used per log call) |
| `TBLOG_LITE_FLOAT` | `0` | You need `%f` (+~2 KB code, pulls in double arithmetic) |
| `TBLOG_LITE_LONG_LONG` | `1` | `0`: avoid 64-bit division helpers on 32-bit MCUs |
| `TBLOG_LITE_FILELINE` | `1` | `0`: drop `file:line`, no `__FILE__` strings in flash |
| `TBLOG_LITE_MODULE` | `1` | `0`: drop the module name from the line |
| `TBLOG_LITE_DEFAULT_LEVEL` | `TBLOG_LV_INFO` | Level after reset |
| `TBLOG_COMPILE_LEVEL` | `TBLOG_LV_TRACE` | Remove calls below the level from flash entirely |

## Bundled ports

| Port | Status |
|---|---|
| `lite/port/posix/` | Used for tests and the demo |
| `lite/port/freertos/` | BASEPRI critical section; not yet run on hardware |
| `lite/port/cortex_m_baremetal/` | PRIMASK, SysTick or `DWT->CYCCNT`, UART or RTT; not yet run on hardware |

Commands: `make -C lite` (library), `test`, `test-asan`, `demo`, `size`. CI runs all four.
