#pragma once
// v167: texture packs in PCSX2's replacement format (<TEX0 hash>-<CLUT hash>-<bits>.dds), as the
// Panda Venom pack for SotC: each PS2 texture keeps its own decode and upload, and a pack texture
// found for it is loaded on a thread of its own and takes its place (same key) a few frames later.
// The menu's switch applies at once: the keys of textures while it is on carry a salt (keySalt), so
// switching it on decodes them anew (and looks them up), switching it off finds the PS2 ones again.
#include <cstdint>
#include <vector>

namespace sotc_texpack
{
    // Salt for this frame's texture keys (0 = off). Game thread.
    uint64_t keySalt();
    // What a PS2 palette texture is, for the lookup (on any thread). `indices`: every level's palette
    // indices (one byte a texel, rows of `width`), level after level as the texture holds them.
    struct Source
    {
        uint32_t psm = 0, width = 0, height = 0, levels = 1;
        const uint32_t *palette = nullptr; // 16 (PSMT4) or 256 (PSMT8) entries, index order
        const uint8_t *indices = nullptr;
        // The PS2 texture's alpha figures (for the alpha grid's size rule).
        bool gridWanted = false;
    };
    // Looked up (worker thread): 0 nothing, else a handle request() takes on the game thread.
    uint32_t find(const Source &source);
    // Game thread: load pack entry `handle` for the texture uploaded under `key`.
    void request(uint64_t key, uint32_t handle, uint32_t ps2Width, uint32_t ps2Height, bool gridWanted);
    // Game thread, once a frame: finished loads uploaded, the switch followed.
    void frame();
}
