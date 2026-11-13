/* tblog-lite: C99 logging for microcontrollers (Cortex-M, FreeRTOS or
 * bare-metal). Source-compatible with the logging macros of full tblog
 * (include/tblog/log.h): TBLOG_TRACE..TBLOG_FATAL, TBLOG_MTRACE..TBLOG_MFATAL,
 * tblog_level / TBLOG_LV_*, TBLOG_COMPILE_LEVEL.
 *
 * Model: a log call checks the level, formats the line into a stack buffer
 * (TBLOG_LITE_LINE_MAX bytes), then copies it into a static ring buffer
 * (TBLOG_LITE_BUF_SIZE bytes) inside a short critical section. It never
 * blocks: a line that does not fit is dropped and counted. Bytes reach the
 * port's write() callback only from tblog_lite_process() / tblog_lite_flush(),
 * which you call from the idle task / main loop / a low-priority task.
 * Log calls are safe from ISRs when the port's critical section masks them.
 * No malloc, no libc printf, no floating point unless TBLOG_LITE_FLOAT=1.
 */
#ifndef TBLOG_LITE_H
#define TBLOG_LITE_H

#ifdef TBLOG_LOG_H
#error "include either <tblog/log.h> (full tblog) or tblog_lite.h, not both"
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TBLOG_LV_TRACE = 0,
    TBLOG_LV_DEBUG = 1,
    TBLOG_LV_INFO  = 2,
    TBLOG_LV_WARN  = 3,
    TBLOG_LV_ERROR = 4,
    TBLOG_LV_FATAL = 5,
    TBLOG_LV_OFF   = 6
} tblog_level;

#include "tblog_lite_config.h"

#if defined(__GNUC__)
#define TBLOG_LITE_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define TBLOG_LITE_PRINTF(f, a)
#endif

/* Platform glue. Only `write` is required; NULL members are skipped.
 * The structure must outlive the logger (make it static const). */
typedef struct tblog_lite_port {
    /* Output: called only from tblog_lite_process()/tblog_lite_flush(), never
     * from a log call. May be slow (polling UART) or start a DMA/RTT write, but
     * must have consumed the bytes when it returns. */
    void (*write)(const char *data, size_t len);
    /* Timestamp printed in each header (ticks, ms, cycles: your choice). */
    uint32_t (*timestamp)(void);
    /* Critical section that excludes every context that may log (tasks and
     * ISRs). enter returns the previous state that exit restores, so it may
     * be used from any context (e.g. PRIMASK save/restore). Never nested by
     * the library. NULL = single context, no protection. */
    uint32_t (*enter_critical)(void);
    void (*exit_critical)(uint32_t state);
    /* Called after a FATAL line was logged and flushed (full tblog aborts).
     * NULL = return to the caller. */
    void (*fatal)(void);
} tblog_lite_port;

/* Install the port (may be called again to replace it). Lines logged before
 * init are kept in the ring and printed by the first process(). */
void tblog_lite_init(const tblog_lite_port *port);

/* Write out everything queued at the time of the call, then a
 * "N line(s) dropped" notice if lines were lost since the last report.
 * Returns the number of bytes written. Not re-entrant: a nested call (e.g.
 * from an ISR while the main loop is in process()) returns 0 immediately. */
size_t tblog_lite_process(void);

/* Like process(), but loops until the ring is empty (e.g. before reset or
 * from a fault handler). */
void tblog_lite_flush(void);

/* Runtime level of the whole logger (no per-module levels in lite). */
void        tblog_lite_set_level(tblog_level lv);
tblog_level tblog_lite_get_level(void);

/* Total number of lines dropped because the ring was full. */
uint32_t tblog_lite_dropped_count(void);

/* The log call behind the macros. module and file may be NULL. */
void tblog_lite_write(tblog_level lv, const char *module, const char *file, int line,
                      const char *fmt, ...) TBLOG_LITE_PRINTF(5, 6);

/* The formatter, usable on its own. C99 snprintf semantics (always
 * NUL-terminates when n > 0, returns the untruncated length) for:
 * %d %i %u %x %X %o %c %s %p %% ; flags - 0 + space # ; width and precision
 * (number or *) ; length hh h l ll z ; %f %F %e %E %g %G if TBLOG_LITE_FLOAT. */
int tblog_lite_snprintf(char *buf, size_t n, const char *fmt, ...) TBLOG_LITE_PRINTF(3, 4);
int tblog_lite_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* Current runtime level; read by the macros (do not write it directly). */
extern volatile unsigned char tblog_lite_level_;

#if TBLOG_LITE_FILELINE
#define TBLOG_LITE_SRC_ TBLOG_LITE_FILE, __LINE__
#else
#define TBLOG_LITE_SRC_ (const char *)0, 0
#endif
#if TBLOG_LITE_MODULE
#define TBLOG_LITE_MOD_(mod) (mod)
#else
#define TBLOG_LITE_MOD_(mod) (const char *)0
#endif

/* The first test is a constant: below TBLOG_COMPILE_LEVEL the whole call,
 * format string and argument expressions included, is removed by the
 * compiler (verified at -O0 and -Os by `make test`). */
#define TBLOG_LOG(lv, mod, ...)                                                      \
    do {                                                                             \
        if ((int)(lv) >= (int)(TBLOG_COMPILE_LEVEL) && (int)(lv) >= tblog_lite_level_) \
            tblog_lite_write((lv), TBLOG_LITE_MOD_(mod), TBLOG_LITE_SRC_, __VA_ARGS__); \
    } while (0)

#define TBLOG_TRACE(...) TBLOG_LOG(TBLOG_LV_TRACE, (const char *)0, __VA_ARGS__)
#define TBLOG_DEBUG(...) TBLOG_LOG(TBLOG_LV_DEBUG, (const char *)0, __VA_ARGS__)
#define TBLOG_INFO(...)  TBLOG_LOG(TBLOG_LV_INFO,  (const char *)0, __VA_ARGS__)
#define TBLOG_WARN(...)  TBLOG_LOG(TBLOG_LV_WARN,  (const char *)0, __VA_ARGS__)
#define TBLOG_ERROR(...) TBLOG_LOG(TBLOG_LV_ERROR, (const char *)0, __VA_ARGS__)
#define TBLOG_FATAL(...) TBLOG_LOG(TBLOG_LV_FATAL, (const char *)0, __VA_ARGS__)

#define TBLOG_MTRACE(mod, ...) TBLOG_LOG(TBLOG_LV_TRACE, mod, __VA_ARGS__)
#define TBLOG_MDEBUG(mod, ...) TBLOG_LOG(TBLOG_LV_DEBUG, mod, __VA_ARGS__)
#define TBLOG_MINFO(mod, ...)  TBLOG_LOG(TBLOG_LV_INFO,  mod, __VA_ARGS__)
#define TBLOG_MWARN(mod, ...)  TBLOG_LOG(TBLOG_LV_WARN,  mod, __VA_ARGS__)
#define TBLOG_MERROR(mod, ...) TBLOG_LOG(TBLOG_LV_ERROR, mod, __VA_ARGS__)
#define TBLOG_MFATAL(mod, ...) TBLOG_LOG(TBLOG_LV_FATAL, mod, __VA_ARGS__)

#if TBLOG_LITE_COMPAT
/* Full-tblog names. set_level/get_level act on the single root level: a
 * non-root module name is ignored (set) or answered with the root level (get). */
void tblog_lite_set_level_compat_(const char *module, tblog_level lv);
#define tblog_flush()                tblog_lite_flush()
#define tblog_shutdown()             tblog_lite_flush()
#define tblog_set_level(module, lv)  tblog_lite_set_level_compat_((module), (lv))
#define tblog_get_level(module)      ((void)(module), tblog_lite_get_level())
#define tblog_dropped_count()        ((uint64_t)tblog_lite_dropped_count())
#endif

#ifdef __cplusplus
}
#endif

#endif /* TBLOG_LITE_H */
