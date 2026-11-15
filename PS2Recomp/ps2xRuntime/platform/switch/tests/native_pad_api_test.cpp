#include "native_pad_api.hpp"
#include "native_input.hpp"
#include "native_memory.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
using Packet=std::array<uint8_t,18>;
Packet packet(DataMemory &memory,uint32_t address)
{ Packet result{}; if(!memory.read(address,result.data(),result.size())) throw std::runtime_error("test packet read"); return result; }
}
int main()
{
    DataMemory memory;
    NativePadSample sample{}; unsigned polls=0;
    NativeInput input([&]{++polls; return sample;});
    NativePadApi api(input,memory);
    std::array<uint32_t,3> params{2,0,0};
    memory.write(0x100,params.data(),sizeof(params));
    require(api.createSocket(0x100,0x1000)==-1,"socket requires initialized HID");
    require(api.initialize(1)==-1,"unknown initialization flags rejected");
    require(api.initialize()==1 && api.initialize()==1 && polls==0,"real input initializes idempotently");
    const auto first=api.createSocket(0x100,0x1000);
    require(first==0,"zero is valid first source socket");
    params[1]=1; memory.write(0x100,params.data(),sizeof(params));
    const auto second=api.createSocket(0x100,0x1140);
    require(second>first,"logical IDs increase");
    sample.connected=true; sample.buttons=padBit(NativePadButton::B)|padBit(NativePadButton::R);
    sample.leftX=32767; sample.leftY=-32768; sample.rightX=-32768; sample.rightY=32767;
    require(api.getState(first)==1,"connected physical controller stable");
    const auto beforeSecond=polls;
    require(api.getState(second)==0 && polls==beforeSecond,"second logical port does not sample player one");
    memory.mainRam()[0x300-1]=71; memory.mainRam()[0x300+18]=72;
    require(api.read(first,0x300)==18,"source read returns18 bytes");
    Packet expected={0xFF,0xB7,0,0,255,255,0,0,0,0,0,0,255,0,0,255,0,0};
    require(packet(memory,0x300)==expected,"exact source buttons axes and canonical pressure bytes");
    require(memory.mainRam()[0x2FF]==71 && memory.mainRam()[0x312]==72,"write exactly packet size");
    const auto beforeProfile=polls;
    require(api.getButtonProfile(first,0x400)==4 && polls==beforeProfile+1,"profile reflects connected input");
    std::array<uint8_t,4> profile{}; memory.read(0x400,profile.data(),profile.size());
    require(profile==std::array<uint8_t,4>{255,255,255,255},"button axes pressure capability bytes");
    const auto beforeInvalid=polls;
    require(api.read(first,DataMemory::MainBytes-17)==-1 && api.getButtonProfile(first,DataMemory::MainBytes-3)==-1,"truncated destinations rejected");
    require(api.read(first,0x12000000)==-1 && polls==beforeInvalid,"device output rejected before sampling");
    require(api.createSocket(0,0x1000)==-1 && api.createSocket(DataMemory::MainBytes-8,0x1000)==-1,"bounded descriptor");
    require(api.createSocket(0x100,DataMemory::MainBytes-0x13F)==-1,"bounded optional work data");
    for(unsigned word=0;word<3;++word) {
        auto bad=params; bad[word]=word==0 ? 1 : word==1 ? 2 : 1;
        memory.write(0x100,bad.data(),sizeof(bad));
        require(api.createSocket(0x100,0x1000)==-1,"unknown socket type port slot rejected");
    }
    const auto beforePortOne=polls;
    require(api.read(second,0x500)==-1 && polls==beforePortOne,"second port disconnected read cannot alias physical pad");
    Packet neutral{}; neutral[0]=255; neutral[1]=255;
    neutral[2]=neutral[3]=neutral[4]=neutral[5]=128;
    require(packet(memory,0x500)==neutral,"unavailable port emits neutral source packet");
    require(api.getButtonProfile(second,0x400)==-1 && polls==beforePortOne,"no fabricated second pad profile");
    sample.connected=false;
    require(api.getState(first)==0,"physical unplug reflected");
    require(api.read(first,0x300)==-1 && packet(memory,0x300)==neutral,"unplug clears stale input");
    require(api.getButtonProfile(first,0x400)==-1,"no connected capability claim after unplug");
    require(api.deleteSocket(first)==1 && api.deleteSocket(first)==-1,"logical close rejects stale handle");
    require(api.getState(first)==0 && api.read(first,0x300)==-1,"closed handle inactive");
    require(api.getState(-1)==0 && api.getState(INT32_MAX)==0 && api.deleteSocket(-1)==-1,"unknown identifiers rejected");
    require(api.getButtonProfile(second,0)==-1 && api.read(second,0)==-1,"null destinations rejected");
    params={2,0,0}; memory.write(0x100,params.data(),sizeof(params));
    const auto replacement=api.createSocket(0x100);
    require(replacement>second,"closed identifiers are never reused");
    for(unsigned i=2;i<NativePadApi::MaxSockets;++i)
        require(api.createSocket(0x100)>=0,"fill bounded live sockets");
    require(api.createSocket(0x100)==-1,"live socket cap enforced");
    const auto counts=api.statistics();
    require(counts.created==9 && counts.closed==1 && counts.successfulReads==1 && counts.readAttempts==3,"counters reflect actual operations");
    require(counts.inputPolls==polls,"input poll counter matches real injected calls");
    require(api.end()==1 && api.getState(replacement)==0,"end closes all sockets");
    require(api.statistics().closed==9,"end counts actual closures");
    require(api.initialize()==1 && api.createSocket(0x100)>replacement,"reinitialization preserves stale ID protection");
    NativeInput failedInput([]()->NativePadSample { throw std::runtime_error("injected HID acquisition failure"); });
    NativePadApi failed(failedInput,memory); failed.initialize();
    const auto failedSocket=failed.createSocket(0x100);
    memory.mainRam()[0x600]=0xA5;
    bool caught=false;
    try { (void)failed.read(failedSocket,0x600); } catch(const std::runtime_error &) { caught=true; }
    require(caught && memory.mainRam()[0x600]==0xA5,"HID acquisition failure cannot fabricate or partially write input");
    require(failed.statistics().inputPolls==1 && failed.statistics().successfulReads==0,"failed acquisition counted without fake successful read");
#ifndef __SWITCH__
    NativeInput unavailableInput; NativePadApi unavailable(unavailableInput,memory);
    caught=false;
    try { (void)unavailable.initialize(); } catch(const std::runtime_error &) { caught=true; }
    require(caught && unavailable.createSocket(0x100)==-1,"SDK initialization failure leaves API inactive");
#endif
    std::printf("native pad2 API: %u checks passed\n",checks);
}

