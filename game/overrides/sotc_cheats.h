#pragma once
#include <cstdint>

// v170: the cheats menu's codes (sotc_cheats.cpp). Bit i of live settings' `cheats` turns cheat i on.
namespace sotc_cheats
{
    constexpr int kCount = 5;
    constexpr int kColossus15Index = 3, kColossus16Index = 4;
    // Once a frame on the game thread (syncFrame): the enabled cheats' codes on guest memory.
    void frame(uint8_t *rdram);
    const char *name(int i);
}
