#pragma once
#include <cstdint>
#include <memory>
#include <functional>

struct R5900Context;
namespace celosia::native {
class DataMemory;
class SemaphoreSet;
class NativeThreads;
class NativeServices;
// SCUS_974.72 library boundary for the statically compiled build. Enable only
// after validating the matching ELF data. No kernel patch or device code runs.
// The owner thread executes calls; lifecycle/configuration is not concurrent.
class NativeLibrary final {
public:
    NativeLibrary(DataMemory &data,SemaphoreSet &semaphores,NativeThreads &threads,NativeServices *sound=nullptr);
    ~NativeLibrary();
    NativeLibrary(const NativeLibrary &)=delete;
    NativeLibrary &operator=(const NativeLibrary &)=delete;
    void enableScus();
    void setDisplayInitializer(std::function<void()> initialize);
    void setExceptionInitializer(std::function<void()> initialize);
    void setPowerOffInitializer(std::function<void()> initialize);
    void setTransferSynchronizer(std::function<void()> synchronize);
    void setPlatformInitializer(std::function<void()> initialize);
    void setGraphicsInitializer(std::function<void()> initialize);
    void setModuleImageInitializer(std::function<int32_t(const R5900Context &)> initialize);
    void setSaveHandler(std::function<bool(uint32_t,R5900Context &)> handle);
    void setModulePublisher(std::function<void(const R5900Context &)> publish);
    void publishModule(const R5900Context &context);
    void requirePublishedEntry(uint32_t address) const;
    bool handle(uint32_t address,R5900Context &context);
    bool ready() const noexcept;
    int32_t currentThreadId() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
