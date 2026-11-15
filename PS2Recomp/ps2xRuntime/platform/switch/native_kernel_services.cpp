#include "native_kernel_services.hpp"
#include "native_aot_context.hpp"
#include "native_memory.hpp"
#include "native_semaphore.hpp"
#include <cstdio>
#include <stdexcept>
namespace celosia::native {
namespace {
constexpr uint64_t KernelHz=150000000; // iosGetSecondFromTCount@1AD550 divisor.
constexpr uint32_t ExternalSemaphores=0x13BB90,KernelMutex=0x1E0BE8;
uint32_t readWord(const DataMemory &data,uint32_t address)
{
    uint32_t value;
    if(!data.read(address,&value,4)) throw std::out_of_range("Native kernel metadata read failed");
    return value;
}
void writeWord(DataMemory &data,uint32_t address,uint32_t value)
{
    if(!data.write(address,&value,4)) throw std::out_of_range("Native kernel metadata write failed");
}
}
NativeKernelServices::NativeKernelServices(DataMemory &memory,SemaphoreSet &sync)
    : data(memory),semaphores(sync),clock(KernelHz) {}
NativeKernelServices::NativeKernelServices(DataMemory &memory,SemaphoreSet &sync,ClockSource source)
    : data(memory),semaphores(sync),clock(source,KernelHz) {}
NativeKernelServices::~NativeKernelServices()
{
    try{reset();}catch(...){std::fputs("Native kernel service cleanup failed\n",stderr);}
}
void NativeKernelServices::reset()
{
    if(semaphore>0) {
        const auto id=static_cast<uint32_t>(semaphore);
        if(externalSlot && readWord(data,externalSlot)==id) writeWord(data,externalSlot,UINT32_MAX);
        if(readWord(data,KernelMutex)==id) writeWord(data,KernelMutex,UINT32_MAX);
        (void)semaphores.erase(semaphore); // Source resource cleanup may already have removed it.
    }
    semaphore=-1;externalSlot=0;(void)clock.end();
}
void NativeKernelServices::initialize()
{
    if(semaphore>0) {
        if(semaphores.status(semaphore) && readWord(data,externalSlot)==uint32_t(semaphore) &&
           readWord(data,KernelMutex)==uint32_t(semaphore)) return;
        reset();
    }
    uint32_t slot=0;
    for(unsigned index=0;index<256;++index) {
        const auto address=ExternalSemaphores+index*4;
        if(static_cast<int32_t>(readWord(data,address))<0) {slot=address;break;}
    }
    if(!slot) throw std::runtime_error("Native kernel external semaphore list is full");
    const auto id=semaphores.create(1,1,0,0);
    if(id<=0) throw std::runtime_error("Native kernel mutex creation failed");
    try {
        if(!clock.initialize() || !clock.start()) throw std::logic_error("Native kernel clock initialization failed");
        writeWord(data,slot,static_cast<uint32_t>(id));
        writeWord(data,KernelMutex,static_cast<uint32_t>(id));
        writeWord(data,0x1FD8A8,UINT32_MAX);writeWord(data,0x1FD8AC,0);
        semaphore=id;externalSlot=slot;
    } catch(...) {(void)semaphores.erase(id);(void)clock.end();throw;}
}
bool NativeKernelServices::handle(uint32_t address,R5900Context &context)
{
    if(address==0x1AD460) {
        initialize();
        setReturnS32(&context,-1); // Final v0 of the original void initializer.
    } else if(address==0x1AD510 || address==0x1AD528) {
        if(semaphore<=0) throw std::runtime_error("Native kernel time queried before initialization");
        const auto elapsed=clock.now();
        setReturnU64(&context,address==0x1AD510 ? elapsed&~uint64_t(0xFFFF) : elapsed);
    } else return false;
    context.pc=getRegU32(&context,31);
    return true;
}
}
