#pragma once
#include "native_kernel_publication.hpp"
struct R5900Context;
namespace celosia::native {
class DataMemory;
class NativeFiles;
// Verify the known MANAGER/GAMECORE image and relocated static-code DATA
// against the AOT input at isysLaunchExecModule's publication boundary.
// Does not execute source instructions, restore state or model a code cache.
// Caller owns exclusive game-data access; hasher is trusted native code and
// must not mutate buffers. Throws on every mismatch; no data/context mutation.
void validateGamePublication(DataMemory &memory,const R5900Context &context,
                             NativeFiles &files,Hasher hasher);
}
