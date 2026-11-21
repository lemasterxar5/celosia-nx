// v173m: frame generation as Cemu-nx 1.3.3's Deko3D DKFrameGenerator (NaGaa95 / PalindromicBreadLoaf, over
// lsfg-deko3d, GPL-3.0-or-later): see lsfg.hpp. The structure follows DKFrameGenerator.cpp; the differences are
// this renderer's: the history images are render targets (the post pass draws the real frame straight into
// them instead of a blit), the generated images get one more sampled descriptor (the blit onto the display
// image), and the prepass and each generated frame are recorded where render.cpp asks for them.
#include "lsfg.hpp"
#include "gpu_timing.hpp"
#include <lsfg/common/cache_format.hpp>
#include <lsfg/common/dksh.hpp>
#include <lsfg/common/error.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>

namespace native_renderer::detail::deko::lsfg {
namespace {

namespace backend = ::lsfg::backend;
namespace graph = ::lsfg::graph;

constexpr uint32_t DescriptorSize = sizeof(DkImageDescriptor);
static_assert(sizeof(DkImageDescriptor) == sizeof(DkSamplerDescriptor));
static_assert(backend::uniform_buffer_stride % DK_UNIFORM_BUF_ALIGNMENT == 0);
static_assert(backend::max_texture_slots <= DK_NUM_TEXTURE_BINDINGS);
static_assert(backend::max_storage_slots <= DK_NUM_IMAGE_BINDINGS);
constexpr uint64_t MemoryBudget = 128ull * 1024 * 1024; // Cemu-nx's
constexpr uint8_t HazardRead = 1, HazardWritten = 2;

DkImageFormat formatOf(graph::Format format)
{
    switch (format) {
    case graph::Format::rgba8: return DkImageFormat_RGBA8_Unorm;
    case graph::Format::r8: return DkImageFormat_R8_Unorm;
    case graph::Format::rgba16f: return DkImageFormat_RGBA16_Float;
    }
    return DkImageFormat_None;
}

void describeSampler(uint32_t index, DkSampler *sampler)
{
    dkSamplerDefaults(sampler);
    sampler->minFilter = DkFilter_Linear;
    sampler->magFilter = DkFilter_Linear;
    sampler->mipFilter = DkMipFilter_Linear;
    const bool edge = index == uint32_t(graph::Sampler::edge) || index == backend::introduced_sampler;
    for (DkWrapMode &mode : sampler->wrapMode) mode = edge ? DkWrapMode_ClampToEdge : DkWrapMode_ClampToBorder;
    if (index == uint32_t(graph::Sampler::border_white))
        for (auto &channel : sampler->borderColor) channel.value_f = 1.0f;
}

[[noreturn]] void fail(const std::string &why)
{
    std::fprintf(stderr, "[lsfg] %s\n", why.c_str());
    throw std::runtime_error("lsfg: " + why);
}

std::string name(::lsfg::ErrorCode code) { return std::string(::lsfg::error_name(code)); }

}

std::string FrameGen::cacheDirectory(bool performance)
{
    return std::string("sdmc:/switch/celosia_nx/lsfg/Cache/") + (performance ? "Performance" : "Quality");
}

bool FrameGen::shadersPresent(bool performance)
{
    struct stat st;
    return stat((cacheDirectory(performance) + "/manifest.bin").c_str(), &st) == 0;
}

FrameGen::FrameGen(DkDevice dev, std::shared_ptr<AllocationBudget> b, uint32_t width, uint32_t height, float flowDiv,
                   bool performance, uint32_t maxGen)
    : device(dev), budget(std::move(b)), w(width), h(height)
{
    config.performance = performance;
    config.generated_frames = std::clamp(maxGen, 1u, graph::max_generated_frames);
    config.flow_numerator = 1;
    config.flow_denominator = flowDiv >= 3.0f ? 4u : flowDiv >= 1.5f ? 2u : 1u;

    // LoadChain
    if (const auto code = ::lsfg::cache::read(cacheDirectory(performance), cache); !::lsfg::succeeded(code))
        fail("shader cache not readable (" + name(code) + "): copy private/lsfg/prep/cache to the SD");
    if (const auto code = graph::build(config, cache.graph); !::lsfg::succeeded(code))
        fail("no chain for this configuration (" + name(code) + ")");
    ::lsfg::cache::describe(cache.header, cache.graph);
    {
        std::vector<::lsfg::cache::PassEntry> passes;
        std::vector<::lsfg::cache::SlotEntry> slots;
        for (const auto &pass : cache.passes) {
            auto entry = pass.entry;
            entry.slot_first = uint32_t(slots.size());
            passes.push_back(entry);
            slots.insert(slots.end(), pass.slots.begin(), pass.slots.end());
        }
        if (const auto code = ::lsfg::cache::validate(cache.header, passes, slots, cache.graph); !::lsfg::succeeded(code))
            fail("the prepared shaders do not fit this configuration (" + name(code) + ")");
    }
    if (const auto code = backend::schedule(cache.graph, schedule); !::lsfg::succeeded(code))
        fail("the chain cannot be scheduled (" + name(code) + ")");
    if (schedule.cycle == 0) fail("an empty schedule");

    // LoadModules: every pass's code in one block
    {
        std::vector<uint32_t> offsets;
        backend::Arena arena;
        for (const auto &pass : cache.passes) {
            ::lsfg::dksh::FileHeader header;
            std::memcpy(&header, pass.dksh.data(), sizeof(header));
            offsets.push_back(arena.place(header.code_size, DK_SHADER_CODE_ALIGNMENT));
        }
        (void)arena.place(DK_SHADER_CODE_UNUSABLE_SIZE, 1);
        if (arena.overflowed()) fail("its shaders do not fit in one code block");
        code = std::make_shared<Buffer>(device, arena.block_size(), budget,
            uint32_t(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code));
        auto *bytes = static_cast<uint8_t *>(code->cpu());
        shaders.assign(cache.passes.size(), DkShader{});
        for (size_t i = 0; i < cache.passes.size(); ++i) {
            const auto &pass = cache.passes[i];
            ::lsfg::dksh::FileHeader header;
            std::memcpy(&header, pass.dksh.data(), sizeof(header));
            std::memcpy(bytes + offsets[i], pass.dksh.data() + header.control_size, header.code_size);
            DkShaderMaker maker;
            dkShaderMakerDefaults(&maker, code->handle(), code->offset + offsets[i]);
            maker.control = pass.dksh.data();
            dkShaderInitialize(&shaders[i], &maker);
            if (!dkShaderIsValid(&shaders[i])) fail("one of its shaders would not load");
        }
    }

    // Allocate
    const backend::Request request{
        .config = config,
        .precision = ::lsfg::cache::Precision::high,
        .output = {width, height},
        .memory_budget_bytes = MemoryBudget,
    };
    backend::Rejection why;
    if (!backend::accept(cache, request, plan, why))
        fail("the chain cannot run at " + std::to_string(width) + "x" + std::to_string(height) + " (" + std::string(why.reason) + ")");
    if (const auto code = backend::describe(cache.graph, layout); !::lsfg::succeeded(code))
        fail("its descriptors cannot be laid out (" + name(code) + ")");
    for (uint32_t d = 0; d < cache.graph.dispatches.size(); ++d)
        for (uint32_t phase = 0; phase < cache.graph.dispatches[d].variant_count; ++phase) {
            backend::DispatchBinding binding;
            if (const auto code = backend::bind(cache, plan, layout, d, phase, binding); !::lsfg::succeeded(code))
                fail("dispatch " + std::to_string(d) + " cannot be bound (" + name(code) + ")");
        }

    // AllocateImages: the history and generated frames are this renderer's images, the rest one zero-filled block.
    const size_t count = plan.images.size();
    std::vector<DkImageLayout> layouts(count);
    std::vector<uint32_t> offsets(count, 0);
    std::vector<bool> owned(count, false);
    images.assign(count, DkImage{});
    chain.assign(count, nullptr);
    backend::Arena arena;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &image = plan.images[i];
        if (image.role == graph::ImageRole::history || image.role == graph::ImageRole::generated) {
            Img made;
            made.w = image.extent.width; made.h = image.extent.height;
            made.image = std::make_shared<Image>(device, made.w, made.h, DkImageFormat_RGBA8_Unorm,
                DkImageFlags_UsageRender | DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine, budget, 1,
                std::shared_ptr<BufferPool>{}, DkMsMode_1x, true);
            chain[i] = &made.image->image;
            // DKFrameGenerator::GetChainImage: history at 0..1, generated frames right after.
            if (image.role == graph::ImageRole::history) {
                if (i >= graph::history_image_count) fail("a history image out of place");
                history[i] = made;
            } else {
                if (i != graph::history_image_count + generated.size()) fail("a generated image out of place");
                generated.push_back(made);
            }
            continue;
        }
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, device);
        // deko3d lays out 6..8 rows with one-gob tiles but describes the image with two (Cemu-nx's note).
        maker.flags = DkImageFlags_UsageLoadStore | (image.extent.height <= 8 ? DkImageFlags_CustomTileSize : 0u);
        maker.tileSize = DkTileSize_OneGob;
        maker.format = formatOf(image.format);
        maker.dimensions[0] = image.extent.width;
        maker.dimensions[1] = image.extent.height;
        dkImageLayoutInitialize(&layouts[i], &maker);
        offsets[i] = arena.place(dkImageLayoutGetSize(&layouts[i]), dkImageLayoutGetAlignment(&layouts[i]));
        owned[i] = true;
    }
    if (!history[0].image || !history[1].image || generated.size() != config.generated_frames)
        fail("unexpected history / output images");
    if (arena.overflowed()) fail("its images do not fit in one memory block");
    imageMemory = std::make_shared<Buffer>(device, arena.block_size(), budget,
        uint32_t(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image | DkMemBlockFlags_ZeroFillInit));
    for (uint32_t i = 0; i < count; ++i)
        if (owned[i]) {
            dkImageInitialize(&images[i], &layouts[i], imageMemory->handle(), imageMemory->offset + offsets[i]);
            chain[i] = &images[i];
        }

    // WriteDescriptors (+ one sampled descriptor per generated frame after the graph's)
    imageDescriptors = layout.image_descriptors + uint32_t(generated.size());
    imageSet = std::make_shared<Buffer>(device, size_t(imageDescriptors) * DescriptorSize, budget);
    samplerSet = std::make_shared<Buffer>(device, size_t(backend::sampler_descriptor_count) * DescriptorSize, budget);
    auto *table = static_cast<uint8_t *>(imageSet->cpu());
    for (uint32_t i = 0; i < layout.images.size(); ++i) {
        const auto &entry = layout.images[i];
        if (!chain[i]) continue;
        DkImageView view;
        dkImageViewDefaults(&view, chain[i]);
        if (entry.sampled != backend::no_descriptor)
            dkImageDescriptorInitialize(reinterpret_cast<DkImageDescriptor *>(table + entry.sampled * DescriptorSize), &view, false, false);
        if (entry.storage != backend::no_descriptor)
            dkImageDescriptorInitialize(reinterpret_cast<DkImageDescriptor *>(table + entry.storage * DescriptorSize), &view, true, false);
    }
    for (size_t g = 0; g < generated.size(); ++g) {
        generated[g].tex = layout.image_descriptors + uint32_t(g);
        const DkImageView view = generated[g].image->view();
        dkImageDescriptorInitialize(reinterpret_cast<DkImageDescriptor *>(table + generated[g].tex * DescriptorSize), &view, false, false);
    }
    auto *samplers = static_cast<uint8_t *>(samplerSet->cpu());
    for (uint32_t i = 0; i < backend::sampler_descriptor_count; ++i) {
        DkSampler sampler;
        describeSampler(i, &sampler);
        dkSamplerDescriptorInitialize(reinterpret_cast<DkSamplerDescriptor *>(samplers + i * DescriptorSize), &sampler);
    }

    // WriteUniformBuffers
    if (plan.uniform_buffers) {
        uniforms = std::make_shared<Buffer>(device, size_t(plan.uniform_buffers) * backend::uniform_buffer_stride, budget);
        auto *u = static_cast<uint8_t *>(uniforms->cpu());
        for (uint32_t i = 0; i < plan.uniform_buffers; ++i) {
            const graph::ConstantBuffer contents = graph::constant_buffer(i, config);
            std::memcpy(u + i * backend::uniform_buffer_stride, &contents, sizeof(contents));
        }
    }
    hazards.assign(cache.graph.images.size(), 0);
    hazardImages.reserve(cache.graph.images.size());
    stamps = std::make_shared<Buffer>(device, 4 * 8 * sizeof(GpuReport), budget);
    std::memset(stamps->cpu(), 0, 4 * 8 * sizeof(GpuReport));
    std::fprintf(stderr, "[lsfg173m] ready %ux%u %s flow 1/%u: %zu passes, %zu dispatches, cycle %u, warmup %u, %zu images (%.1f MiB own)\n",
        width, height, performance ? "performance" : "quality", config.flow_denominator, cache.passes.size(),
        cache.graph.dispatches.size(), schedule.cycle, schedule.warmup_frames, count, double(arena.used()) / 1048576.0);
}

DkGpuAddr FrameGen::imageSetGpu() const { return imageSet->gpu(); }
DkGpuAddr FrameGen::samplerSetGpu() const { return samplerSet->gpu(); }

void FrameGen::barrier(DkCmdBuf cmd, uint32_t invalidate)
{
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, invalidate);
    for (const uint32_t image : hazardImages) hazards[image] = 0;
    hazardImages.clear();
}

void FrameGen::mark(uint32_t image, uint8_t hazard)
{
    if (hazards[image] == 0) hazardImages.push_back(image);
    hazards[image] |= hazard;
}

void FrameGen::record(DkCmdBuf cmd, uint32_t dispatch)
{
    backend::DispatchBinding binding;
    if (const auto code = backend::bind(cache, plan, layout, dispatch, phase, binding); !::lsfg::succeeded(code))
        fail("dispatch " + std::to_string(dispatch) + " cannot be bound (" + name(code) + ")");
    bool wait = false;
    uint32_t invalidate = 0;
    for (uint32_t i = 0; i < binding.texture_count; ++i)
        if (hazards[binding.textures[i].image] & HazardWritten) {wait = true; invalidate |= DkInvalidateFlags_Image;}
    for (uint32_t i = 0; i < binding.storage_count; ++i)
        if (hazards[binding.storages[i].image] != 0) wait = true;
    if (wait) barrier(cmd, invalidate);
    for (uint32_t i = 0; i < binding.texture_count; ++i) mark(binding.textures[i].image, HazardRead);
    for (uint32_t i = 0; i < binding.storage_count; ++i) mark(binding.storages[i].image, HazardWritten);
    const DkShader *shader = &shaders[binding.pass];
    dkCmdBufBindShaders(cmd, DkStageFlag_Compute, &shader, 1);
    std::array<DkResHandle, backend::max_texture_slots> textures{};
    uint32_t textureSlots = 0;
    for (uint32_t i = 0; i < binding.texture_count; ++i) {
        const auto &t = binding.textures[i];
        textures[t.slot] = dkMakeTextureHandle(t.descriptor, t.sampler);
        textureSlots = std::max(textureSlots, t.slot + 1);
    }
    if (textureSlots) dkCmdBufBindTextures(cmd, DkStage_Compute, 0, textures.data(), textureSlots);
    std::array<DkResHandle, backend::max_storage_slots> storages{};
    uint32_t storageSlots = 0;
    for (uint32_t i = 0; i < binding.storage_count; ++i) {
        const auto &st = binding.storages[i];
        storages[st.slot] = dkMakeImageHandle(st.descriptor);
        storageSlots = std::max(storageSlots, st.slot + 1);
    }
    if (storageSlots) dkCmdBufBindImages(cmd, DkStage_Compute, 0, storages.data(), storageSlots);
    if (binding.uniform_slot != backend::no_slot)
        dkCmdBufBindUniformBuffer(cmd, DkStage_Compute, binding.uniform_slot,
            uniforms->gpu() + binding.uniform_buffer * backend::uniform_buffer_stride, backend::uniform_buffer_stride);
    dkCmdBufDispatchCompute(cmd, binding.groups_x, binding.groups_y, 1);
}

// DKFrameGenerator::Generate after its blit (the real frame is already drawn into input()).
bool FrameGen::recordInput(DkCmdBuf cmd, uint64_t)
{
    dkCmdBufBindImageDescriptorSet(cmd, imageSet->gpu(), imageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(cmd, samplerSet->gpu(), backend::sampler_descriptor_count);
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    for (const uint32_t image : hazardImages) hazards[image] = 0;
    hazardImages.clear();
    for (uint32_t d = 0; d < cache.graph.dispatches.size(); ++d) record(cmd, d);
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    phase = (phase + 1) % schedule.cycle;
    ++framesSinceReset;
    return ready();
}


void FrameGen::stamp(DkCmdBuf cmd, uint64_t frame, int index)
{
    const size_t at = size_t(frame % 4) * 8 + size_t(index);
    static_cast<GpuReport *>(stamps->cpu())[at] = GpuReport{};
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
    dkCmdBufReportCounter(cmd, DkCounter_Timestamp, stamps->gpu() + at * sizeof(GpuReport));
}

bool FrameGen::stageNs(uint64_t frame, int from, int to, uint64_t &ns) const
{
    const auto *r = static_cast<const GpuReport *>(stamps->cpu()) + size_t(frame % 4) * 8;
    const auto v = gpuIntervalNs(r[from], r[to]);
    if (!v) return false;
    ns = *v;
    return true;
}

}
