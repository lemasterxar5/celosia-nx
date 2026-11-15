#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "ps2_syscalls.h"
#include <array>
#include <exception>
#include <stdexcept>
#include <cstdio>
namespace {
constexpr int32_t IllegalPriority=-403,IllegalId=-406,Dormant=-413,NotDormant=-414;
struct NativeThreadExit final : std::exception {};
struct Descriptor { uint32_t entry,stack,size,gp,priority; };
void validate(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    if(!runtime || !context || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native thread call: invalid context/data base");
}
int32_t decode(PS2Runtime &runtime,const R5900Context &context,Descriptor &out)
{
    const auto address=getRegU32(&context,4);
    std::array<uint32_t,9> words{};
    if(!address || address>=celosia::native::DataMemory::MainBytes ||
       sizeof(words)>celosia::native::DataMemory::MainBytes-address ||
       !runtime.memory().read(address,words.data(),sizeof(words))) return -1;
    if(words[5]<1 || words[5]>127) return IllegalPriority;
    const auto gp=words[4];
    const auto stack=words[2],size=words[3];
    // GP=0 needs StartThread-caller inheritance, which is not ported yet.
    // Reject it rather than capturing the wrong creator's GP.
    if(!gp || words[7] || !runtime.guestFunctionAt(words[1]) || !stack || size<16 ||
       ((stack|size)&15) || stack>=celosia::native::DataMemory::MainBytes ||
       size>celosia::native::DataMemory::MainBytes-stack || gp>=celosia::native::DataMemory::MainBytes)
        return -1;
    out={words[1],stack,size,gp,words[5]};
    return 0;
}
void runTask(PS2Runtime &runtime,const Descriptor &task,uint32_t argument)
{
    R5900Context context;
    context.pc=task.entry;
    const auto set=[&](unsigned reg,uint32_t value) { context.r[reg]=_mm_set_epi32(0,0,0,value); };
    set(4,argument); set(25,task.entry); set(28,task.gp); set(29,task.stack+task.size);
    set(31,PS2Runtime::ReturnSentinel);
    try {
        while(!runtime.services().threads().stopRequested()) {
            const auto result=runtime.runSlice(context,16384);
            if(result!=PS2Runtime::SliceResult::Yielded) break;
        }
    } catch(const NativeThreadExit &) {
        // Both explicit exits unwind the actual native callback. A self-delete
        // is marked before unwinding and joined/reaped by another native task.
    } catch(const std::exception &error) {
        std::array<char,1024> detail{};
        std::snprintf(detail.data(),detail.size(),"Native task PC=0x%08x RA=0x%08x SP=0x%08x: %s",
                      context.pc,getRegU32(&context,31),getRegU32(&context,29),error.what());
        runtime.services().recordTaskFailure(detail.data()); runtime.requestStop(); throw;
    } catch(...) {
        runtime.services().recordTaskFailure("Unknown native compiled task failure"); runtime.requestStop(); throw;
    }
}
int32_t wakeTask(PS2Runtime &runtime,int32_t id,bool interruptStyle)
{
    auto &threads=runtime.services().threads();
    if(id<=0 || (!interruptStyle && id==threads.currentId())) return IllegalId;
    const auto status=threads.status(id);
    if(!status || status->retired) return celosia::native::NativeThreads::Unknown;
    if(!status->started || status->finished) return Dormant;
    const auto result=threads.wake(id);
    return result>0 ? 0 : result==celosia::native::NativeThreads::Busy ? Dormant : result;
}
}
namespace ps2_syscalls {
void InitThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    setReturnS32(ctx,runtime->services().threads().registerMain());
}
void GetThreadId(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    setReturnS32(ctx,runtime->services().threads().currentId());
}
void CreateThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    Descriptor descriptor{};
    const auto result=decode(*runtime,*ctx,descriptor);
    if(result) { setReturnS32(ctx,result); return; }
    auto &threads=runtime->services().threads();
    bool reaped=false;
    runtime->nativeBlocking([&] { reaped=threads.reapRetired(); });
    if(!reaped) { setReturnS32(ctx,-1); return; }
    const int priority=0x2C+int((descriptor.priority-1)*15/126);
    const auto id=threads.create([runtime,descriptor](uint32_t,uint32_t argument) {
        runTask(*runtime,descriptor,argument);
    },priority,0x100000);
    setReturnS32(ctx,id);
}
void StartThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    const auto id=static_cast<int32_t>(getRegU32(ctx,4));
    const auto argument=getRegU32(ctx,5);
    int32_t result=-1;
    runtime->nativeBlocking([&] { result=runtime->services().threads().start(id,argument); });
    if(result==celosia::native::NativeThreads::Busy) result=NotDormant;
    setReturnS32(ctx,result>0 ? 0 : result);
}
void DeleteThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    const auto id=static_cast<int32_t>(getRegU32(ctx,4));
    if(id<=1) { setReturnS32(ctx,IllegalId); return; }
    const auto state=runtime->services().threads().status(id);
    if(!state || state->retired) { setReturnS32(ctx,celosia::native::NativeThreads::Unknown); return; }
    int32_t result=-1;
    runtime->nativeBlocking([&] { result=runtime->services().threads().erase(id); });
    if(result==celosia::native::NativeThreads::Busy) result=NotDormant;
    setReturnS32(ctx,result>0 ? 0 : result);
}
void SleepThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    int32_t result=-1;
    runtime->nativeBlocking([&] { result=runtime->services().threads().sleepCurrent(); });
    setReturnS32(ctx,result>0 ? 0 : result);
}
void WakeupThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    setReturnS32(ctx,wakeTask(*runtime,static_cast<int32_t>(getRegU32(ctx,4)),false));
}
void iWakeupThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    setReturnS32(ctx,wakeTask(*runtime,static_cast<int32_t>(getRegU32(ctx,4)),true));
}
void ChangeThreadPriority(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    const auto priority=getRegU32(ctx,5);
    if(priority<1 || priority>127) { setReturnS32(ctx,IllegalPriority); return; }
    auto id=static_cast<int32_t>(getRegU32(ctx,4));
    if(!id) id=runtime->services().threads().currentId();
    const auto result=runtime->services().threads().setPriority(id,0x2C+int((priority-1)*15/126));
    setReturnS32(ctx,result>0 ? 0 : result);
}
void iChangeThreadPriority(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{ ChangeThreadPriority(ram,ctx,runtime); }
void ExitThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    if(runtime->services().threads().currentId()<=1)
        throw std::runtime_error("Native main-task exit is not ported");
    throw NativeThreadExit{};
}
void ExitDeleteThread(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
{
    validate(ram,ctx,runtime);
    if(runtime->services().threads().retireCurrent()<0)
        throw std::runtime_error("Native main/unbound task self-delete is not ported");
    throw NativeThreadExit{};
}
}

