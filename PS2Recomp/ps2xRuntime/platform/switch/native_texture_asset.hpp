#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace celosia::native {
struct NativeTextureAsset {
    struct Level {uint32_t width,height;size_t offset;};
    std::vector<Level> levels;
    // RGBA bytes retain the asset's original alpha scale (128 = opaque).
    // Material shaders must apply that scale, as the existing native renderer does.
    std::vector<uint8_t> rgba;
};
// Pure NTO2 asset conversion. The span starts at the 32-byte NTO2 header;
// pixel/palette offsets are relative to it. Supports psm00,13,14, <=1024 per
// dimension and <=7 requested mip levels. Throws invalid_argument for malformed
// or unsupported input; bad_alloc propagates. No caller memory is modified.
NativeTextureAsset decodeNativeTextureAsset(std::span<const uint8_t> asset);
}
