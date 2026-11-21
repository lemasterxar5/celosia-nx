// gsReg (0x1c5b00, round 44): one GS register write of the 2D code, a
// (register, value) pair appended to the current packet. ~0.2% of the game
// thread recompiled in round 43's Vita3K profile (21.6 k instructions a
// frame: thousands of calls, eighteen MIPS instructions each).
//
// a0 the register (its 64-bit A+D address word), a1 the value. The packet
// state is *0x1e11a4 (a record: +0xc the write pointer, +0x1c a "closed"
// word). Closed: nothing written. Otherwise the value goes at the pointer
// and the register after it (an A+D pair is data, then address), each
// 8 bytes, the pointer moved past each; the record is read again from
// 0x1e11a4 before the second store, as the code does.
//
// Registers as the code leaves them: a3 0x1e11a4, v1 the record (or its
// +0xc on the write path), a2 the record's +0xc, v0 the closed word (or the
// pointer after the pair).
//
// sotc_native hooks gsReg (the native renderer reads the 2D writes by their
// return address) and calls what behindHook gives it: this native, its
// difftest checker or the recompiled code (SOTC_HOTLIST44=0,
// SOTC_DECOMP_SKIP=gsReg). Without the native renderer it is the table's.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstring>

namespace decomp_hot44g
{
    constexpr uint32_t kGsReg = 0x001C5B00u, kPacket = 0x001E11A4u;

    inline uint64_t sx(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

    void gsReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!sotc_decomp::hotList44On())
            return sotc_decomp::recompiled(kGsReg)(rdram, ctx, runtime);
        // v159: main RAM throughout (the record, its two words, the pair's 16 bytes): plain loads and stores,
        // the same values and registers as below; anything else (or a write trace) takes the macros.
#if !PS2X_GUEST_WRITE_TRACE
        {
            uint32_t record, closed, at;
            std::memcpy(&record, rdram + kPacket, 4);
            if (record + 0x20u <= PS2_RAM_SIZE && record >= 0x100000u)
            {
                std::memcpy(&closed, rdram + record + 0x1Cu, 4);
                std::memcpy(&at, rdram + record + 0xCu, 4);
                if (closed == 0u && (at & 7u) == 0u && at >= 0x100000u && at + 16u <= PS2_RAM_SIZE &&
                    (at + 16u <= kPacket || at >= kPacket + 4u) && (at + 16u <= record + 0xCu || at >= record + 0x20u))
                {
                    const uint64_t value = GPR_U64(ctx, 5), reg = GPR_U64(ctx, 4);
                    std::memcpy(rdram + at, &value, 8);
                    std::memcpy(rdram + at + 8u, &reg, 8);
                    const uint32_t end = at + 16u;
                    std::memcpy(rdram + record + 0xCu, &end, 4);
                    SET_GPR_U64(ctx, 7, sx(kPacket));
                    SET_GPR_U64(ctx, 3, sx(record + 0xCu));
                    SET_GPR_U64(ctx, 6, sx(record + 0xCu));
                    SET_GPR_U64(ctx, 2, sx(end));
                    sotc_decomp::returnTo(ctx);
                    return;
                }
            }
        }
#endif
        SET_GPR_U64(ctx, 7, sx(kPacket));
        uint32_t record = READ32(kPacket);
        SET_GPR_U64(ctx, 3, sx(record));
        const uint32_t closed = READ32(record + 0x1Cu);
        SET_GPR_U64(ctx, 6, sx(record + 0xCu));
        if (closed != 0u)
        {
            SET_GPR_U64(ctx, 2, sx(closed));
            sotc_decomp::returnTo(ctx);
            return;
        }
        uint32_t at = READ32(record + 0xCu);
        WRITE64(at, GPR_U64(ctx, 5));
        WRITE32(record + 0xCu, at + 8u);
        record = READ32(kPacket) + 0xCu;
        SET_GPR_U64(ctx, 3, sx(record));
        at = READ32(record);
        WRITE64(at, GPR_U64(ctx, 4));
        SET_GPR_U64(ctx, 2, sx(at + 8u));
        WRITE32(record, at + 8u);
        sotc_decomp::returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x001C5B00u, gsReg, decomp_hot44g::gsReg);
