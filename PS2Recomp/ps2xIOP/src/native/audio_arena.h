#pragma once
#include "audio_ram.h"
#include <map>
#include <mutex>
#include <vector>

namespace ps2x::iop::detail {
// Host data buffers retaining SG2's address ABI. There are deliberately no
// instruction fetches, device registers, interrupts or hardware dispatch here.
class AudioArena final : public AudioRam {
public:
    static constexpr uint32_t Size = 0x200000;
    static constexpr uint32_t HeapBase = 0x80000;
    static constexpr uint32_t HeapLimit = 0x1F0000;
    AudioArena() : bytes(Size, 0) {}
    void reset();
    uint32_t allocate(uint32_t size, uint32_t alignment = 16);
    bool freeAllocation(uint32_t address);
    bool readRam(uint32_t address, void *destination, size_t size) const override;
    bool writeRam(uint32_t address, const void *source, size_t size) override;
    bool zeroRam(uint32_t address, size_t size);
    bool ownsRamRange(uint32_t address, size_t size) const override;
private:
    static bool range(uint32_t address, size_t size, uint32_t &offset);
    mutable std::mutex mutex;
    std::vector<uint8_t> bytes;
    std::map<uint32_t,uint32_t> allocations;
};
}
