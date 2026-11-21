// v170: cheats ("Trucos"). The NTSC-U disc's known codes (SCUS-97472), in the raw CodeBreaker / PCSX2 form,
// run once a frame on the game thread (syncFrame), as PCSX2 runs them at each vsync. A static recompilation
// keeps the PS2's memory layout, so a code that writes DATA does the same here; a code that patches
// INSTRUCTIONS would write bytes no one executes (the code is native), so none of those is listed.
// Every address below was checked against the recompiled functions' ranges: all are outside any function
// (0x0116AE80 - 0x01339E68 holds no code; 0x0013D85C, the joker's pad word, is data too).
// Proof that each acts: "[cheat]" lines in the log (every 600 frames with the cheat on): how many frames its
// condition held, how many words it wrote, and the target's value read back after the write.
// v170a: the listed codes were made for another build: their data block (0x012Dxxxx) is 0xFA380 lower in this
// ELF (relocate: getPlayerGrip / getPlayerHp read 0x011E3EE0 / 0x011E3EE4, the codes 0x012DE260 / 264); v170
// wrote unused memory, so none of them acted. Health and grip now write the game's own maximum.
#include "sotc_cheats.h"

#include "../../PS2Recomp/ps2xRuntime/platform/switch/hybrid/live_settings.hpp"

#include "runtime/ps2_stats.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sotc_cheats
{
    namespace
    {
        struct Line
        {
            uint32_t a, v;
        };
        struct Cheat
        {
            const char *name;
            const Line *lines;
            int count;
            uint32_t probe;  // the address read back for the log (the code's; relocated as the writes)
            int maxIndex;    // v170a: >= 0: the 32-bit write is the game's own maximum (its stat table entry)
        };
        // v170a: the codes' data block (0x012Dxxxx) is 0xFA380 lower in this ELF (see relocate).
        constexpr uint32_t kBlockFrom = 0x012C0000u, kBlockTo = 0x01300000u, kShift = 0x000FA380u;
        uint32_t relocate(uint32_t a) { return a >= kBlockFrom && a < kBlockTo ? a - kShift : a; }
        // The player's stat table (_gameFWorkChk 0x134f120: *(0x01465B70) + index * 4): 0xB max HP, 0xC max grip.
        constexpr uint32_t kStatTable = 0x01465B70u, kHp = 0x011E3EE4u;
        // Sources: psxdatacenter.com (raw codes, SCUS-97472); almarsguides.com (GameShark v3-4 / Xploder v4
        // lists whose lines are already raw: type nibble + address).
        constexpr Line kHealth[] = {{0x212DE264u, 0x447A0000u}};
        constexpr Line kGrip[] = {{0x212DE260u, 0x447A0000u}};
        // v171s (user: Wander's held items, then Wander himself, gone): the code's two words land on 0x011E0060-67 here, the
        // bonus items' SELECTED flags (_omakeSelectFlagChk: 0x011E0062 + bit) -- every item equipped, the invisibility cloak
        // too. The OWNED flags are 0x011E005A-61 (_omakeGetFlagChk): all items = those 8 bytes set (type 0 lines), nothing
        // equipped.
        constexpr Line kItems[] = {{0x011E005Au, 0xFFu}, {0x011E005Bu, 0xFFu}, {0x011E005Cu, 0xFFu}, {0x011E005Du, 0xFFu},
                                   {0x011E005Eu, 0xFFu}, {0x011E005Fu, 0xFFu}, {0x011E0060u, 0xFFu}, {0x011E0061u, 0xFFu}};
        constexpr Line kColossus15[] = {{0xE00AF7BFu, 0x0013D85Cu}, {0x012D9BC0u, 0x0000000Eu}, {0x012D9800u, 0x00000000u},
                                        {0x012D9B5Cu, 0x000000EFu}, {0x012D9B5Bu, 0x000000FFu}, {0x012D9B5Du, 0x000000FFu},
                                        {0x012D9B55u, 0x00000080u}, {0x012D9B56u, 0x00000080u}, {0x112D9B58u, 0x0000FDFCu},
                                        {0x012D9B5Au, 0x00000001u}, {0x112D9B88u, 0x00000839u}};
        constexpr Line kColossus16[] = {{0xE007F77Fu, 0x0013D85Cu}, {0x012D9BC0u, 0x0000000Fu}, {0x012D9B5Du, 0x000000DFu},
                                        {0x012D9B5Bu, 0x000000FFu}, {0x012D9B5Cu, 0x000000FFu}, {0x012D9B55u, 0x00000000u},
                                        {0x012D9B56u, 0x00000080u}, {0x112D9B88u, 0x00000839u}};
        const Cheat kCheats[kCount] = {
            {"infinite health", kHealth, 1, 0x012DE264u, 0xB},
            {"infinite grip", kGrip, 1, 0x012DE260u, 0xC},
            {"all items", kItems, 8, 0x011E005Cu, -1},
            {"story skip to colossus 15 (R1 + down)", kColossus15, 11, 0x012D9BC0u, -1},
            {"story skip to colossus 16 (R1 + left)", kColossus16, 8, 0x012D9BC0u, -1},
        };
        struct Stats
        {
            uint32_t frames = 0, held = 0, writes = 0;
        };
        Stats g_stats[kCount];
        uint32_t g_lastJoker = 0xFFFFFFFFu;

        constexpr uint32_t kRam = 32u << 20;
        bool ok(uint32_t a, uint32_t bytes) { return a < kRam && bytes <= kRam - a; }
        uint16_t rd16(const uint8_t *rdram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, rdram + a, 2);
            return v;
        }
        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + a, 4);
            return v;
        }

        // One line's write (types 0 / 1 / 2); false: not a write type or out of memory.
        bool write(uint8_t *rdram, uint32_t type, uint32_t a, uint32_t v, Stats &st)
        {
            if (type == 0u && ok(a, 1))
                rdram[a] = static_cast<uint8_t>(v);
            else if (type == 1u && ok(a & ~1u, 2))
            {
                const uint16_t h = static_cast<uint16_t>(v);
                std::memcpy(rdram + (a & ~1u), &h, 2);
            }
            else if (type == 2u && ok(a & ~3u, 4))
                std::memcpy(rdram + (a & ~3u), &v, 4);
            else
                return false;
            ++st.writes;
            return true;
        }

        // The raw CodeBreaker types these codes use: 0 / 1 / 2 (8 / 16 / 32-bit write), 4 (32-bit serial
        // write: count, step in words, the next line's value and increment), D (16-bit equal: the next line
        // only if it holds), E (16-bit equal: the next n lines only if it holds).
        void run(uint8_t *rdram, const Cheat &c, Stats &st)
        {
            bool held = true;
            if (c.maxIndex >= 0)
            {
                // v170a: only with the player alive and the stat table in RAM; the value is its maximum.
                const uint32_t table = rd32(rdram, kStatTable), at = table + static_cast<uint32_t>(c.maxIndex) * 4u;
                float hp, most;
                std::memcpy(&hp, rdram + kHp, 4);
                held = table >= 0x00100000u && ok(at, 4);
                if (held)
                    std::memcpy(&most, rdram + at, 4);
                held = held && hp > 0.0f && most > 0.0f && most < 1e6f;
                if (held)
                {
                    uint32_t v;
                    std::memcpy(&v, &most, 4);
                    write(rdram, 2u, relocate(c.lines[0].a & 0x0FFFFFFFu), v, st);
                }
                ++st.frames;
                st.held += held ? 1u : 0u;
                return;
            }
            for (int i = 0; i < c.count; ++i)
            {
                const uint32_t type = c.lines[i].a >> 28, a = relocate(c.lines[i].a & 0x0FFFFFFFu), v = c.lines[i].v;
                if (type == 0xEu && (c.lines[i].v & 0x0FFFFFFFu) == 0x0013D85Cu)
                {
                    // v171s: the joker (the other build's pad word, never matching here): the buttons the game read
                    const uint32_t raw = c.lines[i].a & 0x0FFFFFFFu, n = (raw >> 16) & 0xFFu, want = raw & 0xFFFFu;
                    if (ps2_stats::padButtons.load(std::memory_order_relaxed) != want)
                    {
                        held = false;
                        i += static_cast<int>(n);
                    }
                    continue;
                }
                if (type <= 2u)
                    write(rdram, type, a, v, st);
                else if (type == 4u && i + 1 < c.count)
                {
                    const uint32_t n = v >> 16, step = (v & 0xFFFFu) * 4u;
                    uint32_t value = c.lines[i + 1].a;
                    const uint32_t inc = c.lines[i + 1].v;
                    for (uint32_t k = 0; k < n; ++k, value += inc)
                        write(rdram, 2u, a + k * step, value, st);
                    ++i;
                }
                else if (type == 0xDu)
                {
                    if (!ok(a & ~1u, 2) || rd16(rdram, a & ~1u) != static_cast<uint16_t>(v)) // (a relocated)
                    {
                        held = false;
                        ++i; // skip the next line
                    }
                }
                else if (type == 0xEu)
                {
                    const uint32_t raw = c.lines[i].a & 0x0FFFFFFFu; // E: the address field holds n / value
                    const uint32_t n = (raw >> 16) & 0xFFu, want = raw & 0xFFFFu, at = relocate(v & 0x0FFFFFFFu), cmp = v >> 28;
                    const bool eq = ok(at & ~1u, 2) && rd16(rdram, at & ~1u) == want;
                    if (cmp != 0u || !eq)
                    {
                        held = false;
                        i += static_cast<int>(n);
                    }
                }
            }
            ++st.frames;
            st.held += held ? 1u : 0u;
        }
    }

    const char *name(int i) { return i >= 0 && i < kCount ? kCheats[i].name : ""; }

    void frame(uint8_t *rdram)
    {
        const uint32_t mask = celosia::live::settings().cheats.load(std::memory_order_relaxed);
        if (!rdram)
            return;
        {
            // v171s: v170a's all-items code equipped every bonus item (0x011E0062-67 all set; the invisibility cloak among
            // them: Wander not drawn), kept in RAM / saves / states: set like that, it is undone once (nothing equipped)
            static bool repaired = false;
            bool all = true;
            for (uint32_t a = 0x011E0062u; a <= 0x011E0067u; ++a)
                all = all && rdram[a] == 0xFFu;
            if (all)
            {
                std::memset(rdram + 0x011E0062u, 0, 6);
                if (!repaired)
                    std::fprintf(stderr, "[cheat] every bonus item was equipped (v170a's all-items code): unequipped\n");
                repaired = true;
            }
        }
        if (mask == 0u)
            return;
        // The joker cheats' pad word (buttons active low): logged when it changes, to prove the address.
        if ((mask & ((1u << kColossus15Index) | (1u << kColossus16Index))) != 0u)
        {
            const uint32_t pad = rd16(rdram, 0x0013D85Cu);
            if (pad != g_lastJoker)
            {
                std::fprintf(stderr, "[cheat] pad word 0x0013D85C = %04X\n", pad);
                g_lastJoker = pad;
            }
        }
        for (int i = 0; i < kCount; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            Stats &st = g_stats[i];
            run(rdram, kCheats[i], st);
            if (i == 2 && rd32(rdram, 0x011E0074u) == 0u)
            {
                // v171u: the pause menu's bonus items need the game cleared once too (pauseMenuScript: getGameClearCnt
                // 0x011E0074 > 0, then GetNumOmakeItem > 0): with all items on, a count of 0 becomes 1
                const uint32_t one = 1u;
                std::memcpy(rdram + 0x011E0074u, &one, 4);
                std::fprintf(stderr, "[cheat] all items: game clear count 0 -> 1 (the pause menu's items need it)\n");
            }
            if (st.frames % 600u == 1u)
                std::fprintf(stderr, "[cheat] %s: %u frames, condition held %u, %u words written, 0x%08X = %08X\n", kCheats[i].name,
                             st.frames, st.held, st.writes, relocate(kCheats[i].probe), rd32(rdram, relocate(kCheats[i].probe) & ~3u));
        }
    }
}
