#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
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
    PS2Runtime runtime;
    R5900Context context;
    auto *data=runtime.memory().mainRam().data();
    reg(context,4,0);
    ps2_stubs::sceSifInitRpc(data,&context,&runtime);
    require(runtime.services().services().ready() && getRegU32(&context,2)==0,"real native services initialize");
    ps2_stubs::sceSifInitIopHeap(data,&context,&runtime);
    reg(context,4,64); ps2_stubs::sceSifAllocIopHeap(data,&context,&runtime);
    const auto block=getRegU32(&context,2);
    require(block && runtime.services().services().services().isMemoryRange(block,64),"native audio allocation ABI");
    reg(context,4,block); ps2_stubs::sceSifFreeIopHeap(data,&context,&runtime);
    require(getRegU32(&context,2)==0,"native audio free ABI");
    ps2_stubs::sceSifFreeIopHeap(data,&context,&runtime);
    require(getRegU32(&context,2)==UINT32_MAX,"double free returns real error");
    constexpr char path[]="cdrom0:\\SG2IOPM1.IRX;1";
    runtime.memory().write(0x200,path,sizeof(path));
    reg(context,4,0x200); reg(context,5,0); reg(context,6,0);
    ps2_syscalls::sceSifLoadModule(data,&context,&runtime);
    require(getRegU32(&context,2)>0 && runtime.services().services().services().canBindRpc(0x11223344),
            "native module selection installs real sound route");
    reg(context,5,UINT32_MAX);
    ps2_syscalls::sceSifLoadModule(data,&context,&runtime);
    require(getRegU32(&context,2)==UINT32_MAX,"oversized arguments rejected before allocation");
    reg(context,4,0x12000000); reg(context,5,0);
    ps2_syscalls::sceSifLoadModule(data,&context,&runtime);
    require(getRegU32(&context,2)==UINT32_MAX,"device address cannot become native module path");
    bool rejected=false;
    try { ps2_stubs::sceSifInitRpc(nullptr,&context,&runtime); } catch(const std::invalid_argument &) { rejected=true; }
    require(rejected,"foreign source data rejected");
    std::printf("native service ABI: %u checks passed\n",checks);
}
