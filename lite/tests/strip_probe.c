/* Compiled by tests/check_strip.sh with -DTBLOG_COMPILE_LEVEL=TBLOG_LV_WARN:
 * everything below WARN must vanish from the object (format strings, module
 * names, and the functions called only in the arguments). */
#include "tblog_lite.h"

extern int stripped_helper_fn(void);
extern int kept_helper_fn(void);

void probe(void);
void probe(void) {
    TBLOG_TRACE("STRIPPED_TRACE_MARKER %d", stripped_helper_fn());
    TBLOG_DEBUG("STRIPPED_DEBUG_MARKER %d", stripped_helper_fn());
    TBLOG_MINFO("STRIPPED_MODULE_MARKER", "STRIPPED_INFO_MARKER %d", stripped_helper_fn());
    TBLOG_WARN("KEPT_WARN_MARKER %d", kept_helper_fn());
    TBLOG_MERROR("KEPT_MODULE_MARKER", "KEPT_ERROR_MARKER");
}
