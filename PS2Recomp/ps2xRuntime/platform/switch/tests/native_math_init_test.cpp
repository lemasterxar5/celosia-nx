#include "native_math_init.hpp"
#include "native_memory.hpp"
#include "native_semaphore.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
uint32_t word(DataMemory &data,uint32_t at){uint32_t value;require(data.read(at,&value,4),"metadata readable");return value;}
void freeSlots(DataMemory &data){std::array<int32_t,256> slots;slots.fill(-1);data.write(0x13BB90,slots.data(),sizeof(slots));}
}
int main()
{
    DataMemory data;SemaphoreSet semaphores;freeSlots(data);
    const uint32_t canary=0xDEADBEEF;data.write(0x212EC4,&canary,4);
    initializeNativeMath(data,semaphores);
    const auto id=static_cast<int32_t>(word(data,0x202AB0));
    require(id>0 && word(data,0x13BB90)==uint32_t(id),"math stack has actual registered native mutex");
    require(semaphores.poll(id)==id && semaphores.poll(id)==SemaphoreSet::Empty && semaphores.signal(id)==id,
            "math mutex actually synchronizes native workers");
    require(word(data,0x1E0F60)==0 && word(data,0x1E0F84)==1,"quaternion and lookup readiness metadata initialized");
    std::array<float,4> quaternion{};data.read(0x202AC0,quaternion.data(),sizeof(quaternion));
    require(quaternion==std::array<float,4>{0,0,0,1},"quaternion starts with actual identity value");
    std::array<float,16385> cosine{};data.read(0x202EC0,cosine.data(),sizeof(cosine));
    require(cosine.front()==1 && cosine.back()==0 && std::abs(cosine[8192]-std::sqrt(0.5f))<0.000001f,
            "native cosine lookup matches verified source callee identities and endpoints");
    for(size_t index=1;index<cosine.size();++index)
        require(std::isfinite(cosine[index]) && cosine[index]<=cosine[index-1] && cosine[index]>=0,
                "cosine quarter wave remains finite descending and bounded");
    require(word(data,0x212EC4)==canary,"lookup writes exactly the original table extent");
    const float changed=0.25f;data.write(0x202EC0,&changed,4);
    initializeNativeMath(data,semaphores);
    float preserved=0;data.read(0x202EC0,&preserved,4);
    require(word(data,0x202AB0)==uint32_t(id) && preserved==changed,"repeat initialization preserves ready table and actual mutex");
    DataMemory full;std::array<int32_t,256> used;used.fill(1);full.write(0x13BB90,used.data(),sizeof(used));
    bool rejected=false;try{initializeNativeMath(full,semaphores);}catch(const std::runtime_error&){rejected=true;}
    require(rejected && word(full,0x1E0F84)==0 && word(full,0x202AB0)==0,"full resource list cannot publish math readiness");
    SemaphoreSet closed;closed.shutdown();DataMemory missing;freeSlots(missing);
    rejected=false;try{initializeNativeMath(missing,closed);}catch(const std::runtime_error&){rejected=true;}
    require(rejected && word(missing,0x1E0F84)==0,"failed native mutex cannot claim initialized math");
    std::printf("native math initialization: %u checks passed\n",checks);
}
