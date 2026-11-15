#include "native_aot_runtime.hpp"
#include "native_boot_services.hpp"
#include "native_pad_api.hpp"

namespace {
celosia::native::NativePadApi &checked(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    if(!runtime || !context || ram!=runtime->memory().mainRam().data())
        throw std::invalid_argument("Native pad call: invalid data context");
    return runtime->services().pads();
}
}
namespace ps2_stubs {
// SCUS libpad2 ABI: flags/parameter/socket in a0; work/output data in a1.
// The NativePadApi owns ordinary handles and directly samples native HID.
void scePad2Init(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &pads=checked(ram,context,runtime);
    setReturnS32(context,pads.initialize(getRegU32(context,4)));
}
void scePad2End(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    setReturnS32(context,checked(ram,context,runtime).end());
}
void scePad2CreateSocket(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &pads=checked(ram,context,runtime);
    setReturnS32(context,pads.createSocket(getRegU32(context,4),getRegU32(context,5)));
}
void scePad2DeleteSocket(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &pads=checked(ram,context,runtime);
    setReturnS32(context,pads.deleteSocket(static_cast<int32_t>(getRegU32(context,4))));
}
void scePad2GetState(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &pads=checked(ram,context,runtime);
    setReturnS32(context,pads.getState(static_cast<int32_t>(getRegU32(context,4))));
}
void scePad2GetButtonProfile(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &pads=checked(ram,context,runtime);
    setReturnS32(context,pads.getButtonProfile(static_cast<int32_t>(getRegU32(context,4)),getRegU32(context,5)));
}
void scePad2Read(uint8_t *ram,R5900Context *context,PS2Runtime *runtime)
{
    auto &pads=checked(ram,context,runtime);
    setReturnS32(context,pads.read(static_cast<int32_t>(getRegU32(context,4)),getRegU32(context,5)));
}
}
