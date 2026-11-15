#include "native_lifecycle.hpp"

namespace celosia::native {
static_assert(std::atomic<bool>::is_always_lock_free);
NativeLifecycle::~NativeLifecycle()
{
#ifdef __SWITCH__
    if(hooked) appletUnhook(&cookie);
#endif
}
bool NativeLifecycle::prepare() noexcept
{
#ifdef __SWITCH__
    if(!hooked) {
        appletHook(&cookie,onAppletHook,this);
        hooked=true;
    }
#endif
    return hooked;
}
#ifdef __SWITCH__
void NativeLifecycle::onAppletHook(AppletHookType type,void *user) noexcept
{
    if(type==AppletHookType_OnExitRequest)
        static_cast<NativeLifecycle *>(user)->requested.store(true,std::memory_order_release);
}
#endif
}
