#pragma once
#include <atomic>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace celosia::native {
// Own on the applet main-loop thread, and destroy before appletExit.
// appletMainLoop dispatches notifications; this observer never consumes messages.
class NativeLifecycle final {
public:
    NativeLifecycle()=default;
    ~NativeLifecycle();
    NativeLifecycle(const NativeLifecycle &)=delete;
    NativeLifecycle &operator=(const NativeLifecycle &)=delete;
    NativeLifecycle(NativeLifecycle &&)=delete;
    NativeLifecycle &operator=(NativeLifecycle &&)=delete;
    bool prepare() noexcept;
    bool exitRequested() const noexcept { return requested.load(std::memory_order_acquire); }
private:
#ifdef __SWITCH__
    static void onAppletHook(AppletHookType type,void *user) noexcept;
    AppletHookCookie cookie{};
#endif
    std::atomic<bool> requested{false};
    bool hooked=false;
};
}
