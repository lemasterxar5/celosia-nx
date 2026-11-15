#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace celosia::native {
// Address-preserving data storage for statically compiled game functions.
// No hardware registers, BIOS, microcode, DMA queues or device callbacks.
class DataMemory final {
public:
    static constexpr uint32_t MainBytes = 32u * 1024u * 1024u;
    static constexpr uint32_t ScratchBytes = 16u * 1024u;
    static constexpr uint32_t Matrix0Bytes = 4u * 1024u;
    static constexpr uint32_t Matrix1Bytes = 16u * 1024u;
    DataMemory();
    ~DataMemory();
    DataMemory(const DataMemory &) = delete;
    DataMemory &operator=(const DataMemory &) = delete;
    // Stable addresses are required by AOT functions; storage cannot move.
    bool contains(uint32_t address, size_t bytes) const noexcept;
    bool read(uint32_t address, void *destination, size_t bytes) const noexcept;
    bool write(uint32_t address, const void *source, size_t bytes) noexcept;
    bool zero(uint32_t address, size_t bytes) noexcept;
    void clear() noexcept;
    std::span<uint8_t> mainRam() noexcept;
    std::span<const uint8_t> mainRam() const noexcept;
    std::array<std::span<uint8_t>,4> nativeDataBuffers() noexcept;
    uint8_t *getVU0Data() noexcept; // AOT ABI name for the matrix stack data buffer.
private:
    struct Storage;
    std::unique_ptr<Storage> storage;
    const uint8_t *locate(uint32_t address, size_t bytes) const noexcept;
};
}
