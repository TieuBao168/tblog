/* tblog-lite port for FreeRTOS. UNTESTED ON HARDWARE (host syntax check only). */
#include "FreeRTOS.h"
#include "task.h"

#include "tblog_lite_port_freertos.h"

#define TASK_CONTEXT 0x80000000u /* marks a state returned by the task path */

static uint32_t rtos_enter(void) {
    if (TBLOG_LITE_FREERTOS_IN_ISR()) return (uint32_t)taskENTER_CRITICAL_FROM_ISR();
    taskENTER_CRITICAL();
    return TASK_CONTEXT;
}

static void rtos_exit(uint32_t state) {
    if (state == TASK_CONTEXT) taskEXIT_CRITICAL();
    else taskEXIT_CRITICAL_FROM_ISR((UBaseType_t)state);
}

static uint32_t rtos_ticks(void) {
    return (uint32_t)(TBLOG_LITE_FREERTOS_IN_ISR() ? xTaskGetTickCountFromISR() : xTaskGetTickCount());
}

static tblog_lite_port port_;

void tblog_lite_freertos_init(void (*write)(const char *data, size_t len), void (*fatal)(void)) {
    port_.write = write;
    port_.timestamp = rtos_ticks;
    port_.enter_critical = rtos_enter;
    port_.exit_critical = rtos_exit;
    port_.fatal = fatal;
    tblog_lite_init(&port_);
}

void tblog_lite_freertos_idle_hook(void) {
    (void)tblog_lite_process();
}

static void drain_task(void *arg) {
    (void)arg;
    for (;;) {
        (void)tblog_lite_process();
        vTaskDelay(pdMS_TO_TICKS(TBLOG_LITE_FREERTOS_PERIOD_MS));
    }
}

long tblog_lite_freertos_start_task(unsigned long priority, unsigned short stack_words) {
    return (long)xTaskCreate(drain_task, "tblog", stack_words, NULL, (UBaseType_t)priority, NULL);
}
