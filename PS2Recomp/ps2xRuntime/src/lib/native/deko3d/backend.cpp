#include "backend.hpp"
#include "render_progress.hpp"
#include "opaque_policy.hpp"
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
    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, device); queueMaker.flags = DkQueueFlags_Graphics;
    queue.reset(dkQueueCreate(&queueMaker));
    if (!queue.value) throw std::runtime_error("deko3d: queue creation failed");
    const DkImage *images[Frames];
    for (uint32_t i = 0; i < Frames; ++i) {
        colors[i] = std::make_shared<Image>(device, Width, Height, DkImageFormat_RGBA8_Unorm,
            DkImageFlags_UsageRender | DkImageFlags_UsagePresent, allocationBudget);
        images[i] = &colors[i]->image;
    }
    depth = std::make_shared<Image>(device, Width, Height, DkImageFormat_ZF32_X24S8,
        DkImageFlags_UsageRender, allocationBudget);
    DkSwapchainMaker swapMaker;
    dkSwapchainMakerDefaults(&swapMaker, device, nwindowGetDefault(), images, Frames);
    swapchain.reset(dkSwapchainCreate(&swapMaker));
    if (!swapchain.value) throw std::runtime_error("deko3d: swapchain creation failed");
    dkSwapchainSetSwapInterval(swapchain, 1);
    vertexShader = std::make_unique<Shader>(device,
        "romfs:/shaders/celosia_native.vert.dksh", DkStage_Vertex, allocationBudget);
    fragmentShader = std::make_unique<Shader>(device,
        "romfs:/shaders/celosia_native.frag.dksh", DkStage_Fragment, allocationBudget);
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
    samplers = std::make_shared<Buffer>(device, 4 * sizeof(DkSamplerDescriptor), allocationBudget,bufferPool);
    for (uint32_t mode = 0; mode < 4; ++mode) {
        DkSampler sampler; dkSamplerDefaults(&sampler);
        sampler.minFilter = sampler.magFilter = DkFilter_Linear;
        sampler.mipFilter = DkMipFilter_Linear;
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
    if(!transient || !recording)return std::make_shared<Buffer>(device,bytes,allocationBudget,bufferPool);
    auto &frame=frames[frameIndex];
    const auto span=frame.vertexCursor.reserve(bytes);
    if(span.page==frame.vertexPages.size())
        frame.vertexPages.push_back(std::make_shared<Buffer>(device,span.capacity,allocationBudget));
    offset=span.offset;return frame.vertexPages.at(span.page);
}
void DekoBackend::uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool transient) {
    const auto bytes = meshBytes(count);
    if (!vertices) throw std::invalid_argument("deko3d: null mesh vertices");
    auto mesh = std::make_shared<Mesh>();
    mesh->minAlpha=minimumVertexAlpha(vertices,count);
    mesh->vertices = vertexStorage(bytes,mesh->vertexOffset,transient);
    std::memcpy(static_cast<uint8_t *>(mesh->vertices->cpu())+mesh->vertexOffset, vertices, bytes);
    mesh->count = count; mesh->transient = transient;
    meshes[id] = std::move(mesh);
}
void DekoBackend::uploadTexture(uint64_t id, uint32_t w, uint32_t h,
                                const uint8_t *rgba, uint32_t levels) {
    const auto bytes = textureBytes(w, h, levels);
    if (!id || !rgba) throw std::invalid_argument("deko3d: null/reserved texture");
    if (uploads.size() >= 4096) throw std::runtime_error("deko3d: excessive pending textures");
    auto texture = std::make_shared<Texture>();
    texture->minAlpha=minimumTextureAlpha(rgba,bytes);
    texture->image = std::make_shared<Image>(device, w, h, DkImageFormat_RGBA8_Unorm, 0, allocationBudget, levels);
    texture->descriptor = std::make_shared<Buffer>(device, sizeof(DkImageDescriptor), allocationBudget,bufferPool);
    auto view = texture->image->view();
    dkImageDescriptorInitialize(static_cast<DkImageDescriptor *>(texture->descriptor->cpu()),
        &view, false, false);
    auto staging = std::make_shared<Buffer>(device, bytes, allocationBudget,bufferPool);
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
        }
        frame.gpuTimingPending=false;
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
    Draw draw{id, it->second, texture, {}, state};
    std::copy_n(clip, 16, draw.clip.begin()); draws.push_back(std::move(draw));
}
void DekoBackend::setFog(const Fog &value) {
    validateFog(value);
    fog = value;
}
void DekoBackend::setBloom(const Bloom &b) {
    if (b.enabled) throw std::runtime_error("deko3d: bloom not implemented");
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
