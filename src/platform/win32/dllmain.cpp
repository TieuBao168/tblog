// DLL lifecycle (see pf::process_exiting in platform/platform.h).
//
// tblog_init registers tblog_shutdown with atexit(). In tblog.dll that
// handler runs from DLL_PROCESS_DETACH, i.e. under the loader lock, after
// DllMain below: on process exit every other thread is already gone (maybe
// killed while holding a lock), on FreeLibrary they still run but cannot be
// joined (a thread exit needs the loader lock). Either way tblog_shutdown must
// not join or wait: g_dll_detaching tells it so.
#include "platform/win32/win32.h"

#include <atomic>

namespace tblog {
namespace pf {
namespace win {
std::atomic<bool> g_dll_detaching{false};
}  // namespace win
}  // namespace pf
}  // namespace tblog

#if defined(TBLOG_BUILDING_DLL)
// Both CRTs (MSVC's _DllMainCRTStartup, MinGW-w64's DllMainCRTStartup) call
// DllMain before running the DLL's atexit table on DLL_PROCESS_DETACH.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH)
        tblog::pf::win::g_dll_detaching.store(true, std::memory_order_release);
    return TRUE;
}
#endif
