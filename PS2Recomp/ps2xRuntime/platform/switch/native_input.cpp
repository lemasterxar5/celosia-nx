#include "native_input.hpp"
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>
#ifdef __SWITCH__
extern "C" {
#include <switch/runtime/pad.h>
}
#endif
namespace celosia::native {
namespace {
struct ButtonMap { NativePadButton button; uint16_t sourceBit; };
constexpr std::array<ButtonMap,16> buttons={{
    {NativePadButton::Minus,0x0001},{NativePadButton::LeftStick,0x0002},
    {NativePadButton::RightStick,0x0004},{NativePadButton::Plus,0x0008},
    {NativePadButton::Up,0x0010},{NativePadButton::Right,0x0020},
    {NativePadButton::Down,0x0040},{NativePadButton::Left,0x0080},
    {NativePadButton::ZL,0x0100},{NativePadButton::ZR,0x0200},
    {NativePadButton::L,0x0400},{NativePadButton::R,0x0800},
    {NativePadButton::X,0x1000},{NativePadButton::A,0x2000},
    {NativePadButton::B,0x4000},{NativePadButton::Y,0x8000}}};
constexpr std::array<uint16_t,12> pressureBits={
    0x0020,0x0080,0x0010,0x0040,0x1000,0x2000,0x4000,0x8000,0x0400,0x0800,0x0100,0x0200};
uint8_t axis(int32_t value,bool invert)
{
    const int64_t wide=invert ? -int64_t(value) : int64_t(value);
    return static_cast<uint8_t>((std::clamp<int64_t>(wide,-32768,32767)+32768)>>8);
}
}
NativePadSnapshot mapNativePad(const NativePadSample &sample) noexcept
{
    NativePadSnapshot result;
    if(!sample.connected) return result;
    result.connected=true;
    for(const auto &mapping:buttons)
        if(sample.buttons&padBit(mapping.button))
            result.sourceButtonsActiveLow=static_cast<uint16_t>(result.sourceButtonsActiveLow&~mapping.sourceBit);
    result.leftX=axis(sample.leftX,false); result.leftY=axis(sample.leftY,true);
    result.rightX=axis(sample.rightX,false); result.rightY=axis(sample.rightY,true);
    for(size_t i=0;i<result.pressures.size();++i)
        result.pressures[i]=(result.sourceButtonsActiveLow&pressureBits[i]) ? 0 : 255;
    return result;
}
struct NativeInput::Impl {
    std::mutex mutex;
    Sampler sampler;
    bool initialized=false;
#ifdef __SWITCH__
    PadState pad{};
    NativePadSample readHid()
    {
        padUpdate(&pad);
        NativePadSample result;
        result.connected=padIsConnected(&pad);
        const auto pressed=padGetButtons(&pad);
        constexpr std::array<std::pair<uint64_t,NativePadButton>,16> hidButtons={{
            {HidNpadButton_A,NativePadButton::A},{HidNpadButton_B,NativePadButton::B},
            {HidNpadButton_X,NativePadButton::X},{HidNpadButton_Y,NativePadButton::Y},
            {HidNpadButton_Plus,NativePadButton::Plus},{HidNpadButton_Minus,NativePadButton::Minus},
            {HidNpadButton_L,NativePadButton::L},{HidNpadButton_R,NativePadButton::R},
            {HidNpadButton_ZL,NativePadButton::ZL},{HidNpadButton_ZR,NativePadButton::ZR},
            {HidNpadButton_StickL,NativePadButton::LeftStick},{HidNpadButton_StickR,NativePadButton::RightStick},
            {HidNpadButton_Up,NativePadButton::Up},{HidNpadButton_Right,NativePadButton::Right},
            {HidNpadButton_Down,NativePadButton::Down},{HidNpadButton_Left,NativePadButton::Left}}};
        for(const auto &[native,button]:hidButtons) if(pressed&native) result.buttons|=padBit(button);
        const auto left=padGetStickPos(&pad,0),right=padGetStickPos(&pad,1);
        result.leftX=left.x; result.leftY=left.y; result.rightX=right.x; result.rightY=right.y;
        return result;
    }
#endif
    explicit Impl(Sampler source):sampler(std::move(source)) {}
};
NativeInput::NativeInput(Sampler sampler):impl(std::make_unique<Impl>(std::move(sampler))) {}
NativeInput::~NativeInput()=default;
bool NativeInput::initialize()
{
    std::lock_guard lock(impl->mutex);
    if(impl->initialized) return true;
    if(!impl->sampler) {
#ifdef __SWITCH__
        padConfigureInput(1,HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&impl->pad);
#else
        throw std::runtime_error("Native HID is unavailable on this host; provide an explicit sampler");
#endif
    }
    impl->initialized=true;
    return true;
}
NativePadSnapshot NativeInput::poll()
{
    std::lock_guard lock(impl->mutex);
    if(!impl->initialized) throw std::logic_error("Native input is not initialized");
    if(impl->sampler) return mapNativePad(impl->sampler());
#ifdef __SWITCH__
    return mapNativePad(impl->readHid());
#else
    throw std::logic_error("Native input sampler invariant");
#endif
}
}
