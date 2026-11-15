#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_files.hpp"
#include "native_assets.hpp"
#include "ps2_stubs.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
static void reg(R5900Context &c,unsigned n,uint32_t value) { c.r[n]=_mm_set_epi32(0,0,0,value); }
int main()
{
    using namespace celosia::native;
    const auto fixture=std::filesystem::absolute("native-asset-calls-fixture");
    require(!std::filesystem::exists(fixture),"exclusive asset fixture");
    std::filesystem::create_directory(fixture);
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::filesystem::remove_all(path);} } cleanup{fixture};
    { std::ofstream file(fixture/"TEST.BIN",std::ios::binary); const std::string payload(2500,'z'); file.write(payload.data(),payload.size()); }
    { std::ofstream file(fixture/"IOPRP300.IMG",std::ios::binary); file.seekp(275344); file.put('\0'); }
    NativeFiles files(fixture.generic_string());
    PS2Runtime runtime;
    const std::array<AssetEntry,2> entries{{{"TEST.BIN",0x4000,2500},{"IOPRP300.IMG",0x10000,275345}}};
    runtime.services().configureAssets(files,entries);
    auto *ram=runtime.memory().mainRam().data();
    R5900Context context;
    reg(context,4,0); ps2_stubs::sceCdInit(ram,&context,&runtime);
    require(getRegU32(&context,2)==1 && runtime.services().assets().ready(),"native extracted assets verified");
    reg(context,4,2); ps2_stubs::sceCdMmode(ram,&context,&runtime);
    require(getRegU32(&context,2)==1,"supported native asset data format");
    ps2_stubs::sceCdDiskReady(ram,&context,&runtime); require(getRegU32(&context,2)==2,"initialized readonly files are ready");
    constexpr char path[]="cdrom0:\\TEST.BIN;1";
    runtime.memory().write(0x200,path,sizeof(path));
    reg(context,4,0x100); reg(context,5,0x200); ps2_stubs::sceCdSearchFile(ram,&context,&runtime);
    std::array<uint8_t,32> result{}; runtime.memory().read(0x100,result.data(),result.size());
    uint32_t unit=0,size=0; std::memcpy(&unit,result.data(),4); std::memcpy(&size,result.data()+4,4);
    require(getRegU32(&context,2)==1 && unit==8 && size==2500 && std::memcmp(result.data()+8,"TEST.BIN",8)==0,
            "source file result carries verified native archive metadata");
    reg(context,4,8); reg(context,5,2); reg(context,6,0x1000); reg(context,7,0);
    ps2_stubs::sceCdRead(ram,&context,&runtime);
    require(getRegU32(&context,2)==1 && ram[0x1000]=='z' && ram[0x1000+2499]=='z' &&
            ram[0x1000+2500]==0 && ram[0x1FFF]==0,"actual native bytes and final archive-block padding");
    reg(context,4,0); ps2_stubs::sceCdSync(ram,&context,&runtime); require(getRegU32(&context,2)==0,"synchronous native read is completed");
    ps2_stubs::sceCdGetError(ram,&context,&runtime); require(getRegU32(&context,2)==0,"successful native read has no error");
    reg(context,4,8); reg(context,5,2); reg(context,6,0x1FFFFF0);
    ps2_stubs::sceCdRead(ram,&context,&runtime); require(getRegU32(&context,2)==0,"invalid destination cannot wrap data arena");
    ps2_stubs::sceCdGetError(ram,&context,&runtime); require(getRegU32(&context,2)!=0,"native read error is visible");
    reg(context,4,0); ps2_stubs::sceCdCallback(ram,&context,&runtime); require(getRegU32(&context,2)==0,"no callback remains disabled");
    reg(context,4,0x1000); bool rejected=false;
    try { ps2_stubs::sceCdCallback(ram,&context,&runtime); } catch(const std::runtime_error &) { rejected=true; }
    require(rejected,"unported completion callback cannot fake registration");
    runtime.services().services().initialize();
    auto &native=runtime.services().services().services();
    require(native.loadModule("cdrom0:\\SG2IOPM1.IRX;1").moduleId>0 && native.canBindRpc(0x11223344),
            "real sound route installed before reset");
    constexpr char package[]="cdrom0:\\IOPRP300.IMG;1";
    runtime.memory().write(0x300,package,sizeof(package));
    reg(context,4,0x300); reg(context,5,0x136B00);
    ps2_stubs::sceSifRebootIop(ram,&context,&runtime);
    require(getRegU32(&context,2)==1 && !native.canBindRpc(0x11223344),"one-argument native service reset clears routes without reading unrelated register state");
    ps2_stubs::sceSifSyncIop(ram,&context,&runtime);
    require(getRegU32(&context,2)==1,"synchronous native service reset completed");
    reg(context,4,0x200); rejected=false;
    try { ps2_stubs::sceSifRebootIop(ram,&context,&runtime); } catch(const std::runtime_error &) { rejected=true; }
    require(rejected,"unmatched source service package is rejected");
    std::printf("native asset ABI: %u checks passed\n",checks);
}
