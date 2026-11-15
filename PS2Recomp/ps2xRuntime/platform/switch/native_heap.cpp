#include "native_heap.hpp"
#include "native_memory.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace celosia::native {
namespace {
uint32_t canonical(uint32_t address)
{
    for (const uint32_t base : {0u,0x20000000u,0x80000000u,0xA0000000u})
        if (address >= base && address - base < DataMemory::MainBytes) return address - base;
    return UINT32_MAX;
}
bool validAlignment(uint32_t alignment)
{ return alignment && alignment <= 4096 && !(alignment & (alignment-1)); }
uint64_t alignUp(uint64_t value, uint32_t alignment)
{ return (value+alignment-1) & ~uint64_t(alignment-1); }
void zeroBlock(DataMemory &memory, uint32_t address, size_t size)
{
    // Heap/map bounds guarantee main-data ownership. Never report allocation
    // success if that invariant changes in the underlying storage contract.
    if (!memory.zero(address,size)) throw std::logic_error("Native heap: invalid owned data range");
}
}
bool DataHeap::configure(uint32_t start, uint32_t end)
{
    const auto aligned = alignUp(start,16);
    if (start < 16 || aligned >= end || end > DataMemory::MainBytes) return false;
    std::lock_guard lock(mutex);
    if (!blocks.empty()) return false;
    first = static_cast<uint32_t>(aligned); last = end;
    return true;
}
uint32_t DataHeap::allocateLocked(uint32_t size, uint32_t alignment)
{
    if (!first || !size || !validAlignment(alignment)) return 0;
    alignment = std::max(alignment,16u);
    const uint64_t capacity = alignUp(size,16);
    if (capacity > last-first) return 0;
    uint64_t cursor = first;
    auto fit = [&](uint32_t end) -> uint32_t {
        const uint64_t address = alignUp(cursor,alignment);
        if (address > end || capacity > end-address) return 0;
        const auto pointer = static_cast<uint32_t>(address);
        zeroBlock(memory,pointer,static_cast<size_t>(capacity));
        blocks.emplace(pointer,Block{size,static_cast<uint32_t>(capacity)});
        return pointer;
    };
    for (const auto &[address,block] : blocks) {
        if (const auto pointer = fit(address)) return pointer;
        cursor = uint64_t(address)+block.capacity;
    }
    return fit(last);
}
uint32_t DataHeap::allocate(uint32_t size, uint32_t alignment)
{
    std::lock_guard lock(mutex);
    return allocateLocked(size,alignment);
}
uint32_t DataHeap::calloc(uint32_t count, uint32_t size)
{
    const uint64_t total = uint64_t(count)*size;
    return total <= DataMemory::MainBytes ? allocate(static_cast<uint32_t>(total)) : 0;
}
bool DataHeap::releaseLocked(uint32_t pointer)
{
    if (!pointer) return true;
    const auto found = blocks.find(canonical(pointer));
    if (found == blocks.end()) return false;
    zeroBlock(memory,found->first,found->second.capacity);
    blocks.erase(found);
    return true;
}
bool DataHeap::release(uint32_t pointer)
{
    std::lock_guard lock(mutex);
    return releaseLocked(pointer);
}
uint32_t DataHeap::reallocate(uint32_t pointer, uint32_t size, uint32_t alignment)
{
    std::lock_guard lock(mutex);
    if (!pointer) return allocateLocked(size,alignment);
    const auto found = blocks.find(canonical(pointer));
    if (found == blocks.end()) return 0;
    if (!size) { releaseLocked(pointer); return 0; }
    if (!validAlignment(alignment)) return 0;
    alignment = std::max(alignment,16u);
    const auto address = found->first;
    const auto previous = found->second;
    const uint64_t capacity = alignUp(size,16);
    const auto next = std::next(found);
    const uint32_t end = next == blocks.end() ? last : next->first;
    if (!(address & (alignment-1)) && capacity <= end-address) {
        if (size > previous.size) zeroBlock(memory,address+previous.size,size-previous.size);
        if (capacity < previous.capacity) zeroBlock(memory,address+static_cast<uint32_t>(capacity),previous.capacity-capacity);
        found->second = Block{size,static_cast<uint32_t>(capacity)};
        return address;
    }
    const auto replacement = allocateLocked(size,alignment);
    if (!replacement) return 0;
    auto data = memory.mainRam();
    std::memmove(data.data()+replacement,data.data()+address,std::min(size,previous.size));
    releaseLocked(address);
    return replacement;
}
uint32_t DataHeap::base() const { std::lock_guard lock(mutex); return first; }
uint32_t DataHeap::limit() const { std::lock_guard lock(mutex); return last; }
}
