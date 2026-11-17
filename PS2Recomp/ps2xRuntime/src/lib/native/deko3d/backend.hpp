#pragma once
#include "../native_backend.h"
#include "resources.hpp"
#include "policy.hpp"
#include "arena.hpp"
#include "frame_rate.hpp"
#include "pipeline.hpp"
#include "gpu_timing.hpp"
#include "gpu_segments.hpp"
#include "descriptor_slots.hpp"
#include <chrono>
#include <array>
#include <unordered_map>
#include <vector>

namespace native_renderer::detail::deko {
constexpr uint32_t Width = 1280, Height = 720, Frames = 3;
struct Mesh {
    std::shared_ptr<Buffer> vertices, indices;
    uint32_t count = 0, indexCount = 0;
    uint32_t vertexOffset = 0, indexOffset = 0;
    uint8_t minAlpha = 0;
    bool transient = false;
    MeshBounds bounds;
};
// One shared image descriptor pool: textures take a slot and draws select them through
// the texture handle, so the GPU texture header pool base is bound once per frame.
// Textures that find the pool exhausted keep a private one-entry descriptor set.
constexpr uint32_t ImageDescriptorSlots = 4096;
struct ImageDescriptorPool {
    std::shared_ptr<Buffer> storage;
    DescriptorSlots slots{ImageDescriptorSlots};
    DkImageDescriptor *entries() const { return static_cast<DkImageDescriptor *>(storage->cpu()); }
};
struct Texture {
    std::shared_ptr<Image> image;
    std::shared_ptr<Buffer> descriptor;      // private set, used only when slot == None
    std::shared_ptr<ImageDescriptorPool> pool;
    uint32_t slot = DescriptorSlots::None;
    uint8_t minAlpha = 0;
    uint16_t width = 1, height = 1, levels = 1;
    ~Texture() { if (pool) pool->slots.release(slot); }
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
    // GPU time by draw category (gpu_segments.hpp), on counter-sampled frames only.
    std::shared_ptr<Buffer> segmentReports;
    std::array<uint8_t,MaxGpuSegments> segmentKinds{};
    unsigned segmentCount=0;
    bool segmentsPending=false;
};
struct alignas(16) Material {
    float clip[16], directions[16], colours[16];
    float params[4], fog[4], fogColour[4], lod[4], alpha[4], debug[4], bgFog[4];
};
static_assert(sizeof(Material) == 304);

class DekoBackend final : public Backend {
    // Counts actual aligned DkMemBlock bytes, including resources retained by
    // in-flight frames and upload staging. Driver contexts and CPU metadata are separate.
    std::shared_ptr<AllocationBudget> allocationBudget =
        std::make_shared<AllocationBudget>(512u * 1024 * 1024);
    Unique<DkDevice, dkDeviceDestroy> device;
    std::shared_ptr<BufferPool> bufferPool,imagePool; // imagePool: v108, texture storage pages
    Unique<DkQueue, dkQueueDestroy> queue;
    std::array<std::shared_ptr<Image>, Frames> colors;
    std::shared_ptr<Image> depth;
    Unique<DkSwapchain, dkSwapchainDestroy> swapchain;
    std::unique_ptr<Shader> vertexShader, fragmentShader;
    std::unique_ptr<Shader> opaqueFragmentShader;
    // Generic shader with forced early depth tests; used only for draws that write neither
    // depth nor stencil, where testing before the shader cannot change the result.
    std::unique_ptr<Shader> earlyFragmentShader;
    uint32_t lastEarlyDraws=0,lastLeanDraws=0;
    const DkShader *quadShaderOverride=nullptr; // v115 calibration: bloomQuad with a scene fragment shader
    // v107: this frame's resource creation on the render thread (slow-frame breakdown in present-times).
    uint64_t frameTexNs=0,frameMeshNs=0;uint32_t frameTexCount=0,frameMeshCount=0;
    std::unique_ptr<Shader> leanFragmentShaders[256];bool leanEnabled=true; // CELOSIA_DIAG_NO_LEAN=1 reverts
    uint32_t leanVariants=0xffffffffu; // CELOSIA_DIAG_LEAN_MASK (hex) limits the variants used
    // Depth-only shader for the planned pre-pass (prepass_policy.hpp); boundFragment 3.
    std::unique_ptr<Shader> depthFragmentShader;
    std::vector<uint8_t> prepassFlags;
    uint32_t lastPrepassDraws=0,lastPrepassBackdrops=0;
    GpuSegmentRecorder segments;
    GpuSegmentWindow segmentWindow;
    DkGpuAddr segmentBase=0;
    void markSegment(GpuSegment kind){
        const int slot=segments.change(kind);
        if(slot>=0)dkCmdBufReportCounter(commands,DkCounter_Timestamp,segmentBase+DkGpuAddr(slot)*16);
    }
    bool prepassEnabled=true;
    // Off: measured on hardware (v55) it removed only 0.6% of fragment invocations, so the
    // failure-pass cost is genuinely failing (translucent) pixels. CELOSIA_DIAG_FAILURE_STRICT=1 enables it.
    bool strictFailureDepth=false;
    uint32_t lastStrictFailureDraws=0;
    // Additive world textures seen so far (logged once each: size, mips and LOD of the light effects).
    std::vector<uint64_t> loggedAdditiveTextures;
    uint64_t frameDrawNs=0; // v123: the render thread's time in draw() this frame
    uint32_t mergedAlphaDraws=0,lastMergedAlphaDraws=0,lastMergedBgAlpha=0;bool mergeAlphaNoop=true,mergeBgAlpha=true; // CELOSIA_DIAG_NO_ALPHA_MERGE=1 reverts
    std::unique_ptr<Shader> shadowVertexShader,shadowFragmentShader;
    std::shared_ptr<Buffer> uniforms, samplers, shadowTriangle;
    std::array<Frame, Frames> frames;
    Unique<DkCmdBuf, dkCmdBufDestroy> commands;
    std::unordered_map<uint64_t, std::shared_ptr<Mesh>> meshes;
    std::unordered_map<uint64_t, std::shared_ptr<Texture>> textures;
    std::shared_ptr<Texture> dummyTexture;
    std::shared_ptr<ImageDescriptorPool> imageDescriptors;
    uint32_t privateDescriptorTextures=0;
    uint32_t lastDescriptorSetBinds=0,lastTextureHandleBinds=0;
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
    // Background (sky, layer 1) and screen draws by Blend (Normal, Additive, Opaque, Subtract).
    uint32_t lastBackgroundBlend[4]{},lastScreenBlend[4]{};
    // CELOSIA_DIAG_SKY (development diagnosis only, default 0 = unchanged rendering):
    // 1 skip background Subtract, 2 skip background Additive, 3 skip background Normal/Opaque,
    // 4 skip all background, 5 background Subtract drawn as Additive.
    int diagSky=0,diagSkyRequested=0;
    // Clear to the fog colour on frames with background layers (as GL); CELOSIA_DIAG_SKY_CLEAR_BLACK=1 reverts.
    bool backgroundClearFog=true,lastClearToFog=false;
    // CELOSIA_DIAG_SKY_AFTER: seconds after renderer start before the mode applies (menus stay intact).
    std::chrono::steady_clock::time_point diagSkyFrom{};
    // CELOSIA_DIAG_GPU_CYCLE=K (development diagnosis only, default 0 = unchanged rendering): each
    // hardware-status window renders with mode (window index % K), reported as diag_gpu_mode:
    // 0 unchanged, 1 failure pass skipped, 2 failure pass without colour writes, 3 nearest mip filter,
    // 4 failure pass with the generic shader (no shader switch; same image), 5 generic shader for
    // every colour draw (same image). The value is a digit list, e.g. "041" cycles modes 0,4,1.
    std::vector<int> diagGpuCycle;
    int diagGpuMode=0;std::size_t diagGpuIndex=0;
    // The background image, as the GL renderer's skyImage / zFog path and the PS2 draw it: layer 1
    // (the sky, the far scenery) is drawn at the PS2's 512 x 448 into bgColour (depth tested into
    // bgDepth), reduced to 256 x 256 into bgSmall (bgEnd), and laid behind the scene; fogged draws,
    // and the world drawn before the fog's layer (zFog, by its slices), fade to it by view depth.
    // Used on frames with the fog on and layer-1 draws. CELOSIA_DIAG_SKY_DIRECT=1: layer 1 drawn
    // directly into the picture with flat-colour fog (v59..v68 behaviour).
    std::shared_ptr<Image> bgColour,bgDepth,bgSmall;
    // SotC's bloom (the GS chain, as GL/Metal draw it; bloom.frag.glsl). At the bloom's layer the
    // scene at or beyond the threshold depth is marked in bloomMask (the main depth tested), reduced
    // to 256 / 128 / 64, mixed with the previous frame's blur (temporal), blurred to 64 (the next
    // history) and 128, stretched to 256 and added to the picture in the bloom colour.
    // CELOSIA_DIAG_NO_BLOOM=1 turns it off.
    Bloom bloom{};
    bool bloomAllowed=true,bloomHistory=false;
    std::shared_ptr<Image> bloomMask,bloomChain[7];
    uint32_t bloomMaskSlot=DescriptorSlots::None,bloomChainSlot[7]{};
    std::unique_ptr<Shader> bloomFragmentShader;
    uint32_t lastBloomRuns=0;
    void runBloom(int slot);
    // blend: 0 none, 1 added (one, one), 2 darkened (zero, 1 - source alpha). stencil: only where the
    // shadow count is not zero, clearing it.
    void bloomQuad(int mode,uint32_t source,uint32_t second,const float params[4],const float colour[4],int blend,float maskDepth,bool stencil=false);
    // The soft shadows (the GL renderer's default, as the PS2 shows them): the stencil count as a
    // mask (bloomMask), reduced 4 x 4, blurred, and the picture darkened by it.
    // CELOSIA_DIAG_SHADOW_HARD=1: every counted pixel darkened alike (up to v87).
    bool softShadows=true;
    std::shared_ptr<Image> shadowSmall[2];
    uint32_t shadowSmallSlot[2]{};
    uint32_t lastSoftShadows=0,lastZeroAddDraws=0;
    bool zeroAddDiscard=true;
    uint64_t lastHeavyFrame=0;
    uint32_t bgColourSlot=DescriptorSlots::None,bgSmallSlot=DescriptorSlots::None;
    std::unique_ptr<Shader> skyFragmentShader;
    bool skyImageEnabled=true,skyActive=false,inBackgroundPass=false;
    uint32_t lastSkyImage=0,lastBgFogged=0,lastBgNear=0,lastBgTarget=0;
    // CELOSIA_DIAG_HEAVY_MS=N (development diagnosis only): the first 3 frames whose GPU time
    // exceeds N ms are captured (native-frame-*.ppm) with a per-draw list (native-frame-*.csv).
    uint64_t heavyCaptureNs=0;unsigned heavyCaptures=0,heavyModeCaptured=0;
    // Capture frames only: a GPU timestamp after each draw's work; owners[k] names what ran between
    // reports k-1 and k (draw index * 4 + 0 colour pass, + 1 failure pass, + 2 background image;
    // negative: -1 start, -2 sky image copies, -3 depth pre-pass, -4 shadow resolve).
    std::shared_ptr<Buffer> drawTimeReports;std::vector<int> drawTimeOwners;bool drawTiming=false;
    static constexpr std::size_t MaxDrawTimes=16384;
    void markDrawTime(int owner){
        if(!drawTiming || drawTimeOwners.size()>=MaxDrawTimes)return;
        dkCmdBufReportCounter(commands,DkCounter_Timestamp,drawTimeReports->gpu()+DkGpuAddr(drawTimeOwners.size())*16);
        drawTimeOwners.push_back(owner);
    }
    void bindTarget(const DkImage &colour,const DkImage *depthImage,uint32_t width,uint32_t height);
    void drawImageQuad(uint32_t slot,float invWidth,float invHeight);
    // Failure-pass draws on the last frame: by fail action (GS AFAIL 0..3), by Blend, and by
    // alpha reference (<64, 64..127, 128, >128).
    uint32_t lastFailureActions[4]{},lastFailureBlend[4]{},lastFailureRefs[4]{};
    // Sampled-frame screen coverage upper bounds (screen multiples) by pass kind.
    struct CoverageSample{uint32_t frame=0;float opaque=0,generic=0,failure=0,backdrop=0,shadow=0;uint32_t fullScreenDraws=0;} lastCoverage;
    int boundDepth=-1,boundMask=-1,boundBlend=-1;
    uint32_t boundHandle=0; // 0 is never a bound handle: slot 0 with clamp 0 is dummy/none state reset
    // Bound fragment shader: 0 generic, 1 opaque (no discard), 2 generic with early fragment tests.
    int boundFragment=0;
    // v124: the native vertex shader is bound (every bind site but the shadow volumes, which restore it)
    bool nativeVertexBound=false;
    uint64_t frameSortNs=0; // v124: the draw sort this frame
    std::vector<uint64_t> sortKeys; // v125: (order << 32 | index) of each draw
    std::vector<Draw> sortedDraws;  // v125: the draws in order (storage kept between frames)
    bool boundHandleValid=false;
    DkGpuAddr boundImages=0;
    void bindTexture(const Texture &, uint8_t clamp);
    uint32_t submittedFrames = 0, diagnosticCaptureFrame = 0, diagnosticCaptureInterval = 0;
    std::shared_ptr<Buffer> latestCapture;
    uint32_t captureFrameIndex = 0;
    Frame *activeFrame = nullptr;
    std::shared_ptr<Buffer> vertexStorage(size_t, uint32_t &, bool);
    static void addCommandMemory(void *, DkCmdBuf, size_t);
    void startCommands(Frame &);
    void emitUploads(const std::vector<Upload> &);
    void emitDraw(const Draw &, bool failurePass = false, bool farBackdrop = false);
    void emitDepthOnly(const Draw &);
    void submitGeometry(const Mesh &);
    void emitShadowVolume(const Draw &);
    void resolveShadows(float amount,int slot);
    void restoreShadowState();
    Material materialFor(const Draw &, bool) const;
    void configureTargets(int slot, bool clearToFog);
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
