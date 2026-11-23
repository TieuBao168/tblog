/* tblog-lite compile-time configuration.
 *
 * Every value here is a default: override it with -DNAME=value on the compiler
 * command line, or create your own "tblog_lite_user_config.h" and build with
 * -DTBLOG_LITE_USER_CONFIG to have it included first. The library sources and
 * the application MUST be compiled with the same values.
 */
#ifndef TBLOG_LITE_CONFIG_H
#define TBLOG_LITE_CONFIG_H

#ifdef TBLOG_LITE_USER_CONFIG
#include "tblog_lite_user_config.h"
#endif

/* Size in bytes of the static ring buffer holding formatted lines (RAM).
 * One byte is always kept free, so capacity is BUF_SIZE - 1. */
#ifndef TBLOG_LITE_BUF_SIZE
#define TBLOG_LITE_BUF_SIZE 2048
#endif

/* Longest line (header + message + '\n'); longer lines are truncated and still
 * end in '\n'. This is the stack buffer used by every log call. */
#ifndef TBLOG_LITE_LINE_MAX
#define TBLOG_LITE_LINE_MAX 128
#endif

/* 1 = support %f %F %e %E %g %G (pulls in double arithmetic). */
#ifndef TBLOG_LITE_FLOAT
#define TBLOG_LITE_FLOAT 0
#endif

/* 1 = support the ll length modifier with full 64-bit values. 0 = ll still
 * consumes a 64-bit argument but prints only its low 32 bits; this avoids
 * linking the 64-bit division helper (__aeabi_uldivmod) on 32-bit MCUs. */
#ifndef TBLOG_LITE_LONG_LONG
#define TBLOG_LITE_LONG_LONG 1
#endif

/* 1 = put "file:line " in every line header. 0 = drop it: no __FILE__ strings
 * in flash and smaller call sites. */
#ifndef TBLOG_LITE_FILELINE
#define TBLOG_LITE_FILELINE 1
#endif

/* What the macros pass as the file name when TBLOG_LITE_FILELINE=1. GCC >= 12
 * and clang offer __FILE_NAME__ (basename only, saves flash); otherwise use
 * -fmacro-prefix-map. The header prints only the part after the last '/'. */
#ifndef TBLOG_LITE_FILE
#define TBLOG_LITE_FILE __FILE__
#endif

/* 1 = put the module name in the header of TBLOG_Mxxx lines. 0 = ignore it
 * (module strings are then not referenced and the linker can drop them). */
#ifndef TBLOG_LITE_MODULE
#define TBLOG_LITE_MODULE 1
#endif

/* 1 = escape control characters in the message, as full tblog's
 * message.escape: '\n' -> "\\n", '\r' -> "\\r", other 0x00-0x1F and 0x7F ->
 * "\\xHH" (tab kept), trailing newlines dropped. Data logged with %s can then
 * not start a forged line on the UART / RTT stream. 0 = written as is. */
#ifndef TBLOG_LITE_ESCAPE
#define TBLOG_LITE_ESCAPE 1
#endif

/* Runtime root level after reset (same default as full tblog). */
#ifndef TBLOG_LITE_DEFAULT_LEVEL
#define TBLOG_LITE_DEFAULT_LEVEL TBLOG_LV_INFO
#endif

/* 1 = also define the full-tblog function names (tblog_flush,
 * tblog_set_level, tblog_get_level, tblog_dropped_count, tblog_shutdown) as
 * macros onto the lite API, so more application code compiles unchanged. */
#ifndef TBLOG_LITE_COMPAT
#define TBLOG_LITE_COMPAT 1
#endif

/* Minimum level compiled in at all; calls below it vanish (format strings and
 * arguments included). Same macro as full tblog. Accepts TBLOG_LV_xxx names. */
#ifndef TBLOG_COMPILE_LEVEL
#define TBLOG_COMPILE_LEVEL TBLOG_LV_TRACE
#endif

#endif /* TBLOG_LITE_CONFIG_H */
