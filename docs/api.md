# API

**English** | [Tiếng Việt](vi/api.md)

| Header | Usable from | Format style |
|---|---|---|
| `<tblog/log.h>` | C99 (MSVC: C11), C++11 | `printf`, format checked by the compiler |
| `<tblog/tblog.hpp>` | C++11, header-only, built on the C API | `{}`, accepts user-defined types |

## Lifecycle

```c
int  tblog_init(const char *config_path);   /* 0 = OK, -1 = could not open the log file / buffer */
void tblog_shutdown(void);                  /* write the rest, fsync, stop threads; safe to call repeatedly */
void tblog_flush(void);                     /* write + fsync every line so far, waits at most 2 s */
```

- `tblog_init`: if the configuration file cannot be read, defaults are used and it still returns `0`. Calling it again while running does nothing. It registers `tblog_shutdown` with `atexit()`.
- Init only once: every init/shutdown pair keeps that round's buffer.
- Before `tblog_init` and after `tblog_shutdown`, log lines go straight to stderr (levels are still filtered).
- `_exit`, `quick_exit`, `kill -9`: no shutdown, whatever is in the buffer is lost ([durability](architecture.md#durability-by-scenario)).

## Logging (C)

```c
TBLOG_TRACE(fmt, ...)  TBLOG_DEBUG  TBLOG_INFO  TBLOG_WARN  TBLOG_ERROR  TBLOG_FATAL
TBLOG_MINFO(module, fmt, ...)  ...                /* per module */
TBLOG_LOG(level, module, fmt, ...)
```

- **A filtered call does not format and does not evaluate its arguments**: 1.16 ns, versus 0.60 ns for `if (flag) fprintf(...)`.
- `module` must be constant (usually a literal): it is read only the first time the call site runs, then cached.
- Lines are at most 1024 bytes; longer ones are truncated with `...`.
- A log call **waits** in two cases: the buffer is full with `full_policy = block` (the default), and the line is ≥ `flush.on_level` ([configuration.md](configuration.md#choices-and-their-cost)).
- `TBLOG_FATAL`: writes, `tblog_flush()`, then `abort()`. If filtered out, it does not `abort()`.

### Removing log calls at compile time

```c
#define TBLOG_COMPILE_LEVEL TBLOG_LV_INFO   /* or -DTBLOG_COMPILE_LEVEL=TBLOG_LV_INFO */
#include <tblog/log.h>
```

Calls below the level are removed entirely (format string included). Pro: 0 ns and a smaller binary. Con: cannot be re-enabled at run time. Applies per source file, C++ API included.

## Levels and modules

```c
void          tblog_set_level(const char *module, tblog_level lv);   /* NULL = root */
tblog_level   tblog_get_level(const char *module);
tblog_module *tblog_module_get(const char *name);
uint64_t      tblog_dropped_count(void);   /* number of dropped lines (full_policy = drop only) */
```

- `TBLOG_LV_TRACE` < `DEBUG` < `INFO` < `WARN` < `ERROR` < `FATAL` < `OFF`.
- Changing the root level changes every module without its own level. Takes effect immediately.
- At most 256 modules, names ≤ 45 characters; beyond that the root module is returned.

## Custom sinks

```c
typedef void (*tblog_sink_write_fn)(void *ctx, const char *data, size_t len);
typedef void (*tblog_sink_flush_fn)(void *ctx);   /* may be NULL */
int tblog_add_sink(tblog_sink_write_fn write, tblog_sink_flush_fn flush, void *ctx);

static void to_stdout(void *ctx, const char *d, size_t n) { fwrite(d, 1, n, (FILE *)ctx); }
tblog_add_sink(to_stdout, NULL, stdout);          /* before tblog_init */
tblog_init("log.conf");
```

- Call it **before** `tblog_init`; sinks cannot be removed. Returns `-1` if the logger is running or `write == NULL`.
- `write` receives one or more complete lines (ending in `\n`, no NUL). `flush` is called after each batch.
- Runs on the background thread, never concurrently, never inside a signal handler. Do not call `tblog_init` / `shutdown` / `flush` from inside a sink.
- **Cons**: a slow sink slows every sink; it does not receive the crash drain (only file and console do).

## C++ API

```cpp
#include <tblog/tblog.hpp>

TBLOGF_INFO("user {} from {}", id, ip);
TBLOGF_MWARN("network", "retry {}/{} after {:.1f}s", n, max, secs);

tblog::Logger net("network");
net.info("connected to {}:{}", host, port);
TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "dump {}", expensive());   // expensive() not called if filtered
```

**Macro or `Logger`?**

| | `TBLOGF_*` | `tblog::Logger` |
|---|---|---|
| Arguments when filtered | Not evaluated | **Still evaluated** (only formatting is skipped) |
| Syntax | Macro | Member function, cheap to copy (one pointer) |
| Use when | Arguments are expensive | Arguments are cheap, you want the module attached to an object |

With `Logger` and expensive arguments: use `TBLOGF_LOGGER(lg, lv, ...)` or `if (lg.enabled(lv))`.

- Formats into a stack buffer of `TBLOGF_BUF_SIZE` bytes (1024 by default), no allocation.
- `Logger`'s `file:line` comes from `__builtin_FILE/LINE` (GCC 4.8+, Clang 9+, MSVC 19.26+); older compilers write `?:0`. Use `TBLOG_HERE` to pass it explicitly.

### Format syntax

- `{}` next argument, `{N}` argument N, `{{` `}}` are literal braces.
- Spec: `{[N]:[[fill]align][sign][#][0][width][.precision][type]}`; `type`: `d x X o b B c e E f F g G s p`.
- Built-in types: integers, `bool`, `char`, floating point, C strings, `std::string`, `std::string_view` (C++17), pointers, `nullptr`, enums (printed as numbers).
- `double` on a hot path: `{:.Nf}` is faster than `{}`.
- A bad format never crashes or throws; the log line contains `{!missing-arg}`, `{!extra-args}`, `{!bad-spec}` or `{!bad-format}`.

### User-defined types

Define `tblog_format` in the type's namespace (found through ADL):

```cpp
namespace geo {
struct Point { int x, y; };
inline void tblog_format(tblog::Sink &s, const Point &p) { s.format("({}, {})", p.x, p.y); }
}
TBLOGF_INFO("at {:>12}", geo::Point{3, 4});
```

Lookup order: `tblog_format(Sink&, const T&, const FormatSpec&)` (handles the spec itself) → `tblog_format(Sink&, const T&)` (tblog pads the width) → enum → `operator<<` (writes straight into the `Sink`, no `ostringstream`). None available: a compile error with a clear message.

`tblog::format_to(buf, cap, fmt, args...)` formats into your own buffer (returns the length, writes no NUL).

## Thread safety

- Every log call can be made from any thread, concurrently. Lines from the same thread are always in order.
- `tblog_init`, `tblog_shutdown`, `tblog_add_sink` are serialised by a mutex; the level functions are thread-safe.
- Do not call the tblog API from your own signal handlers.
