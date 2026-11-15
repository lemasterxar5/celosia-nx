#include "native_iop.hpp"
#include "native_memory.hpp"
#include "native_heap.hpp"
#include <cstdio>
#include <limits>
#include <stdexcept>
#if !defined(PS2X_IOP_NATIVE_ONLY)
#error "NativeServices must link the CPU-free native-only services target"
#endif

namespace celosia::native {
using namespace ps2x::iop;
struct NativeServices::Impl final : IopHost {
    DataMemory &data;
    DataHeap &heap;
    NativeFileAccess files;
    std::function<bool()> initializeController;
    std::function<void()> resetController;
    std::unique_ptr<IopSubsystem> subsystem;
    uint32_t nextHandle=1;
    Impl(DataMemory &memory,DataHeap &allocator) : data(memory),heap(allocator) {}
    bool initializeNativeController() override
    { return initializeController && initializeController(); }
    void resetNativeController() override
    { if(resetController) resetController(); }
    bool readGuest(uint32_t address,void *out,size_t size) const override { return data.read(address,out,size); }
    bool writeGuest(uint32_t address,const void *in,size_t size) override { return data.write(address,in,size); }
    bool zeroGuest(uint32_t address,size_t size) override { return data.zero(address,size); }
    bool normalizeGuestAddress(uint32_t address,uint32_t &normalized) const override
    {
        for(const uint32_t base:{0u,0x20000000u,0x80000000u,0xA0000000u}) {
            if(address>=base && address-base<DataMemory::MainBytes) { normalized=address-base; return true; }
        }
        return false;
    }
    bool readIopMemory(uint32_t a,void *out,size_t size) const override
    { return subsystem && subsystem->readMemory(a,out,size); }
    bool writeIopMemory(uint32_t a,const void *in,size_t size) override
    { return subsystem && subsystem->writeMemory(a,in,size); }
    bool zeroIopMemory(uint32_t a,size_t size) override
    { return subsystem && subsystem->zeroMemory(a,size); }
    bool normalizeIopAddress(uint32_t a,uint32_t &normalized) const override
    {
        if(!subsystem || !subsystem->isMemoryRange(a,1)) return false;
        normalized=a; return true;
    }
    uint32_t allocateIopHandle(IopHandleKind) override
    {
        if(nextHandle==std::numeric_limits<uint32_t>::max()) throw std::overflow_error("Native service handles exhausted");
        return nextHandle++;
    }
    uint32_t allocateGuest(uint32_t size,uint32_t alignment) override { return heap.allocate(size,alignment); }
    void freeGuest(uint32_t address) override
    { if(address && !heap.release(address)) throw std::invalid_argument("Native service: foreign data allocation"); }
    void audioCommand(uint32_t,uint32_t,GuestBuffer,GuestBuffer) override
    { throw std::runtime_error("Native service: libsd command provider is not ported"); }
    std::string hostPath(HostPathKind kind) const override
    { return files.rootPath ? files.rootPath(kind) : std::string{}; }
    std::string translateGuestPath(std::string_view path) const override
    {
        if(!files.translate) throw std::runtime_error("Native asset files are not configured");
        return files.translate(path);
    }
    uint64_t openHostFile(std::string_view path) override { return files.open ? files.open(path) : 0; }
    bool hostFileSize(uint64_t handle,uint64_t &size) const override
    { return files.size && files.size(handle,size); }
    bool readHostFile(uint64_t handle,uint64_t offset,void *out,size_t size,size_t &read) override
    {
        read=0;
        if((!out && size) || !files.read) return false;
        return files.read(handle,offset,std::span<uint8_t>(static_cast<uint8_t *>(out),size),read);
    }
    void closeHostFile(uint64_t handle) override
    {
        if(!handle) return;
        if(!files.close) throw std::runtime_error("Native file close provider is not configured");
        files.close(handle);
    }
    int32_t memoryCard(const MemoryCardRequest &request) override
    { return files.memoryCard ? files.memoryCard(request) : -1; }
    bool hasGuestFunction(uint32_t) const override { return false; }
    bool invokeGuestFunction(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t *) override
    { throw std::runtime_error("Native service callback is not ported"); }
    bool sendSifCommand(uint32_t,const void *,size_t) override
    { throw std::runtime_error("Native service notification is not ported"); }
    void log(LogLevel level,std::string_view message) override
    {
        if(files.log) { files.log(level,message); return; }
        std::fputs("Native service: ",stderr);
        (void)std::fwrite(message.data(),1,message.size(),stderr);
        std::fputc('\n',stderr);
    }
};
NativeServices::NativeServices(DataMemory &data,DataHeap &heap) : impl(std::make_unique<Impl>(data,heap)) {}
NativeServices::~NativeServices()=default;
void NativeServices::configureFiles(NativeFileAccess files)
{
    if(impl->subsystem) throw std::logic_error("Native service configuration is frozen");
    impl->files=std::move(files);
}
void NativeServices::configureControllerHooks(std::function<bool()> initialize,std::function<void()> reset)
{
    if(impl->subsystem) throw std::logic_error("Native controller configuration is frozen");
    impl->initializeController=std::move(initialize); impl->resetController=std::move(reset);
}
void NativeServices::initialize()
{ if(!impl->subsystem) impl->subsystem=std::make_unique<IopSubsystem>(*impl); }
void NativeServices::reset() { services().reset(); }
bool NativeServices::ready() const noexcept { return bool(impl->subsystem); }
bool NativeServices::soundReady() const
{ return impl->subsystem && impl->subsystem->soundReady(); }
bool NativeServices::submitSoundCommands(std::span<const std::array<uint32_t,4>> commands,
                                          std::span<uint8_t,400> reply)
{ return impl->subsystem && impl->subsystem->submitSoundCommands(commands,reply); }
IopSubsystem &NativeServices::services()
{
    if(!impl->subsystem) throw std::logic_error("Native services are not initialized");
    return *impl->subsystem;
}
IopHost &NativeServices::host() noexcept { return *impl; }
size_t NativeServices::readAudio(int16_t *output,size_t frames) noexcept
{ return impl->subsystem ? impl->subsystem->readAudio(output,frames) : 0; }
}
