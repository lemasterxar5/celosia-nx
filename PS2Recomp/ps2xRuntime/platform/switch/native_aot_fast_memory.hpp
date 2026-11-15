#pragma once
// Included after the native AOT ABI. These helpers only accept the main data
// buffer and its documented aliases. No masking of device addresses or wraps.
static PS2_FORCE_INLINE uint32_t Ps2NativeRamOffset(uint32_t address, size_t bytes)
{
    for (const uint32_t base : {0u,0x20000000u,0x80000000u,0xA0000000u}) {
        if (address < base) continue;
        const uint32_t offset = address - base;
        if (offset <= PS2_RAM_SIZE && bytes <= PS2_RAM_SIZE - offset) return offset;
    }
    throw std::out_of_range("Native AOT fast access outside main data buffer");
}
template<class T> static PS2_FORCE_INLINE T Ps2NativeRamRead(const uint8_t *ram, uint32_t address)
{
    if (!ram) throw std::invalid_argument("Native AOT null data buffer");
    const auto offset = Ps2NativeRamOffset(address, sizeof(T));
    T value;
    std::memcpy(&value, ram + offset, sizeof(T));
    return value;
}
template<class T> static PS2_FORCE_INLINE void Ps2NativeRamWrite(uint8_t *ram, uint32_t address, T value)
{
    if (!ram) throw std::invalid_argument("Native AOT null data buffer");
    const auto offset = Ps2NativeRamOffset(address, sizeof(T));
    std::memcpy(ram + offset, &value, sizeof(T));
}
#define NATIVE_FAST_MEMORY(bits, Type) \
static PS2_FORCE_INLINE Type Ps2FastRead##bits(const uint8_t *ram, uint32_t address) \
{ return Ps2NativeRamRead<Type>(ram, address); } \
static PS2_FORCE_INLINE void Ps2FastWrite##bits(uint8_t *ram, uint32_t address, Type value) \
{ Ps2NativeRamWrite<Type>(ram, address, value); }
NATIVE_FAST_MEMORY(8, uint8_t)
NATIVE_FAST_MEMORY(16, uint16_t)
NATIVE_FAST_MEMORY(32, uint32_t)
NATIVE_FAST_MEMORY(64, uint64_t)
NATIVE_FAST_MEMORY(128, __m128i)
#undef NATIVE_FAST_MEMORY
