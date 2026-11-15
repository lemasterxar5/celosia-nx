#pragma once
#include "runtime/native_renderer.h"
#include <cstddef>
#include <algorithm>
#include <cmath>
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
    if (f.enabled) throw std::invalid_argument("deko3d: zFog composite not implemented");
    if (!std::isfinite(f.base) || !std::isfinite(f.scale) || !std::isfinite(f.skyNear) ||
        f.skyNear < 0)
        throw std::invalid_argument("deko3d: invalid vertex fog");
    for (const auto colour : f.colour)
        if (!std::isfinite(colour)) throw std::invalid_argument("deko3d: invalid fog colour");
}
}
