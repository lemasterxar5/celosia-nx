#pragma once
#include "ps2x/iop/iop_subsystem.h"
#include <functional>
#include <memory>
#include <span>

namespace celosia::native {
class DataMemory;
class DataHeap;
struct NativeFileAccess {
    std::function<std::string(ps2x::iop::HostPathKind)> rootPath;
    std::function<std::string(std::string_view)> translate;
    std::function<uint64_t(std::string_view)> open;
    std::function<bool(uint64_t,uint64_t &)> size;
    std::function<bool(uint64_t,uint64_t,std::span<uint8_t>,size_t &)> read;
    std::function<void(uint64_t)> close;
    std::function<int32_t(const ps2x::iop::MemoryCardRequest &)> memoryCard;
    std::function<void(ps2x::iop::LogLevel,std::string_view)> log;
};
// Native C++ sound/save services only. No IOP instruction engine, peripheral
// objects, IRX execution or hardware timing. Caller serializes game data access
// and stops the native audio output before destroying this producer.
class NativeServices final {
public:
    NativeServices(DataMemory &data,DataHeap &heap);
    ~NativeServices();
    void configureFiles(NativeFileAccess files);
    // Frozen before initialize(). Captured provider owners must outlive this
    // service; reset must not throw or re-enter the service dispatcher.
    void configureControllerHooks(std::function<bool()> initialize,std::function<void()> reset);
    void initialize();
    void reset();
    bool ready() const noexcept;
    bool soundReady() const;
    // Direct native mixer command batch; no SIF/RPC transport or guest callback.
    // Main-data ownership remains with the serialized caller; output changes
    // only after actual successful processing by the loaded native provider.
    bool submitSoundCommands(std::span<const std::array<uint32_t,4>> commands,
                             std::span<uint8_t,400> reply);
    ps2x::iop::IopSubsystem &services();
    ps2x::iop::IopHost &host() noexcept;
    size_t readAudio(int16_t *output,size_t frames) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
