// C++11 compatibility helpers shared by the library. The library builds as
// C++11, 14 and 17 (C++14 and 17 with MSVC, which has no C++11 mode), with
// GCC, Clang, MinGW-w64 and MSVC.
#pragma once

#include "platform/platform.h"

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>  // __iso_volatile_load8 / __iso_volatile_store8 (compiler intrinsics)
#endif

// The language level. MSVC keeps __cplusplus at 199711L unless /Zc:__cplusplus
// is given (CMakeLists.txt passes it), but always reports it in _MSVC_LANG.
#if defined(_MSVC_LANG) && _MSVC_LANG > __cplusplus
#define TBLOG_CPP_LEVEL _MSVC_LANG
#else
#define TBLOG_CPP_LEVEL __cplusplus
#endif

#if TBLOG_CPP_LEVEL < 201103L
#error "tblog needs C++11 or newer to build (its public header log.h is plain C)"
#endif

// Branch hint for the rare side of a hot-path test.
#if defined(__GNUC__) || defined(__clang__)
#define TBLOG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define TBLOG_UNLIKELY(x) (x)
#endif

namespace tblog {

// std::make_unique is C++14.
template <class T, class... Args>
std::unique_ptr<T> make_unique(Args &&...args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// std::atomic<T>::is_always_lock_free is C++17; the GCC/Clang builtin gives
// the same compile-time answer on C++11/14. Signal handlers rely on it. MSVC
// in C++14 mode: every trivially copyable type of at most 8 bytes is
// lock-free on the targets it supports (x86, x64, ARM64).
#if TBLOG_CPP_LEVEL >= 201703L
#define TBLOG_ASSERT_LOCK_FREE(T) \
    static_assert(std::atomic<T>::is_always_lock_free, "std::atomic<" #T "> must be lock-free")
#elif defined(__GNUC__) || defined(__clang__)
#define TBLOG_ASSERT_LOCK_FREE(T) \
    static_assert(__atomic_always_lock_free(sizeof(T), 0), "std::atomic<" #T "> must be lock-free")
#else
#define TBLOG_ASSERT_LOCK_FREE(T) \
    static_assert(sizeof(T) <= 8, "std::atomic<" #T "> must be lock-free")
#endif

// Relaxed atomic byte access to a plain `unsigned char` (tblog_module::level,
// a C struct that cannot hold a std::atomic). Same lowering as
// TBLOG_LOAD_RELAXED_U8_ in <tblog/log.h>.
inline unsigned char relaxed_load_u8(const unsigned char *p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#else
    return static_cast<unsigned char>(__iso_volatile_load8(reinterpret_cast<const volatile char *>(p)));
#endif
}
inline void relaxed_store_u8(unsigned char *p, unsigned char v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
#else
    __iso_volatile_store8(reinterpret_cast<volatile char *>(p), static_cast<char>(v));
#endif
}

// Before C++17, `new` ignores alignas() above 16 bytes, so cache-line-aligned
// types (alignas(64) to avoid false sharing) could land misaligned. Types that
// use alignas(64) inherit this to get 64-byte-aligned heap allocations on every
// standard. Works through a base-class pointer as long as the destructor is
// virtual (delete then looks up operator delete in the dynamic type).
struct CacheAlignedNew {
    static void *operator new(std::size_t n) { return alloc(n); }
    static void *operator new[](std::size_t n) { return alloc(n); }
    static void operator delete(void *p) noexcept { pf::free_aligned(p); }
    static void operator delete[](void *p) noexcept { pf::free_aligned(p); }

private:
    static void *alloc(std::size_t n) {
        void *p = pf::alloc_aligned(64, n ? n : 1);
        if (!p) throw std::bad_alloc();
        return p;
    }
};

}  // namespace tblog
