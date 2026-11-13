/* tblog-lite bare-metal Cortex-M port. UNTESTED ON HARDWARE (host syntax
 * check only: the inline assembly is not assembled on the host). */
#include "tblog_lite_port_cortex_m.h"

#if TBLOG_LITE_CM_OUTPUT == 1
#include "SEGGER_RTT.h"
#endif

/* ---- critical section: PRIMASK save / disable / restore ---------------- */
static uint32_t cm_enter(void) {
    uint32_t primask;
    __asm__ __volatile__("mrs %0, primask" : "=r"(primask));
    __asm__ __volatile__("cpsid i" ::: "memory");
    return primask;
}

static void cm_exit(uint32_t primask) {
    __asm__ __volatile__("msr primask, %0" ::"r"(primask) : "memory");
}

/* ---- timestamp ----------------------------------------------------------- */
#if TBLOG_LITE_CM_TIMESTAMP == 1
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define DWT_LAR    (*(volatile uint32_t *)0xE0001FB0u) /* Cortex-M7 lock */

static uint32_t cm_ts(void) {
    return DWT_CYCCNT;
}

static void ts_init(void) {
    DEMCR |= 1u << 24; /* TRCENA */
    DWT_LAR = 0xC5ACCE55u;
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u; /* CYCCNTENA */
}
#elif defined(TBLOG_LITE_CM_GET_TICK)
static uint32_t cm_ts(void) {
    return (uint32_t)TBLOG_LITE_CM_GET_TICK();
}
static void ts_init(void) {
}
#else
static volatile uint32_t ms_;

void tblog_lite_cm_tick(void) {
    ms_++;
}
static uint32_t cm_ts(void) {
    return ms_;
}
static void ts_init(void) {
}
#endif

/* ---- output -------------------------------------------------------------- */
#if TBLOG_LITE_CM_OUTPUT == 1
static void cm_write(const char *data, size_t len) {
    SEGGER_RTT_Write(0, data, (unsigned)len);
}
#else
#define cm_write tblog_lite_cm_uart_write
#endif

static tblog_lite_port port_;

void tblog_lite_cm_init(void (*fatal)(void)) {
    ts_init();
    port_.write = cm_write;
    port_.timestamp = cm_ts;
    port_.enter_critical = cm_enter;
    port_.exit_critical = cm_exit;
    port_.fatal = fatal;
    tblog_lite_init(&port_);
}
