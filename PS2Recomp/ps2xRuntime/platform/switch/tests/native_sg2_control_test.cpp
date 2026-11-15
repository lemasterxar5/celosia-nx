#include "native_sg2_control.hpp"
#include "native_aot_context.hpp"
#include "native_memory.hpp"
#include "native_heap.hpp"
#include "native_iop.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
template<class Action>void rejects(Action action){bool caught=false;try{action();}catch(const std::exception&){caught=true;}require(caught,"invalid/failed sound operation rejects");}
uint32_t word(DataMemory &data,uint32_t at){uint32_t result;require(data.read(at,&result,4),"read sound metadata");return result;}
void put(DataMemory &data,uint32_t at,uint32_t value){require(data.write(at,&value,4),"write sound fixture");}
void args(R5900Context &context,uint32_t a,uint32_t b=0,uint32_t c=0,uint32_t d=0)
{for(unsigned index=0;const auto value:{a,b,c,d})context.r[4+index++]=_mm_set_epi32(0,0,0,value);}
}
int main()
{
    DataMemory data;DataHeap heap(data);NativeServices services(data,heap);
    NativeSg2Control control(data,services);R5900Context context;context.r[31]=_mm_set_epi32(0,0,0,0x1000);
    require(!control.handle(0x1234,context),"unrelated functions remain unhandled");
    rejects([&]{control.handle(0x1D23D8,context);});
    services.initialize();require(services.services().loadModule("SG2IOPM1.IRX").moduleId>0,"actual native sound provider loaded");
    put(data,0x273D60,0x1234);put(data,0x287540,0xDEADBEEF);
    rejects([&]{control.handle(0x1D23D8,context);});
    require(word(data,0x287540)==0xDEADBEEF,"occupied tick slot fails before ring mutation");
    put(data,0x273D60,0x1CC028);put(data,0x29B700,0xAABBCCDD);
    require(control.handle(0x1D23D8,context) && getRegU32(&context,2)==0 && context.pc==0x1000,"real native sound initialization returns source ABI");
    require(word(data,0x273D60)==0x1D21E8 && word(data,0x273D64)==0,"actual tick installed in ordinary application callback slot");
    require(word(data,0x29B700)==0xAABBCCDD,"no synthetic SIF client/server state created");
    args(context,1,0,123,321);control.handle(0x1D2010,context);
    require(word(data,0x287500)==1 && word(data,0x287540)==1 && word(data,0x28754C)==321,"command queue preserves four source words");
    control.handle(0x1D23D8,context);require(word(data,0x287500)==1,"repeat init preserves queued application work");
    control.handle(0x1D21E8,context);
    require(word(data,0x287504)==1 && word(data,0x289540)==1,"successful real mixer submission commits queue tail and staged batch");
    control.handle(0x1D21C8,context);require(getRegU32(&context,2)==0,"synchronous status is idle after actual processing");
    args(context,1,0,40,50);control.handle(0x1D20A0,context);const auto slot=getRegU32(&context,2);
    require(slot==0 && word(data,0x29B750)==UINT32_MAX,"sound transfer command queued with pending status");
    control.handle(0x1D21E8,context);args(context,slot);control.handle(0x1D2150,context);
    require(getRegU32(&context,2)==0 && word(data,0x29B72C)==1,"transfer completion follows actual successful mixer reply");
    args(context,32);rejects([&]{control.handle(0x1D2150,context);});
    args(context,48);rejects([&]{control.handle(0x1D2170,context);});
    rejects([&]{control.handle(0x1D2198,context);});
    args(context,2);rejects([&]{control.handle(0x1D21B0,context);});
    args(context,1,0,20,30);
    for(unsigned index=0;index<511;++index)control.handle(0x1D2010,context);
    const auto full=word(data,0x287500);rejects([&]{control.handle(0x1D2010,context);});
    require(word(data,0x287500)==full,"full command ring never overwrites unconsumed work");
    control.handle(0x1D21E8,context);require(word(data,0x287504)==full,"full valid ring can be processed and recovered");
    for(unsigned index=0;index<31;++index)control.handle(0x1D20A0,context);
    const auto dmaHead=word(data,0x29B728);rejects([&]{control.handle(0x1D20A0,context);});
    require(word(data,0x29B728)==dmaHead,"full transfer command ring never overwrites work");
    for(unsigned index=0;index<31;++index)control.handle(0x1D21E8,context);
    require(word(data,0x29B72C)==dmaHead,"one transfer per tick preserves FIFO completion");
    const auto tail=word(data,0x287504);args(context,0xFFFFFFFF);control.handle(0x1D2010,context);
    put(data,0x29B540,0xDEADBEEF);rejects([&]{control.handle(0x1D21E8,context);});
    require(word(data,0x287504)==tail && word(data,0x29B540)==0xDEADBEEF,"rejected batch commits neither tail nor false reply");
    rejects([&]{control.handle(0x1D21C8,context);});
    control.reset();put(data,0x273D60,0x1CC028);control.handle(0x1D23D8,context);
    services.reset();rejects([&]{control.handle(0x1D21C8,context);});
    std::printf("native SG2 control: %u checks passed\n",checks);
}
