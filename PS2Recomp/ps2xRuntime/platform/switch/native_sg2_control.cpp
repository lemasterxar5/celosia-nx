#include "native_sg2_control.hpp"
#include "native_aot_context.hpp"
#include "native_iop.hpp"
#include "native_memory.hpp"
#include <span>
#include <stdexcept>
namespace celosia::native {
namespace {
constexpr uint32_t Ring=0x287540,Head=0x287500,Tail=0x287504,Scratch=0x289540;
constexpr uint32_t Transfers=0x29B740,TransferHead=0x29B728,TransferTail=0x29B72C;
constexpr uint32_t Reply=0x29B540,TickSlot=0x273D60,TickEntry=0x1D21E8;
uint32_t word(const DataMemory &data,uint32_t address)
{uint32_t value;if(!data.read(address,&value,4))throw std::out_of_range("Native sound data read failed");return value;}
void put(DataMemory &data,uint32_t address,uint32_t value)
{if(!data.write(address,&value,4))throw std::out_of_range("Native sound metadata write failed");}
void indexBound(uint32_t index,uint32_t count)
{if(index>=count)throw std::out_of_range("Native sound queue/status index exceeds bounds");}
}
struct NativeSg2Control::Plan {
    uint32_t head,tail,transferHead,transferTail,status=0;
    bool appendTransfer=false,pendingTransfer=false;
    size_t count=0;
};
void NativeSg2Control::requireReady() const
{
    if(!initialized || failed || !services.soundReady())
        throw std::runtime_error("Native sound controller is uninitialized, unavailable or failed");
}
void NativeSg2Control::initialize()
{
    if(failed || !services.soundReady()) throw std::runtime_error("Native sound provider is unavailable or failed");
    if(initialized) return;
    const auto entry=word(data,TickSlot);
    if(entry!=0x1CC028 && entry!=TickEntry) throw std::runtime_error("Native sound tick slot is occupied");
    if(!data.contains(Ring,8192) || !data.contains(Transfers,640) || !data.contains(Reply,400))
        throw std::out_of_range("Native sound application buffers exceed bounds");
    std::array<uint8_t,400> reply{};
    if(!services.submitSoundCommands({},reply)) throw std::runtime_error("Native sound initial status processing failed");
    if(!data.zero(Ring,8192) || !data.zero(Transfers,640) || !data.write(Reply,reply.data(),reply.size()))
        throw std::logic_error("Native sound validated initialization write failed");
    for(const auto at:{Head,Tail,TransferHead,TransferTail})put(data,at,0);
    put(data,TickSlot,TickEntry);put(data,TickSlot+4,0);initialized=true;
}
uint32_t NativeSg2Control::enqueue(const R5900Context &context,bool transfer)
{
    const uint32_t capacity=transfer?32:512,headAt=transfer?TransferHead:Head,tailAt=transfer?TransferTail:Tail;
    const auto head=word(data,headAt),tail=word(data,tailAt);
    indexBound(head,capacity);indexBound(tail,capacity);
    const auto next=(head+1)%capacity;
    if(next==tail)throw std::runtime_error("Native sound command queue is full");
    const auto at=(transfer?Transfers:Ring)+head*(transfer?20:16);
    const Command command{getRegU32(&context,4),getRegU32(&context,5),getRegU32(&context,6),getRegU32(&context,7)};
    if(!data.write(at,command.data(),sizeof(command)))throw std::logic_error("Native sound validated queue write failed");
    if(transfer)put(data,at+16,UINT32_MAX);
    put(data,headAt,next);return transfer?head:0;
}
NativeSg2Control::Plan NativeSg2Control::stage()
{
    Plan plan{word(data,Head),word(data,Tail),word(data,TransferHead),word(data,TransferTail)};
    indexBound(plan.head,512);indexBound(plan.tail,512);indexBound(plan.transferHead,32);indexBound(plan.transferTail,32);
    plan.count=(plan.head+512-plan.tail)%512;
    for(size_t index=0;index<plan.count;++index)
        if(!data.read(Ring+uint32_t((plan.tail+index)%512)*16,staged[index].data(),16))
            throw std::logic_error("Native sound validated command read failed");
    plan.pendingTransfer=plan.transferHead!=plan.transferTail;
    if(plan.pendingTransfer) {
        plan.status=word(data,Transfers+plan.transferTail*20+16);
        if(plan.status!=UINT32_MAX && plan.status!=UINT32_MAX-1)
            throw std::runtime_error("Native sound transfer command state is invalid");
        plan.appendTransfer=plan.status==UINT32_MAX;
        if(plan.appendTransfer) {
            if(plan.count>=511)throw std::runtime_error("Native sound ring lacks room for pending transfer command");
            if(!data.read(Transfers+plan.transferTail*20,staged[plan.count].data(),16))
                throw std::logic_error("Native sound transfer command read failed");
            ++plan.count;
        }
    }
    return plan;
}
void NativeSg2Control::commit(const Plan &plan,const std::array<uint8_t,400> &reply)
{
    if(!data.write(Scratch,staged.data(),plan.count*16) || !data.write(Reply,reply.data(),reply.size()))
        throw std::logic_error("Native sound validated batch/status write failed");
    auto head=plan.head;
    if(plan.appendTransfer) {
        if(!data.write(Ring+head*16,staged[plan.count-1].data(),16))throw std::logic_error("Native sound ring commit failed");
        head=(head+1)%512;put(data,Head,head);
        put(data,Transfers+plan.transferTail*20+16,UINT32_MAX-1);
    }
    put(data,Tail,head);
    const uint32_t busy=uint32_t(reply[0x184])|(uint32_t(reply[0x185])<<8)|(uint32_t(reply[0x186])<<16)|(uint32_t(reply[0x187])<<24);
    if(plan.pendingTransfer && !busy) {
        put(data,Transfers+plan.transferTail*20+16,0);
        put(data,TransferTail,(plan.transferTail+1)%32);
    }
}
void NativeSg2Control::tick()
{
    const auto plan=stage();std::array<uint8_t,400> reply{};
    try {
        if(!services.submitSoundCommands(std::span<const Command>(staged.data(),plan.count),reply))
            throw std::runtime_error("Native sound command processing failed; batch will not be retried");
        commit(plan,reply);
    } catch(...) {failed=true;throw;}
}
uint32_t NativeSg2Control::get(uint32_t address,uint32_t index)
{
    switch(address) {
    case 0x1D2150:indexBound(index,32);return word(data,Transfers+index*20+16);
    case 0x1D2170:indexBound(index,48);return word(data,Reply+0xC0+index*4);
    case 0x1D2188:return word(data,Reply+0x180);
    case 0x1D2198:indexBound(index,48);return word(data,Reply+index*4);
    case 0x1D21B0:indexBound(index,2);return word(data,Reply+0x188+index*4);
    default:throw std::logic_error("Native sound status entry is invalid");
    }
}
bool NativeSg2Control::handle(uint32_t address,R5900Context &context)
{
    switch(address) {
    case 0x1D23D8:initialize();setReturnU32(&context,0);break;
    case 0x1D2010:case 0x1D20A0:
        requireReady();setReturnU32(&context,enqueue(context,address==0x1D20A0));break;
    case 0x1D2150:case 0x1D2170:case 0x1D2188:case 0x1D2198:case 0x1D21B0:
        requireReady();setReturnS32(&context,static_cast<int32_t>(get(address,getRegU32(&context,4))));break;
    case 0x1D21C8:requireReady();setReturnU32(&context,0);break;
    case 0x1D21E8:requireReady();tick();setReturnU32(&context,0);break;
    default:return false;
    }
    context.pc=getRegU32(&context,31);return true;
}
}
