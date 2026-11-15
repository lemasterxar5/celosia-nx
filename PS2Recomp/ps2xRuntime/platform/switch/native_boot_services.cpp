#include "native_boot_services.hpp"
#include "native_aot_runtime.hpp"
#include "native_memory.hpp"
#include "native_assets.hpp"
#include "native_file_api.hpp"
#include "native_pad_api.hpp"
#include "native_save_control.hpp"
#include <algorithm>
#include <stdexcept>

namespace celosia::native {
BootServices::BootServices(DataMemory &memory)
    : data(memory),allocator(memory),nativeInput(std::make_unique<NativeInput>()),
      nativePads(std::make_unique<NativePadApi>(*nativeInput,memory)),nativeServices(memory,allocator),
      nativeLibrary(memory,synchronization,nativeThreads,&nativeServices)
{
    nativeServices.configureControllerHooks([this] { return pads().initialize()==1; },
        [this] { (void)nativePads->end(); });
}
BootServices::~BootServices()=default;
void BootServices::configureSaveRoot(std::string root)
{
    if(nativeSaveStore)throw std::logic_error("Native save configuration is frozen");
    auto store=std::make_unique<NativeSaveStore>(std::move(root));
    auto control=std::make_unique<NativeSaveControl>(data,*store);
    nativeSaveStore=std::move(store);nativeSaveControl=std::move(control);
    nativeLibrary.setSaveHandler([this](uint32_t address,R5900Context &context) {
        return nativeSaveControl->handle(address,context);
    });
}
void BootServices::configureAssets(NativeFiles &files,std::span<const AssetEntry> entries)
{
    if(nativeAssets) throw std::logic_error("Native asset configuration is frozen");
    auto archive=std::make_unique<NativeAssets>(files,data,entries);
    auto api=std::make_unique<NativeFileApi>(files,data);
    nativeAssets=std::move(archive); nativeFilesApi=std::move(api);
}
NativeAssets &BootServices::assets()
{
    if(!nativeAssets) throw std::logic_error("Native assets are not configured");
    return *nativeAssets;
}
NativeFileApi &BootServices::fileApi()
{
    if(!nativeFilesApi) throw std::logic_error("Native file descriptors are not configured");
    return *nativeFilesApi;
}
void BootServices::configureInput(NativeInput::Sampler sampler)
{
    if(inputLocked) throw std::logic_error("Native input configuration is frozen");
    auto input=std::make_unique<NativeInput>(std::move(sampler));
    auto pads=std::make_unique<NativePadApi>(*input,data);
    nativePads=std::move(pads); nativeInput=std::move(input);
}
NativePadApi &BootServices::pads()
{ inputLocked=true; return *nativePads; }
void BootServices::recordTaskFailure(const std::string &message)
{
    std::lock_guard lock(failureMutex);
    if(firstTaskFailure.empty()) firstTaskFailure=message;
}
std::string BootServices::taskFailure() const
{ std::lock_guard lock(failureMutex); return firstTaskFailure; }
void BootServices::reserveContext(R5900Context *context)
{
    std::lock_guard lock(contextsMutex);
    if(std::find(activeContexts.begin(),activeContexts.end(),context)!=activeContexts.end())
        throw std::logic_error("Native runtime: context already executing on an OS thread");
    const auto slot=std::find(activeContexts.begin(),activeContexts.end(),nullptr);
    if(slot==activeContexts.end()) throw std::runtime_error("Native runtime: active context limit");
    *slot=context;
}
void BootServices::releaseContext(R5900Context *context) noexcept
{
    std::lock_guard lock(contextsMutex);
    const auto slot=std::find(activeContexts.begin(),activeContexts.end(),context);
    if(slot!=activeContexts.end()) *slot=nullptr;
}
namespace {
bool overlaps(uint32_t a, uint32_t endA, uint32_t b, uint32_t endB)
{ return a < endB && b < endA; }
}
void BootServices::setupStack(R5900Context &ctx)
{
    const auto gp = getRegU32(&ctx,4), raw = getRegU32(&ctx,5), size = getRegU32(&ctx,6);
    if (!size || size > DataMemory::MainBytes || !raw || (gp && !data.contains(gp,1))) {
        setReturnS32(&ctx,-1); return;
    }
    const uint64_t begin = raw == UINT32_MAX ? DataMemory::MainBytes-size : raw;
    const uint64_t end = begin+size;
    if (begin < 16 || end > DataMemory::MainBytes || (begin & 15) || (end & 15) ||
        (stackEnd && (begin != stackBegin || end != stackEnd)) ||
        (allocator.limit() && overlaps(static_cast<uint32_t>(begin),static_cast<uint32_t>(end),
                                       allocator.base(),allocator.limit()))) {
        setReturnS32(&ctx,-1); return;
    }
    stackBegin = static_cast<uint32_t>(begin); stackEnd = static_cast<uint32_t>(end);
    if (gp) ctx.r[28] = _mm_set_epi64x(0,static_cast<int64_t>(static_cast<int32_t>(gp)));
    setReturnU32(&ctx,stackEnd);
}
void BootServices::setupHeap(R5900Context &ctx)
{
    const auto raw = getRegU32(&ctx,4), size = getRegU32(&ctx,5);
    const uint64_t begin = (uint64_t(raw)+15)&~uint64_t(15);
    constexpr uint32_t DefaultLimit = 0x01F00000;
    const uint64_t end = !size || size == UINT32_MAX ? DefaultLimit : begin+size;
    if (raw < 16 || begin >= end || end > DefaultLimit ||
        (stackEnd && overlaps(static_cast<uint32_t>(begin),static_cast<uint32_t>(end),stackBegin,stackEnd)) ||
        !allocator.configure(static_cast<uint32_t>(begin),static_cast<uint32_t>(end))) {
        setReturnS32(&ctx,-1); return;
    }
    setReturnU32(&ctx,allocator.base());
}
bool BootServices::handle(uint32_t id, R5900Context &ctx)
{
    // Native waits must not hold the bootstrap/configuration mutex: another
    // native thread needs to signal/delete the semaphore while this one sleeps.
    if(handleSemaphore(id,ctx)) return true;
    std::lock_guard lock(mutex);
    switch(id) {
    case 0x3C: setupStack(ctx); return true;
    case 0x3D: setupHeap(ctx); return true;
    case 0x3E: setReturnU32(&ctx,allocator.limit()); return true;
    case 0x7F: setReturnU32(&ctx,DataMemory::MainBytes); return true;
    default: return false;
    }
}
}
namespace ps2_syscalls {
void SetupThread(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ runtime->handleSyscall(ram,ctx,0x3C); }
void EndOfHeap(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ runtime->handleSyscall(ram,ctx,0x3E); }
void GetMemorySize(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{ runtime->handleSyscall(ram,ctx,0x7F); }
}
