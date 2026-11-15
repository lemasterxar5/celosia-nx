#include "audio_arena.h"
#include <algorithm>
#include <cstring>

namespace ps2x::iop::detail {
bool AudioArena::range(uint32_t address, size_t size, uint32_t &offset)
{
    // Retain only the cached/uncached aliases used by the buffer protocol.
    if (address < Size) offset = address;
    else if (address >= 0x80000000u && address < 0xC0000000u)
        offset = address & 0x1FFFFFFFu;
    else return false;
    return offset <= Size && size <= Size - offset;
}
void AudioArena::reset()
{
    std::lock_guard lock(mutex);
    std::fill(bytes.begin(), bytes.end(), 0);
    allocations.clear();
}
uint32_t AudioArena::allocate(uint32_t size, uint32_t alignment)
{
    if (!size || !alignment || (alignment & (alignment - 1)) || alignment > 4096)
        return 0;
    std::lock_guard lock(mutex);
    uint64_t cursor = HeapBase;
    auto fit = [&](uint32_t end) -> uint32_t {
        const uint64_t aligned = (cursor + alignment - 1) & ~uint64_t(alignment - 1);
        if (aligned > end || size > end - aligned) return 0;
        const auto result = static_cast<uint32_t>(aligned);
        allocations.emplace(result, size);
        std::fill(bytes.begin() + result, bytes.begin() + result + size, 0);
        return result;
    };
    for (const auto &[address, length] : allocations) {
        if (const auto result = fit(address)) return result;
        cursor = uint64_t(address) + length;
    }
    return fit(HeapLimit);
}
bool AudioArena::freeAllocation(uint32_t address)
{
    uint32_t offset;
    if (!range(address, 1, offset)) return false;
    std::lock_guard lock(mutex);
    const auto found = allocations.find(offset);
    if (found == allocations.end()) return false;
    std::fill(bytes.begin() + offset, bytes.begin() + offset + found->second, 0);
    allocations.erase(found);
    return true;
}
bool AudioArena::readRam(uint32_t address, void *destination, size_t size) const
{
    uint32_t offset;
    if ((!destination && size) || !range(address, size, offset)) return false;
    std::lock_guard lock(mutex);
    if (size) std::memcpy(destination, bytes.data() + offset, size);
    return true;
}
bool AudioArena::writeRam(uint32_t address, const void *source, size_t size)
{
    uint32_t offset;
    if ((!source && size) || !range(address, size, offset)) return false;
    std::lock_guard lock(mutex);
    if (size) std::memcpy(bytes.data() + offset, source, size);
    return true;
}
bool AudioArena::zeroRam(uint32_t address, size_t size)
{
    uint32_t offset;
    if (!range(address, size, offset)) return false;
    std::lock_guard lock(mutex);
    std::fill(bytes.begin() + offset, bytes.begin() + offset + size, 0);
    return true;
}
bool AudioArena::ownsRamRange(uint32_t address, size_t size) const
{
    uint32_t offset;
    return range(address, size, offset);
}
}
