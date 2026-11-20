// C++11 compatibility helpers shared by the library. The library builds as
// C++11, 14 and 17.
#pragma once

#include "platform/platform.h"

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

#if __cplusplus < 201103L
#error "tblog needs C++11 or newer to build (its public header log.h is plain C)"
#endif

namespace tblog {

// std::make_unique is C++14.
template <class T, class... Args>
std::unique_ptr<T> make_unique(Args &&...args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// std::atomic<T>::is_always_lock_free is C++17; the GCC/Clang builtin gives
// the same compile-time answer on C++11/14. Signal handlers rely on it.
#if __cplusplus >= 201703L
#define TBLOG_ASSERT_LOCK_FREE(T) \
    static_assert(std::atomic<T>::is_always_lock_free, "std::atomic<" #T "> must be lock-free")
#else
#define TBLOG_ASSERT_LOCK_FREE(T) \
    static_assert(__atomic_always_lock_free(sizeof(T), 0), "std::atomic<" #T "> must be lock-free")
#endif

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
