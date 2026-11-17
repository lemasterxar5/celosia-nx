#pragma once
#include "runtime/native_renderer.h"
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <limits>
#include <atomic>
#include <stdexcept>

namespace native_renderer::detail::deko {
struct ShadowBatch {
    bool pending=false;
    uint32_t order=0;
    float amount=0;
    bool resolveBefore(uint32_t nextOrder) const { return pending && nextOrder>order; }
    void add(uint32_t layer,float darkness) { pending=true;order=layer;amount=darkness; }
    void clear() { pending=false;amount=0; }
};
class AllocationBudget {
    const size_t limit;
    std::atomic<size_t> live{0};
public:
    explicit AllocationBudget(size_t bytes) : limit(bytes) {}
    bool tryReserve(size_t bytes) {
        auto current = live.load(std::memory_order_relaxed);
        do {
            if (bytes > limit - current) return false;
        } while (!live.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
        return true;
    }
    // Called only by the owner of a matching successful reservation.
    void release(size_t bytes) { live.fetch_sub(bytes, std::memory_order_relaxed); }
    size_t used() const { return live.load(std::memory_order_relaxed); }
};
struct PackedImageLayout { uint32_t rowBytes, layerBytes; };
inline PackedImageLayout packedImageLayout(uint32_t w, uint32_t h) {
    if (!w || !h || w > 16384 || h > 16384 || uint64_t(w) * h * 4 > 256 * 1024 * 1024)
        throw std::invalid_argument("deko3d: invalid packed image layout");
    // dk_image.cpp uses rowLength as m_horizontal and imageHeight as m_layerStride,
    // both in bytes. Neither field is a pixel count despite its historical name.
    return {w * 4, w * h * 4};
}
inline size_t textureBytes(uint32_t w, uint32_t h, uint32_t levels) {
    if (!w || !h || w > 16384 || h > 16384 || !levels)
        throw std::invalid_argument("deko3d: invalid texture dimensions/mip count");
    uint32_t maximum = 1;
    for (auto n = std::max(w, h); n > 1; n >>= 1) ++maximum;
    if (levels > maximum) throw std::invalid_argument("deko3d: excessive mip count");
    size_t bytes = 0;
    for (uint32_t i = 0; i < levels; ++i) {
        bytes += size_t(w) * h * 4;
        w = std::max(1u, w / 2); h = std::max(1u, h / 2);
    }
    if (bytes > 256 * 1024 * 1024)
        throw std::invalid_argument("deko3d: single texture exceeds 256 MiB budget");
    return bytes;
}
// Model-space axis-aligned bounds of a mesh, gathered once at upload so sampled
// frames can estimate screen coverage without reading GPU-visible memory back.
struct MeshBounds { float lo[3]{0,0,0}, hi[3]{0,0,0}; bool valid=false; };
inline MeshBounds meshBounds(const Vertex *vertices, uint32_t count) {
    MeshBounds b;
    if (!vertices || !count) return b;
    b.lo[0]=b.hi[0]=vertices[0].x; b.lo[1]=b.hi[1]=vertices[0].y; b.lo[2]=b.hi[2]=vertices[0].z;
    for (uint32_t i = 1; i < count; ++i) {
        const float p[3]{vertices[i].x, vertices[i].y, vertices[i].z};
        for (int k = 0; k < 3; ++k) { b.lo[k] = std::min(b.lo[k], p[k]); b.hi[k] = std::max(b.hi[k], p[k]); }
    }
    b.valid = std::isfinite(b.lo[0]) && std::isfinite(b.lo[1]) && std::isfinite(b.lo[2]) &&
              std::isfinite(b.hi[0]) && std::isfinite(b.hi[1]) && std::isfinite(b.hi[2]);
    return b;
}
// Greatest clip w (the shaders' view depth) over the bounds: w is linear in position, so its
// maximum over the box is at a corner. Invalid bounds give +infinity (no conclusion).
inline float maxClipW(const float clip[16], const MeshBounds &b) {
    if (!b.valid) return std::numeric_limits<float>::infinity();
    float result = -std::numeric_limits<float>::infinity();
    for (unsigned corner = 0; corner < 8; ++corner) {
        const float p[3]{corner & 1 ? b.hi[0] : b.lo[0], corner & 2 ? b.hi[1] : b.lo[1], corner & 4 ? b.hi[2] : b.lo[2]};
        const float w = clip[3]*p[0] + clip[7]*p[1] + clip[11]*p[2] + clip[15];
        if (!std::isfinite(w)) return std::numeric_limits<float>::infinity();
        result = std::max(result, w);
    }
    return result;
}
// Upper-bound fraction of the screen a draw can touch: the clipped NDC bounding box of the
// bounds' eight corners under the column-major clip matrix (c = clip * (p,1), x/w, y/w).
// Corners at or behind the camera plane make the estimate conservative: all behind -> 0,
// mixed -> 1 (whole screen). Screen-space draws pass their positions through unchanged.
inline float screenCoverage(const float clip[16], const MeshBounds &b, bool screenSpace = false) {
    if (!b.valid) return 1.0f;
    float minX = 1, maxX = -1, minY = 1, maxY = -1;
    unsigned behind = 0;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const float p[3]{corner & 1 ? b.hi[0] : b.lo[0], corner & 2 ? b.hi[1] : b.lo[1], corner & 4 ? b.hi[2] : b.lo[2]};
        float x, y, w;
        if (screenSpace) { x = p[0]; y = p[1]; w = 1; }
        else {
            x = clip[0]*p[0] + clip[4]*p[1] + clip[8]*p[2] + clip[12];
            y = clip[1]*p[0] + clip[5]*p[1] + clip[9]*p[2] + clip[13];
            w = clip[3]*p[0] + clip[7]*p[1] + clip[11]*p[2] + clip[15];
        }
        if (!(w > 1e-6f) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w)) { ++behind; continue; }
        const float nx = x / w, ny = y / w;
        minX = std::min(minX, nx); maxX = std::max(maxX, nx);
        minY = std::min(minY, ny); maxY = std::max(maxY, ny);
    }
    if (behind == 8) return 0.0f;
    if (behind) return 1.0f;
    minX = std::clamp(minX, -1.0f, 1.0f); maxX = std::clamp(maxX, -1.0f, 1.0f);
    minY = std::clamp(minY, -1.0f, 1.0f); maxY = std::clamp(maxY, -1.0f, 1.0f);
    if (maxX <= minX || maxY <= minY) return 0.0f;
    return (maxX - minX) * (maxY - minY) * 0.25f;
}
inline size_t vertexBytes(uint32_t count) {
    if (!count || size_t(count) * sizeof(Vertex) > 128 * 1024 * 1024)
        throw std::invalid_argument("deko3d: empty mesh or mesh exceeds 128 MiB");
    return size_t(count) * sizeof(Vertex);
}
inline size_t meshBytes(uint32_t count) {
    if (count % 3) throw std::invalid_argument("deko3d: invalid triangle list");
    return vertexBytes(count);
}
inline void validateIndices(const uint32_t *indices, uint32_t count, uint32_t vertices) {
    if (!indices || !count || count % 3 || count > 16 * 1024 * 1024)
        throw std::invalid_argument("deko3d: invalid index list");
    for (uint32_t i = 0; i < count; ++i)
        if (indices[i] >= vertices) throw std::invalid_argument("deko3d: index exceeds vertex range");
}
// CPU oracle for the material shader's exact quantized alpha comparisons.
inline bool alphaPass(float alpha, int test, int reference) {
    const float a = std::floor(alpha * 128.0f + 0.001f);
    switch (test) {
    case 0: return false; case 1: return true;
    case 2: return a < reference; case 3: return a <= reference;
    case 4: return a == reference; case 5: return a >= reference;
    case 6: return a > reference; case 7: return a != reference;
    default: throw std::invalid_argument("deko3d: invalid alpha comparison");
    }
}
inline void validateDraw(const DrawState &d, bool depthFogDisabled = false) {
    // Without the depth-fog composite these are ordinary sky/world draws.
    // Their geometry, material, vertex fog and sky near-cut still render.
    if ((d.background || d.fogged) && !depthFogDisabled)
        throw std::invalid_argument("deko3d: depth-fog background composite not implemented");
    if(d.shadowVolume && (!std::isfinite(d.shadow) || d.shadow<0 || d.shadow>1))
        throw std::invalid_argument("deko3d: invalid shadow amount");
    if(d.reflectionSource || d.reflectionSample)throw std::invalid_argument("deko3d: reflection not implemented");
    if(d.refractionSample)throw std::invalid_argument("deko3d: refraction not implemented");
    if(d.screenCopy)throw std::invalid_argument("deko3d: screen copy not implemented");
    if(d.previousFrame)throw std::invalid_argument("deko3d: previous-frame sampling not implemented");
    if(d.skinPalette || d.skinPaletteCount)throw std::invalid_argument("deko3d: GPU skinning not implemented");
    if(d.mirrorRipple[1] != 0)throw std::invalid_argument("deko3d: mirror ripple not implemented");
    if (d.clamp > 3 || static_cast<unsigned>(d.blend) > static_cast<unsigned>(Blend::Subtract))
        throw std::invalid_argument("deko3d: invalid sampler/blend mode");
    if (!std::isfinite(d.opacity) || !std::isfinite(d.lodK) ||
        !std::isfinite(d.lodScale) || !std::isfinite(d.maxLod) ||
        !std::isfinite(d.uvOffset[0]) || !std::isfinite(d.uvOffset[1]) ||
        !std::isfinite(d.mirrorRipple[0]))
        throw std::invalid_argument("deko3d: non-finite material parameter");
    for (const auto value : d.light)
        if (!std::isfinite(value)) throw std::invalid_argument("deko3d: non-finite light matrix");
}
inline void validateFog(const Fog &f) {
    // zFog (enabled): fogged draws and the world before the fog's layer fade to the background
    // image (render.cpp); start/width/steps must describe a usable ramp.
    if (f.enabled && (!std::isfinite(f.start) || !std::isfinite(f.width) || f.width <= 0 || f.steps < 0 || f.steps > 128))
        throw std::invalid_argument("deko3d: invalid zFog ramp");
    if (!std::isfinite(f.base) || !std::isfinite(f.scale) || !std::isfinite(f.skyNear) ||
        f.skyNear < 0)
        throw std::invalid_argument("deko3d: invalid vertex fog");
    for (const auto colour : f.colour)
        if (!std::isfinite(colour)) throw std::invalid_argument("deko3d: invalid fog colour");
}
}
