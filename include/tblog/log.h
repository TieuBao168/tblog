/* tblog — public C API. Usable from C (C99) and C++ (C++11 and later), with
 * GCC, Clang, MSVC and MinGW-w64. */
#ifndef TBLOG_LOG_H
#define TBLOG_LOG_H

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h> /* __iso_volatile_load8, _ReadWriteBarrier, __ldar64, __stlr64 */
#include <sal.h>    /* _Printf_format_string_ */
#endif

/* ---- Portability macros --------------------------------------------------
 *
 * TBLOG_API                 Export/import marker of the public functions.
 *   ELF/Mach-O (GCC, Clang): __attribute__((visibility("default"))). The
 *     library is built with -fvisibility=hidden, so every public tblog_*
 *     function MUST carry TBLOG_API (`make check-symbols` fails otherwise).
 *   Windows (MSVC, MinGW, clang-cl): __declspec(dllexport) while building
 *     the DLL (TBLOG_BUILDING_DLL), __declspec(dllimport) for a consumer of
 *     the DLL (TBLOG_DLL), nothing for the static library. CMake sets both
 *     (tblog::tblog_shared carries TBLOG_DLL in its usage requirements).
 *   May be predefined by the user to override all of the above.
 *
 * The macros below are internal (trailing underscore); do not use them.
 *
 * TBLOG_PRINTF_FMT_(f, a)   printf format checking of argument f against the
 *   arguments from a on: __attribute__((format(printf, f, a))) on GCC/Clang.
 *   MinGW GCC uses the gnu_printf archetype (C99 semantics: %zu, %lld,
 *   PRIu64 = "llu"); that is what MinGW's own __MINGW_PRINTF_FORMAT selects
 *   in C99/C++11 mode, spelled out because that macro only exists after
 *   <stdio.h>. Empty on MSVC (see TBLOG_FMT_STRING_).
 * TBLOG_FMT_STRING_         _Printf_format_string_ (SAL) on MSVC, so that
 *   /analyze checks the format; empty elsewhere.
 * TBLOG_NOINLINE_           Never inline: __attribute__((noinline)) or
 *   __declspec(noinline).
 * TBLOG_CONST_COND_BEGIN_ / TBLOG_CONST_COND_END_
 *   Bracket a macro statement whose condition is a compile-time constant on
 *   purpose (the TBLOG_COMPILE_LEVEL cut): silence MSVC C4127 for it only.
 *   Empty elsewhere.
 * TBLOG_SITE_LOAD_(dst, slot) / TBLOG_SITE_STORE_(slot, val)
 *   Acquire load / release store of a per-call-site `tblog_module *` cache
 *   (see TBLOG_SITE_). The hot path is a plain load on x86-64 and one LDAR
 *   on ARM64: no fence, no interlocked read-modify-write.
 * TBLOG_LOAD_RELAXED_U8_(p) Relaxed atomic load of an unsigned char (module
 *   level): a plain byte load on every target.
 * ------------------------------------------------------------------------- */

#ifndef TBLOG_API
#if defined(_WIN32)
#if defined(TBLOG_BUILDING_DLL)
#define TBLOG_API __declspec(dllexport)
#elif defined(TBLOG_DLL)
#define TBLOG_API __declspec(dllimport)
#else
#define TBLOG_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define TBLOG_API __attribute__((__visibility__("default")))
#else
#define TBLOG_API
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if defined(__MINGW32__) && !defined(__clang__)
#define TBLOG_PRINTF_FMT_(f, a) __attribute__((__format__(__gnu_printf__, f, a)))
#else
#define TBLOG_PRINTF_FMT_(f, a) __attribute__((__format__(__printf__, f, a)))
#endif
#define TBLOG_NOINLINE_ __attribute__((__noinline__))
#elif defined(_MSC_VER)
#define TBLOG_PRINTF_FMT_(f, a)
#define TBLOG_NOINLINE_ __declspec(noinline)
#else
#define TBLOG_PRINTF_FMT_(f, a)
#define TBLOG_NOINLINE_
#endif

#if defined(_MSC_VER)
#define TBLOG_FMT_STRING_ _Printf_format_string_
#else
#define TBLOG_FMT_STRING_
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define TBLOG_CONST_COND_BEGIN_ __pragma(warning(push)) __pragma(warning(disable : 4127))
#define TBLOG_CONST_COND_END_   __pragma(warning(pop))
#else
#define TBLOG_CONST_COND_BEGIN_
#define TBLOG_CONST_COND_END_
#endif

#if defined(__GNUC__) || defined(__clang__)
/* GCC, Clang, MinGW, clang-cl: the __atomic builtins (C and C++). */
#define TBLOG_LOAD_RELAXED_U8_(p)    __atomic_load_n((p), __ATOMIC_RELAXED)
#define TBLOG_SITE_LOAD_(dst, slot)  (dst) = __atomic_load_n((slot), __ATOMIC_ACQUIRE)
#define TBLOG_SITE_STORE_(slot, val) __atomic_store_n((slot), (val), __ATOMIC_RELEASE)
#elif defined(_MSC_VER)
/* MSVC (C and C++), same lowering as MSVC's own <atomic>:
 * - Relaxed byte load: __iso_volatile_load8 is a single plain load whatever
 *   /volatile: mode is used (it never gets the /volatile:ms acquire
 *   semantics), and it is atomic for an aligned byte on every target.
 * - x86/x64 (not ARM64EC): every ordinary load is an acquire and every
 *   ordinary store a release in the hardware memory model (TSO), so only the
 *   compiler must be kept from reordering: a volatile access plus
 *   _ReadWriteBarrier() (a compiler-only barrier, no instruction emitted)
 *   after the load / before the store. _ReadWriteBarrier is documented as
 *   deprecated in favour of <atomic>, which C cannot use; MSVC's <atomic>
 *   itself calls it with C4995/C4996 silenced, and so do we.
 * - ARM64 and ARM64EC (ARM64 code): LDAR / STLR, i.e. __ldar64 / __stlr64,
 *   which are exactly the acquire load / release store (no DMB). */
#define TBLOG_RWBARRIER_() \
    __pragma(warning(push)) __pragma(warning(disable : 4995 4996)) _ReadWriteBarrier() __pragma(warning(pop))
#define TBLOG_LOAD_RELAXED_U8_(p) ((unsigned char)__iso_volatile_load8((const volatile char *)(p)))
#if defined(_M_ARM64) || defined(_M_ARM64EC)
#define TBLOG_SITE_LOAD_(dst, slot) \
    (dst) = (tblog_module *)__ldar64((unsigned __int64 volatile *)(slot))
#define TBLOG_SITE_STORE_(slot, val) \
    __stlr64((unsigned __int64 volatile *)(slot), (unsigned __int64)(val))
#elif defined(_M_X64) || defined(_M_IX86)
#define TBLOG_SITE_LOAD_(dst, slot)                         \
    (dst) = *(tblog_module *const volatile *)(slot);        \
    TBLOG_RWBARRIER_()
#define TBLOG_SITE_STORE_(slot, val)                        \
    TBLOG_RWBARRIER_();                                     \
    *(tblog_module *volatile *)(slot) = (val)
#else
#error "tblog: unsupported MSVC target architecture (x86, x64, ARM64, ARM64EC are supported)"
#endif
#else
#error "tblog: unsupported compiler (GCC, Clang or MSVC is required)"
#endif

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
 * atomically (relaxed). Do not touch fields directly. */
typedef struct tblog_module {
    unsigned char level;      /* effective level (atomic access only)          */
    unsigned char explicit_;  /* 1 if level was set explicitly for this module */
    char name[46];            /* "" = root module                              */
} tblog_module;

/* Lifecycle. config_path may be NULL (defaults). Returns 0 on success.
 * Calling tblog_write before tblog_init writes to stderr. */
TBLOG_API int  tblog_init(const char *config_path);
TBLOG_API void tblog_shutdown(void);  /* drain, fsync, stop worker; idempotent */
TBLOG_API void tblog_flush(void);     /* ask the worker to flush now and wait for it */

/* Levels. module NULL or "" = root; setting root updates every module that
 * has no explicit level of its own. */
TBLOG_API void        tblog_set_level(const char *module, tblog_level lv);
TBLOG_API tblog_level tblog_get_level(const char *module);

/* Returns a stable pointer (never freed) for the module name; NULL/"" = root.
 * Names longer than 45 chars are truncated. Thread-safe. */
TBLOG_API tblog_module *tblog_module_get(const char *name);

static inline tblog_level tblog_module_level(const tblog_module *m) {
    return (tblog_level)TBLOG_LOAD_RELAXED_U8_(&m->level);
}

TBLOG_API void tblog_write(tblog_module *m, tblog_level lv, const char *file, int line,
                           const char *func, TBLOG_FMT_STRING_ const char *fmt, ...)
    TBLOG_PRINTF_FMT_(6, 7);

/* Number of lines dropped because the buffer was full. */
TBLOG_API uint64_t tblog_dropped_count(void);

#ifndef TBLOG_COMPILE_LEVEL
#define TBLOG_COMPILE_LEVEL TBLOG_LV_TRACE
#endif

/* Internal: the call-site logic shared by TBLOG_LOG and TBLOGF_LOG (tblog.hpp).
 * In order: compile-time level cut; the module of this call site, looked up
 * once and cached in a function-local static (acquire/release, so another
 * thread sees a fully initialised pointer; racing first calls both store the
 * same stable pointer); the runtime level check. EMIT -- an expression that
 * names the module `tblog_site_m_` and evaluates the user's arguments -- runs
 * only when the line is enabled. */
#define TBLOG_SITE_(lv, mod, EMIT)                                                \
    do {                                                                          \
        TBLOG_CONST_COND_BEGIN_                                                   \
        if ((lv) >= TBLOG_COMPILE_LEVEL) {                                        \
            static tblog_module *tblog_site_cache_ = 0;                           \
            tblog_module *tblog_site_m_;                                          \
            TBLOG_SITE_LOAD_(tblog_site_m_, &tblog_site_cache_);                  \
            if (!tblog_site_m_) {                                                 \
                tblog_site_m_ = tblog_module_get(mod);                            \
                TBLOG_SITE_STORE_(&tblog_site_cache_, tblog_site_m_);             \
            }                                                                     \
            if ((lv) >= tblog_module_level(tblog_site_m_))                        \
                EMIT;                                                             \
        }                                                                         \
        TBLOG_CONST_COND_END_                                                     \
    } while (0)

#define TBLOG_LOG(lv, mod, ...) \
    TBLOG_SITE_(lv, mod, tblog_write(tblog_site_m_, (lv), __FILE__, __LINE__, __func__, __VA_ARGS__))

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
