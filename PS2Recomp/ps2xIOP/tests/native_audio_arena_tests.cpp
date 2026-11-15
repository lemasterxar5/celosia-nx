#include "native/audio_arena.h"
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#include <atomic>

using ps2x::iop::detail::AudioArena;
static unsigned checks = 0;
static void require(bool condition, const char *message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
int main()
{
    AudioArena arena;
    const auto a = arena.allocate(32, 16);
    const auto b = arena.allocate(64, 64);
    require(a >= AudioArena::HeapBase && a % 16 == 0, "first allocation");
    require(b >= a + 32 && b % 64 == 0, "aligned nonoverlap");
    std::array<uint8_t, 32> data{}, out{};
    data.fill(0xA5);
    require(arena.writeRam(a, data.data(), data.size()), "write allocated buffer");
    require(arena.readRam(a | 0x80000000u, out.data(), out.size()) && out == data, "cached address ABI");
    require(arena.readRam(a | 0xA0000000u, out.data(), out.size()) && out == data, "uncached address ABI");
    require(!arena.readRam(0x1F801000u, out.data(), out.size()), "device registers must not exist");
    require(!arena.readRam(0x40000000u | a, out.data(), out.size()), "unmapped segment");
    require(!arena.readRam(AudioArena::Size - 1, out.data(), 2), "end overflow");
    require(!arena.writeRam(std::numeric_limits<uint32_t>::max(), data.data(), data.size()), "address wrap");
    require(!arena.readRam(a, nullptr, 1), "null destination");
    require(!arena.writeRam(a, nullptr, 1), "null source");
    require(arena.readRam(a, nullptr, 0), "empty copy");
    require(arena.zeroRam(a, data.size()), "zero valid range");
    out.fill(1);
    require(arena.readRam(a, out.data(), out.size()) && out == std::array<uint8_t,32>{}, "zero contents");
    require(!arena.zeroRam(AudioArena::Size - 2, 4), "zero bounds");
    require(!arena.freeAllocation(a + 1), "interior pointer rejected");
    require(arena.freeAllocation(a), "free allocation");
    require(!arena.freeAllocation(a), "double free rejected");
    require(arena.allocate(16, 16) == a, "freed gap reused");
    require(arena.allocate(0, 16) == 0, "zero allocation");
    require(arena.allocate(16, 3) == 0 && arena.allocate(16, 0) == 0, "invalid alignment");
    require(arena.allocate(std::numeric_limits<uint32_t>::max(), 16) == 0, "allocation overflow");
    arena.reset();
    require(arena.allocate(AudioArena::HeapLimit - AudioArena::HeapBase, 16) == AudioArena::HeapBase, "whole heap");
    require(arena.allocate(16, 16) == 0, "exhaustion fails");
    arena.reset();
    const auto shared = arena.allocate(256, 16);
    std::atomic<bool> corrupt{false};
    std::thread writer([&] {
        std::array<uint8_t,256> bytes{};
        for (unsigned n = 0; n < 1000; ++n) {
            bytes.fill(static_cast<uint8_t>(n));
            if (!arena.writeRam(shared, bytes.data(), bytes.size())) corrupt = true;
        }
    });
    for (unsigned n = 0; n < 1000; ++n) {
        std::array<uint8_t,256> bytes{};
        if (!arena.readRam(shared, bytes.data(), bytes.size())) corrupt = true;
        for (auto value : bytes) if (value != bytes[0]) corrupt = true;
    }
    writer.join();
    require(!corrupt, "concurrent refill copies cannot tear");
    std::printf("native audio arena: %u checks passed\n", checks);
}
