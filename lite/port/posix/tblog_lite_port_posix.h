/* tblog-lite host port for Linux/POSIX (used by the tests; also handy for
 * running MCU application code on a PC). Tested. */
#ifndef TBLOG_LITE_PORT_POSIX_H
#define TBLOG_LITE_PORT_POSIX_H

#include "tblog_lite.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Port writing to file descriptor `fd` (e.g. 1 = stdout), timestamps in ms
 * since the first call, critical section = all signals blocked + spinlock,
 * so log calls are safe from signal handlers (the "ISR" of a POSIX process)
 * and from several threads. */
const tblog_lite_port *tblog_lite_port_posix(int fd);

/* The building blocks, to assemble a custom port (e.g. a capturing write). */
uint32_t tblog_lite_posix_enter(void);
void     tblog_lite_posix_exit(uint32_t state);
uint32_t tblog_lite_posix_ms(void);

#ifdef __cplusplus
}
#endif

#endif
