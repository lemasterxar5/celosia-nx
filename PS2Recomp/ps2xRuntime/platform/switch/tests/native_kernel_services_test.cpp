#include "native_kernel_services.hpp"
#include "native_memory.hpp"
#include "native_semaphore.hpp"
#include "native_aot_context.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
uint64_t readTick(void *p){return *static_cast<uint64_t *>(p);}
uint32_t word(DataMemory &data,uint32_t at){uint32_t value;require(data.read(at,&value,4),"read metadata");return value;}
template<class Action>void rejects(Action action){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}require(rejected,"required service failure propagates");}
void freeSlots(DataMemory &data){std::array<int32_t,256> slots;slots.fill(-1);require(data.write(0x13BB90,slots.data(),sizeof(slots)),"external resource list initialized");}
}
int main()
{
    DataMemory data;SemaphoreSet semaphores;uint64_t ticks=0;
    freeSlots(data);
    NativeKernelServices services(data,semaphores,{readTick,&ticks,1000000000});
    R5900Context context;context.r[31]=_mm_set_epi32(0,0,0,0x1000);
    require(!services.handle(0x1234,context),"unknown function remains unhandled");
    rejects([&]{services.handle(0x1AD510,context);});
    require(services.handle(0x1AD460,context) && context.pc==0x1000,"native kernel initialization returns to caller");
    const auto id=static_cast<int32_t>(word(data,0x1E0BE8));
    require(id>0 && word(data,0x13BB90)==uint32_t(id),"actual native semaphore registered in source resource list");
    require(word(data,0x1FD8A8)==UINT32_MAX && word(data,0x1FD8AC)==0,"ordinary kernel metadata initialized");
    require(semaphores.poll(id)==id && semaphores.poll(id)==SemaphoreSet::Empty && semaphores.signal(id)==id,
            "kernel semaphore performs actual native synchronization");
    ticks=1000000000;
    require(services.handle(0x1AD528,context) && static_cast<uint64_t>(_mm_cvtsi128_si64(context.r[2]))==150000000,
            "extended duration derives from real monotonic source in documented units");
    require(services.handle(0x1AD510,context) && static_cast<uint64_t>(_mm_cvtsi128_si64(context.r[2]))==(150000000ull&~65535ull),
            "coarse duration preserves source API precision without timer interrupts");
    require(services.handle(0x1AD460,context) && word(data,0x1E0BE8)==uint32_t(id),"repeated initialization does not leak resources");
    ticks=0;rejects([&]{services.handle(0x1AD528,context);});ticks=2000000000;
    services.reset();
    require(!semaphores.status(id) && word(data,0x13BB90)==UINT32_MAX && word(data,0x1E0BE8)==UINT32_MAX,
            "reset removes owned native semaphore and matching source references");
    services.reset();rejects([&]{services.handle(0x1AD510,context);});
    std::array<int32_t,256> full;full.fill(7);data.write(0x13BB90,full.data(),sizeof(full));
    rejects([&]{services.handle(0x1AD460,context);});
    require(word(data,0x13BB90)==7 && word(data,0x1E0BE8)==UINT32_MAX,"full external list failure leaves metadata intact");
    freeSlots(data);
    require(services.handle(0x1AD460,context),"native services restart after reset");
    const auto next=static_cast<int32_t>(word(data,0x1E0BE8));
    require(next==id+1,"full external list failure did not allocate a semaphore");
    // External source cleanup may already have removed its semaphore. Reset
    // must not erase an unrelated replacement reference in that data slot.
    require(semaphores.erase(next)==next,"source cleanup removed native semaphore");
    const uint32_t replacement=123;data.write(0x13BB90,&replacement,4);services.reset();
    require(word(data,0x13BB90)==replacement,"reset preserves a replaced external reference");
    SemaphoreSet closed;closed.shutdown();freeSlots(data);
    NativeKernelServices failed(data,closed,{readTick,&ticks,1000000000});
    rejects([&]{failed.handle(0x1AD460,context);});
    require(word(data,0x13BB90)==UINT32_MAX,"closed native semaphore set cannot publish success");
    std::printf("native kernel services: %u checks passed\n",checks);
}
