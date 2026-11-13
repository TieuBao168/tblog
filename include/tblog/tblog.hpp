// tblog — header-only C++11 API with "{}" formatting on top of the C API.
//
//   TBLOGF_INFO("user {} logged in from {}", id, ip);
//   TBLOGF_MWARN("network", "retry {}/{} after {:.1f}s", n, max, delay);
//
//   tblog::Logger net("network");
//   net.info("connected to {}:{}", host, port);      // call site captured
//   net.info(TBLOG_HERE, "explicit location {}", x);  // same, explicit
//
// Formatting happens in a fixed stack buffer (TBLOGF_BUF_SIZE, default 1024)
// and the finished text is handed to tblog_write() as "%.*s".
#ifndef TBLOG_TBLOG_HPP
#define TBLOG_TBLOG_HPP

#include "tblog/log.h"
#include "tblog/detail/format.hpp"

#ifndef TBLOGF_BUF_SIZE
#define TBLOGF_BUF_SIZE 1024
#endif

namespace tblog {

struct SourceLoc {
    const char *file;
    int line;
    const char *func;
};

#define TBLOG_HERE (::tblog::SourceLoc{__FILE__, __LINE__, __func__})

namespace detail {

static_assert(TBLOGF_BUF_SIZE >= 16, "TBLOGF_BUF_SIZE too small");

// Out of line on purpose: keeps the 1 KiB buffer and the formatting loop out
// of the caller's frame/code.
__attribute__((noinline)) inline void vlog(tblog_module *m, tblog_level lv, const char *file, int line,
                                           const char *func, const char *fmt, const Arg *args, std::size_t n) {
    char buf[TBLOGF_BUF_SIZE];
    Sink s(buf, sizeof buf);
    vformat(s, fmt, args, n);
    std::size_t len = s.size();
    if (s.truncated()) {
        std::memcpy(buf + sizeof buf - 3, "...", 3);
        len = sizeof buf;
    }
    tblog_write(m, lv, file, line, func, "%.*s", static_cast<int>(len), buf);
}

template <class... A>
inline void write_fmt(tblog_module *m, tblog_level lv, const char *file, int line, const char *func,
                      const char *fmt, const A &...args) {
    const Arg arr[sizeof...(A) + 1] = {make_arg(args)..., Arg()};
    vlog(m, lv, file, line, func, fmt, arr, sizeof...(A));
}

}  // namespace detail

// A format string that remembers where it was written. Converting a string
// literal to FmtLoc evaluates the default arguments at the caller's location
// (GCC >= 4.8 / Clang >= 9 builtins), so Logger::info("...") keeps file/line.
#if defined(__clang__)
#if defined(__has_builtin)
#if __has_builtin(__builtin_FILE) && __has_builtin(__builtin_LINE) && __has_builtin(__builtin_FUNCTION)
#define TBLOGF_HAS_BUILTIN_LOC 1
#endif
#endif
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
#define TBLOGF_HAS_BUILTIN_LOC 1
#endif
struct FmtLoc {
#ifdef TBLOGF_HAS_BUILTIN_LOC
    FmtLoc(const char *f, const char *file_ = __builtin_FILE(), int line_ = __builtin_LINE(),
           const char *func_ = __builtin_FUNCTION())
        : fmt(f), file(file_), line(line_), func(func_) {}
#else
    FmtLoc(const char *f) : fmt(f), file("?"), line(0), func("?") {}
#endif
    const char *fmt;
    const char *file;
    int line;
    const char *func;
};

// Object-style logger bound to one module. Cheap to copy (one pointer).
// Note: unlike the macros, arguments are evaluated before the level check
// (formatting is still skipped). Guard expensive arguments with enabled().
class Logger {
public:
    Logger() : m_(tblog_module_get(nullptr)) {}
    explicit Logger(const char *module) : m_(tblog_module_get(module)) {}

    tblog_module *module() const { return m_; }
    const char *name() const { return m_->name; }
    bool enabled(tblog_level lv) const { return lv >= TBLOG_COMPILE_LEVEL && lv >= tblog_module_level(m_); }

    template <class... A> void log(tblog_level lv, FmtLoc f, const A &...args) const {
        if (enabled(lv)) detail::write_fmt(m_, lv, f.file, f.line, f.func, f.fmt, args...);
    }
    template <class... A> void log(tblog_level lv, const SourceLoc &w, const char *fmt, const A &...args) const {
        if (enabled(lv)) detail::write_fmt(m_, lv, w.file, w.line, w.func, fmt, args...);
    }

#define TBLOGF_LOGGER_METHOD(name, LV)                                                          \
    template <class... A> void name(FmtLoc f, const A &...args) const {                         \
        if (enabled(LV)) detail::write_fmt(m_, LV, f.file, f.line, f.func, f.fmt, args...);     \
    }                                                                                           \
    template <class... A> void name(const SourceLoc &w, const char *fmt, const A &...args) const { \
        if (enabled(LV)) detail::write_fmt(m_, LV, w.file, w.line, w.func, fmt, args...);       \
    }
    TBLOGF_LOGGER_METHOD(trace, TBLOG_LV_TRACE)
    TBLOGF_LOGGER_METHOD(debug, TBLOG_LV_DEBUG)
    TBLOGF_LOGGER_METHOD(info, TBLOG_LV_INFO)
    TBLOGF_LOGGER_METHOD(warn, TBLOG_LV_WARN)
    TBLOGF_LOGGER_METHOD(error, TBLOG_LV_ERROR)
    TBLOGF_LOGGER_METHOD(fatal, TBLOG_LV_FATAL)
#undef TBLOGF_LOGGER_METHOD

private:
    tblog_module *m_;
};

}  // namespace tblog

// Same pattern as TBLOG_LOG: compile-time cut, per-call-site cached module,
// runtime level check -- all BEFORE the arguments are evaluated or formatted.
#define TBLOGF_LOG(lv, mod, ...)                                                          \
    do {                                                                                  \
        if ((lv) >= TBLOG_COMPILE_LEVEL) {                                                \
            static tblog_module *tblogf_m_cache_ = 0;                                     \
            tblog_module *tblogf_m_ = __atomic_load_n(&tblogf_m_cache_, __ATOMIC_ACQUIRE); \
            if (!tblogf_m_) {                                                             \
                tblogf_m_ = tblog_module_get(mod);                                        \
                __atomic_store_n(&tblogf_m_cache_, tblogf_m_, __ATOMIC_RELEASE);          \
            }                                                                             \
            if ((lv) >= tblog_module_level(tblogf_m_))                                    \
                ::tblog::detail::write_fmt(tblogf_m_, (lv), __FILE__, __LINE__, __func__, \
                                           __VA_ARGS__);                                  \
        }                                                                                 \
    } while (0)

#define TBLOGF_TRACE(...) TBLOGF_LOG(TBLOG_LV_TRACE, 0, __VA_ARGS__)
#define TBLOGF_DEBUG(...) TBLOGF_LOG(TBLOG_LV_DEBUG, 0, __VA_ARGS__)
#define TBLOGF_INFO(...)  TBLOGF_LOG(TBLOG_LV_INFO,  0, __VA_ARGS__)
#define TBLOGF_WARN(...)  TBLOGF_LOG(TBLOG_LV_WARN,  0, __VA_ARGS__)
#define TBLOGF_ERROR(...) TBLOGF_LOG(TBLOG_LV_ERROR, 0, __VA_ARGS__)
#define TBLOGF_FATAL(...) TBLOGF_LOG(TBLOG_LV_FATAL, 0, __VA_ARGS__)

#define TBLOGF_MTRACE(mod, ...) TBLOGF_LOG(TBLOG_LV_TRACE, mod, __VA_ARGS__)
#define TBLOGF_MDEBUG(mod, ...) TBLOGF_LOG(TBLOG_LV_DEBUG, mod, __VA_ARGS__)
#define TBLOGF_MINFO(mod, ...)  TBLOGF_LOG(TBLOG_LV_INFO,  mod, __VA_ARGS__)
#define TBLOGF_MWARN(mod, ...)  TBLOGF_LOG(TBLOG_LV_WARN,  mod, __VA_ARGS__)
#define TBLOGF_MERROR(mod, ...) TBLOGF_LOG(TBLOG_LV_ERROR, mod, __VA_ARGS__)
#define TBLOGF_MFATAL(mod, ...) TBLOGF_LOG(TBLOG_LV_FATAL, mod, __VA_ARGS__)

// Logger variant that keeps the "arguments not evaluated when filtered"
// guarantee: TBLOGF_LOGGER(net, TBLOG_LV_DEBUG, "dump {}", expensive());
#define TBLOGF_LOGGER(lg, lv, ...)                                                        \
    do {                                                                                  \
        if ((lv) >= TBLOG_COMPILE_LEVEL && (lg).enabled(lv))                              \
            ::tblog::detail::write_fmt((lg).module(), (lv), __FILE__, __LINE__, __func__, \
                                       __VA_ARGS__);                                      \
    } while (0)

#endif  // TBLOG_TBLOG_HPP
