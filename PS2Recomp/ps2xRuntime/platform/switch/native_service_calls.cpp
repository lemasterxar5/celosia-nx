#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_assets.hpp"
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
celosia::native::NativeServices &owner(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    if(!runtime || !context || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native service: invalid context/data base");
    return runtime->services().services();
}
std::optional<std::string> modulePath(PS2Runtime &runtime,uint32_t address)
{
    if(!address) return {};
    std::string path;
    for(uint32_t i=0;i<1024;++i) {
        char value=0;
        const uint64_t at=uint64_t(address)+i;
        if(at>UINT32_MAX || !runtime.memory().read(static_cast<uint32_t>(at),&value,1)) return {};
        if(!value) return path.empty() ? std::nullopt : std::optional(path);
        path.push_back(value);
    }
    return {};
}
void initialize(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=owner(ram,context,runtime);
    if(getRegU32(context,4)!=0) throw std::runtime_error("Native service: unsupported initialization mode");
    native.initialize();
    setReturnS32(context,0);
}
void loadModule(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=owner(ram,context,runtime);
    const auto path=modulePath(*runtime,getRegU32(context,4));
    const auto size=getRegU32(context,5),address=getRegU32(context,6);
    if(!path || size>65536 || (size && (!address || !runtime->memory().contains(address,size)))) {
        setReturnS32(context,-1); return;
    }
    std::vector<uint8_t> arguments(size);
    if(size && !runtime->memory().read(address,arguments.data(),size)) { setReturnS32(context,-1); return; }
    const auto result=native.services().loadModule(*path,size ? arguments.data() : nullptr,size);
    setReturnS32(context,result.handled ? result.moduleId : -1);
}
}
namespace ps2_stubs {
void sceSifInitRpc(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { initialize(ram,context,runtime); }
void sceSifInitIopHeap(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{ owner(ram,context,runtime).initialize(); setReturnS32(context,0); }
void sceSifAllocIopHeap(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{ setReturnU32(context,owner(ram,context,runtime).services().allocateMemory(getRegU32(context,4))); }
void sceSifFreeIopHeap(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{ setReturnS32(context,owner(ram,context,runtime).services().freeMemory(getRegU32(context,4)) ? 0 : -1); }
void sceSifLoadModule(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { loadModule(ram,context,runtime); }
void sceSifRebootIop(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &native=owner(ram,context,runtime);
    const auto path=modulePath(*runtime,getRegU32(context,4));
    if(!path) throw std::runtime_error("Native services: unsupported source service package");
    const auto image=runtime->services().assets().search(*path);
    if(!image || image->name!="IOPRP300.IMG" || image->bytes!=275345)
        throw std::runtime_error("Native services: source package does not match this static build");
    // Reset real C++ module routes, arena allocations and mixer state. The
    // original package is identified as data; its processor code is not loaded.
    native.reset();
    setReturnS32(context,1);
}
void sceSifSyncIop(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{ setReturnS32(context,owner(ram,context,runtime).ready() ? 1 : 0); }
}
namespace ps2_syscalls {
void SifInitRpc(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { initialize(ram,context,runtime); }
void sceSifLoadModule(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { loadModule(ram,context,runtime); }
void SifLoadModule(uint8_t *ram,R5900Context *context,PS2Runtime *runtime) { loadModule(ram,context,runtime); }
}
