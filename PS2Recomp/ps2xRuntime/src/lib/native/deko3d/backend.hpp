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
#include "lsfg.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <array>
#include <unordered_map>
#include <vector>

namespace native_renderer::detail::deko {
constexpr uint32_t Width = 1280, Height = 720, Frames = 3;
// v141: a clipped mesh's vertex: the colour as floats (the Uscaled attribute's values, 0..255), so a
// vertex cut on an edge carries the interpolated colour itself.
struct ClipVertex { float x, y, z, nx, ny, nz, u, v, r, g, b, a; };
// v166: the GPU copy of a Vertex with its normal packed as 10:10:10:2 snorm (28 bytes, not 36): a step of
// 1/511 per component, well under the lighting's 8-bit output. Only for meshes whose normal components all
// lie in [-1, 1] (the shader uses the normal unnormalised, so larger ones keep the float layout).
struct PackedVertex { float x, y, z; uint32_t n; float u, v; uint8_t r, g, b, a; };
static_assert(sizeof(PackedVertex) == 28);
inline uint32_t packSnorm10(float c){
    const float k=c>1.0f?1.0f:(c<-1.0f?-1.0f:c);
    const int32_t q=int32_t(k*511.0f+(k>=0.0f?0.5f:-0.5f));
    return uint32_t(q)&0x3FFu;
}
inline PackedVertex packVertex(const Vertex &v){
    return PackedVertex{v.x,v.y,v.z,packSnorm10(v.nx)|(packSnorm10(v.ny)<<10)|(packSnorm10(v.nz)<<20),v.u,v.v,v.r,v.g,v.b,v.a};
}
struct Mesh {
    std::shared_ptr<Buffer> vertices, indices;
    uint32_t count = 0, indexCount = 0;
    uint32_t vertexOffset = 0, indexOffset = 0;
    uint8_t minAlpha = 0;
    // v126: the largest vertex alpha, and per triangle (triangle lists only) the largest of its three
    // vertex alphas with their least value (kept when that least value is under 128).
    uint8_t maxAlpha = 255, triangleFloor = 255;
    std::vector<uint8_t> triangleAlpha;
    std::vector<uint8_t> triangleMin; // v128: per triangle the least of its three vertex alphas
    // v167: per triangle its uv rectangle (u0, u1, v0, v1) for the alpha-test split by texture region
    // (triangle lists up to 16384 triangles).
    std::vector<float> triangleUV;
    // v128: a kept mesh's alpha-test split (the index lists of the two passes) for one set of bounds.
    struct AlphaSplit {
        float lo = -1, hi = -1, need = -1; bool failure = false;
        // v167: the texture region bounds it was cut with (texture, uv offset, clamp; null: none).
        const void *regionTexture = nullptr; float regionU = 0, regionV = 0; uint8_t regionClamp = 0;
        std::shared_ptr<Mesh> main, fail; bool mainEmpty = false, failEmpty = false;
        uint32_t mainDropped = 0, failDropped = 0;
    };
    std::shared_ptr<AlphaSplit> split;
    // v134/v139: a CPU copy of faint meshes (largest vertex alpha under 64, at most 8192 vertices) for
    // clipToTexture, shared by the sub-meshes cut from them; no indices: a triangle list.
    std::shared_ptr<const std::vector<Vertex>> cpuVertices; std::vector<uint32_t> cpuIndices;
    bool transient = false;
    bool floatColour = false; // v141: ClipVertex vertices
    bool packed = false; // v166: PackedVertex vertices
    uint32_t stride() const {return floatColour?uint32_t(sizeof(ClipVertex)):packed?uint32_t(sizeof(PackedVertex)):uint32_t(sizeof(Vertex));}
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
    uint8_t minAlpha = 0, maxAlpha = 255; // v126: maxAlpha over every uploaded mip
    uint16_t fullTexels = 0, clearTexels = 0; // v127 (diagnostics): permille of texels with alpha >= 128 / <= 2
    uint16_t width = 1, height = 1, levels = 1;
    bool replaced = false; // v167: a texture pack's (BC3, the hardware's mip choice)
    // v134: the alpha of small one-level textures that are mostly clear, and by least passing texel
    // alpha the half-planes (u, v) bounding where a filtered sample can reach it (0 unknown, 1 none).
    std::vector<uint8_t> alphaGrid; uint16_t gridW = 0, gridH = 0;
    std::vector<uint8_t> regionState; std::vector<std::vector<float>> regions;
    std::vector<std::vector<uint8_t>> maxLevels; // v135: 2 x 2 maxima of alphaGrid, level 1 up (wrapped)
    ~Texture() { if (pool) pool->slots.release(slot); }
};
struct Upload {
    std::shared_ptr<Texture> texture;
    std::shared_ptr<Buffer> staging;
    uint32_t width, height, levels;
    bool bc3 = false; // v167: BC3 blocks (16 bytes per 4 x 4), levels one after the other
};
struct Draw {
    uint64_t meshId = 0;
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<Texture> texture;
    std::array<float, 16> clip;
    DrawState state;
    bool faint = false; // v126: no fragment of it survives the shaders' a < 0.02 discard
    // v128: the alpha-test failure pass's own triangles (null: the draw's mesh), and passes left empty
    std::shared_ptr<Mesh> failureMesh;
    bool mainEmpty = false, failureEmpty = false;
    bool bgMainEmpty = false; // v170a: a background draw whose alpha test fails at every fragment (main half not drawn)
    std::shared_ptr<Mesh> wholeMesh; // v170a: a split background draw's own mesh (the v121 merged pass draws it all)
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
    // v150: the scene's render size (rw x rh, 16:9; the output stays Width x Height). Below the output
    // the scene is drawn into `scene` and scaled onto the swapchain image at the frame's end, the menu's
    // screen draws (order >= MenuOrder) after it at the output's size.
    static constexpr uint32_t MenuOrder=0x7FFF0000u;
    uint32_t rw=Width,rh=Height;std::shared_ptr<Image> scene;uint32_t sceneSlot=DescriptorSlots::None;
    uint32_t targetFlags=0;DkImageFormat maskFormatNow=DkImageFormat_R8_Unorm;
    std::vector<std::size_t> menuDraws;
    // v159: the frame's last screen draws that cover the whole picture (fog, fades) drawn on the resolved
    // (1x) scene in postQuad instead of on the 4x samples; CELOSIA_DIAG_NO_RESOLVED_TAIL=1 reverts.
    std::vector<std::size_t> resolvedDraws;const std::vector<Draw> *resolvedFrom=nullptr;bool resolvedTail=true;
    bool resolvedEligible(const Draw &draw) const;
    // v159: the bloom's composite kept for the resolved picture (postQuad), its scale and colour.
    bool deferComposite=false,compositePending=false;float compositeScale[4]{},compositeColour[4]{};
    // v151: the output (swapchain) size: 1280x720 in handheld, 1920x1080 docked (ReverseNX-RT respected).
    uint32_t ow=Width,oh=Height;int swapIntervalNow=1;bool dockedNow=false;
    uint32_t captureW=Width,captureH=Height;
    void makeSwapchain(uint32_t width,uint32_t height);
    void resizeOutput(uint32_t width,uint32_t height);
    // v152: MSAA (samples 1, 2, 4) and the post pass (FXAA, sharpening, brightness, colour). With either,
    // or a render size other than the output's, the scene has its own target (`scene`, multisampled when
    // msaa > 1, resolved into `sceneResolved`), drawn onto the swapchain image by postQuad.
    uint32_t msaa=1;bool postOn=false,separate=false;
    std::shared_ptr<Image> sceneResolved,bloomMaskMs;
    std::unique_ptr<Shader> postFragmentShader;
    // v164: post.frag specialised: FXAA on (no bicubic path), and no option on (one tap). CELOSIA_DIAG_NO_POST_VARIANTS=1 reverts.
    std::unique_ptr<Shader> postFxShader,postPlainShader;bool postVariants=true;
    // v160: the hard shadow resolve's trivial early-test fragment; the post as a compute pass into postOut
    // (copied onto the target) when the scene is the output's size. CELOSIA_DIAG_NO_SOLID_RESOLVE=1 /
    // CELOSIA_DIAG_NO_COMPUTE_POST=1 revert each.
    std::unique_ptr<Shader> solidFragmentShader,postComputeShader;
    // v162: the mask passes' (bloomQuad mode 1) early-test fragment; CELOSIA_DIAG_NO_EARLY_MASK=1 reverts.
    std::unique_ptr<Shader> oneFragmentShader;bool earlyMask=true;
    // v162: the soft-shadow modes (7, 19, 9) on their own shader; CELOSIA_DIAG_NO_SHADOW_SHADER=1 reverts.
    std::unique_ptr<Shader> shadowSoftShader;bool shadowSoft=true;
    // v163: the bloom chain (15, 17, 16) and composite (18) on their own shaders; CELOSIA_DIAG_NO_BLOOM_SHADERS=1 reverts.
    std::unique_ptr<Shader> bloomChainShader,bloomCompShader;bool bloomSplit=true;
    std::shared_ptr<Image> postOut;uint32_t postOutSlot=DescriptorSlots::None,postOutTexSlot=DescriptorSlots::None;bool blitCopies=true;std::shared_ptr<Buffer> postConstants;
    bool solidResolve=true,computePost=false; // v162: compute post off (v160/v161: +4.5 ms)
    void bindSamples(bool multi);
    bool samplesBound=false;int boundSamplesMulti=0;
    void resolveMask();
    void postQuad(const DkImage &target,uint32_t directSlot=DescriptorSlots::None);
    uint32_t lastBgNeverPass=0; // v170a (CELOSIA_DIAG_NO_BGNEVER170=1: off)
    uint32_t lastBgAlwaysPass=0,lastClipCopies=0; // v168: background draws whose alpha test always passes, drawn without it
    uint32_t fgInSlot[2]{DescriptorSlots::None,DescriptorSlots::None}; // v168: the LSFG inputs as storage images
    // v161: a same-size image drawn onto the bound target (one bilinear tap at each texel centre: the texels
    // as they are), in place of dkCmdBufCopyImage onto the swapchain image (v160: ~4.7 ms a copy).
    // imageSet / samplerSet 0: the ones bound stay.
    void blitTexture(DkResHandle handle,DkGpuAddr imageSet,uint32_t imageSlots,DkGpuAddr samplerSet,uint32_t samplerSlots);
    // v153: frame generation (lsfg.hpp). fgOn: this frame goes through it (the scene into its input, the
    // generated frames then the real one presented, the menu drawn on each).
    std::unique_ptr<lsfg::FrameGen> frameGen;
    bool fgOn=false;uint64_t fgFrame=0,fgLastExitNs=0,fgLastRealNs=0;uint32_t fgCount=1;
    // v164: frame generation's images presented on their own queue and thread. The render queue draws the
    // generated and the real picture (menu included) into a pair of display images and goes on with the next
    // frame; presentWorker waits for them on its queue, acquires the swapchain images (the waits for the
    // display: before, on the render queue and thread, v162e "copy+menu" 10-14 ms a real frame), copies and
    // presents them with the same intervals. CELOSIA_DIAG_SYNC_PRESENT=1: as before (presentCopy).
    // v167: real = the frame generation's input image shown as it is (no menu over it): no copy into dispReal.
    struct PresentJob{uint32_t k;int intervalGen,intervalReal;bool gen;std::shared_ptr<Image> real;std::shared_ptr<Image> genImage;
        uint64_t cpuStart=0,waitA=0,waitB=0,subA=0,subB=0,push=0,gate=0;}; // v173p CELOSIA_DIAG_PACE173: the render side's times (ns)
    // v167 (CELOSIA_DIAG_NO_LSFG_DIRECT=1: off): the present slot whose job shows each input image (-1 none):
    // waited (job copied, its fence) before the post pass writes that image again.
    bool lsfgDirectReal=true;int inputJobSlot[2]{-1,-1};
    // v170 (CELOSIA_DIAG_NO_LSFG_DIRECTGEN170=1: off): the generated picture shown straight from the frame
    // generation's output (two in turn) when no menu is drawn over it; the present slot showing each one.
    bool lsfgDirectGen=true;int genJobSlot[2]{-1,-1};
    std::shared_ptr<Buffer> lsfgDump;uint32_t lsfgDumpW=0,lsfgDumpH=0;unsigned lsfgDumpCount=0; // v173k CELOSIA_DIAG_LSFG_DUMP
    Unique<DkQueue, dkQueueDestroy> presentQueue;
    Unique<DkCmdBuf, dkCmdBufDestroy> presentCommands;
    std::array<std::shared_ptr<Buffer>,2> presentCmdMem;
    std::array<std::shared_ptr<Image>,2> dispGen,dispReal;
    // v173n: the present thread's copy of the real picture, taken as soon as the GPU has it, so the slot is free again
    // before the real picture waits a refresh for the display (the game thread waited for that refresh: 16-23 ms a
    // frame in heavy views). CELOSIA_DIAG_NO_PRESENT_STAGE=1: off (the slot held until the real one is presented).
    std::shared_ptr<Image> presentStage;
    DkFence presentEnd[2]{};bool presentEndPending[2]{}; // v173n: a job's last commands done (its command memory reused)
    DkFence presentReady[2]{},presentDone[2]{};
    bool presentDonePending[2]{};
    std::thread presentThread;
    std::mutex presentMutex;std::condition_variable presentCv;
    std::deque<PresentJob> presentJobs;
    bool presentBusy[2]{},presentStop=false,presentAsync=true,presentUsed=false;
    uint32_t presentSlot=0,dispW=0,dispH=0;
    bool pendingJobValid=false;PresentJob pendingJob{};
    void presentWorker();
    void presentDrain();
    void ensureDisplayImages();
    void bindSceneSamplers();
    void presentCopy(const lsfg::Img &image,Frame &frame,int interval);
    // v155: pacing and its report (real frame interval average, shown images, stage GPU times).
    // v157: passthrough while the game reaches the mode's rate by itself (frame generation only below it).
    std::atomic<uint32_t> fgHeldFrames{0}; // v173: frames held (not shown) while frame generation is on
    bool fgPassthrough=true;uint32_t fgSlow=0,fgFast=0;uint64_t fgPassLast=0,fgLsfgNs=0;
    double fgRealEma=0;uint64_t fgShownStart=0,fgStageSum[4]{};uint32_t fgShownCount=0,fgStageCount=0;
    bool scaled() const{return separate;}
    const DkImage &maskImage() const{return bloomMaskMs?bloomMaskMs->image:bloomMask->image;}
    const DkImage &sceneImage(int slot) const{return scaled()?scene->image:colors[slot]->image;}
    void resizeScene(uint32_t width,uint32_t height,uint32_t samples,bool post);
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
    // v126: faint triangles removed (CELOSIA_DIAG_NO_FAINT_CULL=1 reverts)
    bool faintCull=true;uint32_t lastFaintDraws=0,lastFaintPartial=0,lastFaintTriangles=0;
    std::vector<uint8_t> triangleScratch;
    void cullFaint(Draw &draw);
    // v134: triangles clipped to the texture region where a fragment can pass the a < 0.02 discard.
    void clipToTexture(Draw &draw);
    const std::vector<float> *alphaRegion(Texture &texture,int need);
    uint8_t alphaMax(Texture &texture,int i0,int i1,int j0,int j1,uint8_t clamp);
    std::vector<uint8_t> regionScratch; // v167: splitAlphaTest's per-triangle region maxima
    bool tailRects=true;mutable uint32_t lastTailRects=0; // v167: pixel-grid rectangles in the resolved tail
    // v167 (CELOSIA_DIAG_NO_REGION_SPLIT=1: off): the alpha-test split also by the texture region each
    // triangle can read (the largest texel alpha there), mipmapped textures' alpha grids included.
    bool alphaRegionSplit=true;
    uint32_t clipLogged=0;uint64_t clipGate[12]{};uint32_t clipGateFrames=0;
    bool textureClip=true;uint32_t lastClipDraws=0,lastClipTriangles=0;
    std::vector<Vertex> clipScratch;std::vector<ClipVertex> clipFloat;
    // v132: the screen rectangle (pixels) holding every shadow volume of the pending batch
    // (CELOSIA_DIAG_NO_SHADOW_RECT=1 reverts); full when a volume reaches behind the camera.
    int diagBloom=0; // v133 measures (CELOSIA_DIAG_BLOOM): 1 no composite draw (target rebound), 2 no bloom at all
    bool shadowRectOn=true,shadowRectAny=false,shadowRectFull=false;
    float shadowRect[4]{};uint32_t lastShadowRectArea=0;
    // v128: alpha-test split by triangle (CELOSIA_DIAG_NO_ALPHA_SPLIT=1 reverts)
    bool alphaSplit=true;uint32_t lastSplitDraws=0,lastSplitMainDropped=0,lastSplitFailDropped=0;
    void splitAlphaTest(Draw &draw);
    std::shared_ptr<Mesh> subMesh(const Mesh &from,const std::vector<uint32_t> &triangles,bool transient);
    std::vector<uint32_t> keepScratch;
    std::vector<uint64_t> sortKeys; // v125: (order << 32 | index) of each draw
    std::vector<Draw> sortedDraws;  // v125: the draws in order (storage kept between frames)
    bool boundHandleValid=false;
    DkGpuAddr boundImages=0;
    void bindTexture(const Texture &, uint8_t clamp);
    // v166: the vertex format bound (0 Vertex, 1 PackedVertex, 2 ClipVertex; -1 unknown, each list's start).
    int vtxFormat=-1;bool packNormals=true;uint64_t packedMeshes=0,floatMeshes=0;
    void bindVtxFormat(int format);
    // v166: tiled caching (the GPU keeps each screen tile's colour / depth in L2 while it is drawn), flushed
    // before anything reads the targets outside the 3D pipeline. v167: off unless CELOSIA_DIAG_TILED_CACHE=1.
    bool tiledCache=true;
    void tcFlush(){if(tiledCache)dkCmdBufTiledCacheOp(commands,DkTiledCacheOp_Flush);}
    void tcBarrier(DkBarrier mode,uint32_t invalidate){tcFlush();dkCmdBufBarrier(commands,mode,invalidate);}
    DkCmdList tcFinish(){tcFlush();const DkCmdList list=dkCmdBufFinishList(commands);if(tiledCache)dkCmdBufTiledCacheOp(commands,DkTiledCacheOp_Enable);return list;}
    // v165: the uniform block's rows as the GPU holds them (this command list's pushes so far); only the rows
    // that differ are pushed. Invalid at each command list's start. CELOSIA_DIAG_NO_PUSH165=1: every row always.
    Material pushedMaterial{};bool pushedValid=false,pushDelta=true;uint64_t pushRowsSent=0,pushRowsAll=0;
    void pushMaterial(const Material &material);
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
    bool uploadCompressed(uint64_t, const native_renderer::CompressedTexture &) override;
    void dropTexture(uint64_t id) override { if(id>1)textures.erase(id); }
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
