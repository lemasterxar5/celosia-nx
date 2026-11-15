#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include <cstdio>
#include <stdexcept>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
static unsigned checks = 0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    PS2Runtime runtime;
    R5900Context ctx;
    auto *ram = runtime.memory().mainRam().data();
    auto args = [&](uint32_t a,uint32_t b=0,uint32_t c=0) {
        ctx.r[4]=_mm_set_epi32(0,0,0,a); ctx.r[5]=_mm_set_epi32(0,0,0,b); ctx.r[6]=_mm_set_epi32(0,0,0,c);
    };
    runtime.handleSyscall(ram,&ctx,0x7F);
    require(getRegU32(&ctx,2)==0x2000000,"native data size, not physical system RAM");
    args(0x141D70,0x1FE0000,0x20000); ctx.r[3]=_mm_set_epi32(0,0,0,0x3C);
    runtime.handleSyscall(ram,&ctx,0);
    require(getRegU32(&ctx,2)==0x2000000 && getRegU32(&ctx,28)==0x141D70,"real entry stack/GP bootstrap");
    args(0,0,0); runtime.handleSyscall(ram,&ctx,0x3C);
    require(getRegU32(&ctx,2)==UINT32_MAX,"empty data stack rejected");
    args(0x12000000,0x1FE0000,0x20000); runtime.handleSyscall(ram,&ctx,0x3C);
    require(getRegU32(&ctx,2)==UINT32_MAX && getRegU32(&ctx,28)==0x141D70,"device-valued GP rejected transactionally");
    args(0x123,0x1FFFFFF,0x1000); runtime.handleSyscall(ram,&ctx,0x3C);
    require(getRegU32(&ctx,2)==UINT32_MAX && getRegU32(&ctx,28)==0x141D70,"bad stack cannot corrupt GP");
    args(0x16A398,0x40000); runtime.handleSyscall(ram,&ctx,0x3D);
    require(getRegU32(&ctx,2)==0x16A3A0 && runtime.services().heap().limit()==0x1AA3A0,"real entry aligned heap bounds");
    args(32); ps2_stubs::malloc(ram,&ctx,&runtime); const auto block=getRegU32(&ctx,2);
    require(block==0x16A3A0,"native allocation uses configured game data heap");
    ram[block]=77; args(block,64); ps2_stubs::realloc(ram,&ctx,&runtime);
    require(getRegU32(&ctx,2)==block && ram[block]==77 && ram[block+32]==0,"native realloc preserve/grow");
    args(0x1000,0x100); runtime.handleSyscall(ram,&ctx,0x3D);
    require(getRegU32(&ctx,2)==UINT32_MAX && ram[block]==77,"live heap cannot be reset");
    args(block); ps2_stubs::free(ram,&ctx,&runtime);
    require(ram[block]==0,"native free clears released data");
    args(UINT32_MAX,0x100); runtime.handleSyscall(ram,&ctx,0x3D);
    require(getRegU32(&ctx,2)==UINT32_MAX,"heap alignment cannot wrap");
    args(0x1FD0000,0x30000); runtime.handleSyscall(ram,&ctx,0x3D);
    require(getRegU32(&ctx,2)==UINT32_MAX,"heap cannot overlap game data stack");
    runtime.handleSyscall(ram,&ctx,0x3E); require(getRegU32(&ctx,2)==0x1AA3A0,"configured heap end");
    bool rejected=false;
    try { runtime.handleSyscall(ram,&ctx,0x83); } catch(const std::runtime_error &) { rejected=true; }
    require(rejected,"kernel signature scans remain unsupported");
    rejected=false;
    try { runtime.handleSyscall(ram,nullptr,0x7F); } catch(const std::invalid_argument &) { rejected=true; }
    require(rejected,"invalid service context");
    using Function=void (*)(uint8_t *,R5900Context *,PS2Runtime *);
    const Function allocateFunctions[]={ps2_stubs::calloc,ps2_stubs::memalign,ps2_stubs::malloc_r,
        ps2_stubs::calloc_r,ps2_stubs::memalign_r};
    for (unsigned i=0;i<5;++i) {
        if (i==0) args(2,16);
        if (i==1) args(64,32);
        if (i==2) args(0xDEADBEEF,32);
        if (i==3) args(0xDEADBEEF,2,16);
        if (i==4) args(0xDEADBEEF,64,32);
        allocateFunctions[i](ram,&ctx,&runtime); const auto pointer=getRegU32(&ctx,2);
        require(pointer && pointer%((i==1||i==4)?64:16)==0,"allocation provider ABI/alignment");
        require(ram[pointer]==0 && ram[pointer+31]==0,"allocation provider zero data");
        ram[pointer]=55; args(0xDEADBEEF,pointer,128); ps2_stubs::realloc_r(ram,&ctx,&runtime);
        const auto grown=getRegU32(&ctx,2);
        require(grown && ram[grown]==55 && ram[grown+64]==0,"reentrant realloc arguments and preservation");
        args(0xDEADBEEF,grown); ps2_stubs::free_r(ram,&ctx,&runtime);
        require(ram[grown]==0,"reentrant free argument");
    }
    args(0x12000000); rejected=false;
    try { ps2_stubs::free(ram,&ctx,&runtime); } catch(const std::invalid_argument &) { rejected=true; }
    require(rejected,"invalid allocation pointer rejected");
    rejected=false;
    try { ps2_stubs::malloc(nullptr,&ctx,&runtime); } catch(const std::invalid_argument &) { rejected=true; }
    require(rejected,"foreign allocation data base rejected");
    for (Function fn : {ps2_stubs::malloc,ps2_stubs::free,ps2_stubs::calloc,ps2_stubs::realloc,
            ps2_stubs::memalign,ps2_stubs::malloc_r,ps2_stubs::free_r,ps2_stubs::calloc_r,
            ps2_stubs::realloc_r,ps2_stubs::memalign_r}) {
        rejected=false;
        try { fn(ram,nullptr,&runtime); } catch(const std::invalid_argument &) { rejected=true; }
        require(rejected,"allocation providers validate context before argument access");
    }
    ps2_syscalls::GetMemorySize(ram,&ctx,&runtime); require(getRegU32(&ctx,2)==0x2000000,"typed data-size service");
    ps2_syscalls::EndOfHeap(ram,&ctx,&runtime); require(getRegU32(&ctx,2)==0x1AA3A0,"typed heap-end service");
    args(0x141D70,UINT32_MAX,0x20000); ps2_syscalls::SetupThread(ram,&ctx,&runtime);
    require(getRegU32(&ctx,2)==0x2000000,"top-of-data stack sentinel");
    PS2Runtime lowStack;
    R5900Context low;
    auto *lowRam=lowStack.memory().mainRam().data();
    low.r[5]=_mm_set_epi32(0,0,0,0x180000); low.r[6]=_mm_set_epi32(0,0,0,0x10000);
    lowStack.handleSyscall(lowRam,&low,0x3C);
    require(getRegU32(&low,2)==0x190000,"low data stack accepted");
    low.r[4]=_mm_set_epi32(0,0,0,0x170000); low.r[5]=_mm_set_epi32(0,0,0,0x20000);
    lowStack.handleSyscall(lowRam,&low,0x3D);
    require(getRegU32(&low,2)==UINT32_MAX && !lowStack.services().heap().base(),"actual heap/stack overlap rejected");
    std::printf("native boot/alloc: %u checks passed\n",checks);
}
