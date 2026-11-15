#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "ps2_syscalls.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1040;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=16;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[16]{};
namespace {
unsigned checks=0;
std::atomic<int32_t> observedId=0,observedArgument=0,observedSleep=-99;
std::atomic<uint32_t> observedStack=0,observedGp=0,observedT9=0;
void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
void reg(R5900Context &c,unsigned n,uint32_t v) { c.r[n]=_mm_set_epi32(0,0,0,v); }
int32_t result(const R5900Context &c) { return static_cast<int32_t>(getRegU32(&c,2)); }
template<class F> void until(F predicate,const char *why)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) { if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error(why); std::this_thread::yield(); }
}
void worker(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    observedArgument=getRegU32(ctx,4); observedStack=getRegU32(ctx,29);
    observedGp=getRegU32(ctx,28); observedT9=getRegU32(ctx,25);
    ps2_syscalls::GetThreadId(ram,ctx,runtime); observedId=result(*ctx);
    ps2_syscalls::SleepThread(ram,ctx,runtime); observedSleep=result(*ctx);
    ps2_syscalls::ExitThread(ram,ctx,runtime);
    throw std::runtime_error("ExitThread returned");
}
void badWorker(uint8_t *,R5900Context *,PS2Runtime *) { throw std::runtime_error("injected compiled worker failure"); }
void deleteWorker(uint8_t *ram,R5900Context *ctx,PS2Runtime *r)
{ ps2_syscalls::ExitDeleteThread(ram,ctx,r); throw std::runtime_error("ExitDeleteThread returned"); }
void wakeMain(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{ ps2_syscalls::WakeupThread(ram,ctx,runtime); ctx->pc=PS2Runtime::ReturnSentinel; }
void rootPump(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    ps2_syscalls::StartThread(ram,ctx,runtime);
    if(result(*ctx)!=0) throw std::runtime_error("root start failed");
    ps2_syscalls::SleepThread(ram,ctx,runtime);
    if(result(*ctx)!=0) throw std::runtime_error("root sleep failed");
    ctx->pc=PS2Runtime::ReturnSentinel;
}
using Descriptor=std::array<uint32_t,9>;
Descriptor valid(){ return {0,0x1000,0x8000,0x1000,0x4000,32,0,0,0}; }
int32_t create(PS2Runtime &runtime,R5900Context &c,const Descriptor &d)
{ runtime.memory().write(0x200,d.data(),sizeof(d)); reg(c,4,0x200); ps2_syscalls::CreateThread(runtime.memory().mainRam().data(),&c,&runtime); return result(c); }
}
int main()
{
    g_ps2RecompiledFunctionTable[0]=worker;
    g_ps2RecompiledFunctionTable[1]=badWorker;
    g_ps2RecompiledFunctionTable[2]=deleteWorker;
    g_ps2RecompiledFunctionTable[3]=wakeMain;
    g_ps2RecompiledFunctionTable[4]=rootPump;
    PS2Runtime runtime; R5900Context c;
    auto *ram=runtime.memory().mainRam().data();
    ps2_syscalls::InitThread(ram,&c,&runtime); const auto main=result(c);
    require(main>0,"actual main registered");
    reg(c,4,main); reg(c,5,128); ps2_syscalls::ChangeThreadPriority(ram,&c,&runtime);
    require(result(c)==-403,"source priority bound checked before OS update");
    reg(c,4,0x7FFFFFFF); reg(c,5,1); ps2_syscalls::ChangeThreadPriority(ram,&c,&runtime);
    require(result(c)==-408,"unknown native thread cannot receive priority");
    ps2_syscalls::GetThreadId(ram,&c,&runtime); require(result(c)==main,"main ID from native TLS");
    auto d=valid();
    { auto zeroGp=d; zeroGp[4]=0; require(create(runtime,c,zeroGp)==-1,"unsupported StartThread GP inheritance cannot use creator GP"); }
    for(unsigned field : {1u,2u,3u,4u,5u,7u}) {
        auto bad=d;
        bad[field]=field==1 ? 0x103C : field==2 ? 0xFFFFFFF0 : field==3 ? 0 : field==4 ? 0x12000000 : field==5 ? 128 : 1;
        require(create(runtime,c,bad)<0,"invalid descriptor rejected");
    }
    reg(c,4,0x1FFFFF0); ps2_syscalls::CreateThread(ram,&c,&runtime); require(result(c)==-1,"truncated descriptor");
    const auto child=create(runtime,c,d); require(child>main,"created compiled callback");
    // Create copied descriptor data, so reusing the temporary game stack is safe.
    runtime.memory().zero(0x200,sizeof(d));
    reg(c,4,child); reg(c,5,77); ps2_syscalls::StartThread(ram,&c,&runtime); require(result(c)==0,"StartThread ABI returns zero");
    until([&]{ return runtime.services().threads().status(child)->sleeping; },"compiled worker did not sleep");
    require(observedId==child && observedArgument==77,"compiled worker ID and argument");
    require(observedStack==0x9000 && observedGp==0x4000 && observedT9==0x1000,"compiled context stack GP T9");
    reg(c,4,child); ps2_syscalls::DeleteThread(ram,&c,&runtime); require(result(c)==-414,"cannot delete running task");
    ps2_syscalls::WakeupThread(ram,&c,&runtime); require(result(c)==0,"WakeupThread ABI returns zero");
    until([&]{ return runtime.services().threads().status(child)->finished; },"worker did not exit");
    require(observedSleep==0 && !runtime.services().threads().status(child)->failed,"native sleep and normal exit");
    reg(c,4,child); reg(c,5,88); ps2_syscalls::StartThread(ram,&c,&runtime);
    require(result(c)==0,"ordinary ExitThread task restarts under original id");
    until([&]{ return runtime.services().threads().status(child)->sleeping; },"restarted worker sleep");
    require(observedArgument==88 && observedId==child,"restart preserves identity but replaces argument");
    ps2_syscalls::iWakeupThread(ram,&c,&runtime);
    until([&]{ return runtime.services().threads().status(child)->finished; },"restarted worker exit");
    ps2_syscalls::DeleteThread(ram,&c,&runtime); require(result(c)==0 && !runtime.services().threads().status(child),"DeleteThread joins and returns zero");
    reg(c,4,child); ps2_syscalls::WakeupThread(ram,&c,&runtime); require(result(c)==-408,"unknown target");
    reg(c,4,main); ps2_syscalls::WakeupThread(ram,&c,&runtime); require(result(c)==-406,"ordinary self wake illegal");
    ps2_syscalls::iWakeupThread(ram,&c,&runtime); require(result(c)==0,"interrupt-style self wake retains permit");
    ps2_syscalls::SleepThread(ram,&c,&runtime); require(result(c)==0,"main consumes retained wake");
    const auto dormant=create(runtime,c,d); reg(c,4,dormant); ps2_syscalls::WakeupThread(ram,&c,&runtime); require(result(c)==-413,"dormant wake error");
    ps2_syscalls::DeleteThread(ram,&c,&runtime); require(result(c)==0,"delete dormant task");
    d[1]=0x100C; const auto waking=create(runtime,c,d);
    reg(c,4,waking); reg(c,5,main); c.pc=0x1010;
    require(runtime.runSlice(c,100)==PS2Runtime::SliceResult::Returned,"compiled main sleep drops execution gate for compiled child wake");
    until([&]{ return runtime.services().threads().status(waking)->finished; },"waking worker exit");
    reg(c,4,waking); ps2_syscalls::DeleteThread(ram,&c,&runtime); require(result(c)==0,"gated child cleanup");
    d[1]=0x1008; const auto deleting=create(runtime,c,d); reg(c,4,deleting); ps2_syscalls::StartThread(ram,&c,&runtime);
    until([&]{ return runtime.services().threads().status(deleting)->finished; },"self-delete worker not done");
    require(!runtime.services().threads().status(deleting)->failed,"ExitDeleteThread normal control flow");
    reg(c,4,deleting); ps2_syscalls::DeleteThread(ram,&c,&runtime); require(result(c)==-408,"logical self-delete hides task id immediately after exit");
    d[1]=0x1004; const auto bad=create(runtime,c,d);
    require(!runtime.services().threads().status(deleting),"next CreateThread reaps self-deleted native worker"); reg(c,4,bad); ps2_syscalls::StartThread(ram,&c,&runtime);
    until([&]{ return runtime.services().threads().status(bad)->finished; },"error worker not done");
    const auto failure=runtime.services().taskFailure();
    require(failure.find("PC=0x00001004")!=std::string::npos &&
            failure.find("RA=0xfffffffc")!=std::string::npos &&
            failure.find("injected compiled worker failure")!=std::string::npos,
            "compiled task error includes actual failing worker context and cause");
    require(runtime.services().threads().stopRequested(),"compiled task failure stops runtime and all tasks");
    std::printf("native thread ABI: %u checks passed\n",checks);
}
