#pragma once
#include <cstdint>
#include <map>
#include <mutex>

namespace celosia::native {
class DataMemory;
// Suballocates only the game's native main-data buffer. Synchronization covers
// allocation ownership; callers coordinate data accesses with release/reset.
// DataMemory must outlive this allocator.
class DataHeap final {
public:
    explicit DataHeap(DataMemory &data) : memory(data) {}
    bool configure(uint32_t base, uint32_t limit);
    uint32_t allocate(uint32_t size, uint32_t alignment = 16);
    uint32_t calloc(uint32_t count, uint32_t size);
    uint32_t reallocate(uint32_t pointer, uint32_t size, uint32_t alignment = 16);
    bool release(uint32_t pointer);
    uint32_t base() const;
    uint32_t limit() const;
private:
    struct Block { uint32_t size, capacity; };
    DataMemory &memory;
    mutable std::mutex mutex;
    std::map<uint32_t,Block> blocks;
    uint32_t first = 0, last = 0;
    uint32_t allocateLocked(uint32_t size, uint32_t alignment);
    bool releaseLocked(uint32_t pointer);
};
}
