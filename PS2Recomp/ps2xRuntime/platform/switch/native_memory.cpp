#include "native_memory.hpp"
#include <array>
#include <cstring>

namespace celosia::native {
struct DataMemory::Storage {
    alignas(64) std::array<uint8_t, MainBytes> main{};
    alignas(64) std::array<uint8_t, ScratchBytes> scratch{};
    alignas(64) std::array<uint8_t, Matrix0Bytes> matrix0{};
    alignas(64) std::array<uint8_t, Matrix1Bytes> matrix1{};
};
DataMemory::DataMemory() : storage(std::make_unique<Storage>()) {}
DataMemory::~DataMemory() = default;

const uint8_t *DataMemory::locate(uint32_t address, size_t bytes) const noexcept
{
    struct Segment { uint32_t base; std::span<const uint8_t> data; };
    const Segment segments[] = {
        {0, storage->main}, {0x20000000, storage->main},
        {0x80000000, storage->main}, {0xA0000000, storage->main},
        {0x70000000, storage->scratch}, {0xF0000000, storage->scratch},
        {0x11004000, storage->matrix0}, {0x1100C000, storage->matrix1}
    };
    for (const auto &segment : segments) {
        if (address < segment.base) continue;
        const uint32_t offset = address - segment.base;
        if (offset <= segment.data.size() && bytes <= segment.data.size() - offset)
            return segment.data.data() + offset;
    }
    return nullptr;
}
bool DataMemory::contains(uint32_t address, size_t bytes) const noexcept
{
    return locate(address, bytes) != nullptr;
}
bool DataMemory::read(uint32_t address, void *destination, size_t bytes) const noexcept
{
    const auto *source = locate(address, bytes);
    if (!source || (!destination && bytes)) return false;
    if (bytes) std::memmove(destination, source, bytes);
    return true;
}
bool DataMemory::write(uint32_t address, const void *source, size_t bytes) noexcept
{
    auto *destination = const_cast<uint8_t *>(locate(address, bytes));
    if (!destination || (!source && bytes)) return false;
    if (bytes) std::memmove(destination, source, bytes);
    return true;
}
bool DataMemory::zero(uint32_t address, size_t bytes) noexcept
{
    auto *destination = const_cast<uint8_t *>(locate(address, bytes));
    if (!destination) return false;
    if (bytes) std::memset(destination, 0, bytes);
    return true;
}
void DataMemory::clear() noexcept
{
    storage->main.fill(0);
    storage->scratch.fill(0);
    storage->matrix0.fill(0);
    storage->matrix1.fill(0);
}
std::span<uint8_t> DataMemory::mainRam() noexcept { return storage->main; }
std::span<const uint8_t> DataMemory::mainRam() const noexcept { return storage->main; }
std::array<std::span<uint8_t>,4> DataMemory::nativeDataBuffers() noexcept
{ return {storage->main,storage->scratch,storage->matrix0,storage->matrix1}; }
uint8_t *DataMemory::getVU0Data() noexcept { return storage->matrix0.data(); }
}
