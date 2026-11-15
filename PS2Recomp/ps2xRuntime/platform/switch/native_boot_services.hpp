#pragma once
#include "native_heap.hpp"
#include "native_semaphore.hpp"
#include "native_library.hpp"
#include "native_threads.hpp"
#include "native_iop.hpp"
#include "native_input.hpp"
#include <mutex>
#include <array>
#include <string>

struct R5900Context;
namespace celosia::native {
class DataMemory;
class NativeFiles;
class NativeAssets;
class NativeFileApi;
class NativePadApi;
class NativeSaveStore;
class NativeSaveControl;
struct AssetEntry;
// Native bootstrap data and OS synchronization. No virtual threads/devices.
class BootServices final {
public:
    explicit BootServices(DataMemory &memory);
    ~BootServices();
    bool handle(uint32_t id, R5900Context &context);
    DataHeap &heap() noexcept { return allocator; }
    SemaphoreSet &semaphores() noexcept { return synchronization; }
    NativeLibrary &library() noexcept { return nativeLibrary; }
    NativeThreads &threads() noexcept { return nativeThreads; }
    NativeServices &services() noexcept { return nativeServices; }
    void configureAssets(NativeFiles &files,std::span<const AssetEntry> entries);
    NativeAssets &assets();
    NativeFileApi &fileApi();
    void configureInput(NativeInput::Sampler sampler);
    NativePadApi &pads();
    void configureSaveRoot(std::string root);
    int32_t assetError() const noexcept { return nativeAssetError; }
    void setAssetError(int32_t error) noexcept { nativeAssetError=error; }
    void recordTaskFailure(const std::string &message);
    std::string taskFailure() const;
    std::mutex &executionMutex() noexcept { return executionGate; }
    void reserveContext(R5900Context *context);
    void releaseContext(R5900Context *context) noexcept;
private:
    DataMemory &data;
    DataHeap allocator;
    std::unique_ptr<NativeInput> nativeInput;
    std::unique_ptr<NativePadApi> nativePads;
    bool inputLocked=false;
    NativeServices nativeServices;
    SemaphoreSet synchronization;
    NativeThreads nativeThreads;
    NativeLibrary nativeLibrary;
    std::unique_ptr<NativeAssets> nativeAssets;
    std::unique_ptr<NativeFileApi> nativeFilesApi;
    std::unique_ptr<NativeSaveStore> nativeSaveStore;
    std::unique_ptr<NativeSaveControl> nativeSaveControl;
    int32_t nativeAssetError=0;
    std::mutex mutex;
    std::mutex executionGate,contextsMutex;
    std::array<R5900Context *,256> activeContexts{};
    mutable std::mutex failureMutex;
    std::string firstTaskFailure;
    uint32_t stackBegin = 0, stackEnd = 0;
    void setupStack(R5900Context &context);
    void setupHeap(R5900Context &context);
    bool handleSemaphore(uint32_t id,R5900Context &context);
};
}
