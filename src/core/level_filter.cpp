// Levels & module registry.
#include "core/levels.h"

#include "core/compat.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace tblog {
namespace {

constexpr size_t kMaxModules = 256;
constexpr size_t kMaxName = sizeof(((tblog_module *)nullptr)->name) - 1;  // 45

// Slot 0 is the root module (name ""). Statically allocated: pointers are
// stable for the lifetime of the process and never freed.
tblog_module g_modules[kMaxModules] = {{TBLOG_LV_INFO, 0, {0}}};
size_t g_count = 1;  // guarded by g_mu
std::mutex g_mu;

inline tblog_level clamp(tblog_level lv) {
    return ((unsigned)lv > (unsigned)TBLOG_LV_OFF) ? TBLOG_LV_OFF : lv;
}
inline void store_level(tblog_module *m, tblog_level lv) {
    relaxed_store_u8(&m->level, (unsigned char)lv);
}
inline tblog_level load_level(const tblog_module *m) {
    return (tblog_level)relaxed_load_u8(&m->level);
}
inline bool is_root(const char *name) { return name == nullptr || name[0] == '\0'; }

// Caller holds g_mu. Compares against the (possibly truncated) stored name.
tblog_module *find_locked(const char *name) {
    for (size_t i = 1; i < g_count; ++i)
        if (std::strncmp(g_modules[i].name, name, kMaxName) == 0) return &g_modules[i];
    return nullptr;
}

// Caller holds g_mu. Returns root on overflow.
tblog_module *get_or_create_locked(const char *name) {
    if (is_root(name)) return &g_modules[0];
    if (tblog_module *m = find_locked(name)) return m;
    if (g_count >= kMaxModules) return &g_modules[0];
    tblog_module *m = &g_modules[g_count];
    size_t n = std::strlen(name);
    if (n > kMaxName) n = kMaxName;
    std::memcpy(m->name, name, n);
    m->name[n] = '\0';
    m->explicit_ = 0;
    store_level(m, load_level(&g_modules[0]));
    ++g_count;
    return m;
}

}  // namespace

const char *level_name(tblog_level lv) {
    static const char *const kNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"};
    // Read the raw value: a caller may pass any integer (C enums are ints), and
    // loading an out-of-range enum is undefined (UBSan -fsanitize=enum).
    std::underlying_type<tblog_level>::type raw;
    std::memcpy(&raw, &lv, sizeof raw);
    const unsigned i = static_cast<unsigned>(raw);
    return i <= (unsigned)TBLOG_LV_OFF ? kNames[i] : "UNKNOWN";
}

bool parse_level(const char *s, tblog_level *out) {
    if (!s || !out) return false;
    static const struct { const char *name; tblog_level lv; } kTable[] = {
        {"trace", TBLOG_LV_TRACE}, {"debug", TBLOG_LV_DEBUG}, {"info", TBLOG_LV_INFO},
        {"warn", TBLOG_LV_WARN},   {"warning", TBLOG_LV_WARN}, {"error", TBLOG_LV_ERROR},
        {"fatal", TBLOG_LV_FATAL}, {"off", TBLOG_LV_OFF},
    };
    for (const auto &e : kTable) {
        const char *a = s, *b = e.name;
        while (*a && *b) {
            char c = *a;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != *b) break;
            ++a, ++b;
        }
        if (*a == '\0' && *b == '\0') {
            *out = e.lv;
            return true;
        }
    }
    return false;
}

void reset_levels_for_test() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (size_t i = 0; i < g_count; ++i) {
        g_modules[i].explicit_ = 0;
        store_level(&g_modules[i], TBLOG_LV_INFO);
    }
}

}  // namespace tblog

using namespace tblog;

extern "C" tblog_module *tblog_module_get(const char *name) {
    if (is_root(name)) return &g_modules[0];
    std::lock_guard<std::mutex> lk(g_mu);
    return get_or_create_locked(name);
}

extern "C" void tblog_set_level(const char *module, tblog_level lv) {
    lv = clamp(lv);
    std::lock_guard<std::mutex> lk(g_mu);
    if (is_root(module)) {
        store_level(&g_modules[0], lv);
        for (size_t i = 1; i < g_count; ++i)
            if (!g_modules[i].explicit_) store_level(&g_modules[i], lv);
        return;
    }
    tblog_module *m = get_or_create_locked(module);
    if (m == &g_modules[0]) return;  // registry full: do not clobber root
    m->explicit_ = 1;
    store_level(m, lv);
}

extern "C" tblog_level tblog_get_level(const char *module) {
    if (is_root(module)) return load_level(&g_modules[0]);
    std::lock_guard<std::mutex> lk(g_mu);
    tblog_module *m = find_locked(module);
    return load_level(m ? m : &g_modules[0]);
}
