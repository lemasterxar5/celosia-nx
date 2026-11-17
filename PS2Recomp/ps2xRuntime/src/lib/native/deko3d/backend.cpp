#include "backend.hpp"
#include "render_progress.hpp"
#include "opaque_policy.hpp"
#include "pipeline.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#if defined(PS2X_SWITCH_HYBRID)
#include "hybrid/owned_backend.hpp"
extern "C" void switchFaultRecord(const char *,size_t);
#endif

namespace native_renderer::detail::deko {
static void deviceMessage(void *, const char *context, DkResult result, const char *message) {
#if defined(PS2X_SWITCH_HYBRID)
    if(result!=DkResult_Success){
        char report[1024];const int count=std::snprintf(report,sizeof(report),"DEKO FATAL %s: %s (%d)\n",context,message,int(result));
        if(count>0)switchFaultRecord(report,std::min(static_cast<size_t>(count),sizeof(report)-1));
    }
#endif
    std::fprintf(stderr, "[deko3d] %s: %s (%d)\n", context, message, int(result));
    if (result != DkResult_Success) std::abort();
}
DekoBackend::DekoBackend() {
    // Explicit local diagnostic opt-in; normal presentation has no readback.
    if(auto *file=std::fopen("sdmc:/switch/celosia-native-boot/capture-frame.txt","r")) {
        unsigned requested=0,interval=0;
        const int fields=std::fscanf(file,"%u %u",&requested,&interval);
        if(fields>=1 && requested>0 && requested<=1000000) {
            diagnosticCaptureFrame=requested;
            if(fields==2 && interval>=60 && interval<=1000000)diagnosticCaptureInterval=interval;
        }
        std::fclose(file);
    }
    DkDeviceMaker maker;
    dkDeviceMakerDefaults(&maker); maker.cbDebug = deviceMessage;
    device.reset(dkDeviceCreate(&maker));
    if (!device.value) throw std::runtime_error("deko3d: device creation failed");
    bufferPool=std::make_shared<BufferPool>([gpu=device.value,budget=allocationBudget]{
        return std::make_shared<BufferBlock>(gpu,BufferPool::PageBytes,budget);
    },128);
    if(!std::getenv("CELOSIA_DIAG_NO_IMAGE_POOL"))
        imagePool=std::make_shared<BufferPool>([gpu=device.value,budget=allocationBudget]{
            return std::make_shared<BufferBlock>(gpu,BufferPool::PageBytes,budget,uint32_t(DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image));
        },64);
    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, device); queueMaker.flags = DkQueueFlags_Graphics;
    queue.reset(dkQueueCreate(&queueMaker));
    if (!queue.value) throw std::runtime_error("deko3d: queue creation failed");
    const DkImage *images[Frames];
    // Lossless hardware framebuffer compression on the colour and depth targets, as in the deko3d
    // examples: it cuts the memory traffic of blending and depth testing (layered additive effects).
    // CELOSIA_DIAG_NO_FB_COMPRESSION=1 reverts.
    const char *noCompression=std::getenv("CELOSIA_DIAG_NO_FB_COMPRESSION");
    const uint32_t targetCompression=noCompression && std::atoi(noCompression)!=0?0u:uint32_t(DkImageFlags_HwCompression);
    for (uint32_t i = 0; i < Frames; ++i) {
        colors[i] = std::make_shared<Image>(device, Width, Height, DkImageFormat_RGBA8_Unorm,
            DkImageFlags_UsageRender | DkImageFlags_UsagePresent | targetCompression, allocationBudget);
        images[i] = &colors[i]->image;
    }
    depth = std::make_shared<Image>(device, Width, Height, DkImageFormat_ZF32_X24S8,
        DkImageFlags_UsageRender | targetCompression, allocationBudget);
    DkSwapchainMaker swapMaker;
    dkSwapchainMakerDefaults(&swapMaker, device, nwindowGetDefault(), images, Frames);
    swapchain.reset(dkSwapchainCreate(&swapMaker));
    if (!swapchain.value) throw std::runtime_error("deko3d: swapchain creation failed");
    dkSwapchainSetSwapInterval(swapchain, 1);
    vertexShader = std::make_unique<Shader>(device,
        "romfs:/shaders/celosia_native.vert.dksh", DkStage_Vertex, allocationBudget);
    fragmentShader = std::make_unique<Shader>(device,
        "romfs:/shaders/celosia_native.frag.dksh", DkStage_Fragment, allocationBudget);
    if(const char *sky=std::getenv("CELOSIA_DIAG_SKY"))diagSkyRequested=std::atoi(sky);
    diagSkyFrom=std::chrono::steady_clock::now()+std::chrono::seconds(
        std::getenv("CELOSIA_DIAG_SKY_AFTER")?std::atoi(std::getenv("CELOSIA_DIAG_SKY_AFTER")):0);
    earlyFragmentShader=std::make_unique<Shader>(device,
        "romfs:/shaders/celosia_native_early.frag.dksh",DkStage_Fragment,allocationBudget);
    depthFragmentShader=std::make_unique<Shader>(device,
        "romfs:/shaders/celosia_depth.frag.dksh",DkStage_Fragment,allocationBudget);
    for(int v=0;v<256;++v){
        char path[64];std::snprintf(path,sizeof(path),"romfs:/shaders/celosia_lean%03d.frag.dksh",v);
        leanFragmentShaders[v]=std::make_unique<Shader>(device,path,DkStage_Fragment,allocationBudget);
    }
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_LEAN"))leanEnabled=std::atoi(off)==0;
    if(const char *mask=std::getenv("CELOSIA_DIAG_LEAN_MASK"))leanVariants=uint32_t(std::strtoul(mask,nullptr,16));
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_PREPASS"))prepassEnabled=std::atoi(off)==0;
    if(const char *black=std::getenv("CELOSIA_DIAG_SKY_CLEAR_BLACK"))backgroundClearFog=std::atoi(black)==0;
    if(const char *strict=std::getenv("CELOSIA_DIAG_FAILURE_STRICT"))strictFailureDepth=std::atoi(strict)!=0;
    if(const char *cycle=std::getenv("CELOSIA_DIAG_GPU_CYCLE"))
        for(const char *c=cycle;*c && diagGpuCycle.size()<16;++c)if(*c>='0' && *c<='9')diagGpuCycle.push_back(*c-'0');else if(*c>='a' && *c<='t')diagGpuCycle.push_back(10+(*c-'a'));
    if(!diagGpuCycle.empty())diagGpuMode=diagGpuCycle[0];
    opaqueFragmentShader=std::make_unique<Shader>(device,
        "romfs:/shaders/celosia_opaque.frag.dksh",DkStage_Fragment,allocationBudget);
    shadowVertexShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_shadow.vert.dksh",DkStage_Vertex,allocationBudget);
    shadowFragmentShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_shadow.frag.dksh",DkStage_Fragment,allocationBudget);
    uniforms = std::make_shared<Buffer>(device, 512, allocationBudget,bufferPool);
    shadowTriangle=std::make_shared<Buffer>(device,3*sizeof(Vertex),allocationBudget,bufferPool);
    Vertex triangle[3]{};
    triangle[0].x=-1;triangle[0].y=-1;
    triangle[1].x=3;triangle[1].y=-1;
    triangle[2].x=-1;triangle[2].y=3;
    for(auto &vertex:triangle)vertex.r=vertex.g=vertex.b=vertex.a=128;
    std::memcpy(shadowTriangle->cpu(),triangle,sizeof(triangle));
    imageDescriptors=std::make_shared<ImageDescriptorPool>();
    imageDescriptors->storage=std::make_shared<Buffer>(device,size_t(ImageDescriptorSlots)*sizeof(DkImageDescriptor),allocationBudget);
    std::memset(imageDescriptors->storage->cpu(),0,size_t(ImageDescriptorSlots)*sizeof(DkImageDescriptor));
    if(const char *direct=std::getenv("CELOSIA_DIAG_SKY_DIRECT"))skyImageEnabled=std::atoi(direct)==0;
    if(const char *merge=std::getenv("CELOSIA_DIAG_NO_ALPHA_MERGE"))mergeAlphaNoop=std::atoi(merge)==0;
    if(const char *heavy=std::getenv("CELOSIA_DIAG_HEAVY_MS"))heavyCaptureNs=uint64_t(std::max(0,std::atoi(heavy)))*1000000ull;
    bgColour=std::make_shared<Image>(device,512,448,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender,allocationBudget);
    bgDepth=std::make_shared<Image>(device,512,448,DkImageFormat_ZF32_X24S8,DkImageFlags_UsageRender,allocationBudget);
    bgSmall=std::make_shared<Image>(device,256,256,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender,allocationBudget);
    bgColourSlot=imageDescriptors->slots.acquire();bgSmallSlot=imageDescriptors->slots.acquire();
    if(bgColourSlot==DescriptorSlots::None || bgSmallSlot==DescriptorSlots::None)
        throw std::runtime_error("deko3d: no descriptor slots for the background image");
    {
        const auto colourView=bgColour->view(),smallView=bgSmall->view();
        dkImageDescriptorInitialize(imageDescriptors->entries()+bgColourSlot,&colourView,false,false);
        dkImageDescriptorInitialize(imageDescriptors->entries()+bgSmallSlot,&smallView,false,false);
    }
    skyFragmentShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_sky.frag.dksh",DkStage_Fragment,allocationBudget);
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_BLOOM"))bloomAllowed=std::atoi(off)==0;
    if(const char *hard=std::getenv("CELOSIA_DIAG_SHADOW_HARD"))softShadows=std::atoi(hard)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_BG_ALPHA_MERGE"))mergeBgAlpha=std::atoi(off)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_ADD_DISCARD"))zeroAddDiscard=std::atoi(off)==0;
    {
        bloomFragmentShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_bloom.frag.dksh",DkStage_Fragment,allocationBudget);
        // v120: the mask and the soft shadows' reduced images hold one channel (every reader takes .r:
        // bloom 15, shadows 7 / 19 / 9; the mask is 0 or 1, the reduced values r = g = b): R8 stores the
        // same values in a quarter of the bytes (clear, write and reads). CELOSIA_DIAG_MASK_RGBA=1 reverts.
        const char *rgbaMask=std::getenv("CELOSIA_DIAG_MASK_RGBA");
        const DkImageFormat maskFormat=rgbaMask && std::atoi(rgbaMask)!=0?DkImageFormat_RGBA8_Unorm:DkImageFormat_R8_Unorm;
        bloomMask=std::make_shared<Image>(device,Width,Height,maskFormat,DkImageFlags_UsageRender,allocationBudget);
        auto describe=[&](const std::shared_ptr<Image> &image,uint32_t &slot){
            slot=imageDescriptors->slots.acquire();
            if(slot==DescriptorSlots::None)throw std::runtime_error("deko3d: no descriptor slot for the bloom");
            const auto view=image->view();
            dkImageDescriptorInitialize(imageDescriptors->entries()+slot,&view,false,false);
        };
        describe(bloomMask,bloomMaskSlot);
        for(int i=0;i<7;++i){
            const uint32_t size=(i==0 || i==6)?256u:((i==1 || i==5)?128u:64u);
            bloomChain[i]=std::make_shared<Image>(device,size,size,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender,allocationBudget);
            describe(bloomChain[i],bloomChainSlot[i]);
        }
        for(int i=0;i<2;++i){
            shadowSmall[i]=std::make_shared<Image>(device,Width/4,Height/4,maskFormat,DkImageFlags_UsageRender,allocationBudget);
            describe(shadowSmall[i],shadowSmallSlot[i]);
        }
    }
    // Descriptors 4..7 (nearest mip filter) are bound only by CELOSIA_DIAG_GPU_CYCLE mode 3.
    samplers = std::make_shared<Buffer>(device, 8 * sizeof(DkSamplerDescriptor), allocationBudget,bufferPool);
    for (uint32_t mode = 0; mode < 8; ++mode) {
        DkSampler sampler; dkSamplerDefaults(&sampler);
        sampler.minFilter = sampler.magFilter = DkFilter_Linear;
        sampler.mipFilter = mode & 4 ? DkMipFilter_Nearest : DkMipFilter_Linear;
        sampler.wrapMode[0] = mode & 1 ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        sampler.wrapMode[1] = mode & 2 ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        auto *desc = static_cast<DkSamplerDescriptor *>(samplers->cpu());
        dkSamplerDescriptorInitialize(desc + mode, &sampler);
    }
    DkCmdBufMaker cmdMaker;
    dkCmdBufMakerDefaults(&cmdMaker, device);
    cmdMaker.userData = this; cmdMaker.cbAddMem = addCommandMemory;
    commands.reset(dkCmdBufCreate(&cmdMaker));
    if (!commands.value) throw std::runtime_error("deko3d: command buffer creation failed");
    // An untextured shader variant may still speculatively sample: always bind a
    // valid image descriptor, never depend on branch execution for GPU safety.
    const uint8_t white[]{255, 255, 255, 128};
    uploadTexture(1, 1, 1, white, 1);
    dummyTexture = textures.at(1); textures.erase(1);
}
DekoBackend::~DekoBackend() {
    if (queue.value) dkQueueWaitIdle(queue);
    // Commands and swapchain must be destroyed before their backing allocations.
    commands.reset(); swapchain.reset();
}
void DekoBackend::addCommandMemory(void *user, DkCmdBuf cmd, size_t minimum) {
    auto &self = *static_cast<DekoBackend *>(user);
    try {
        if (!self.activeFrame || self.activeFrame->commands.size() >= 128)
            throw std::runtime_error("command frame exceeds memory budget");
        auto memory = std::make_shared<Buffer>(self.device, std::max<size_t>(minimum, 65536), self.allocationBudget,self.bufferPool);
        dkCmdBufAddMemory(cmd, memory->handle(), memory->offset, memory->bytes);
        self.activeFrame->commands.push_back(std::move(memory));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[deko3d] command allocation failed: %s\n", e.what());
        // Never continue rendering with a missing command or throw through the C callback.
        std::abort();
    }
}
void DekoBackend::startCommands(Frame &frame) {
    if (frame.pending && dkFenceWait(&frame.fence, -1) != DkResult_Success)
        throw std::runtime_error("deko3d: frame fence wait failed");
    frame.pending = false;
    dkCmdBufClear(commands);
    frame.commands.clear(); frame.draws.clear(); frame.uploads.clear(); frame.capture.reset();
    activeFrame = &frame;
    addCommandMemory(this, commands, 65536);
}
std::shared_ptr<Buffer> DekoBackend::vertexStorage(size_t bytes,uint32_t &offset,bool transient) {
    offset=0;
    if(!transient || !recording){
        // v109: kept meshes over 64 KB from a pooled page too (a memblock each cost ~0.8 ms).
        if(imagePool && bytes>65536)if(auto pooled=Buffer::pooled(bufferPool,bytes))return pooled;
        return std::make_shared<Buffer>(device,bytes,allocationBudget,bufferPool);
    }
    auto &frame=frames[frameIndex];
    const auto span=frame.vertexCursor.reserve(bytes);
    if(span.page==frame.vertexPages.size())
        frame.vertexPages.push_back(std::make_shared<Buffer>(device,span.capacity,allocationBudget));
    offset=span.offset;return frame.vertexPages.at(span.page);
}
void DekoBackend::uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool transient) {
    const auto timerStart=armTicksToNs(armGetSystemTick());
    struct Timer{DekoBackend &b;uint64_t start;~Timer(){b.frameMeshNs+=armTicksToNs(armGetSystemTick())-start;++b.frameMeshCount;}} timer{*this,timerStart};
    const auto bytes = meshBytes(count);
    if (!vertices) throw std::invalid_argument("deko3d: null mesh vertices");
    auto mesh = std::make_shared<Mesh>();
    mesh->vertices = vertexStorage(bytes,mesh->vertexOffset,transient);
    // v111: one pass for the copy, the least vertex alpha and the bounds (same values as
    // minimumVertexAlpha / meshBounds; the copy is the same bytes).
    {
        if(!count)throw std::invalid_argument("Invalid vertex alpha range");
        auto *to=reinterpret_cast<Vertex *>(static_cast<uint8_t *>(mesh->vertices->cpu())+mesh->vertexOffset);
        uint8_t alpha=255;
        MeshBounds b;
        b.lo[0]=b.hi[0]=vertices[0].x; b.lo[1]=b.hi[1]=vertices[0].y; b.lo[2]=b.hi[2]=vertices[0].z;
        for(uint32_t i=0;i<count;++i){
            const Vertex &v=vertices[i];
            alpha=std::min(alpha,v.a);
            b.lo[0]=std::min(b.lo[0],v.x);b.hi[0]=std::max(b.hi[0],v.x);
            b.lo[1]=std::min(b.lo[1],v.y);b.hi[1]=std::max(b.hi[1],v.y);
            b.lo[2]=std::min(b.lo[2],v.z);b.hi[2]=std::max(b.hi[2],v.z);
        }
        std::memcpy(to,vertices,bytes);
        b.valid=std::isfinite(b.lo[0]) && std::isfinite(b.lo[1]) && std::isfinite(b.lo[2]) &&
                std::isfinite(b.hi[0]) && std::isfinite(b.hi[1]) && std::isfinite(b.hi[2]);
        mesh->minAlpha=alpha;mesh->bounds=b;
    }
    mesh->count = count; mesh->transient = transient;
    meshes[id] = std::move(mesh);
}
void DekoBackend::uploadTexture(uint64_t id, uint32_t w, uint32_t h,
                                const uint8_t *rgba, uint32_t levels) {
    const auto timerStart=armTicksToNs(armGetSystemTick());
    struct Timer{DekoBackend &b;uint64_t start;~Timer(){b.frameTexNs+=armTicksToNs(armGetSystemTick())-start;++b.frameTexCount;}} timer{*this,timerStart};
    const auto bytes = textureBytes(w, h, levels);
    if (!id || !rgba) throw std::invalid_argument("deko3d: null/reserved texture");
    if (uploads.size() >= 4096) throw std::runtime_error("deko3d: excessive pending textures");
    auto texture = std::make_shared<Texture>();
    texture->minAlpha=minimumTextureAlpha(rgba,bytes);
    texture->width=uint16_t(w);texture->height=uint16_t(h);texture->levels=uint16_t(levels);
    texture->image = std::make_shared<Image>(device, w, h, DkImageFormat_RGBA8_Unorm, 0, allocationBudget, levels, imagePool);
    auto view = texture->image->view();
    // Slot writes target entries no in-flight frame references (slots free only after their
    // Texture died, i.e. after every frame holding it retired); the per-frame upload barrier
    // invalidates the descriptor cache before the first draw that selects the new entry.
    texture->slot=imageDescriptors->slots.acquire();
    if(texture->slot!=DescriptorSlots::None){
        texture->pool=imageDescriptors;
        dkImageDescriptorInitialize(imageDescriptors->entries()+texture->slot,&view,false,false);
    }else{
        ++privateDescriptorTextures;
        texture->descriptor = std::make_shared<Buffer>(device, sizeof(DkImageDescriptor), allocationBudget,bufferPool);
        dkImageDescriptorInitialize(static_cast<DkImageDescriptor *>(texture->descriptor->cpu()),
            &view, false, false);
    }
    auto staging = imagePool && bytes>65536?Buffer::pooled(bufferPool,bytes):nullptr;
    if(!staging)staging = std::make_shared<Buffer>(device, bytes, allocationBudget,bufferPool);
    std::memcpy(staging->cpu(), rgba, bytes);
    uploads.push_back({texture, std::move(staging), w, h, levels});
    textures[id] = std::move(texture);
}
void DekoBackend::beginFrame() {
    if (recording) throw std::logic_error("deko3d: beginFrame while already recording");
    auto &frame=frames[frameIndex];
    markRender(RenderPhase::Fence);
    const auto waitStart=armTicksToNs(armGetSystemTick());
    if(frame.pending && dkFenceWait(&frame.fence,-1)!=DkResult_Success)
        throw std::runtime_error("deko3d: vertex arena fence wait failed");
    lastBeginWaitNs=armTicksToNs(armGetSystemTick())-waitStart;
    // The owning frame fence makes both reports visible before CPU reads or reuse.
    if(frame.gpuTimingPending){
        const auto *reports=static_cast<const GpuReport *>(frame.gpuReports->cpu());
        if(const auto ns=gpuIntervalNs(reports[0],reports[1])){
            lastGpuNs=*ns;lastGpuTimingFrame=frame.gpuTimingFrame;
            (void)gpuTimeWindow.add(*ns);
            // With a diagnosis cycle, one capture per mode (the modes rotate every status window).
            // Without a cycle: up to 8 captures, at least 600 frames apart after the first three.
            const bool modeFree=diagGpuCycle.empty()?(heavyCaptures<3 || (heavyCaptures<8 && submittedFrames-lastHeavyFrame>=600)):!(heavyModeCaptured&(1u<<diagGpuMode));
            if(heavyCaptureNs && modeFree && *ns>heavyCaptureNs && !diagnosticCaptureFrame && frame.draws.size()>=1500){
                heavyModeCaptured|=1u<<diagGpuMode; // gameplay views only, not loading
                diagnosticCaptureFrame=submittedFrames+2;++heavyCaptures;lastHeavyFrame=submittedFrames;
            }
        }
        frame.gpuTimingPending=false;
    }
    if(frame.segmentsPending){
        (void)segmentWindow.add(static_cast<const GpuReport *>(frame.segmentReports->cpu()),frame.segmentKinds.data(),frame.segmentCount);
        frame.segmentsPending=false;
    }
    if(frame.gpuCountersPending){
        const auto *reports=static_cast<const GpuReport *>(frame.gpuReports->cpu());
        const auto vertices=gpuCounterDelta(reports[2],reports[3]);
        const auto fragments=gpuCounterDelta(reports[4],reports[5]);
        const auto primitives=gpuCounterDelta(reports[6],reports[7]);
        std::optional<GpuCounterValues> values;
        if(frame.gpuCounterFrame && frame.gpuCounterFrame==frame.gpuTimingFrame && vertices && fragments && primitives)
            values=GpuCounterValues{*vertices,*fragments,*primitives};
        if(gpuCounterWindow.add(values))lastGpuCounterFrame=frame.gpuCounterFrame;
        frame.gpuCountersPending=false;
    }
    frame.pending=false;frame.vertexCursor.reset();
    markRender(RenderPhase::Uploads);
    recording = true; held = false; draws.clear(); fog = {};
}
void DekoBackend::draw(uint64_t id, const float clip[16], const DrawState &state) {
    const auto drawStart=armGetSystemTick();
    struct DrawTimer{DekoBackend &b;uint64_t start;~DrawTimer(){b.frameDrawNs+=armTicksToNs(armGetSystemTick()-start);}} drawTimer{*this,drawStart};
    if (!recording || !clip) throw std::logic_error("deko3d: draw outside frame/null matrix");
    validateDraw(state, !fog.enabled);
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(clip[i])) throw std::invalid_argument("deko3d: non-finite matrix");
    if (draws.size() >= 16384) throw std::runtime_error("deko3d: excessive draw count");
    auto it = meshes.find(id);
    if (it == meshes.end()) throw std::invalid_argument("deko3d: unknown mesh");
    std::shared_ptr<Texture> texture = dummyTexture;
    if (state.texture) {
        const auto found = textures.find(state.texture);
        if (found == textures.end()) throw std::invalid_argument("deko3d: unknown texture");
        texture = found->second;
    }
    if(state.blend==Blend::Additive && !state.background && !state.screen && state.texture && loggedAdditiveTextures.size()<64 &&
       std::find(loggedAdditiveTextures.begin(),loggedAdditiveTextures.end(),state.texture)==loggedAdditiveTextures.end()){
        loggedAdditiveTextures.push_back(state.texture);
        std::fprintf(stderr,"[additive-texture] id=%llx size=%ux%u levels=%u min_alpha=%u lodK=%.3f lodScale=%.3f maxLod=%.3f clamp=%u alpha_test=0x%04x vertices=%u order=%u\n",
            static_cast<unsigned long long>(state.texture),unsigned(texture->width),unsigned(texture->height),unsigned(texture->levels),
            unsigned(texture->minAlpha),state.lodK,state.lodScale,state.maxLod,unsigned(state.clamp),unsigned(state.alphaTest),
            it->second->count,state.order);
    }
    Draw draw{id, it->second, texture, {}, state};
    if(mergeAlphaNoop && alphaTestWithoutEffect(draw.state)){draw.state.alphaTest=0;++mergedAlphaDraws;}
    std::copy_n(clip, 16, draw.clip.begin()); draws.push_back(std::move(draw));
}
void DekoBackend::setFog(const Fog &value) {
    validateFog(value);
    fog = value;
}
void DekoBackend::setBloom(const Bloom &b) {
    if(b.enabled && (!std::isfinite(b.threshold) || !std::isfinite(b.zA) || !std::isfinite(b.zB) || !std::isfinite(b.temporal) ||
       !std::isfinite(b.color[0]) || !std::isfinite(b.color[1]) || !std::isfinite(b.color[2]) || !std::isfinite(b.color[3])))
        throw std::invalid_argument("deko3d: invalid bloom");
    bloom=b; // kept until changed, as the GL renderer keeps it
}
void DekoBackend::addScreenBlur(const ScreenBlur &b) {
    if (b.steps) throw std::runtime_error("deko3d: screen blur not implemented");
}
void DekoBackend::setGlowComposite(const GlowComposite &g) {
    if (g.enabled) throw std::runtime_error("deko3d: glow composite not implemented");
}
void DekoBackend::setDepthOfField(const DepthOfField &d) {
    if (d.enabled) throw std::runtime_error("deko3d: depth of field not implemented");
}
}
namespace native_renderer::detail {
Backend *createDeko3dBackend() {
#if defined(PS2X_SWITCH_HYBRID)
    return createOwnedDekoBackend([]{return new deko::DekoBackend();});
#else
    return new deko::DekoBackend();
#endif
}
// Diagnostic request: the next non-held endFrame copies while it still owns
// the acquired image. latestFrame retrieves that requested capture afterwards.
void requestDeko3dCapture(Backend &backend) {
    auto *dekoBackend = dynamic_cast<deko::DekoBackend *>(&backend);
    if (!dekoBackend) throw std::invalid_argument("capture request requires a deko3d backend");
    dekoBackend->requestCapture();
}
}
