#pragma once
#include <cstdint>
#include <memory>
#include <functional>

namespace native_renderer::detail { class Backend; }
namespace celosia::native {
// Direct deko3d display for the native game frontend. Caller initializes ROMFS
// first and destroys this owner before romfsExit/applet teardown. No GS/GL path.
// GPU creation, operations and destruction run on one actual native worker.
// Synchronous submissions serialize callers; caller keeps referenced data alive
// and its game execution gate locked until the submission completes.
class NativeDisplay final {
public:
    static constexpr uint32_t Width=1280,Height=720;
    NativeDisplay();
    ~NativeDisplay();
    NativeDisplay(const NativeDisplay &)=delete;
    NativeDisplay &operator=(const NativeDisplay &)=delete;
    // Creates real GPU resources and presents one empty frame. Repeated calls
    // preserve resources and do not clear an existing game frame.
    void initialize();
    bool ready() const;
    void beginFrame();
    void endFrame();
    void synchronize();
    // Batch uploads/draws per frame in this callback to avoid a worker round
    // trip per draw. The backend reference must not escape the callback.
    void submit(std::function<void(native_renderer::detail::Backend &)> operations);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
