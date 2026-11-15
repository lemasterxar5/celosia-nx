#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "ps2_syscalls.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
int main()
{
    PS2Runtime runtime;
    R5900Context context;
    auto *ram=runtime.memory().mainRam().data();
    auto arg=[&](uint32_t a,uint32_t b=0) { context.r[4]=_mm_set_epi32(0,0,0,a); context.r[5]=_mm_set_epi32(0,0,0,b); };
    const std::array<uint32_t,6> descriptor{UINT32_MAX,2,1,123,0,0x138250};
    runtime.memory().write(0x100,descriptor.data(),sizeof(descriptor));
    arg(0x100); ps2_syscalls::CreateSema(ram,&context,&runtime);
    const auto id=getRegU32(&context,2); require(id>0 && id<INT32_MAX,"native semaphore created using init field");
    arg(id,0x200); ps2_syscalls::ReferSemaStatus(ram,&context,&runtime);
    std::array<uint32_t,6> status{}; runtime.memory().read(0x200,status.data(),sizeof(status));
    require(getRegU32(&context,2)==0 && status==std::array<uint32_t,6>{1,2,1,0,0,0x138250},"complete semaphore status ABI");
    arg(id); ps2_syscalls::PollSema(ram,&context,&runtime); require(getRegU32(&context,2)==id,"poll consumes native token");
    ps2_syscalls::iPollSema(ram,&context,&runtime); require(static_cast<int32_t>(getRegU32(&context,2))==-419,"empty native poll");
    ps2_syscalls::SignalSema(ram,&context,&runtime); ps2_syscalls::iSignalSema(ram,&context,&runtime);
    ps2_syscalls::SignalSema(ram,&context,&runtime); require(static_cast<int32_t>(getRegU32(&context,2))==-420,"maximum count enforced");
    ps2_syscalls::WaitSema(ram,&context,&runtime); ps2_syscalls::WaitSema(ram,&context,&runtime);
    std::promise<void> entered;
    auto ready=entered.get_future();
    auto waiter=std::async(std::launch::async,[&] {
        R5900Context threadContext; threadContext.r[4]=_mm_set_epi32(0,0,0,id);
        entered.set_value(); ps2_syscalls::WaitSema(ram,&threadContext,&runtime);
        return static_cast<int32_t>(getRegU32(&threadContext,2));
    });
    ready.wait();
    require(waiter.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"wait really blocks native thread");
    ps2_syscalls::SignalSema(ram,&context,&runtime);
    if(waiter.wait_for(std::chrono::seconds(2))!=std::future_status::ready) {
        runtime.requestStop(); throw std::runtime_error("native wait deadlocked against signal");
    }
    require(waiter.get()==static_cast<int32_t>(id),"signal releases native waiter without bootstrap lock deadlock");
    arg(id); ps2_syscalls::DeleteSema(ram,&context,&runtime); require(getRegU32(&context,2)==id,"native delete");
    ps2_syscalls::PollSema(ram,&context,&runtime); require(static_cast<int32_t>(getRegU32(&context,2))==-408,"deleted ID invalid");
    arg(0x12000000); ps2_syscalls::CreateSema(ram,&context,&runtime);
    require(getRegU32(&context,2)==UINT32_MAX,"descriptor cannot address devices");
    arg(0x100); runtime.handleSyscall(ram,&context,0x40); const auto next=getRegU32(&context,2);
    require(next!=id && next>0 && next<INT32_MAX,"native IDs not reused");
    arg(next); runtime.requestStop(); ps2_syscalls::WaitSema(ram,&context,&runtime);
    require(static_cast<int32_t>(getRegU32(&context,2))<0,"shutdown closes pending and future waits");
    std::printf("native sync ABI: %u checks passed\n",checks);
}
