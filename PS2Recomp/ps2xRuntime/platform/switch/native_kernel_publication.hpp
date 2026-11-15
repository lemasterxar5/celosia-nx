#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
struct R5900Context;
namespace celosia::native {
class DataMemory;
class NativeFiles;
using Hasher=std::function<std::array<uint8_t,32>(std::span<const uint8_t>)>;
// Verify the pinned SCUS KERNEL file and its relocated data layout against the
// static AOT input. Never executes source bytes, restores snapshots or models
// an instruction cache. Probe supplies actual SDK SHA-256; tests inject it.
// Caller owns execution/data access exclusively during validation. The supplied
// hasher is trusted native code and must not mutate the source or game buffers.
// Throws on every mismatch; successful validation changes no game data/context.
void validateKernelPublication(DataMemory &memory,const R5900Context &context,
                               NativeFiles &files,Hasher hasher);
}
