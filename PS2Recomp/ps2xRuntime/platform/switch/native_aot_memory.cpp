#include "native_aot_runtime.hpp"

namespace ps2_guest_mem {
template<class T> T readData(uint8_t *ram, PS2Runtime *runtime, uint32_t address)
{
    T value{};
    if (!runtime || ram != runtime->memory().mainRam().data() ||
        !runtime->memory().read(address, &value, sizeof(value)))
        throw std::out_of_range("Native AOT: access outside data buffers");
    return value;
}
template<class T> void writeData(uint8_t *ram, PS2Runtime *runtime, uint32_t address, T value)
{
    if (!runtime || ram != runtime->memory().mainRam().data() ||
        !runtime->memory().write(address, &value, sizeof(value)))
        throw std::out_of_range("Native AOT: access outside data buffers");
}
#define DATA_ACCESS(bits, Type) \
Type read##bits(uint8_t *ram, R5900Context *, PS2Runtime *runtime, uint32_t address) \
{ return readData<Type>(ram, runtime, address); } \
void write##bits(uint8_t *ram, R5900Context *, PS2Runtime *runtime, uint32_t address, Type value) \
{ writeData<Type>(ram, runtime, address, value); }
DATA_ACCESS(8, uint8_t)
DATA_ACCESS(16, uint16_t)
DATA_ACCESS(32, uint32_t)
DATA_ACCESS(64, uint64_t)
DATA_ACCESS(128, __m128i)
#undef DATA_ACCESS
}
