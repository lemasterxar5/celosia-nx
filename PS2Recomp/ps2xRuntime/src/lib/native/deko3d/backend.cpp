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
    if(auto *file=std::fopen("sdmc:/switch/celosia_nx/capture-frame.txt","r")) {
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
    dkQueueMakerDefaults(&queueMaker, device); queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute; // v153: compute for frame generation
    // v173q: the render queue's own command memory 1 MiB (was deko3d's 64 KiB minimum, flushed every 8 KiB) and
    // room for 512 compute jobs in flight (was 128), so a frame with the whole LSFG chain doesn't make the game
    // thread wait in its submit for the GPU to run most of that frame. Memory only: every fence and wait stays.
    // CELOSIA_DIAG_SMALL_QUEUE173=1: the old sizes (A/B).
    if(!std::getenv("CELOSIA_DIAG_SMALL_QUEUE173")){
        queueMaker.commandMemorySize=1u<<20;queueMaker.flushThreshold=1u<<17;queueMaker.maxConcurrentComputeJobs=512;
    }
    queue.reset(dkQueueCreate(&queueMaker));
    if (!queue.value) throw std::runtime_error("deko3d: queue creation failed");
    // v164: the frame generation's present queue (graphics: copies only) and its command buffer.
    if(const char *off=std::getenv("CELOSIA_DIAG_SYNC_PRESENT"))presentAsync=std::atoi(off)==0;
    if(std::getenv("CELOSIA_DIAG_NO_LSFG_DIRECT"))lsfgDirectReal=false;
    if(std::getenv("CELOSIA_DIAG_NO_LSFG_DIRECTGEN170"))lsfgDirectGen=false;
    if(presentAsync){
        DkQueueMaker presentMaker;
        dkQueueMakerDefaults(&presentMaker,device);presentMaker.flags=DkQueueFlags_Graphics;
        presentMaker.commandMemorySize=DK_QUEUE_MIN_CMDMEM_SIZE;presentMaker.flushThreshold=DK_QUEUE_MIN_CMDMEM_SIZE/8;
        presentQueue.reset(dkQueueCreate(&presentMaker));
        DkCmdBufMaker presentCmdMaker;dkCmdBufMakerDefaults(&presentCmdMaker,device);
        if(presentQueue.value)presentCommands.reset(dkCmdBufCreate(&presentCmdMaker));
        for(auto &m:presentCmdMem)m=std::make_shared<Buffer>(device,65536,allocationBudget);
        if(!presentQueue.value || !presentCommands.value){presentAsync=false;std::fprintf(stderr,"[deko3d] present queue unavailable\n");}
        else presentThread=std::thread([this]{presentWorker();});
    }
    // Lossless hardware framebuffer compression on the colour and depth targets, as in the deko3d
    // examples: it cuts the memory traffic of blending and depth testing (layered additive effects).
    // CELOSIA_DIAG_NO_FB_COMPRESSION=1 reverts.
    const char *noCompression=std::getenv("CELOSIA_DIAG_NO_FB_COMPRESSION");
    const uint32_t targetCompression=noCompression && std::atoi(noCompression)!=0?0u:uint32_t(DkImageFlags_HwCompression);
    depth = std::make_shared<Image>(device, Width, Height, DkImageFormat_ZF32_X24S8,
        DkImageFlags_UsageRender | targetCompression, allocationBudget);
    targetFlags=targetCompression;
    makeSwapchain(Width,Height);
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
    if(std::getenv("CELOSIA_DIAG_NO_PUSH165"))pushDelta=false;
    if(std::getenv("CELOSIA_DIAG_NO_PACK166"))packNormals=false;
    if(std::getenv("CELOSIA_DIAG_NO_REGION_SPLIT"))alphaRegionSplit=false;
    if(std::getenv("CELOSIA_DIAG_NO_TAIL_RECTS"))tailRects=false;
    // v167: off by default (v166 A/B: +0.5 ms GPU in the cinematic, worse gameplay); CELOSIA_DIAG_TILED_CACHE=1: on.
    tiledCache=std::getenv("CELOSIA_DIAG_TILED_CACHE")!=nullptr;
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
    postFragmentShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_post.frag.dksh",DkStage_Fragment,allocationBudget);
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_POST_VARIANTS"))postVariants=std::atoi(off)==0;
    try{postFxShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_postfx.frag.dksh",DkStage_Fragment,allocationBudget);
        postPlainShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_postplain.frag.dksh",DkStage_Fragment,allocationBudget);}
    catch(const std::exception &e){postFxShader.reset();postPlainShader.reset();std::fprintf(stderr,"[deko3d] post variants: %s\n",e.what());}
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_SOLID_RESOLVE"))solidResolve=std::atoi(off)==0;
    if(const char *on=std::getenv("CELOSIA_DIAG_COMPUTE_POST"))computePost=std::atoi(on)!=0; // v162: off by default
    if(const char *off=std::getenv("CELOSIA_DIAG_COPY_IMAGE"))blitCopies=std::atoi(off)==0; // v161: back to dkCmdBufCopyImage
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_EARLY_MASK"))earlyMask=std::atoi(off)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_SHADOW_SHADER"))shadowSoft=std::atoi(off)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_BLOOM_SHADERS"))bloomSplit=std::atoi(off)==0;
    try{bloomChainShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_bloomchain.frag.dksh",DkStage_Fragment,allocationBudget);
        bloomCompShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_bloomcomp.frag.dksh",DkStage_Fragment,allocationBudget);}
    catch(const std::exception &e){bloomChainShader.reset();bloomCompShader.reset();std::fprintf(stderr,"[deko3d] bloom shaders: %s\n",e.what());}
    try{shadowSoftShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_shadowsoft.frag.dksh",DkStage_Fragment,allocationBudget);}
    catch(const std::exception &e){std::fprintf(stderr,"[deko3d] soft shadow shader: %s\n",e.what());}
    try{oneFragmentShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_one.frag.dksh",DkStage_Fragment,allocationBudget);}
    catch(const std::exception &e){std::fprintf(stderr,"[deko3d] mask shader: %s\n",e.what());}
    try{solidFragmentShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_solid.frag.dksh",DkStage_Fragment,allocationBudget);}
    catch(const std::exception &e){std::fprintf(stderr,"[deko3d] solid resolve shader: %s\n",e.what());}
    try{
        postComputeShader=std::make_unique<Shader>(device,"romfs:/shaders/celosia_post.comp.dksh",DkStage_Compute,allocationBudget);
        postConstants=std::make_shared<Buffer>(device,16*256,allocationBudget);
    }catch(const std::exception &e){postComputeShader.reset();std::fprintf(stderr,"[deko3d] compute post shader: %s\n",e.what());}
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_BLOOM"))bloomAllowed=std::atoi(off)==0;
    if(const char *hard=std::getenv("CELOSIA_DIAG_SHADOW_HARD"))softShadows=std::atoi(hard)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_BG_ALPHA_MERGE"))mergeBgAlpha=std::atoi(off)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_FAINT_CULL"))faintCull=std::atoi(off)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_TEXTURE_CLIP"))textureClip=std::atoi(off)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_SHADOW_RECT"))shadowRectOn=std::atoi(off)==0;
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_RESOLVED_TAIL"))resolvedTail=std::atoi(off)==0;
    if(const char *v=std::getenv("CELOSIA_DIAG_BLOOM"))diagBloom=std::atoi(v);
    if(const char *off=std::getenv("CELOSIA_DIAG_NO_ALPHA_SPLIT"))alphaSplit=std::atoi(off)==0;
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
        maskFormatNow=maskFormat;
        sceneSlot=imageDescriptors->slots.acquire();
        if(sceneSlot==DescriptorSlots::None)throw std::runtime_error("deko3d: no descriptor slot for the scene image");
        postOutSlot=imageDescriptors->slots.acquire(); // v160 (None: the fragment post only)
        postOutTexSlot=imageDescriptors->slots.acquire(); // v161: the same image sampled
        for(auto &s:fgInSlot)s=imageDescriptors->slots.acquire(); // v168: the compute post straight into LSFG's input
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
    // v149: five sets of the eight, one per anisotropy level (1, 2, 4, 8, 16: the menu's choice binds its set).
    samplers = std::make_shared<Buffer>(device, 40 * sizeof(DkSamplerDescriptor), allocationBudget,bufferPool);
    for (uint32_t index = 0; index < 40; ++index) {
        const uint32_t mode = index & 7;
        DkSampler sampler; dkSamplerDefaults(&sampler);
        sampler.maxAnisotropy = float(1u << (index >> 3));
        sampler.minFilter = sampler.magFilter = DkFilter_Linear;
        sampler.mipFilter = mode & 4 ? DkMipFilter_Nearest : DkMipFilter_Linear;
        sampler.wrapMode[0] = mode & 1 ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        sampler.wrapMode[1] = mode & 2 ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        auto *desc = static_cast<DkSamplerDescriptor *>(samplers->cpu());
        dkSamplerDescriptorInitialize(desc + index, &sampler);
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
// v151: the swapchain and its images at width x height (the window's buffers resized to match).
void DekoBackend::makeSwapchain(uint32_t width,uint32_t height){
    swapchain.reset();
    const DkImage *images[Frames];
    for (uint32_t i = 0; i < Frames; ++i) {
        colors[i] = std::make_shared<Image>(device, width, height, DkImageFormat_RGBA8_Unorm,
            DkImageFlags_UsageRender | DkImageFlags_UsagePresent | targetFlags, allocationBudget);
        images[i] = &colors[i]->image;
    }
    nwindowSetDimensions(nwindowGetDefault(), width, height);
    DkSwapchainMaker swapMaker;
    dkSwapchainMakerDefaults(&swapMaker, device, nwindowGetDefault(), images, Frames);
    swapchain.reset(dkSwapchainCreate(&swapMaker));
    if (!swapchain.value) throw std::runtime_error("deko3d: swapchain creation failed");
    dkSwapchainSetSwapInterval(swapchain, swapIntervalNow);
    ow=width;oh=height;
}
DekoBackend::~DekoBackend() {
    if(presentThread.joinable()){
        {std::lock_guard<std::mutex> lock(presentMutex);presentStop=true;}
        presentCv.notify_all();presentThread.join();
    }
    if(presentQueue.value)dkQueueWaitIdle(presentQueue);
    presentCommands.reset();
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
    pushedValid = false; // v165: a new command list: its first material push is whole
    vtxFormat = -1;      // v166: so is its first vertex format
    addCommandMemory(this, commands, 65536);
    if (tiledCache) dkCmdBufTiledCacheOp(commands, DkTiledCacheOp_Enable);
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
    // v166: packed when every normal component is within [-1, 1].
    bool pack=packNormals && count!=0;
    for(uint32_t i=0;pack && i<count;++i){const Vertex &v=vertices[i];pack=std::fabs(v.nx)<=1.0f && std::fabs(v.ny)<=1.0f && std::fabs(v.nz)<=1.0f;}
    mesh->packed=pack;(pack?packedMeshes:floatMeshes)++;
    mesh->vertices = vertexStorage(pack?size_t(count)*sizeof(PackedVertex):bytes,mesh->vertexOffset,transient);
    // v111: one pass for the copy, the least vertex alpha and the bounds (same values as
    // minimumVertexAlpha / meshBounds; the copy is the same bytes).
    {
        if(!count)throw std::invalid_argument("Invalid vertex alpha range");
        auto *to=static_cast<uint8_t *>(mesh->vertices->cpu())+mesh->vertexOffset;
        uint8_t alpha=255,top=0;
        MeshBounds b;
        b.lo[0]=b.hi[0]=vertices[0].x; b.lo[1]=b.hi[1]=vertices[0].y; b.lo[2]=b.hi[2]=vertices[0].z;
        for(uint32_t i=0;i<count;++i){
            const Vertex &v=vertices[i];
            alpha=std::min(alpha,v.a);top=std::max(top,v.a);
            b.lo[0]=std::min(b.lo[0],v.x);b.hi[0]=std::max(b.hi[0],v.x);
            b.lo[1]=std::min(b.lo[1],v.y);b.hi[1]=std::max(b.hi[1],v.y);
            b.lo[2]=std::min(b.lo[2],v.z);b.hi[2]=std::max(b.hi[2],v.z);
        }
        if(pack){auto *p=reinterpret_cast<PackedVertex *>(to);for(uint32_t i=0;i<count;++i)p[i]=packVertex(vertices[i]);}
        else std::memcpy(to,vertices,bytes);
        b.valid=std::isfinite(b.lo[0]) && std::isfinite(b.lo[1]) && std::isfinite(b.lo[2]) &&
                std::isfinite(b.hi[0]) && std::isfinite(b.hi[1]) && std::isfinite(b.hi[2]);
        mesh->minAlpha=alpha;mesh->bounds=b;mesh->maxAlpha=top;
        // v126: each triangle's largest vertex alpha (a triangle list: three vertices each).
        // v167: every triangle list up to 16384 triangles (the alpha-test split by texture region needs
        // them with opaque vertex colours too), with each triangle's uv rectangle.
        if(count%3==0 && (alpha<128 || (alphaRegionSplit && count<=3u*16384u))){
            const uint32_t triangles=count/3;
            if(alphaRegionSplit && triangles<=16384u){
                mesh->triangleUV.resize(size_t(triangles)*4);
                for(uint32_t t=0;t<triangles;++t){
                    const Vertex &a=vertices[3*t],&b=vertices[3*t+1],&c=vertices[3*t+2];
                    float *r=&mesh->triangleUV[size_t(t)*4];
                    r[0]=std::min({a.u,b.u,c.u});r[1]=std::max({a.u,b.u,c.u});r[2]=std::min({a.v,b.v,c.v});r[3]=std::max({a.v,b.v,c.v});
                }
            }
            mesh->triangleAlpha.resize(triangles);mesh->triangleMin.resize(triangles);
            uint8_t floor=255;
            for(uint32_t t=0;t<triangles;++t){
                const uint8_t a0=vertices[3*t].a,a1=vertices[3*t+1].a,a2=vertices[3*t+2].a;
                const uint8_t m=std::max(a0,std::max(a1,a2));
                mesh->triangleAlpha[t]=m;mesh->triangleMin[t]=std::min(a0,std::min(a1,a2));floor=std::min(floor,m);
            }
            mesh->triangleFloor=floor;
        }
    }
    mesh->count = count; mesh->transient = transient;
    if(mesh->maxAlpha<64 && count<=8192)mesh->cpuVertices=std::make_shared<const std::vector<Vertex>>(vertices,vertices+count);
    meshes[id] = std::move(mesh);
}
// v167: a texture pack's BC3 texture (see native_renderer::CompressedTexture).
bool DekoBackend::uploadCompressed(uint64_t id, const native_renderer::CompressedTexture &t) {
    // v168: the reason of the first refusals logged (v167b: every pack texture refused, cause unknown).
    static int refusals=0;
    auto refuse=[&](const char *why,size_t need){
        if(refusals++<12)std::fprintf(stderr,"[texpack] backend refused %016llx: %s (%ux%u levels %u bytes %zu need %zu uploads %zu)\n",
            (unsigned long long)id,why,t.width,t.height,t.levels,t.bytes,need,uploads.size());
        return false;
    };
    if (!id || id==1 || !t.data || !t.width || !t.height || t.width>4096 || t.height>4096 || !t.levels || t.levels>13) return refuse("arguments",0);
    size_t need=0;
    for(uint32_t l=0,w=t.width,h=t.height;l<t.levels;++l,w=std::max(1u,w/2),h=std::max(1u,h/2))need+=size_t((w+3)/4)*((h+3)/4)*16;
    if(need>t.bytes)return refuse("short data",need);
    if(uploads.size()>=4096)return refuse("upload queue full",need);
    const auto timerStart=armTicksToNs(armGetSystemTick());
    struct Timer{DekoBackend &b;uint64_t start;~Timer(){b.frameTexNs+=armTicksToNs(armGetSystemTick())-start;++b.frameTexCount;}} timer{*this,timerStart};
    auto texture = std::make_shared<Texture>();
    texture->minAlpha=t.minAlpha;texture->maxAlpha=t.maxAlpha;texture->fullTexels=t.fullTexels;texture->clearTexels=t.clearTexels;
    texture->width=uint16_t(t.width);texture->height=uint16_t(t.height);texture->levels=uint16_t(t.levels);texture->replaced=true;
    if(t.alphaGrid && t.gridW>=4 && t.gridH>=4 && t.gridW<=256 && t.gridH<=256){
        texture->alphaGrid.assign(t.alphaGrid,t.alphaGrid+size_t(t.gridW)*t.gridH);texture->gridW=uint16_t(t.gridW);texture->gridH=uint16_t(t.gridH);
    }
    texture->image = std::make_shared<Image>(device, t.width, t.height, DkImageFormat_RGBA_BC3, 0, allocationBudget, t.levels, imagePool);
    auto view = texture->image->view();
    texture->slot=imageDescriptors->slots.acquire();
    if(texture->slot!=DescriptorSlots::None){
        texture->pool=imageDescriptors;
        dkImageDescriptorInitialize(imageDescriptors->entries()+texture->slot,&view,false,false);
    }else{
        ++privateDescriptorTextures;
        texture->descriptor = std::make_shared<Buffer>(device, sizeof(DkImageDescriptor), allocationBudget,bufferPool);
        dkImageDescriptorInitialize(static_cast<DkImageDescriptor *>(texture->descriptor->cpu()),&view, false, false);
    }
    auto staging = imagePool && need>65536?Buffer::pooled(bufferPool,need):nullptr;
    if(!staging)staging = std::make_shared<Buffer>(device, need, allocationBudget,bufferPool);
    std::memcpy(staging->cpu(), t.data, need);
    uploads.push_back({texture, std::move(staging), t.width, t.height, t.levels, true});
    textures[id] = std::move(texture);
    return true;
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
    {
        uint8_t top=0;size_t full=0,clear=0;
        for(size_t i=3;i<bytes;i+=4){const uint8_t a=rgba[i];top=std::max(top,a);full+=a>=128;clear+=a<=2;}
        texture->maxAlpha=top;const size_t texels=bytes/4;
        texture->fullTexels=uint16_t(full*1000/texels);texture->clearTexels=uint16_t(clear*1000/texels);
    }
    texture->width=uint16_t(w);texture->height=uint16_t(h);texture->levels=uint16_t(levels);
    if((levels==1 || alphaRegionSplit) && w>=4 && h>=4 && w<=256 && h<=256 && texture->clearTexels>=200 && !(w&(w-1)) && !(h&(h-1))){
        texture->alphaGrid.resize(size_t(w)*h);texture->gridW=uint16_t(w);texture->gridH=uint16_t(h);
        for(size_t i=0;i<size_t(w)*h;++i)texture->alphaGrid[i]=rgba[4*i+3];
        // v167: a mipmapped texture: each level-0 cell also holds the largest alpha a bilinear sample of a
        // coarser level can read over it (that level's 3 x 3 maxima, the texture repeating), so a range of
        // the grid bounds a sample at any level: the regions and splits stay conservative.
        size_t at=size_t(w)*h*4;std::vector<uint8_t> dilated;
        for(uint32_t level=1,lw=w,lh=h;level<levels;++level){
            lw=std::max(1u,lw/2);lh=std::max(1u,lh/2);
            const uint8_t *a=rgba+at;at+=size_t(lw)*lh*4;
            dilated.assign(size_t(lw)*lh,0);
            for(uint32_t y=0;y<lh;++y)for(uint32_t x=0;x<lw;++x){
                uint8_t m=0;
                for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                    const uint32_t sy=(y+lh+uint32_t(dy))%lh,sx=(x+lw+uint32_t(dx))%lw;
                    m=std::max(m,a[(size_t(sy)*lw+sx)*4+3]);
                }
                dilated[size_t(y)*lw+x]=m;
            }
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
                uint8_t &g=texture->alphaGrid[size_t(y)*w+x];
                g=std::max(g,dilated[size_t(std::min(y>>level,lh-1))*lw+std::min(x>>level,lw-1)]);
            }
        }
    }
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
            // v165: CELOSIA_DIAG_HEAVY_ANY=1 also takes heavy frames with few draws (the cinematic's overdraw-bound
            // frames have ~260 draws and were never captured).
            static const bool heavyAny=std::getenv("CELOSIA_DIAG_HEAVY_ANY")!=nullptr;
            if(heavyCaptureNs && modeFree && *ns>heavyCaptureNs && !diagnosticCaptureFrame && (heavyAny || frame.draws.size()>=1500)){
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
    fog = value; // v173w: the [fog173] log and its draw counters removed (development only)
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
