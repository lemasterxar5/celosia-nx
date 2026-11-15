#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_input.hpp"
#include "ps2_stubs.h"
#include <array>
#include <cstdio>
#include <stdexcept>
extern const uint32_t g_ps2RecompiledFunctionTableBase=0x1000;
extern const uint32_t g_ps2RecompiledFunctionTableEnd=0x1004;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount=1;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1]{};
namespace {
unsigned checks=0;
void require(bool ok,const char *why) {++checks;if(!ok)throw std::runtime_error(why);}
}
int main()
{
    using namespace celosia::native;
    PS2Runtime runtime;R5900Context context;
    NativePadSample sample{};unsigned polls=0;
    runtime.services().configureInput([&]{++polls;return sample;});
    auto *ram=runtime.memory().mainRam().data();
    auto reg=[&](unsigned index,uint32_t value){context.r[index]=_mm_set_epi32(0,0,0,value);};
    auto result=[&]{return static_cast<int32_t>(getRegU32(&context,2));};
    const std::array<uint32_t,3> parameter{2,0,0};
    require(runtime.memory().write(0x100,parameter.data(),sizeof(parameter)),"descriptor setup");
    reg(4,0x100);reg(5,0x1000);ps2_stubs::scePad2CreateSocket(ram,&context,&runtime);
    require(result()==-1,"create requires native HID initialization");
    reg(4,1);ps2_stubs::scePad2Init(ram,&context,&runtime);require(result()==-1,"unknown flags rejected");
    reg(4,0);ps2_stubs::scePad2Init(ram,&context,&runtime);require(result()==1,"real input initialized");
    reg(4,0x100);reg(5,0x1000);ps2_stubs::scePad2CreateSocket(ram,&context,&runtime);
    const auto socket=result();require(socket==0,"zero source socket valid");
    sample.connected=true;sample.buttons=padBit(NativePadButton::B);sample.rightX=-32768;sample.leftX=32767;
    reg(4,socket);ps2_stubs::scePad2GetState(ram,&context,&runtime);require(result()==1,"connected native controller");
    reg(5,0x300);ram[0x2FF]=71;ram[0x312]=72;
    ps2_stubs::scePad2Read(ram,&context,&runtime);
    require(result()==18 && ram[0x300]==0xFF && ram[0x301]==0xBF,"source active-low Cross from physical B");
    require(ram[0x302]==0 && ram[0x304]==255 && ram[0x30C]==255,"native axes and pressure byte order");
    require(ram[0x2FF]==71 && ram[0x312]==72,"exact18 byte write");
    reg(5,0x400);ps2_stubs::scePad2GetButtonProfile(ram,&context,&runtime);
    require(result()==4 && ram[0x400]==255 && ram[0x403]==255,"real connected profile");
    const auto beforeInvalid=polls;reg(5,0x1FFFFFF);ram[0x1FFFFFF]=0xA5;
    ps2_stubs::scePad2Read(ram,&context,&runtime);
    require(result()==-1 && polls==beforeInvalid && ram[0x1FFFFFF]==0xA5,"invalid full range rejected before acquisition");
    reg(5,0x12000000);ps2_stubs::scePad2GetButtonProfile(ram,&context,&runtime);
    require(result()==-1 && polls==beforeInvalid,"device output rejected");
    sample.connected=false;reg(5,0x300);ps2_stubs::scePad2Read(ram,&context,&runtime);
    require(result()==-1 && ram[0x300]==255 && ram[0x301]==255 && ram[0x302]==128 && ram[0x30C]==0,"unplug neutralizes stale packet");
    ps2_stubs::scePad2GetState(ram,&context,&runtime);require(result()==0,"unplug state");
    ps2_stubs::scePad2DeleteSocket(ram,&context,&runtime);require(result()==1,"actual socket closed");
    ps2_stubs::scePad2DeleteSocket(ram,&context,&runtime);require(result()==-1,"stale close fails");
    ps2_stubs::scePad2End(ram,&context,&runtime);require(result()==1,"native pad shutdown");
    reg(4,0x100);reg(5,0x1FFFF00);ps2_stubs::scePad2CreateSocket(ram,&context,&runtime);
    require(result()==-1,"ended pad rejects socket");
    using Call=void(*)(uint8_t*,R5900Context*,PS2Runtime*);
    const std::array<Call,7> calls{ps2_stubs::scePad2Init,ps2_stubs::scePad2End,ps2_stubs::scePad2CreateSocket,
        ps2_stubs::scePad2DeleteSocket,ps2_stubs::scePad2GetState,ps2_stubs::scePad2GetButtonProfile,ps2_stubs::scePad2Read};
    for(auto call:calls) {
        bool rejected=false;try{call(ram+1,&context,&runtime);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"foreign data base rejected");
        rejected=false;try{call(ram,nullptr,&runtime);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"null context rejected");
        rejected=false;try{call(ram,&context,nullptr);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"null runtime rejected");
    }
    std::printf("native pad ABI calls: %u checks passed\n",checks);
}
