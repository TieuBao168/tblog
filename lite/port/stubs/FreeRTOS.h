/* Minimal stand-in for FreeRTOS.h: lets the host compiler syntax-check
 * port/freertos (gcc -fsyntax-only). NOT a FreeRTOS implementation. */
#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H
#include <stddef.h>
#include <stdint.h>

typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)
#define pdPASS pdTRUE
#define configTICK_RATE_HZ 1000u
#define pdMS_TO_TICKS(ms) ((TickType_t)(((TickType_t)(ms) * (TickType_t)configTICK_RATE_HZ) / (TickType_t)1000u))

BaseType_t xPortIsInsideInterrupt(void);
#endif
