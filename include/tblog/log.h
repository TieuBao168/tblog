/* tblog — public C API. Usable from C (C99) and C++. */
#ifndef TBLOG_LOG_H
#define TBLOG_LOG_H

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

/* A module handle. `level` is the EFFECTIVE level and is only ever accessed
 * with __atomic builtins (relaxed). Do not touch fields directly. */
typedef struct tblog_module {
    unsigned char level;      /* effective level (atomic access only)          */
    unsigned char explicit_;  /* 1 if level was set explicitly for this module */
    char name[46];            /* "" = root module                              */
} tblog_module;

/* Lifecycle. config_path may be NULL (defaults). Returns 0 on success.
 * Calling tblog_write before tblog_init writes to stderr. */
int  tblog_init(const char *config_path);
void tblog_shutdown(void);  /* drain, fsync, stop worker; idempotent */
void tblog_flush(void);     /* ask the worker to flush now and wait for it */

/* Levels. module NULL or "" = root; setting root updates every module that
 * has no explicit level of its own. */
void        tblog_set_level(const char *module, tblog_level lv);
tblog_level tblog_get_level(const char *module);

/* Returns a stable pointer (never freed) for the module name; NULL/"" = root.
 * Names longer than 45 chars are truncated. Thread-safe. */
tblog_module *tblog_module_get(const char *name);

static inline tblog_level tblog_module_level(const tblog_module *m) {
    return (tblog_level)__atomic_load_n(&m->level, __ATOMIC_RELAXED);
}

void tblog_write(tblog_module *m, tblog_level lv, const char *file, int line,
                 const char *func, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

/* Static descriptor of one log statement, emitted by TBLOG_LOG (one per call
 * site, static storage). `fmt` is the format string when it is a compile-time
 * constant (a string literal), else NULL. `cache_` is library-private (atomic
 * access only, starts NULL). */
typedef struct tblog_site {
    const char *file;
    const char *func;
    const char *fmt;
    int line;
    void *cache_;
} tblog_site;

/* Same as tblog_write with the call site described by `site`. With
 * `format.mode = deferred` (buffer backend) and fmt == site->fmt, the
 * arguments are captured in binary and formatted later by the worker. */
void tblog_write_site(tblog_module *m, tblog_level lv, tblog_site *site,
                      const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Number of lines dropped because the buffer/region was full. */
uint64_t tblog_dropped_count(void);

#ifndef TBLOG_COMPILE_LEVEL
#define TBLOG_COMPILE_LEVEL TBLOG_LV_TRACE
#endif

#define TBLOG_LOG(lv, mod, ...)                                                   \
    do {                                                                          \
        if ((lv) >= TBLOG_COMPILE_LEVEL) {                                        \
            static tblog_module *tblog_m_cache_ = 0;                              \
            tblog_module *tblog_m_ = __atomic_load_n(&tblog_m_cache_, __ATOMIC_ACQUIRE); \
            if (!tblog_m_) {                                                      \
                tblog_m_ = tblog_module_get(mod);                                 \
                __atomic_store_n(&tblog_m_cache_, tblog_m_, __ATOMIC_RELEASE);    \
            }                                                                     \
            if ((lv) >= tblog_module_level(tblog_m_))                             \
                TBLOG_EMIT_(tblog_m_, (lv), __VA_ARGS__);                         \
        }                                                                         \
    } while (0)

/* First macro argument (the format). */
#define TBLOG_FMT0_(f, ...) f
#ifdef TBLOG_NO_SITE
/* Opt-out: the pre-site call (always formatted eagerly on the caller). */
#define TBLOG_EMIT_(m, lv, ...) tblog_write(m, lv, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
/* __builtin_constant_p is folded at compile time: a string literal format is
 * recorded in the site, anything else becomes NULL (formatted eagerly). */
#define TBLOG_EMIT_(m, lv, ...)                                                   \
    do {                                                                          \
        static tblog_site tblog_site_ = {                                         \
            __FILE__, __func__,                                                   \
            __builtin_constant_p(TBLOG_FMT0_(__VA_ARGS__, 0))                     \
                ? TBLOG_FMT0_(__VA_ARGS__, 0) : 0,                                \
            __LINE__, 0};                                                         \
        tblog_write_site(m, lv, &tblog_site_, __VA_ARGS__);                       \
    } while (0)
#endif

#define TBLOG_TRACE(...) TBLOG_LOG(TBLOG_LV_TRACE, 0, __VA_ARGS__)
#define TBLOG_DEBUG(...) TBLOG_LOG(TBLOG_LV_DEBUG, 0, __VA_ARGS__)
#define TBLOG_INFO(...)  TBLOG_LOG(TBLOG_LV_INFO,  0, __VA_ARGS__)
#define TBLOG_WARN(...)  TBLOG_LOG(TBLOG_LV_WARN,  0, __VA_ARGS__)
#define TBLOG_ERROR(...) TBLOG_LOG(TBLOG_LV_ERROR, 0, __VA_ARGS__)
#define TBLOG_FATAL(...) TBLOG_LOG(TBLOG_LV_FATAL, 0, __VA_ARGS__)

#define TBLOG_MTRACE(mod, ...) TBLOG_LOG(TBLOG_LV_TRACE, mod, __VA_ARGS__)
#define TBLOG_MDEBUG(mod, ...) TBLOG_LOG(TBLOG_LV_DEBUG, mod, __VA_ARGS__)
#define TBLOG_MINFO(mod, ...)  TBLOG_LOG(TBLOG_LV_INFO,  mod, __VA_ARGS__)
#define TBLOG_MWARN(mod, ...)  TBLOG_LOG(TBLOG_LV_WARN,  mod, __VA_ARGS__)
#define TBLOG_MERROR(mod, ...) TBLOG_LOG(TBLOG_LV_ERROR, mod, __VA_ARGS__)
#define TBLOG_MFATAL(mod, ...) TBLOG_LOG(TBLOG_LV_FATAL, mod, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* TBLOG_LOG_H */
