#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
namespace celosia::native {
enum class AudioFailure : uint32_t { None,InvalidSource,DeviceBusy,Initialize,Format,Allocate,Source,
    DeviceStart,Submit,ThreadCreate,ThreadStart,Wait,ReleasedBuffer,DeviceStop,ThreadClose };
struct AudioOutputStats {
    bool running;
    AudioFailure failure;
    uint32_t result;
    uint64_t submittedFrames,completedBuffers;
};
// Control/start/stop/destruction belong to one owner thread, never the callback.
// Source runs on a native libnx thread and must outlive stop's join. It produces
// interleaved signed16 stereo, 48kHz; there is no resampling/device emulation.
// Source must return promptly; stop/join output before destroying its producer.
class NativeAudioOutput final {
public:
    using Source=std::function<size_t(int16_t *,size_t)>;
    NativeAudioOutput();
    ~NativeAudioOutput();
    bool start(Source source);
    void stop() noexcept;
    AudioOutputStats stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
