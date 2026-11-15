#include "native_memory.hpp"
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>

using celosia::native::DataMemory;
static unsigned checks = 0;
static void require(bool ok, const char *message)
{
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
int main()
{
    DataMemory memory;
    constexpr uint32_t value = 0x12345678;
    uint32_t output = 1;
    require(memory.read(0x100, &output, sizeof(output)) && output == 0, "new data starts cleared");
    require(memory.write(0x100, &value, sizeof(value)), "main data write");
    for (uint32_t address : {0x100u,0x20000100u,0x80000100u,0xA0000100u})
        require(memory.read(address, &output, sizeof(output)) && output == value, "documented data alias");
    for (uint32_t address : {0x10000000u,0x12000000u,0x1F801000u,0x1FC00000u,
                            0x11000000u,0x11008000u,0x40000100u,0xC0000100u,UINT32_MAX}) {
        require(!memory.read(address, &output, sizeof(output)), "device/code/unmapped read must fail");
        require(!memory.write(address, &value, sizeof(value)), "device/code/unmapped write must fail");
    }
    require(!memory.write(DataMemory::MainBytes - 2, &value, sizeof(value)), "main end cannot wrap");
    require(!memory.read(0x100, &output, std::numeric_limits<size_t>::max()), "size overflow");
    require(!memory.read(0x100, nullptr, 1) && !memory.write(0x100, nullptr, 1), "null copy bounds");
    require(memory.read(0x100, nullptr, 0) && memory.write(0x100, nullptr, 0), "empty valid copy");
    require(memory.write(0x70000000, &value, sizeof(value)), "scratch data write");
    require(memory.read(0xF0000000, &output, sizeof(output)) && output == value, "scratch alias");
    require(!memory.read(0x70000000 + DataMemory::ScratchBytes - 2, &output, sizeof(output)), "scratch boundary");
    require(memory.write(0x11004000, &value, sizeof(value)), "matrix workspace 0 data");
    require(memory.read(0x11004000, &output, sizeof(output)) && output == value, "matrix workspace 0 read");
    require(memory.getVU0Data()[0] == 0x78, "AOT matrix pointer shares workspace data");
    require(memory.read(0x1100C000, &output, sizeof(output)) && output == 0, "matrix workspaces are independent");
    require(!memory.write(0x11004FFF, &value, sizeof(value)), "matrix workspace 0 boundary");
    require(!memory.write(0x1100FFFF, &value, sizeof(value)), "matrix workspace 1 boundary");
    require(memory.write(0x1100C000, &value, sizeof(value)), "matrix workspace 1 write");
    require(memory.contains(0x80000100, 4) && !memory.contains(0x10000000, 4), "contains validates data only");
    require(memory.zero(0x100, 4) && memory.read(0x100, &output, 4) && output == 0, "zero bounded data");
    require(!memory.zero(0x10000000, 4), "zero cannot touch devices");
    auto main = memory.mainRam();
    require(main.size() == DataMemory::MainBytes && reinterpret_cast<uintptr_t>(main.data()) % 64 == 0,
            "native AOT base is aligned contiguous storage");
    main[0x200] = 77;
    uint8_t byte = 0;
    require(memory.read(0x200, &byte, 1) && byte == 77, "AOT base and bounded copies share data");
    const DataMemory &constant = memory;
    require(constant.mainRam()[0x200] == 77, "const view");
    memory.clear();
    for (uint32_t address : {0x100u,0x200u,0x70000000u,0x11004000u,0x1100C000u})
        require(memory.read(address, &output, 4) && output == 0, "clear all data workspaces");
    std::printf("native data memory: %u checks passed\n", checks);
}
