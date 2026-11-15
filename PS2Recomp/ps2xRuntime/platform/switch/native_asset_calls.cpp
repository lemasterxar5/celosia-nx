#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_assets.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
namespace {
celosia::native::BootServices &owner(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    if(!runtime || !ctx || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native asset call: invalid context/data");
    return runtime->services();
}
void complete(celosia::native::BootServices &services,R5900Context &ctx,bool ok)
{ services.setAssetError(ok ? 0 : -1); setReturnS32(&ctx,ok ? 1 : 0); }
std::optional<std::string> pathAt(PS2Runtime &runtime,uint32_t address)
{
    if(!address) return {};
    std::string result;
    for(uint32_t i=0;i<1024;++i) {
        char value=0;
        const uint64_t at=uint64_t(address)+i;
        if(at>UINT32_MAX || !runtime.memory().read(static_cast<uint32_t>(at),&value,1)) return {};
        if(!value) return result.empty() ? std::nullopt : std::optional(result);
        result.push_back(value);
    }
    return {};
}
}
// These source import names retain the static compiler ABI. The operations
// below access extracted native files; there is no optical-drive/device model.
namespace ps2_stubs {
void sceCdInit(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    auto &services=owner(ram,ctx,runtime);
    if(getRegU32(ctx,4)!=0) throw std::runtime_error("Native assets: unsupported initialization mode");
    complete(services,*ctx,services.assets().initialize());
}
void sceCdMmode(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    auto &services=owner(ram,ctx,runtime);
    if(getRegU32(ctx,4)!=2) throw std::runtime_error("Native assets: unsupported source data format");
    complete(services,*ctx,services.assets().ready());
}
void sceCdDiskReady(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    auto &services=owner(ram,ctx,runtime);
    const auto ready=services.assets().ready();
    services.setAssetError(ready ? 0 : -1); setReturnS32(ctx,ready ? 2 : 0);
}
void sceCdSearchFile(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    auto &services=owner(ram,ctx,runtime);
    const auto destination=getRegU32(ctx,4);
    const auto path=pathAt(*runtime,getRegU32(ctx,5));
    if(!destination || !path || !runtime->memory().contains(destination,32)) { complete(services,*ctx,false); return; }
    const auto found=services.assets().search(*path);
    if(!found || found->bytes>UINT32_MAX || found->offset/2048>UINT32_MAX) { complete(services,*ctx,false); return; }
    std::array<uint8_t,32> packed{};
    const uint32_t unit=static_cast<uint32_t>(found->offset/2048),size=static_cast<uint32_t>(found->bytes);
    std::memcpy(packed.data(),&unit,4); std::memcpy(packed.data()+4,&size,4);
    auto leaf=found->name;
    if(const auto slash=leaf.find_last_of('/'); slash!=std::string_view::npos) leaf.remove_prefix(slash+1);
    std::copy_n(leaf.begin(),std::min<size_t>(15,leaf.size()),packed.begin()+8);
    complete(services,*ctx,runtime->memory().write(destination,packed.data(),packed.size()));
}
void sceCdRead(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    auto &services=owner(ram,ctx,runtime);
    const auto mode=getRegU32(ctx,7);
    if(mode) {
        std::array<uint8_t,4> fields{};
        if(!runtime->memory().read(mode,fields.data(),fields.size())) { complete(services,*ctx,false); return; }
        if(fields[2]!=0) throw std::runtime_error("Native assets: only 2048-byte data units are supported");
    }
    complete(services,*ctx,services.assets().readAtDataUnits(getRegU32(ctx,4),getRegU32(ctx,5),getRegU32(ctx,6)));
}
void sceCdSync(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    (void)owner(ram,ctx,runtime);
    if(getRegU32(ctx,4)>1) throw std::runtime_error("Native assets: unsupported completion query");
    // Native file reads finish and publish all bytes before returning.
    setReturnS32(ctx,0);
}
void sceCdGetError(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{ setReturnS32(ctx,owner(ram,ctx,runtime).assetError()); }
void sceCdCallback(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    (void)owner(ram,ctx,runtime);
    if(getRegU32(ctx,4)!=0) throw std::runtime_error("Native asset completion callbacks are not ported");
    setReturnU32(ctx,0);
}
}
