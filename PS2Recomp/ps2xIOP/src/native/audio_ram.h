#pragma once
#include <cstddef>
#include <cstdint>

namespace ps2x::iop::detail {
// SG2 transfers samples through numeric buffer addresses. A native sound driver
// needs those bytes, not an IOP CPU, SPU2 registers or a peripheral bus.
class AudioRam {
public:
    virtual ~AudioRam() = default;
    virtual bool readRam(uint32_t address, void *destination, size_t size) const = 0;
    virtual bool writeRam(uint32_t address, const void *source, size_t size) = 0;
    virtual bool ownsRamRange(uint32_t address, size_t size) const = 0;
};
}
