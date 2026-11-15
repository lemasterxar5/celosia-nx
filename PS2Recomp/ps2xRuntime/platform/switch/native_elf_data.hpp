#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace celosia::native {
class DataMemory;
inline constexpr size_t ElfDataMaxBytes=64u*1024u*1024u;
struct ElfDataImage { uint32_t entry,gp,dataEnd,loadSegments; };
// Copies program data into ordinary game buffers, never executes ELF bytes.
// Caller provides exclusive access during loading. All input is validated before
// mutation; section/debug/relocation processing and dynamic linking are absent.
ElfDataImage loadElfData(DataMemory &memory,std::span<const uint8_t> source,uint32_t expectedEntry=0);
}
