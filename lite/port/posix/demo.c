/* Host demo: the application code below is the same as with full tblog,
 * only the include and the init call differ. `make -C lite demo`. */
#include "tblog_lite.h"
#include "tblog_lite_port_posix.h"

static void app(void) {
    int i;
    TBLOG_INFO("boot, %d sensors", 3);
    for (i = 0; i < 3; i++) TBLOG_MDEBUG("adc", "ch%d raw=%u", i, 1000u + (unsigned)i);
    TBLOG_MWARN("net", "link down, retry in %d ms", 500);
    TBLOG_ERROR("crc mismatch: got 0x%08x want 0x%08x", 0xdeadbeefu, 0xcafef00du);
}

int main(void) {
    tblog_lite_init(tblog_lite_port_posix(1)); /* full tblog: tblog_init(NULL) */
    tblog_set_level(NULL, TBLOG_LV_DEBUG);     /* compat name, root level */
    app();
    tblog_lite_process(); /* in firmware: from the idle task / main loop */
    tblog_flush();        /* full tblog: tblog_shutdown() at exit */
    return 0;
}
