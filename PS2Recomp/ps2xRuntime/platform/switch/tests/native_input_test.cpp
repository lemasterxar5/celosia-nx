#include "native_input.hpp"
#include <array>
#include <climits>
#include <cstdio>
#include <stdexcept>
using namespace celosia::native;
namespace {
unsigned checks=0;
void require(bool value,const char *why) { ++checks; if(!value) throw std::runtime_error(why); }
template<class Exception,class F> void rejects(F action,const char *why)
{ bool caught=false; try { action(); } catch(const Exception &) { caught=true; } require(caught,why); }
}
int main()
{
    NativePadSample raw{};
    raw.buttons=UINT32_MAX; raw.leftX=INT_MIN; raw.leftY=INT_MAX;
    auto out=mapNativePad(raw);
    require(!out.connected && out.sourceButtonsActiveLow==0xFFFF,"disconnected buttons neutral");
    require(out.leftX==128 && out.leftY==128 && out.rightX==128 && out.rightY==128,"disconnected axes neutral");
    require(out.pressures==std::array<uint8_t,12>{},"disconnected pressure neutral");
    raw={}; raw.connected=true;
    out=mapNativePad(raw);
    require(out.connected && out.sourceButtonsActiveLow==0xFFFF,"connected released input");
    require(out.leftX==128 && out.leftY==128 && out.rightX==128 && out.rightY==128,"axis center exact");
    const std::array<NativePadButton,16> physical={NativePadButton::Minus,NativePadButton::LeftStick,
        NativePadButton::RightStick,NativePadButton::Plus,NativePadButton::Up,NativePadButton::Right,
        NativePadButton::Down,NativePadButton::Left,NativePadButton::ZL,NativePadButton::ZR,
        NativePadButton::L,NativePadButton::R,NativePadButton::X,NativePadButton::A,
        NativePadButton::B,NativePadButton::Y};
    for(unsigned i=0;i<physical.size();++i) {
        raw.buttons=padBit(physical[i]); out=mapNativePad(raw);
        require(out.sourceButtonsActiveLow==uint16_t(0xFFFFu^(1u<<i)),"physical position to source bit mapping");
    }
    raw.buttons=0xFFFF; out=mapNativePad(raw);
    require(out.sourceButtonsActiveLow==0,"all supported buttons can be pressed together");
    for(auto pressure:out.pressures) require(pressure==255,"digital buttons produce full pressure");
    const std::array<NativePadButton,12> pressureOrder={NativePadButton::Right,NativePadButton::Left,
        NativePadButton::Up,NativePadButton::Down,NativePadButton::X,NativePadButton::A,
        NativePadButton::B,NativePadButton::Y,NativePadButton::L,NativePadButton::R,
        NativePadButton::ZL,NativePadButton::ZR};
    for(unsigned i=0;i<pressureOrder.size();++i) {
        raw.buttons=padBit(pressureOrder[i]); out=mapNativePad(raw);
        for(unsigned j=0;j<pressureOrder.size();++j)
            require(out.pressures[j]==(i==j ? 255 : 0),"canonical pressure byte order");
    }
    raw.buttons=0xFFFF0000u; out=mapNativePad(raw);
    require(out.sourceButtonsActiveLow==0xFFFF,"unknown raw buttons ignored");
    raw={}; raw.connected=true;
    raw.leftX=-32768; raw.leftY=32767; raw.rightX=32767; raw.rightY=-32768;
    out=mapNativePad(raw);
    require(out.leftX==0 && out.leftY==0 && out.rightX==255 && out.rightY==255,"full ranges and inverted Y");
    raw.leftX=INT_MIN; raw.leftY=INT_MIN; raw.rightX=INT_MAX; raw.rightY=INT_MAX;
    out=mapNativePad(raw);
    require(out.leftX==0 && out.leftY==255 && out.rightX==255 && out.rightY==0,"out of range clamped without negation overflow");
    raw.leftX=255; raw.leftY=-256; raw.rightX=256; raw.rightY=1;
    out=mapNativePad(raw);
    require(out.leftX==128 && out.leftY==129 && out.rightX==129 && out.rightY==127,"integer byte boundaries");
    unsigned calls=0;
    NativeInput input([&]{ ++calls; return raw; });
    rejects<std::logic_error>([&]{ (void)input.poll(); },"poll must initialize first");
    require(calls==0,"uninitialized input never samples");
    require(input.initialize() && input.initialize() && calls==0,"initialization idempotent and does not fabricate samples");
    raw.buttons=padBit(NativePadButton::Plus);
    out=input.poll();
    require(calls==1 && out.connected && out.sourceButtonsActiveLow==0xFFF7,"injected sampler is actually polled");
    raw.connected=false; out=input.poll();
    require(calls==2 && !out.connected && out.leftX==128 && out.sourceButtonsActiveLow==0xFFFF,"disconnect clears previous input");
    NativeInput broken([]()->NativePadSample{ throw std::runtime_error("sample failure"); }); broken.initialize();
    rejects<std::runtime_error>([&]{ (void)broken.poll(); },"sampler failures remain explicit");
#ifndef __SWITCH__
    NativeInput unavailable;
    rejects<std::runtime_error>([&]{ (void)unavailable.initialize(); },"host without sampler cannot fake HID availability");
    rejects<std::logic_error>([&]{ (void)unavailable.poll(); },"failed initialization leaves input uninitialized");
#endif
    std::printf("native input: %u checks passed\n",checks);
}
