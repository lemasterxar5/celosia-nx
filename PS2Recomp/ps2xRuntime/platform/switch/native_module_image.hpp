#pragma once
#include "native_kernel_publication.hpp"
#include <cstdint>
struct R5900Context;
namespace celosia::native {
class DataMemory;
class BootServices;
// LoaderSysLoadIopModuleFromEEBuffer@102D70 native replacement. Registers:
// a0 optional diagnostic label, a1 image, a2 bytes, a3 argument bytes, t0 args.
// The pinned image digest selects an existing C++ service; no image code is
// copied to another processor, relocated, decoded or executed. Caller holds
// the execution gate. The trusted hasher must not mutate application memory.
// Invalid data/unknown identity throws; an unavailable native provider returns
// its actual negative error. Success is the actual native module identifier.
int32_t loadNativeModuleImage(DataMemory &data,BootServices &boot,
                             const R5900Context &context,Hasher hasher);
}
