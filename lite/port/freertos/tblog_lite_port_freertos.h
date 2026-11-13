/* tblog-lite port for FreeRTOS (Cortex-M ports with BASEPRI critical sections).
 *
 * UNTESTED ON HARDWARE: only syntax-checked on the host against stub headers
 * (lite/port/stubs). Review before use.
 *
 * Usage:
 *     static void uart_write(const char *d, size_t n) { HAL_UART_Transmit(&huart2, (uint8_t *)d, n, 100); }
 *     tblog_lite_freertos_init(uart_write, NULL);       // before or after vTaskStartScheduler()
 *     tblog_lite_freertos_start_task(tskIDLE_PRIORITY + 1, 256);   // option A: drain task
 *     void vApplicationIdleHook(void) { tblog_lite_freertos_idle_hook(); }  // option B
 *
 * ISR rule: only ISRs whose priority is numerically >= (i.e. not more urgent
 * than) configMAX_SYSCALL_INTERRUPT_PRIORITY may log; more urgent ones are not
 * masked by the FreeRTOS critical section.
 */
#ifndef TBLOG_LITE_PORT_FREERTOS_H
#define TBLOG_LITE_PORT_FREERTOS_H

#include "tblog_lite.h"

/* Drain period of the optional task. */
#ifndef TBLOG_LITE_FREERTOS_PERIOD_MS
#define TBLOG_LITE_FREERTOS_PERIOD_MS 10
#endif

/* How to know we are in an ISR. xPortIsInsideInterrupt() exists in the
 * ARM_CM0/CM3/CM4F/CM7/CM33 ports since FreeRTOS 10.2. */
#ifndef TBLOG_LITE_FREERTOS_IN_ISR
#define TBLOG_LITE_FREERTOS_IN_ISR() (xPortIsInsideInterrupt() != pdFALSE)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the port: write = your (possibly blocking) output, fatal = called
 * after a FATAL line (NULL = return). */
void tblog_lite_freertos_init(void (*write)(const char *data, size_t len), void (*fatal)(void));

/* Call from vApplicationIdleHook(): drains whatever is queued. The write
 * callback must not block on a FreeRTOS object in the idle task. */
void tblog_lite_freertos_idle_hook(void);

/* Creates a task that drains every TBLOG_LITE_FREERTOS_PERIOD_MS.
 * Returns pdPASS on success. */
long tblog_lite_freertos_start_task(unsigned long priority, unsigned short stack_words);

#ifdef __cplusplus
}
#endif

#endif
