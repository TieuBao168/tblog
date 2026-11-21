/* Compile-only check of the public C header: every macro and every function
 * of <tblog/log.h> is used once, so the strict warning flags of each compiler
 * (GCC/Clang -Wall -Wextra -Wpedantic, MSVC /W4) see all of it. Nothing here
 * is ever linked or run. Built as C99 (C11 on MSVC), also with TBLOG_DLL. */
#include <tblog/log.h>

#include <stddef.h>
#include <inttypes.h>

int tblog_header_check_c(const char *conf, size_t n);

static void check_sink_write(void *ctx, const char *data, size_t len) {
    size_t *total = (size_t *)ctx;
    *total += len;
    (void)data;
}

static void check_sink_flush(void *ctx) { (void)ctx; }

static size_t g_check_sink_bytes;

int tblog_header_check_c(const char *conf, size_t n) {
    tblog_module *net;
    tblog_level lv;
    uint64_t dropped;
    long long big = -1;
    tblog_sink_write_fn wfn = check_sink_write;
    tblog_sink_flush_fn ffn = check_sink_flush;

    if (tblog_add_sink(wfn, ffn, &g_check_sink_bytes) != 0) return 3;
    if (tblog_add_sink(check_sink_write, NULL, &g_check_sink_bytes) != 0) return 4;
    if (tblog_init(conf) != 0) return 1;
    tblog_set_level(NULL, TBLOG_LV_INFO);
    tblog_set_level("net", TBLOG_LV_DEBUG);
    lv = tblog_get_level("net");
    net = tblog_module_get("net");
    if (tblog_module_level(net) > lv) return 2;

    TBLOG_TRACE("trace %d", 1);
    TBLOG_DEBUG("debug %s", "x");
    TBLOG_INFO("info %zu", n);
    TBLOG_WARN("warn %lld", big);
    TBLOG_ERROR("error %c %u", 'c', 7u);
    TBLOG_FATAL("fatal, no arguments");

    TBLOG_MTRACE("net", "trace %d", 1);
    TBLOG_MDEBUG("net", "debug %p", (void *)net);
    TBLOG_MINFO("net", "info %.3f", 1.5);
    TBLOG_MWARN("net", "warn %5.2s|", "abc");
    TBLOG_MERROR("net", "error %x", 255u);
    TBLOG_MFATAL("net", "fatal, no arguments");

    /* Usable as the body of an unbraced if/else. */
    if (n)
        TBLOG_INFO("then %d", 1);
    else
        TBLOG_MINFO("net", "else %d", 2);

    TBLOG_LOG(TBLOG_LV_WARN, "net", "explicit level %d", 3);
    tblog_write(net, TBLOG_LV_ERROR, __FILE__, __LINE__, __func__, "direct %s", "write");

    tblog_flush();
    dropped = tblog_dropped_count();
    TBLOG_INFO("dropped %" PRIu64, dropped);
    tblog_shutdown();
    return 0;
}
