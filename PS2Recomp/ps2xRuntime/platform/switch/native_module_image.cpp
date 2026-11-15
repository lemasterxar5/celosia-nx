#include "native_module_image.hpp"
#include "native_aot_context.hpp"
#include "native_boot_services.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace celosia::native {
namespace {
struct ImageIdentity {std::string_view path;uint32_t bytes;std::string_view sha256;};
// Metadata only, pinned to work/disc-full-manifest.json, SCUS_974.72. Image
// bytes identify a native provider; they never enter an executable loader.
constexpr std::array<ImageIdentity,11> Images{{
    {"SG2IOPM1.IRX",27661,"82879fd14220fe07fbb07d5dc7043f16dc1c8bd4c8bd3f71ace2e2ea8c6a5a85"},
    {"MODULES2/MCMAN.IRX",96181,"b4df4061f8fdc9c7cf06a329b96227ad6ca69d008df475c1fc4d3e8d06fe3fa4"},
    {"MODULES2/MCSERV.IRX",7385,"5006b0dd650fa416d03a559bc8e3bae8a7ae95be81f3e4f3fd9bd524fa27e000"},
    {"MODULES/DBCMAN.IRX",15653,"60bfea7d0877df3af370676666e5f1043b6e71723fab94cdff790c19e75e12e7"},
    {"MODULES/DS1O_D.IRX",10469,"ba65aefc36ef49fc2c31b979aef628cf304561c421b817d3734aee066082937c"},
    {"MODULES/LIBSD.IRX",30085,"94915a3d9673e8a370e144ae493552d5e1f3956bb9ae5bd91e2da5f36c83ce7d"},
    {"MODULES/MC2_D.IRX",20269,"e4c9df69322819807761baff5b3fd33a66054ac4cc76e24d6558f6f210b12a04"},
    {"MODULES/PL2303.IRX",4269,"a7138e76254ca6dee575e2053dcef31ccab0b24bc4e1df936668e4d89398a2aa"},
    {"MODULES/SIO2D.IRX",11289,"b07339d0b6ef6021ebb2bd8dba1138f3159b2cdfbf1b29df799694107107d330"},
    {"MODULES/SIO2MAN.IRX",6641,"565ec77e75f174cfddf72c41ccc446c1c07a3d5fb0a2ccfefad337cb8a7d0728"},
    {"MODULES/USBD.IRX",34993,"3e443c652b3bf639139706ef8b6b8c11dc5a03308af956b05f97e69a4840fd8c"}
}};
std::optional<uint32_t> mainOffset(uint32_t address,uint32_t bytes)
{
    if(!address) return {};
    for(const auto base:{0u,0x20000000u,0x80000000u,0xA0000000u}) {
        if(address<base) continue;
        const auto offset=address-base;
        if(offset<=DataMemory::MainBytes && bytes<=DataMemory::MainBytes-offset) return offset;
    }
    return {};
}
std::span<const uint8_t> source(DataMemory &data,uint32_t address,uint32_t bytes)
{
    const auto offset=mainOffset(address,bytes);
    if(!offset) throw std::out_of_range("Native module image: source exceeds main data bounds");
    return data.mainRam().subspan(*offset,bytes);
}
std::string labelAt(DataMemory &data,uint32_t address)
{
    if(!address) return "<unnamed>";
    std::string label;
    for(unsigned index=0;index<1024;++index) {
        const auto at=uint64_t(address)+index;
        if(at>UINT32_MAX) throw std::out_of_range("Native module image: diagnostic label overflows");
        const auto value=source(data,static_cast<uint32_t>(at),1).front();
        if(!value) return label;
        // Labels are diagnostic data only; keep one-line errors bounded even
        // when the source includes control characters or non-ASCII bytes.
        label.push_back(value>=32 && value<127 ? static_cast<char>(value) : '?');
    }
    throw std::invalid_argument("Native module image: unterminated diagnostic label");
}
std::string hexDigest(const std::array<uint8_t,32> &digest)
{
    constexpr std::string_view digits="0123456789abcdef";
    std::string result;result.reserve(64);
    for(const auto byte:digest) {result.push_back(digits[byte>>4]);result.push_back(digits[byte&15]);}
    return result;
}
}
int32_t loadNativeModuleImage(DataMemory &data,BootServices &boot,
                             const R5900Context &context,Hasher hasher)
{
    if(!hasher) throw std::invalid_argument("Native module image: SHA-256 provider missing");
    const auto imageBytes=getRegU32(&context,6),argumentBytes=getRegU32(&context,7);
    if(!imageBytes || imageBytes>128*1024 || argumentBytes>64*1024)
        throw std::invalid_argument("Native module image: image/argument size outside limits");
    const auto image=source(data,getRegU32(&context,5),imageBytes);
    const auto label=labelAt(data,getRegU32(&context,4));
    std::vector<uint8_t> arguments;
    if(argumentBytes) {
        const auto bytes=source(data,getRegU32(&context,8),argumentBytes);
        arguments.assign(bytes.begin(),bytes.end());
    }
    const auto digest=hexDigest(hasher(image));
    const auto known=std::find_if(Images.begin(),Images.end(),[&](const ImageIdentity &candidate) {
        return candidate.bytes==imageBytes && candidate.sha256==digest;
    });
    if(known==Images.end()) throw std::runtime_error("Native module image: unknown identity label="+
        label+" bytes="+std::to_string(imageBytes)+" sha256="+digest);
    auto &native=boot.services();native.initialize();
    const auto result=native.services().loadModule(known->path,
        arguments.empty()?nullptr:arguments.data(),argumentBytes);
    return result.handled ? result.moduleId : -1;
}
}
