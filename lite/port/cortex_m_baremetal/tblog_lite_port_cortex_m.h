/* tblog-lite port for bare-metal Cortex-M (no RTOS).
 *
 * UNTESTED ON HARDWARE: only syntax-checked on the host against stub headers
 * (lite/port/stubs). Review before use.
 *
 * Critical section: PRIMASK save / "cpsid i" / restore, so log calls are safe
 * from any interrupt handler except NMI and HardFault (which PRIMASK does not
 * mask: do not log there, call tblog_lite_flush() at most).
 *
 * Timestamp (TBLOG_LITE_CM_TIMESTAMP):
 *   0 = millisecond counter: call tblog_lite_cm_tick() from SysTick_Handler
 *       (or define TBLOG_LITE_CM_GET_TICK() to an existing counter, e.g.
 *       HAL_GetTick()).
 *   1 = DWT->CYCCNT cycle counter (Cortex-M3/M4/M7/M33; not M0/M0+).
 *
 * Output (TBLOG_LITE_CM_OUTPUT):
 *   0 = you implement   void tblog_lite_cm_uart_write(const char *d, size_t n);
 *       e.g. polling:   while (n--) { while (!(USART2->ISR & USART_ISR_TXE)) {} USART2->TDR = *d++; }
 *   1 = SEGGER RTT channel 0 (needs SEGGER_RTT.c in the build).
 *
 * Usage:
 *     tblog_lite_cm_init(NULL);
 *     for (;;) { do_work(); tblog_lite_process(); __WFI(); }
 */
#ifndef TBLOG_LITE_PORT_CORTEX_M_H
#define TBLOG_LITE_PORT_CORTEX_M_H

#include "tblog_lite.h"

#ifndef TBLOG_LITE_CM_TIMESTAMP
#define TBLOG_LITE_CM_TIMESTAMP 0
#endif
#ifndef TBLOG_LITE_CM_OUTPUT
#define TBLOG_LITE_CM_OUTPUT 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Enables DWT if selected and installs the port. fatal: called after a FATAL
 * line was flushed (e.g. NVIC_SystemReset); NULL = return to the caller. */
void tblog_lite_cm_init(void (*fatal)(void));

#if TBLOG_LITE_CM_TIMESTAMP == 0 && !defined(TBLOG_LITE_CM_GET_TICK)
void tblog_lite_cm_tick(void); /* from SysTick_Handler, 1 kHz */
#endif

#if TBLOG_LITE_CM_OUTPUT == 0
void tblog_lite_cm_uart_write(const char *data, size_t len); /* provided by you */
#endif

#ifdef __cplusplus
}
#endif

#endif
