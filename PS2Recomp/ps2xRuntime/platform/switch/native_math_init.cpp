#include "native_math_init.hpp"
#include "native_memory.hpp"
#include "native_semaphore.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace celosia::native {
namespace {
constexpr uint32_t MathMutex=0x202AB0,Table=0x202EC0,TableReady=0x1E0F84;
constexpr size_t TableValues=16385;
uint32_t word(const DataMemory &data,uint32_t address)
{
    uint32_t value;
    if(!data.read(address,&value,4)) throw std::out_of_range("Native math data read failed");
    return value;
}
void store(DataMemory &data,uint32_t address,uint32_t value)
{
    if(!data.write(address,&value,4)) throw std::out_of_range("Native math data write failed");
}
void initializeTable(DataMemory &data)
{
    if(word(data,TableReady)) return;
    std::array<float,TableValues> values{};
    const float halfPi=std::bit_cast<float>(uint32_t(0x3FC90FDA));
    // InitTableSinCos@1BE5B8 calls cosf@1DD7C8 in the lower half and
    // sinf@1DD8C8 then sqrt(1-sin^2) above it. This is a cosine lookup.
    for(size_t index=0;index<values.size();++index) {
        const float fraction=static_cast<float>(index)/16384.0f;
        const float angle=fraction*halfPi;
        if(fraction<=0.5f) values[index]=std::cos(angle);
        else {
            const float sine=std::sin(angle);
            volatile float square=sine*sine; // Source rounds multiply before subtract.
            values[index]=std::sqrt(1.0f-square);
        }
    }
    if(!data.write(Table,values.data(),sizeof(values))) throw std::out_of_range("Native math table write failed");
    store(data,TableReady,1);
}
void initializeMutex(DataMemory &data,SemaphoreSet &semaphores)
{
    const auto previous=static_cast<int32_t>(word(data,MathMutex));
    if(previous>0 && semaphores.status(previous)) return;
    uint32_t slot=0;
    for(unsigned index=0;index<256;++index) {
        const auto at=0x13BB90+index*4;
        if(static_cast<int32_t>(word(data,at))<0) {slot=at;break;}
    }
    if(!slot) throw std::runtime_error("Native math external semaphore list is full");
    const auto id=semaphores.create(1,1,0,0);
    if(id<=0) throw std::runtime_error("Native math mutex creation failed");
    store(data,slot,static_cast<uint32_t>(id));store(data,MathMutex,static_cast<uint32_t>(id));
}
}
void initializeNativeMath(DataMemory &data,SemaphoreSet &semaphores)
{
    if(!data.contains(Table,TableValues*sizeof(float)) || !data.contains(0x202AC0,16) ||
       !data.contains(0x13BB90,256*4)) throw std::out_of_range("Native math data bounds failed");
    initializeMutex(data,semaphores);
    const std::array<float,4> identity{0,0,0,1};
    store(data,0x1E0F60,0);
    if(!data.write(0x202AC0,identity.data(),sizeof(identity))) throw std::out_of_range("Native quaternion write failed");
    initializeTable(data);
}
}
