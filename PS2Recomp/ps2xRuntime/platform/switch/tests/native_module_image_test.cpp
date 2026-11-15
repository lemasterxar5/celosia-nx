#include "native_module_image.hpp"
#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_memory.hpp"
#include "native_aot_context.hpp"
#include <array>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool value,const char *why){++checks;if(!value)throw std::runtime_error(why);}
std::array<uint8_t,32> digest(std::string_view text)
{
    std::array<uint8_t,32> result{};
    auto digit=[](char c){return uint8_t(c<='9'?c-'0':c-'a'+10);};
    for(size_t i=0;i<result.size();++i) result[i]=uint8_t(digit(text[i*2])*16+digit(text[i*2+1]));
    return result;
}
void reg(R5900Context &context,unsigned index,uint32_t value)
{context.r[index]=_mm_set_epi32(0,0,0,value);}
template<class Action>std::string rejects(Action action)
{
    try {action();}catch(const std::exception &error){++checks;return error.what();}
    throw std::runtime_error("expected explicit module image failure");
}
}
int main()
{
    DataMemory data;BootServices boot(data);R5900Context context;
    std::vector<uint8_t> image(27661,0x5A);image.front()=0x7F;image.back()=0x33;
    require(data.write(0x10000,image.data(),image.size()),"bounded image data fixture");
    const char label[]="sound";data.write(0x100,label,sizeof(label));
    reg(context,4,0x100);reg(context,5,0x80010000);reg(context,6,uint32_t(image.size()));
    unsigned hashes=0;
    Hasher sg2=[&](std::span<const uint8_t> input) {
        ++hashes;require(input.size()==image.size() && input.front()==0x7F && input.back()==0x33,
                         "hasher receives complete actual bounded source span");
        return digest("82879fd14220fe07fbb07d5dc7043f16dc1c8bd4c8bd3f71ace2e2ea8c6a5a85");
    };
    rejects([&]{loadNativeModuleImage(data,boot,context,{});});
    require(!boot.services().ready(),"missing hasher cannot initialize services");
    reg(context,7,65537);rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    require(hashes==0 && !boot.services().ready(),"oversized arguments rejected before hashing or activation");
    reg(context,7,2);reg(context,8,0x1FFFFFF);
    rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,8,0);rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,8,0x70000000);rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,7,0);reg(context,5,0x1FFFFFF);
    rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,5,0x12000000);rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,5,0);rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,5,0x10000);reg(context,6,128*1024+1);
    rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,6,0);rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    reg(context,6,uint32_t(image.size()));reg(context,4,0x12000000);
    rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    std::array<char,1024> longLabel;longLabel.fill('x');data.write(0x1000,longLabel.data(),longLabel.size());
    reg(context,4,0x1000);rejects([&]{loadNativeModuleImage(data,boot,context,sg2);});
    require(hashes==0 && !boot.services().ready(),"all range and label failures precede hash and state changes");
    reg(context,4,0x100);
    const auto unknown=rejects([&]{loadNativeModuleImage(data,boot,context,[](auto){return std::array<uint8_t,32>{};});});
    require(unknown.find("sound")!=std::string::npos && unknown.find("27661")!=std::string::npos &&
            unknown.find(std::string(64,'0'))!=std::string::npos,"unknown digest reports bounded identifying metadata");
    require(!boot.services().ready(),"unknown image does not initialize services");
    rejects([&]{loadNativeModuleImage(data,boot,context,[](auto)->std::array<uint8_t,32>{throw std::runtime_error("hash failed");});});
    require(!boot.services().ready(),"hasher exception has no service mutation");
    reg(context,6,4269);const auto usb=loadNativeModuleImage(data,boot,context,[](auto){
        return digest("a7138e76254ca6dee575e2053dcef31ccab0b24bc4e1df936668e4d89398a2aa");});
    require(usb<0 && !boot.services().services().canBindRpc(0x11223344),"verified unsupported USB cannot publish provider success");
    reg(context,6,uint32_t(image.size()));
    const std::array<uint8_t,4> args{1,0,2,0};data.write(0x200,args.data(),args.size());
    reg(context,7,uint32_t(args.size()));reg(context,8,0xA0000200);
    const auto id=loadNativeModuleImage(data,boot,context,sg2);
    require(id==0x40000000 && boot.services().services().canBindRpc(0x11223344),"verified image selects actual native SG2 route and module ID");
    require(std::equal(image.begin(),image.end(),data.mainRam().begin()+0x10000),"module image remains unmodified");
    require(std::equal(args.begin(),args.end(),data.mainRam().begin()+0x200),"argument data remains unmodified");
    reg(context,4,0);reg(context,7,0);reg(context,8,UINT32_MAX);
    require(loadNativeModuleImage(data,boot,context,sg2)==id,"null optional label and zero unused arguments accepted");
    require(hashes==2,"only valid matching SG2 requests reach injected SG2 hasher");
    reg(context,6,27660);const auto wrongSize=rejects([&]{loadNativeModuleImage(data,boot,context,[](auto){
        return digest("82879fd14220fe07fbb07d5dc7043f16dc1c8bd4c8bd3f71ace2e2ea8c6a5a85");});});
    require(wrongSize.find("27660")!=std::string::npos,"matching digest never overrides pinned image size");
    std::printf("native module image: %u checks passed\n",checks);
}
