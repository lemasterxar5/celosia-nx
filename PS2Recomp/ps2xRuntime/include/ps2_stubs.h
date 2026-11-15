#pragma once

struct R5900Context;
class  PS2Runtime;

#include <cstdint>
#include "ps2_call_list.h"
#if !defined(PS2X_SWITCH_NATIVE_ONLY)
#include "runtime/ps2_memory.h"
#endif
#include "Stubs/Unimplemented.h"

namespace ps2_stubs
{
#define PS2_DECLARE_STUB(name) void name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    PS2_STUB_LIST(PS2_DECLARE_STUB)
#undef PS2_DECLARE_STUB

    void resetSifState();
}
