#include "backend.hpp"
#include "render_progress.hpp"
#include "late_mesh.hpp"
#include "opaque_policy.hpp"
#include "prepass_policy.hpp"
// Optional present observer (platform/switch/hybrid/saltynx_fps.cpp: SaltyNX FPS counter block).
extern "C" void celosia_frame_presented(void) __attribute__((weak));
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <unistd.h>

namespace native_renderer::detail::deko {
void DekoBackend::emitUploads(const std::vector<Upload> &pending) {
    for (const auto &upload : pending) {
        auto w = upload.width, h = upload.height;
        uint32_t offset = 0;
        for (uint32_t level = 0; level < upload.levels; ++level) {
            auto view = upload.texture->image->view();
            view.mipLevelOffset = level; view.mipLevelCount = 1;
            const auto packed = packedImageLayout(w, h);
            const DkCopyBuf source{upload.staging->gpu() + offset, packed.rowBytes, packed.layerBytes};
            const DkImageRect region{0, 0, 0, w, h, 1};
            dkCmdBufCopyBufferToImage(commands, &source, &view, &region, 0);
            offset += w * h * 4;
            w = std::max(1u, w / 2); h = std::max(1u, h / 2);
        }
    }
    dkCmdBufBarrier(commands, DkBarrier_Full,
        DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
}
void DekoBackend::bindTexture(const Texture &texture,uint8_t clamp){
    const bool shared=texture.slot!=DescriptorSlots::None;
    const auto set=shared?imageDescriptors->storage->gpu():texture.descriptor->gpu();
    if(boundImages!=set){
        dkCmdBufBindImageDescriptorSet(commands,set,shared?ImageDescriptorSlots:1);
        boundImages=set;boundHandleValid=false;++lastDescriptorSetBinds;
    }
    const auto handle=dkMakeTextureHandle(shared?texture.slot:0,clamp);
    if(!boundHandleValid || boundHandle!=handle){
        dkCmdBufBindTexture(commands,DkStage_Fragment,0,handle);
        boundHandle=handle;boundHandleValid=true;++lastTextureHandleBinds;
    }
}
void DekoBackend::configureTargets(int slot, bool clearToFog) {
    boundDepth=boundMask=boundBlend=-1;boundImages=0;boundHandleValid=false;boundFragment=0;
    lastDescriptorSetBinds=lastTextureHandleBinds=0;
    const auto colour = colors[slot]->view(), z = depth->view();
    dkCmdBufBindRenderTarget(commands, &colour, &z);
    const DkViewport viewport{0, 0, float(Width), float(Height), 0, 1};
    const DkScissor scissor{0, 0, Width, Height};
    dkCmdBufSetViewports(commands, 0, &viewport, 1);
    dkCmdBufSetScissors(commands, 0, &scissor, 1);
    DkRasterizerState rasterizer;
    dkRasterizerStateDefaults(&rasterizer); rasterizer.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(commands, &rasterizer);
    DkColorWriteState write;
    dkColorWriteStateDefaults(&write); dkCmdBufBindColorWriteState(commands, &write);
    // The GS frame starts from a FOGCOL sprite under the sky (GL: the background pass clears to the
    // fog colour, SOTC_BG_CLEAR_FOG). Frames without background layers keep the black clear.
    if(clearToFog)dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, fog.colour[0], fog.colour[1], fog.colour[2], 1);
    else dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    dkCmdBufClearDepthStencil(commands, true, 1, 0xFF, 0);
    const DkShader *shaders[]{&vertexShader->shader, &fragmentShader->shader};
    dkCmdBufBindShaders(commands, DkStageFlag_GraphicsMask, shaders, 2);
    const DkVtxAttribState attributes[]{
        {0, 0, offsetof(Vertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(Vertex, nx), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(Vertex, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(Vertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled, 0}
    };
    const DkVtxBufferState vertexBuffer{sizeof(Vertex), 0};
    dkCmdBufBindVtxAttribState(commands, attributes, 4);
    dkCmdBufBindVtxBufferState(commands, &vertexBuffer, 1);
    dkCmdBufBindUniformBuffer(commands, DkStage_Vertex, 0, uniforms->gpu(), 512);
    dkCmdBufBindUniformBuffer(commands, DkStage_Fragment, 0, uniforms->gpu(), 512);
    dkCmdBufBindSamplerDescriptorSet(commands, samplers->gpu(), 4);
}
Material DekoBackend::materialFor(const Draw &draw, bool failurePass) const {
    const auto &s = draw.state;
    Material result{};
    std::copy(draw.clip.begin(), draw.clip.end(), result.clip);
    std::copy_n(s.light, 16, result.directions);
    std::copy_n(s.light + 16, 16, result.colours);
    result.params[0] = s.opacity;
    result.params[1] = s.uvOffset[0]; result.params[2] = s.uvOffset[1];
    result.params[3] = s.screen;
    result.fog[0] = fog.base; result.fog[1] = fog.scale;
    result.fog[2] = s.background ? fog.skyNear : 0;
    result.fog[3] = fog.vertexFog && !s.screen && (!s.background || s.backgroundFge);
    std::copy_n(fog.colour, 3, result.fogColour);
    result.fogColour[3] = s.texture != 0;
    result.lod[0] = s.lodK; result.lod[1] = s.lodScale; result.lod[2] = s.maxLod;
    result.lod[3] = s.fresnel ? -1 : (s.lit ? 1 : 0);
    result.alpha[0] = s.alphaTest & 1 ? (s.alphaTest >> 1) & 7 : -1;
    result.alpha[1] = (s.alphaTest >> 4) & 255;
    result.alpha[2] = failurePass;
    if (s.debugColour) {
        result.debug[0] = ((s.debugColour >> 16) & 255) / 255.0f;
        result.debug[1] = ((s.debugColour >> 8) & 255) / 255.0f;
        result.debug[2] = (s.debugColour & 255) / 255.0f;
        result.debug[3] = 1;
    }
    return result;
}
void DekoBackend::emitDraw(const Draw &draw, bool failurePass, bool farBackdrop) {
    const auto &s = draw.state;
    if(!failurePass && unsigned(s.blend)<4){
        if(s.background)++lastBackgroundBlend[unsigned(s.blend)];
        else if(s.screen)++lastScreenBlend[unsigned(s.blend)];
    }
    if(diagSky && s.background){
        const bool sub=s.blend==Blend::Subtract,add=s.blend==Blend::Additive;
        if(diagSky==4 || (diagSky==1 && sub) || (diagSky==2 && add) || (diagSky==3 && !sub && !add))return;
    }
    const bool opaque=canUseOpaqueFragment(s,draw.mesh->minAlpha,draw.texture->minAlpha,failurePass);
    if(opaque)++lastOpaqueDraws;else {
        ++lastGenericDraws;
        if(s.screen || s.background)++lastGenericBackdrop;
        else if(s.fresnel)++lastGenericFresnel;
        else if(s.alphaTest&1)++lastGenericAlphaModes[(s.alphaTest>>1)&7];
        else if(s.alphaTest)++lastGenericDisabledAlpha;
        else ++lastGenericOther;
    }
    auto key=pipelineKey(s,failurePass);
    // Planned backdrops test (never write) against the pre-pass depth at the far plane.
    if(farBackdrop){key.depth=1;++lastPrepassBackdrops;}
    // Alpha-test failure pass (no depth write) of a draw whose passing pixels wrote depth: those
    // pixels hold this draw's exact depth (same vertex shader and matrix), and the failure shader
    // would discard them anyway. A strict Less test rejects them before shading instead of after.
    // Overlay draws rely on equal depth over earlier geometry and keep Lequal.
    if(failurePass && strictFailureDepth && !s.overlay && (key.depth&1) && !(key.depth&2) &&
       (pipelineKey(s,false).depth&2)){key.depth|=4;++lastStrictFailureDraws;}
    // Discarding shaders otherwise risk late depth testing. With depth writes off (and stencil
    // disabled in normal draws) an early test rejects exactly the fragments the late test would.
    const bool early=!opaque && (key.depth&1) && !(key.depth&2);
    if(early)++lastEarlyDraws;
    const int fragment=opaque?1:early?2:0;
    if(boundFragment!=fragment){
        const DkShader *selected=fragment==1?&opaqueFragmentShader->shader:fragment==2?&earlyFragmentShader->shader:&fragmentShader->shader;
        const DkShader *shaders[]{&vertexShader->shader,selected};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);boundFragment=fragment;
    }
    if(boundDepth!=key.depth){
        DkDepthStencilState depthState;dkDepthStencilStateDefaults(&depthState);
        depthState.depthTestEnable=(key.depth&1)!=0;
        depthState.depthWriteEnable=(key.depth&2)!=0;
        depthState.depthCompareOp=(key.depth&4)?DkCompareOp_Less:DkCompareOp_Lequal;
        dkCmdBufBindDepthStencilState(commands,&depthState);boundDepth=key.depth;
    }
    if(boundMask!=key.mask){
        DkColorWriteState write;dkColorWriteStateDefaults(&write);
        dkColorWriteStateSetMask(&write,0,key.mask);
        dkCmdBufBindColorWriteState(commands,&write);boundMask=key.mask;
    }
    // Diagnosis mode 5 changes the blend op for background Subtract only: key it separately.
    const int blendKey=int(s.blend)|((diagSky==5 && s.background)?8:0);
    if(boundBlend!=blendKey){
        DkColorState color;dkColorStateDefaults(&color);
        color.blendEnableMask=s.blend!=Blend::Opaque;
        dkCmdBufBindColorState(commands,&color);
        DkBlendState blend;dkBlendStateDefaults(&blend);
        if(s.blend==Blend::Additive || s.blend==Blend::Subtract)blend.dstColorBlendFactor=DkBlendFactor_One;
        if(s.blend==Blend::Subtract && !(diagSky==5 && s.background))blend.colorBlendOp=DkBlendOp_RevSub;
        dkCmdBufBindBlendState(commands,0,&blend);boundBlend=blendKey;
    }
    auto material = materialFor(draw, failurePass);
    material.alpha[3]=farBackdrop;
    dkCmdBufPushConstants(commands, uniforms->gpu(), 512, 0, sizeof(material), &material);
    bindTexture(*draw.texture,s.clamp);
    submitGeometry(*draw.mesh);
}
void DekoBackend::submitGeometry(const Mesh &mesh) {
    dkCmdBufBindVtxBuffer(commands, 0, mesh.vertices->gpu()+mesh.vertexOffset, mesh.count * sizeof(Vertex));
    if (mesh.indices) {
        dkCmdBufBindIdxBuffer(commands, DkIdxFormat_Uint32, mesh.indices->gpu()+mesh.indexOffset);
        dkCmdBufDrawIndexed(commands, DkPrimitive_Triangles, mesh.indexCount, 1, 0, 0, 0);
    } else {
        dkCmdBufDraw(commands, DkPrimitive_Triangles, mesh.count, 1, 0, 0);
    }
}
// Same vertex shader and clip matrix as the later colour draw, so its Lequal test sees
// bit-identical depth. Colour writes are masked; blend state is irrelevant.
void DekoBackend::emitDepthOnly(const Draw &draw) {
    ++lastPrepassDraws;
    if(boundFragment!=3){
        const DkShader *shaders[]{&vertexShader->shader,&depthFragmentShader->shader};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);boundFragment=3;
    }
    if(boundDepth!=3){
        DkDepthStencilState depthState;dkDepthStencilStateDefaults(&depthState);
        depthState.depthTestEnable=true;depthState.depthWriteEnable=true;
        depthState.depthCompareOp=DkCompareOp_Lequal;
        dkCmdBufBindDepthStencilState(commands,&depthState);boundDepth=3;
    }
    if(boundMask!=0){
        DkColorWriteState write;dkColorWriteStateDefaults(&write);
        dkColorWriteStateSetMask(&write,0,0);
        dkCmdBufBindColorWriteState(commands,&write);boundMask=0;
    }
    const auto material=materialFor(draw,false);
    dkCmdBufPushConstants(commands,uniforms->gpu(),512,0,sizeof(material),&material);
    submitGeometry(*draw.mesh);
}
void DekoBackend::restoreShadowState() {
    const DkShader *normalShaders[]{&vertexShader->shader,&fragmentShader->shader};
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,normalShaders,2);
    boundFragment=0;
    DkRasterizerState rasterizer;dkRasterizerStateDefaults(&rasterizer);
    rasterizer.cullMode=DkFace_None;
    dkCmdBufBindRasterizerState(commands,&rasterizer);
    // emitDraw rebinds depth/stencil, colour mask, blend and image/sampler state.
    boundDepth=boundMask=boundBlend=-1;boundImages=0;boundHandleValid=false;
}
void DekoBackend::emitShadowVolume(const Draw &draw) {
    ++lastShadowDraws;
    const DkShader *volumeShaders[]{&shadowVertexShader->shader,&shadowFragmentShader->shader};
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,volumeShaders,2);
    DkRasterizerState rasterizer;dkRasterizerStateDefaults(&rasterizer);
    rasterizer.cullMode=DkFace_None;rasterizer.depthClampEnable=true;
    dkCmdBufBindRasterizerState(commands,&rasterizer);
    DkDepthStencilState state;dkDepthStencilStateDefaults(&state);
    state.depthTestEnable=true;state.depthWriteEnable=false;state.depthCompareOp=DkCompareOp_Lequal;
    state.stencilTestEnable=true;
    state.stencilFrontPassOp=DkStencilOp_IncrWrap;state.stencilBackPassOp=DkStencilOp_DecrWrap;
    state.stencilFrontFailOp=state.stencilBackFailOp=DkStencilOp_Keep;
    state.stencilFrontDepthFailOp=state.stencilBackDepthFailOp=DkStencilOp_Keep;
    dkCmdBufBindDepthStencilState(commands,&state);
    dkCmdBufSetStencil(commands,DkFace_FrontAndBack,0xFF,0,0xFF);
    DkColorWriteState write;dkColorWriteStateDefaults(&write);dkColorWriteStateSetMask(&write,0,0);
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colour;dkColorStateDefaults(&colour);colour.blendEnableMask=0;
    dkCmdBufBindColorState(commands,&colour);
    Material material{};std::copy(draw.clip.begin(),draw.clip.end(),material.clip);
    material.params[0]=1;material.alpha[0]=-1;
    dkCmdBufPushConstants(commands,uniforms->gpu(),512,0,sizeof(material),&material);
    bindTexture(*dummyTexture,0);
    dkCmdBufBindVtxBuffer(commands,0,draw.mesh->vertices->gpu()+draw.mesh->vertexOffset,draw.mesh->count*sizeof(Vertex));
    if(draw.mesh->indices) {
        dkCmdBufBindIdxBuffer(commands,DkIdxFormat_Uint32,draw.mesh->indices->gpu()+draw.mesh->indexOffset);
        dkCmdBufDrawIndexed(commands,DkPrimitive_Triangles,draw.mesh->indexCount,1,0,0,0);
    }else dkCmdBufDraw(commands,DkPrimitive_Triangles,draw.mesh->count,1,0,0);
    restoreShadowState();
}
void DekoBackend::resolveShadows(float amount) {
    if(boundFragment!=0){
        const DkShader *shaders[]{&vertexShader->shader,&fragmentShader->shader};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);boundFragment=0;
    }
    DkDepthStencilState state;dkDepthStencilStateDefaults(&state);
    state.depthTestEnable=false;state.depthWriteEnable=false;state.stencilTestEnable=true;
    state.stencilFrontCompareOp=state.stencilBackCompareOp=DkCompareOp_NotEqual;
    state.stencilFrontPassOp=state.stencilBackPassOp=DkStencilOp_Zero;
    dkCmdBufBindDepthStencilState(commands,&state);
    dkCmdBufSetStencil(commands,DkFace_FrontAndBack,0xFF,0,0xFF);
    DkColorWriteState write;dkColorWriteStateDefaults(&write);dkColorWriteStateSetMask(&write,0,7);
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colour;dkColorStateDefaults(&colour);colour.blendEnableMask=1;
    dkCmdBufBindColorState(commands,&colour);
    DkBlendState blend;dkBlendStateDefaults(&blend);
    dkBlendStateSetFactors(&blend,DkBlendFactor_Zero,DkBlendFactor_InvConstAlpha,DkBlendFactor_Zero,DkBlendFactor_One);
    dkCmdBufBindBlendState(commands,0,&blend);dkCmdBufSetBlendConst(commands,0,0,0,amount);
    Material material{};for(unsigned i=0;i<4;++i)material.clip[i*5]=1;
    material.params[0]=1;material.alpha[0]=-1;material.debug[3]=1;
    // Keep shader alpha opaque: even amount=0 must clear the counted stencil.
    dkCmdBufPushConstants(commands,uniforms->gpu(),512,0,sizeof(material),&material);
    bindTexture(*dummyTexture,0);
    dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    restoreShadowState();
}
void DekoBackend::endFrame(uint32_t width, uint32_t height) {
    const auto cpuStart=armTicksToNs(armGetSystemTick());
    if (!recording) throw std::logic_error("deko3d: endFrame outside frame");
    if (width != Width || height != Height)
        throw std::invalid_argument("deko3d: scene must be native 1280x720");
    const bool diagnosticCapture=diagnosticCaptureFrame && submittedFrames+1==diagnosticCaptureFrame && !held;
    if(diagnosticCapture)captureRequested=true;
    auto &frame = frames[frameIndex];
    startCommands(frame);
    frame.draws = std::move(draws); frame.uploads = std::move(uploads);
    resolvePendingMeshes(frame.draws,meshes);
    if(!held){
        lastOpaqueDraws=lastGenericDraws=lastShadowDraws=lastEarlyDraws=lastPrepassDraws=lastPrepassBackdrops=lastStrictFailureDraws=0;
        diagSky=diagSkyRequested && std::chrono::steady_clock::now()>=diagSkyFrom?diagSkyRequested:0;
        lastGenericAlphaModes.fill(0);
        lastGenericBackdrop=lastGenericFresnel=lastGenericDisabledAlpha=lastGenericOther=0;
        std::fill(std::begin(lastBackgroundBlend),std::end(lastBackgroundBlend),0u);
        std::fill(std::begin(lastScreenBlend),std::end(lastScreenBlend),0u);
        if(!frame.gpuReports)frame.gpuReports=std::make_shared<Buffer>(device,128,allocationBudget,bufferPool);
        std::memset(frame.gpuReports->cpu(),0,128);
        // Bottom-of-pipeline reports exclude work belonging to the preceding frame.
        dkCmdBufReportCounter(commands,DkCounter_Timestamp,frame.gpuReports->gpu());
        frame.gpuTimingFrame=submittedFrames+1;frame.gpuTimingPending=true;
        frame.gpuCountersPending=(++presentedNativeFrames%30)==0;
        frame.gpuCounterFrame=frame.gpuCountersPending?frame.gpuTimingFrame:0;
    }
    segments.active=frame.gpuCountersPending;segments.begin();
    if(segments.active){
        if(!frame.segmentReports)frame.segmentReports=std::make_shared<Buffer>(device,MaxGpuSegments*16,allocationBudget,bufferPool);
        std::memset(frame.segmentReports->cpu(),0,MaxGpuSegments*16);
        segmentBase=frame.segmentReports->gpu();
    }
    const auto uploadStart=armTicksToNs(armGetSystemTick());
    markRender(RenderPhase::Uploads);
    emitUploads(frame.uploads);
    const auto uploadNs=armTicksToNs(armGetSystemTick())-uploadStart;
    uint64_t acquireNs=0,recordNs=0;
    int slot = -1;
    if (!held) {
        const auto acquireStart=armTicksToNs(armGetSystemTick());
        markRender(RenderPhase::Acquire);
        slot = dkQueueAcquireImage(queue, swapchain);
        if (slot < 0 || slot >= int(Frames)) throw std::runtime_error("deko3d: acquire failed");
        bool clearToFog=false;
        if(backgroundClearFog)for(const auto &draw:frame.draws)if(draw.state.background){clearToFog=true;break;}
        lastClearToFog=clearToFog;
        configureTargets(slot,clearToFog);
        acquireNs=armTicksToNs(armGetSystemTick())-acquireStart;
        if(frame.gpuCountersPending){
            dkCmdBufReportCounter(commands,DkCounter_VertexShaderInvocations,frame.gpuReports->gpu()+32);
            dkCmdBufReportCounter(commands,DkCounter_FragmentShaderInvocations,frame.gpuReports->gpu()+64);
            dkCmdBufReportCounter(commands,DkCounter_InputPrimitives,frame.gpuReports->gpu()+96);
        }
        const auto recordStart=armTicksToNs(armGetSystemTick());
        markRender(RenderPhase::Record);
        std::stable_sort(frame.draws.begin(), frame.draws.end(),
            [](const Draw &a, const Draw &b) { return a.state.order < b.state.order; });
        PrepassPlan prepass;
        if(prepassEnabled && !diagSky){
            prepass=planPrepass(frame.draws.size(),[&](std::size_t i)->const DrawState&{return frame.draws[i].state;},
                [&](std::size_t i,uint8_t &va,uint8_t &ta){va=frame.draws[i].mesh->minAlpha;ta=frame.draws[i].texture->minAlpha;},prepassFlags);
            if(prepass.active)
                for(std::size_t i=prepass.first;i<prepass.barrier;++i)if(prepassFlags[i]){markSegment(GpuSegment::Prepass);emitDepthOnly(frame.draws[i]);}
        }
        ShadowBatch shadows;
        // Coverage attribution is CPU-only bookkeeping on counter-sampled frames; the GPU
        // command stream is identical whether or not it runs.
        CoverageSample coverage;const bool attributeCoverage=frame.gpuCountersPending;
        if(attributeCoverage)coverage.frame=frame.gpuCounterFrame;
        for (std::size_t drawIndex=0;drawIndex<frame.draws.size();++drawIndex) {
            const auto &draw=frame.draws[drawIndex];
            if(shadows.resolveBefore(draw.state.order)){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount);shadows.clear();}
            const float area=attributeCoverage?screenCoverage(draw.clip.data(),draw.mesh->bounds,draw.state.screen):0;
            if(attributeCoverage && area>=0.9f)++coverage.fullScreenDraws;
            if(draw.state.shadowVolume){
                markSegment(GpuSegment::Shadow);
                emitShadowVolume(draw);shadows.add(draw.state.order,draw.state.shadow);
                if(attributeCoverage)coverage.shadow+=area;
                continue;
            }
            const bool failure=needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha);
            if(attributeCoverage){
                if(draw.state.screen || draw.state.background)coverage.backdrop+=area;
                else if(canUseOpaqueFragment(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha,false))coverage.opaque+=area;
                else coverage.generic+=area;
                if(failure)coverage.failure+=area;
            }
            markSegment(draw.state.background?GpuSegment::Backdrop:draw.state.screen?GpuSegment::Screen:
                canUseOpaqueFragment(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha,false)?GpuSegment::Opaque:GpuSegment::Generic);
            emitDraw(draw,false,prepass.farBackdrop(drawIndex,draw.state));
            if (failure) {markSegment(GpuSegment::Failure);emitDraw(draw, true);}
        }
        if(attributeCoverage)lastCoverage=coverage;
        if(shadows.pending){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount);}
        if(const int slot=segments.close();slot>=0)
            dkCmdBufReportCounter(commands,DkCounter_Timestamp,segmentBase+DkGpuAddr(slot)*16);
        frame.segmentsPending=segments.active && segments.count>=2;
        if(frame.segmentsPending){frame.segmentKinds=segments.kinds;frame.segmentCount=segments.count;}
        segments.active=false;
        if(frame.gpuCountersPending){
            dkCmdBufReportCounter(commands,DkCounter_VertexShaderInvocations,frame.gpuReports->gpu()+48);
            dkCmdBufReportCounter(commands,DkCounter_FragmentShaderInvocations,frame.gpuReports->gpu()+80);
            dkCmdBufReportCounter(commands,DkCounter_InputPrimitives,frame.gpuReports->gpu()+112);
        }
        dkCmdBufReportCounter(commands,DkCounter_Timestamp,frame.gpuReports->gpu()+16);
        recordNs=armTicksToNs(armGetSystemTick())-recordStart;
        if (captureRequested) {
            frame.capture = std::make_shared<Buffer>(device, Width * Height * 4, allocationBudget);
            const auto view = colors[slot]->view();
            const DkImageRect region{0, 0, 0, Width, Height, 1};
            const auto packed = packedImageLayout(Width, Height);
            const DkCopyBuf destination{frame.capture->gpu(), packed.rowBytes, packed.layerBytes};
            dkCmdBufBarrier(commands, DkBarrier_Full, 0);
            dkCmdBufCopyImageToBuffer(commands, &view, &region, &destination, 0);
            latestCapture = frame.capture; captureFrameIndex = frameIndex;
            captureRequested = false;
        }
    }
    const auto submitStart=armTicksToNs(armGetSystemTick());
    markRender(RenderPhase::Submit);
    dkCmdBufSignalFence(commands, &frame.fence, true);
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(commands));
    frame.pending = true;
    if (!held) {markRender(RenderPhase::Present);dkQueuePresentImage(queue, swapchain, slot);latestSlot=slot;
        if(celosia_frame_presented)celosia_frame_presented();}
    markRender(RenderPhase::Flush);
    dkQueueFlush(queue);
    const auto submitEnd=armTicksToNs(armGetSystemTick());
    activeFrame = nullptr;
    markRender(RenderPhase::Idle);
    frameIndex = (frameIndex + 1) % Frames; recording = false;
    ++submittedFrames;
    // Count submitted presentations, excluding held/repeated game frames.
    // Close each small status file so FTP can read it during execution.
    const auto sampleEndNs=armTicksToNs(armGetSystemTick());
    if(const auto sample=presentationRate.record(sampleEndNs,!held)){
        auto *file=std::fopen("sdmc:/switch/celosia-native-boot/hardware-status.txt","w");
        if(file){
            const bool written=std::fprintf(file,"native_width=1280\nnative_height=720\npresent_fps=%.3f\npresented_frames=%llu\ninterval_ns=%llu\nlast_draws=%zu\ngpu_bytes=%zu\n",
                sample->fps,static_cast<unsigned long long>(sample->frames),static_cast<unsigned long long>(sample->elapsedNs),frame.draws.size(),allocationBudget->used())>0;
            // Per-presentation minimums for this window (CPU submission cadence): worst gap and mean of the slowest 1%.
            const bool lows=std::fprintf(file,"frame_samples=%u\nworst_frame_ms=%.3f\nlow1_frame_ms=%.3f\nlow1_fps=%.3f\n",
                sample->intervalCount,sample->worstFrameNs/1000000.0,sample->low1FrameNs/1000000.0,sample->low1Fps())>0;
            const bool timings=std::fprintf(file,"sample_end_ns=%llu\ntotal_native_frames=%u\nlast_begin_wait_ms=%.3f\nlast_upload_ms=%.3f\nlast_acquire_ms=%.3f\nlast_record_ms=%.3f\nlast_submit_ms=%.3f\nlast_end_ms=%.3f\n",
                static_cast<unsigned long long>(sampleEndNs),submittedFrames,lastBeginWaitNs/1000000.0,uploadNs/1000000.0,acquireNs/1000000.0,recordNs/1000000.0,
                (submitEnd-submitStart)/1000000.0,(submitEnd-cpuStart)/1000000.0)>0;
            {
                std::fprintf(file,"gpu_segments=%s\ngpu_segment_frames=%llu\ngpu_segment_ms=",gpuSegmentNames,
                    static_cast<unsigned long long>(segmentWindow.frames));
                for(unsigned k=0;k<GpuSegmentCount;++k)std::fprintf(file,k?",%.3f":"%.3f",segmentWindow.averageMs(k));
                std::fputc('\n',file);
                segmentWindow.reset();
            }
            const bool gpuTimings=std::fprintf(file,"gpu_timing_frame=%u\nlast_gpu_ms=%.3f\ngpu_samples=%llu\ngpu_average_ms=%.3f\ngpu_peak_ms=%.3f\n",
                lastGpuTimingFrame,lastGpuNs/1000000.0,static_cast<unsigned long long>(gpuTimeWindow.count),gpuTimeWindow.averageNs()/1000000.0,gpuTimeWindow.peakNs/1000000.0)>0;
            const bool counterStats=std::fprintf(file,"last_opaque_draws=%u\nlast_generic_draws=%u\nlast_early_draws=%u\nlast_prepass_draws=%u\nlast_prepass_backdrops=%u\nprepass_enabled=%d\nlast_strict_failure_draws=%u\nlast_shadow_draws=%u\ncounter_samples=%llu\ncounter_invalid_samples=%llu\ncounter_last_frame=%u\ncounter_avg_vertices=%.3f\ncounter_avg_fragments=%.3f\ncounter_avg_primitives=%.3f\ncounter_fragments_per_native_pixel_estimate=%.6f\ncounter_zero_policy=unproven_or_unsupported\n",
                lastOpaqueDraws,lastGenericDraws,lastEarlyDraws,lastPrepassDraws,lastPrepassBackdrops,int(prepassEnabled),lastStrictFailureDraws,lastShadowDraws,static_cast<unsigned long long>(gpuCounterWindow.count),
                static_cast<unsigned long long>(gpuCounterWindow.rejected),lastGpuCounterFrame,gpuCounterWindow.averageVertices(),
                gpuCounterWindow.averageFragments(),gpuCounterWindow.averagePrimitives(),gpuCounterWindow.fragmentInvocationsPerPixel(uint64_t(Width)*Height))>0;
            const bool alphaStats=std::fprintf(file,
                "generic_backdrop_draws=%u\ngeneric_fresnel_draws=%u\ngeneric_disabled_alpha_draws=%u\ngeneric_other_draws=%u\ngeneric_alpha_modes=%u,%u,%u,%u,%u,%u,%u,%u\n",
                lastGenericBackdrop,lastGenericFresnel,lastGenericDisabledAlpha,lastGenericOther,lastGenericAlphaModes[0],lastGenericAlphaModes[1],
                lastGenericAlphaModes[2],lastGenericAlphaModes[3],lastGenericAlphaModes[4],lastGenericAlphaModes[5],lastGenericAlphaModes[6],lastGenericAlphaModes[7])>0;
            const bool skyStats=std::fprintf(file,"background_blend=%u,%u,%u,%u\nscreen_blend=%u,%u,%u,%u\ndiag_sky=%d\nclear_to_fog=%d\n",
                lastBackgroundBlend[0],lastBackgroundBlend[1],lastBackgroundBlend[2],lastBackgroundBlend[3],
                lastScreenBlend[0],lastScreenBlend[1],lastScreenBlend[2],lastScreenBlend[3],diagSky,int(lastClearToFog))>0;
            const bool descriptorStats=std::fprintf(file,"descriptor_set_binds=%u\ntexture_handle_binds=%u\ndescriptor_slots_in_use=%u\ndescriptor_private_textures=%u\n",
                lastDescriptorSetBinds,lastTextureHandleBinds,imageDescriptors->slots.inUse(),privateDescriptorTextures)>0;
            const bool coverageStats=std::fprintf(file,
                "coverage_frame=%u\ncoverage_opaque=%.3f\ncoverage_generic=%.3f\ncoverage_failure=%.3f\ncoverage_backdrop=%.3f\ncoverage_shadow=%.3f\ncoverage_fullscreen_draws=%u\ncoverage_basis=projected_bounds_upper_bound_screen_multiples\n",
                lastCoverage.frame,lastCoverage.opaque,lastCoverage.generic,lastCoverage.failure,lastCoverage.backdrop,lastCoverage.shadow,lastCoverage.fullScreenDraws)>0;
            const bool flushed=std::fflush(file)==0 && ::fsync(::fileno(file))==0;
            if(std::fclose(file)!=0 || !written || !lows || !timings || !gpuTimings || !counterStats || !alphaStats || !descriptorStats || !coverageStats || !flushed)std::fputs("[hardware] status write failed\n",stderr);
        }else std::fputs("[hardware] status file unavailable\n",stderr);
        gpuTimeWindow.reset();
        gpuCounterWindow.reset();
    }
    if(diagnosticCapture) {
        std::vector<uint8_t> rgba;uint32_t w=0,h=0;
        if(!latestFrame(rgba,w,h))throw std::runtime_error("deko3d: diagnostic capture missing");
        char capturePath[128];
        std::snprintf(capturePath,sizeof(capturePath),"sdmc:/switch/celosia-native-boot/native-frame-%06u.ppm",submittedFrames);
        auto *file=std::fopen(capturePath,"wb");
        if(!file)throw std::runtime_error("deko3d: diagnostic capture file unavailable");
        bool okay=std::fprintf(file,"P6\n%u %u\n255\n",w,h)>0;
        std::vector<uint8_t> rgb(size_t(w)*h*3);
        for(size_t i=0;i<size_t(w)*h;++i)std::copy_n(rgba.data()+i*4,3,rgb.data()+i*3);
        okay=std::fwrite(rgb.data(),1,rgb.size(),file)==rgb.size() && okay;
        okay=std::fclose(file)==0 && okay;
        if(!okay)throw std::runtime_error("deko3d: diagnostic capture write failed");
        std::fprintf(stderr,"[deko3d] captured native frame %u: %ux%u, %zu draws\n",submittedFrames,w,h,frame.draws.size());
        diagnosticCaptureFrame=diagnosticCaptureInterval && submittedFrames<=1000000-diagnosticCaptureInterval
            ?submittedFrames+diagnosticCaptureInterval:0;
    }
    for (auto it = meshes.begin(); it != meshes.end();) {
        if (it->second->transient) it = meshes.erase(it); else ++it;
    }
}
bool DekoBackend::latestFrame(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    if (!latestCapture) return false;
    if (recording) throw std::logic_error("deko3d: capture during frame recording");
    // No commands access an image that has already been handed to the compositor.
    // A reused frame fence is safe too: waiting for newer queue work also orders
    // the earlier capture. The immutable readback allocation is retained separately.
    if (dkFenceWait(&frames[captureFrameIndex].fence, -1) != DkResult_Success)
        throw std::runtime_error("deko3d: readback fence wait failed");
    const auto *bytes = static_cast<const uint8_t *>(latestCapture->cpu());
    rgba.assign(bytes, bytes + Width * Height * 4); w = Width; h = Height;
    return true;
}
}
