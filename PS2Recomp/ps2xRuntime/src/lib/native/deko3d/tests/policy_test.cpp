#include "../policy.hpp"
#include <cstdio>
#include <stdexcept>
#include <limits>

using namespace native_renderer;
using namespace native_renderer::detail::deko;
static unsigned checks = 0;
static void check(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> static void rejects(F fn, const char *message) {
    bool threw = false;
    try { fn(); } catch (const std::invalid_argument &) { threw = true; }
    check(threw, message);
}
int main() {
    try {
        {
            // Screen coverage upper bound: identity clip, unit box spanning NDC [-0.5,0.5]^2 -> 1/4 screen.
            Vertex quad[4]{};
            quad[0].x=-0.5f;quad[0].y=-0.5f;quad[1].x=0.5f;quad[1].y=-0.5f;quad[2].x=0.5f;quad[2].y=0.5f;quad[3].x=-0.5f;quad[3].y=0.5f;
            const auto bounds=meshBounds(quad,4);
            check(bounds.valid && bounds.lo[0]==-0.5f && bounds.hi[1]==0.5f, "mesh bounds gather min/max");
            float identity[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            check(std::abs(screenCoverage(identity,bounds)-0.25f)<1e-6f, "quarter-screen box covers 0.25");
            check(std::abs(screenCoverage(identity,bounds,true)-0.25f)<1e-6f, "screen-space box bypasses the matrix");
            float scaled[16]{4,0,0,0, 0,4,0,0, 0,0,1,0, 0,0,0,1};
            check(std::abs(screenCoverage(scaled,bounds)-1.0f)<1e-6f, "oversized box clamps to the whole screen");
            float offscreen[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 5,0,0,1};
            check(screenCoverage(offscreen,bounds)==0.0f, "box entirely outside the viewport covers nothing");
            // w = z + 10 (row 3 of the column-major matrix): the box spans z 0..0 -> greatest w 10.
            float depthClip[16]{1,0,0,0, 0,1,0,0, 0,0,1,1, 0,0,0,10};
            check(std::abs(maxClipW(depthClip,bounds)-10.0f)<1e-6f, "greatest clip w at a corner");
            MeshBounds none;
            check(std::isinf(maxClipW(depthClip,none)), "invalid bounds give no fog conclusion");
            float behind[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,-1};
            check(screenCoverage(behind,bounds)==0.0f, "box behind the camera covers nothing");
            float straddle[16]{1,0,0,0, 0,1,0,0, 0,0,1,1, 0,0,0,0.25f};
            // w = z + 0.25 with z in {0}: all w>0; move one corner behind via z range
            Vertex depthQuad[2]{};depthQuad[0].x=-0.1f;depthQuad[0].y=-0.1f;depthQuad[0].z=-1;depthQuad[1].x=0.1f;depthQuad[1].y=0.1f;depthQuad[1].z=1;
            check(screenCoverage(straddle,meshBounds(depthQuad,2))==1.0f, "box straddling the camera plane is conservative");
            check(screenCoverage(identity,MeshBounds{})==1.0f, "unknown bounds are conservative");
            // Perspective: w=4 shrinks the same box to 1/64 of the screen.
            float perspective[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,4};
            check(std::abs(screenCoverage(perspective,bounds)-0.25f/16)<1e-6f, "perspective divide scales coverage");
        }
        AllocationBudget budget(20);
        check(budget.tryReserve(8), "first allocation fits shared budget");
        check(budget.tryReserve(8), "second allocation counted once");
        check(!budget.tryReserve(8), "old retained resources prevent overcommit");
        check(budget.used() == 16, "failed reservation does not consume budget");
        budget.release(8);
        check(budget.tryReserve(12), "destroyed resource returns allocation capacity");
        check(budget.used() == 20, "all live aligned bytes counted");
        budget.release(20);
        check(budget.used() == 0, "all allocations released");
        check(textureBytes(8, 4, 4) == 172, "RGBA mip chain includes rectangular tail");
        check(textureBytes(1, 1, 1) == 4, "single pixel mip");
        rejects([] { textureBytes(0, 4, 1); }, "zero width rejected");
        rejects([] { textureBytes(4, 4, 4); }, "too many mips rejected");
        rejects([] { textureBytes(16385, 4, 1); }, "GPU dimension limit enforced");
        rejects([] { textureBytes(16384, 16384, 1); }, "texture allocation budget enforced");
        rejects([] { textureBytes(4, 4, 0); }, "empty mip chain rejected");
        rejects([] { meshBytes(0); }, "empty mesh rejected");
        rejects([] { meshBytes(4); }, "triangle list count enforced");
        check(meshBytes(3) == 3 * sizeof(Vertex), "triangle byte size");
        const auto transfer = packedImageLayout(8, 4);
        check(transfer.rowBytes == 32 && transfer.layerBytes == 128,
              "deko transfer pitch and layer stride are BYTES, not pixels");
        rejects([] { packedImageLayout(0, 1); }, "empty transfer rejected");
        rejects([] { packedImageLayout(16384, 16384); }, "oversize transfer rejected");
        check(vertexBytes(4) == 4 * sizeof(Vertex), "indexed quad permits four vertices");
        rejects([] { vertexBytes(0xffffffffu); }, "oversize vertices rejected");
        const uint32_t quad[]{0, 1, 2, 0, 2, 3};
        validateIndices(quad, 6, 4);
        rejects([&] { validateIndices(quad, 6, 3); }, "index exactly at bound rejected");
        rejects([&] { validateIndices(quad, 4, 4); }, "incomplete indexed triangle rejected");
        rejects([] { validateIndices(nullptr, 3, 4); }, "missing index data rejected");
        check(alphaPass(0.5f, 5, 64), "GEQUAL inclusive");
        check(!alphaPass(0.49f, 5, 64), "GEQUAL below threshold");
        check(alphaPass(0.5f, 4, 64), "EQUAL quantized alpha");
        check(!alphaPass(1.0f, 0, 0), "NEVER alpha");
        check(alphaPass(0.0f, 1, 255), "ALWAYS alpha");
        check(alphaPass(0.25f, 2, 64), "LESS alpha");
        check(!alphaPass(0.5f, 2, 64), "LESS equality fails");
        check(alphaPass(0.5f, 3, 64), "LEQUAL inclusive");
        check(!alphaPass(0.75f, 3, 64), "LEQUAL above fails");
        check(!alphaPass(0.25f, 4, 64), "EQUAL mismatch");
        check(alphaPass(0.75f, 6, 64), "GREATER alpha");
        check(!alphaPass(0.5f, 6, 64), "GREATER equality fails");
        check(alphaPass(0.25f, 7, 64), "NOTEQUAL alpha");
        check(!alphaPass(0.5f, 7, 64), "NOTEQUAL equality fails");
        rejects([] { alphaPass(0.5f, 8, 64); }, "invalid alpha comparison rejected");
        DrawState d;
        validateDraw(d);
        d.shadowVolume=true;validateDraw(d);
        check(true,"real shadow volumes accepted");
        ShadowBatch shadows;
        check(!shadows.resolveBefore(8),"empty volume batch does not darken");
        shadows.add(7,0.5f);
        check(!shadows.resolveBefore(7),"same-layer volumes accumulate together");
        check(shadows.resolveBefore(8),"resolve before later transparent layer");
        check(shadows.pending && shadows.amount==0.5f,"frame-end volume resolve remains pending");
        shadows.clear();check(!shadows.pending,"resolved stencil batch is retired");
        shadows.add(7,0);
        check(shadows.resolveBefore(8),"zero-darkness volumes must still clear stencil");
        d.shadow=0;validateDraw(d);d.shadow=1;validateDraw(d);
        d.shadow=-0.1f;rejects([&]{validateDraw(d);},"negative shadow amount rejected");
        d.shadow=1.1f;rejects([&]{validateDraw(d);},"excess shadow amount rejected");
        d.shadow=std::numeric_limits<float>::quiet_NaN();
        rejects([&]{validateDraw(d);},"non-finite shadow amount rejected");
        d={};
        d.background=true;
        validateDraw(d,true);
        check(true,"sky geometry renders directly when depth-fog composite is disabled");
        d={};d.fogged=true;
        validateDraw(d,true);
        check(true,"world geometry renders directly when depth-fog composite is disabled");
        d={};d.reflectionSample=true;
        rejects([&]{validateDraw(d,true);},"plain sky path cannot suppress reflection requirements");
        d={};
        d.reflectionSample = true;
        rejects([&] { validateDraw(d); }, "reflection cannot silently disappear");
        d = {}; d.skinPaletteCount = 4;
        rejects([&] { validateDraw(d); }, "GPU skinning cannot silently disappear");
        d = {}; d.clamp = 4;
        rejects([&] { validateDraw(d); }, "unknown sampler mode rejected");
        d = {}; d.uvOffset[0] = std::numeric_limits<float>::infinity();
        rejects([&] { validateDraw(d); }, "non-finite scrolling rejected");
        d = {}; d.light[31] = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { validateDraw(d); }, "non-finite light matrix rejected");
        bool DrawState::*const unsupported[]{&DrawState::background, &DrawState::fogged,
            &DrawState::reflectionSource, &DrawState::reflectionSample,
            &DrawState::refractionSample, &DrawState::screenCopy, &DrawState::previousFrame};
        for (auto field : unsupported) {
            d = {}; d.*field = true;
            rejects([&] { validateDraw(d); }, "unsupported path rejected explicitly");
        }
        d = {}; d.blend = static_cast<Blend>(255);
        rejects([&] { validateDraw(d); }, "invalid blend rejected");
        d = {}; d.maxLod = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { validateDraw(d); }, "non-finite LOD rejected");
        Fog f;
        validateFog(f);
        f.vertexFog = true; f.scale = 2; validateFog(f);
        f.enabled = true; f.start = 100; f.width = 400; f.steps = 8; validateFog(f);
        f.width = 0;
        rejects([&] { validateFog(f); }, "empty zFog ramp rejected");
        f.width = 400; f.steps = -1;
        rejects([&] { validateFog(f); }, "negative zFog steps rejected");
        f.steps = 8; f.start = std::numeric_limits<float>::infinity();
        rejects([&] { validateFog(f); }, "non-finite zFog start rejected");
        f = {}; f.skyNear = -1;
        rejects([&] { validateFog(f); }, "negative near cut rejected");
        f = {}; f.colour[0] = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { validateFog(f); }, "non-finite fog colour rejected");
        std::printf("policy: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
