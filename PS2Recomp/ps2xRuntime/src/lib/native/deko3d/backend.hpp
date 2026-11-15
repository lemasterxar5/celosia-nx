#pragma once
#include "../native_backend.h"
#include "resources.hpp"
#include "policy.hpp"
#include "arena.hpp"
#include "frame_rate.hpp"
#include "pipeline.hpp"
#include "gpu_timing.hpp"
#include <array>
#include <unordered_map>

namespace native_renderer::detail::deko {
constexpr uint32_t Width = 1280, Height = 720, Frames = 3;
struct Mesh {
    std::shared_ptr<Buffer> vertices, indices;
    uint32_t count = 0, indexCount = 0;
    uint32_t vertexOffset = 0, indexOffset = 0;
    uint8_t minAlpha = 0;
    bool transient = false;
};
struct Texture {
    std::shared_ptr<Image> image;
    std::shared_ptr<Buffer> descriptor;
    uint8_t minAlpha = 0;
};
struct Upload {
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Buffer> staging;
    uint32_t width, height, levels;
};
struct Draw {
    uint64_t meshId = 0;
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<Texture> texture;
    std::array<float, 16> clip;
    DrawState state;
};
struct Frame {
    ArenaCursor vertexCursor;
    std::vector<std::shared_ptr<Buffer>> vertexPages;
    DkFence fence{};
    bool pending = false;
    std::vector<std::shared_ptr<Buffer>> commands;
    std::vector<Upload> uploads;
    std::vector<Draw> draws;
    std::shared_ptr<Buffer> capture;
    std::shared_ptr<Buffer> gpuReports;
    bool gpuTimingPending=false;
    uint32_t gpuTimingFrame=0;
    bool gpuCountersPending=false;
    uint32_t gpuCounterFrame=0;
};
struct alignas(16) Material {
    float clip[16], directions[16], colours[16];
    float params[4], fog[4], fogColour[4], lod[4], alpha[4], debug[4];
};
static_assert(sizeof(Material) == 288);

class DekoBackend final : public Backend {
    // Counts actual aligned DkMemBlock bytes, including resources retained by
    // in-flight frames and upload staging. Driver contexts and CPU metadata are separate.
    std::shared_ptr<AllocationBudget> allocationBudget =
        std::make_shared<AllocationBudget>(512u * 1024 * 1024);
    Unique<DkDevice, dkDeviceDestroy> device;
    std::shared_ptr<BufferPool> bufferPool;
    Unique<DkQueue, dkQueueDestroy> queue;
    std::array<std::shared_ptr<Image>, Frames> colors;
    std::shared_ptr<Image> depth;
    Unique<DkSwapchain, dkSwapchainDestroy> swapchain;
    std::unique_ptr<Shader> vertexShader, fragmentShader;
    std::unique_ptr<Shader> opaqueFragmentShader;
    std::unique_ptr<Shader> shadowVertexShader,shadowFragmentShader;
    std::shared_ptr<Buffer> uniforms, samplers, shadowTriangle;
    std::array<Frame, Frames> frames;
    Unique<DkCmdBuf, dkCmdBufDestroy> commands;
    std::unordered_map<uint64_t, std::shared_ptr<Mesh>> meshes;
    std::unordered_map<uint64_t, std::shared_ptr<Texture>> textures;
    std::shared_ptr<Texture> dummyTexture;
    std::vector<Upload> uploads;
    std::vector<Draw> draws;
    std::vector<Vertex> scratchVertices;
    std::vector<uint32_t> scratchIndices;
    uint32_t scratchVertexCount = 0, scratchIndexCount = 0;
    Fog fog{};
    uint32_t frameIndex = 0;
    int latestSlot = -1;
    bool recording = false, held = false;
    bool captureRequested = false;
    FrameRateWindow presentationRate;
    uint64_t lastBeginWaitNs = 0;
    uint64_t lastGpuNs=0;
    uint32_t lastGpuTimingFrame=0;
    GpuTimeWindow gpuTimeWindow;
    GpuCounterWindow gpuCounterWindow;
    uint64_t presentedNativeFrames=0;
    uint32_t lastGpuCounterFrame=0;
    uint32_t lastOpaqueDraws=0,lastGenericDraws=0,lastShadowDraws=0;
    std::array<uint32_t,8> lastGenericAlphaModes{};
    uint32_t lastGenericBackdrop=0,lastGenericFresnel=0,lastGenericDisabledAlpha=0,lastGenericOther=0;
    int boundDepth=-1,boundMask=-1,boundBlend=-1,boundClamp=-1;
    bool boundOpaque=false;
    DkGpuAddr boundImages=0;
    uint32_t submittedFrames = 0, diagnosticCaptureFrame = 0, diagnosticCaptureInterval = 0;
    std::shared_ptr<Buffer> latestCapture;
    uint32_t captureFrameIndex = 0;
    Frame *activeFrame = nullptr;
    std::shared_ptr<Buffer> vertexStorage(size_t, uint32_t &, bool);
    static void addCommandMemory(void *, DkCmdBuf, size_t);
    void startCommands(Frame &);
    void emitUploads(const std::vector<Upload> &);
    void emitDraw(const Draw &, bool failurePass = false);
    void emitShadowVolume(const Draw &);
    void resolveShadows(float amount);
    void restoreShadowState();
    Material materialFor(const Draw &, bool) const;
    void configureTargets(int slot);
public:
    DekoBackend();
    ~DekoBackend() override;
    bool hasMesh(uint64_t id) override { return meshes.count(id) != 0; }
    void uploadMesh(uint64_t, const Vertex *, uint32_t, bool) override;
    Vertex *frameMesh(uint32_t) override;
    uint32_t *frameMeshIndices(uint32_t, uint32_t &) override;
    void commitFrameMesh(uint64_t, uint32_t, uint32_t) override;
    void cancelFrameMesh() override { scratchVertexCount = scratchIndexCount = 0; }
    bool canSkin() override { return false; }
    void uploadSkinnedMesh(uint64_t, const SkinVertex *, uint32_t,
                           const uint32_t *, uint32_t) override;
    uint32_t framePalette(const float *, uint32_t) override;
    void rewriteFramePalette(uint32_t, const float *, uint32_t) override;
    bool hasTexture(uint64_t id) override { return textures.count(id) != 0; }
    void uploadTexture(uint64_t, uint32_t, uint32_t, const uint8_t *, uint32_t) override;
    void beginFrame() override;
    void draw(uint64_t, const float[16], const DrawState &) override;
    void setFog(const Fog &) override;
    void setBloom(const Bloom &b) override;
    void addScreenBlur(const ScreenBlur &b) override;
    void setGlowComposite(const GlowComposite &g) override;
    void setDepthOfField(const DepthOfField &d) override;
    void endFrame(uint32_t, uint32_t) override;
    void holdFrame() override { held = true; }
    bool latestFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &) override;
    void requestCapture() { captureRequested = true; }
    void finishFrames() override { dkQueueWaitIdle(queue); }
};
}
