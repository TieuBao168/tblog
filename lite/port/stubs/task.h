/* Minimal stand-in for FreeRTOS task.h (host syntax check only). */
#ifndef STUB_TASK_H
#define STUB_TASK_H
#include "FreeRTOS.h"

void vPortEnterCritical(void);
void vPortExitCritical(void);
UBaseType_t ulPortRaiseBASEPRI(void);
void vPortSetBASEPRI(UBaseType_t);

#define taskENTER_CRITICAL() vPortEnterCritical()
#define taskEXIT_CRITICAL() vPortExitCritical()
#define taskENTER_CRITICAL_FROM_ISR() ulPortRaiseBASEPRI()
#define taskEXIT_CRITICAL_FROM_ISR(x) vPortSetBASEPRI(x)

TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
void vTaskDelay(TickType_t);
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint16_t, void *, UBaseType_t, TaskHandle_t *);
#endif
