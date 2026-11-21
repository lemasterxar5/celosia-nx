#pragma once
// v173m: frame generation as NaGaa95's Cemu-nx 1.3.3 runs it (Deko3D DKFrameGenerator over lsfg-deko3d,
// dependencies/lsfg-deko3d, GPL-3.0-or-later): the image graph, schedule and bindings of lsfg-deko3d, the
// high-precision SPIR-V block of the user's own Lossless.dll prepared on the PC (private/lsfg/prep, lsfgprep)
// into sdmc:/switch/celosia_nx/lsfg/Cache/{Quality,Performance}. Quality / performance network and a flow
// divisor of 1, 2 or 4, as Cemu-nx offers. (v153-v173l: lsfg-vk v1.0.0's graph over the DXBC shaders.)
#include "resources.hpp"
#include <lsfg/backend/binding.hpp>
#include <lsfg/backend/cache_load.hpp>
#include <lsfg/backend/layout.hpp>
#include <lsfg/backend/schedule.hpp>
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/image_graph.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace native_renderer::detail::deko::lsfg {

struct Img {
    std::shared_ptr<Image> image;
    uint32_t w = 0, h = 0;
    uint32_t tex = 0; // a sampled descriptor in imageSetGpu() (the generated frames, for render.cpp's blit)
};

class FrameGen {
public:
    // width x height: the presented picture; flowDiv: 1, 2 or 4 (Cemu-nx's flow scale); performance: lsfg's
    // performance network; maxGen: generated frames per real one (Cemu-nx's multiplier - 1).
    // Throws when the shader cache is missing or memory runs out.
    FrameGen(DkDevice device, std::shared_ptr<AllocationBudget> budget, uint32_t width, uint32_t height,
             float flowDiv, bool performance, uint32_t maxGen);
    // As DKFrameGenerator::Generate: the real frame is rendered into input() (the history image of the current
    // phase), then recordInput records every dispatch of the chain for that phase and advances the phase. It
    // returns true once more than the schedule's warmup frames went in since the last reset (the generated
    // frames are valid then). Leaves the frame generation's descriptor sets bound.
    const Img &input(uint64_t = 0) const { return history[phase % ::lsfg::graph::history_image_count]; }
    const Img &historyImage(uint32_t i) const { return history[i % 2]; }
    uint32_t inputIndex() const { return phase % ::lsfg::graph::history_image_count; }
    const Img &outputFor(uint64_t, uint32_t pass) const { return generated.at(pass); }
    bool recordInput(DkCmdBuf cmd, uint64_t frame);
    void recordGenerated(DkCmdBuf, uint64_t, uint32_t, float) {} // all recorded by recordInput
    void reset() { framesSinceReset = 0; }
    bool ready() const { return framesSinceReset > schedule.warmup_frames; }
    uint32_t width() const { return w; }
    uint32_t height() const { return h; }
    float flow() const { return float(config.flow_denominator); }
    bool performance() const { return config.performance; }
    DkGpuAddr imageSetGpu() const;
    uint32_t imageSlots() const { return imageDescriptors; }
    DkGpuAddr samplerSetGpu() const;
    uint32_t maxGenerated() const { return uint32_t(generated.size()); }
    static bool shadersPresent(bool performance);
    static std::string cacheDirectory(bool performance);
    // GPU timestamps of its stages (8 per frame, frame % 4), read back two real frames later.
    void stamp(DkCmdBuf cmd, uint64_t frame, int index);
    bool stageNs(uint64_t frame, int from, int to, uint64_t &ns) const;

private:
    DkDevice device;
    std::shared_ptr<AllocationBudget> budget;
    uint32_t w, h;
    ::lsfg::graph::Config config;
    ::lsfg::cache::Loaded cache;
    ::lsfg::backend::Schedule schedule;
    ::lsfg::backend::Plan plan;
    ::lsfg::backend::DescriptorLayout layout;
    std::shared_ptr<Buffer> code, imageMemory, imageSet, samplerSet, uniforms, stamps;
    std::vector<DkShader> shaders;
    std::vector<DkImage> images;        // the graph's own images (one block, zero-filled as Cemu-nx's)
    std::vector<const DkImage *> chain; // every graph image (own, history, generated)
    std::array<Img, 2> history;
    std::vector<Img> generated;
    std::vector<uint8_t> hazards;
    std::vector<uint32_t> hazardImages;
    uint32_t imageDescriptors = 0, phase = 0, framesSinceReset = 0;

    void record(DkCmdBuf cmd, uint32_t dispatch);
    void barrier(DkCmdBuf cmd, uint32_t invalidate);
    void mark(uint32_t image, uint8_t hazard);
};

}
