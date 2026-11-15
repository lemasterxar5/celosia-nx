#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
namespace celosia::native {
enum class NativePadButton : uint32_t {
    A=1u<<0,B=1u<<1,X=1u<<2,Y=1u<<3,Plus=1u<<4,Minus=1u<<5,
    L=1u<<6,R=1u<<7,ZL=1u<<8,ZR=1u<<9,LeftStick=1u<<10,RightStick=1u<<11,
    Up=1u<<12,Right=1u<<13,Down=1u<<14,Left=1u<<15
};
constexpr uint32_t padBit(NativePadButton button) noexcept { return static_cast<uint32_t>(button); }
struct NativePadSample {
    bool connected=false;
    uint32_t buttons=0;
    int32_t leftX=0,leftY=0,rightX=0,rightY=0;
};
struct NativePadSnapshot {
    bool connected=false;
    uint16_t sourceButtonsActiveLow=0xFFFF;
    uint8_t leftX=128,leftY=128,rightX=128,rightY=128;
    // Canonical order: right,left,up,down,triangle,circle,cross,square,L1,R1,L2,R2.
    // Standard Switch buttons are digital: pressure is either0 or255.
    std::array<uint8_t,12> pressures{};
};
NativePadSnapshot mapNativePad(const NativePadSample &sample) noexcept;
// Native HID acquisition only; no controller bus, IOP or device emulator.
// initialize/poll are internally serialized; injected samplers must not reenter.
class NativeInput final {
public:
    using Sampler=std::function<NativePadSample()>;
    explicit NativeInput(Sampler sampler={});
    ~NativeInput();
    NativeInput(const NativeInput &)=delete;
    NativeInput &operator=(const NativeInput &)=delete;
    bool initialize();
    NativePadSnapshot poll();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
