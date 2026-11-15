#include "backend.hpp"
#include "render_progress.hpp"
#include "late_mesh.hpp"
#include "opaque_policy.hpp"
#include <cstddef>
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
void DekoBackend::configureTargets(int slot) {
    boundDepth=boundMask=boundBlend=boundClamp=-1;boundImages=0;boundOpaque=false;
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
    dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, 0, 0, 0, 1);
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
void DekoBackend::emitDraw(const Draw &draw, bool failurePass) {
    const auto &s = draw.state;
    const bool opaque=canUseOpaqueFragment(s,draw.mesh->minAlpha,draw.texture->minAlpha,failurePass);
    if(opaque)++lastOpaqueDraws;else {
        ++lastGenericDraws;
        if(s.screen || s.background)++lastGenericBackdrop;
        else if(s.fresnel)++lastGenericFresnel;
        else if(s.alphaTest&1)++lastGenericAlphaModes[(s.alphaTest>>1)&7];
        else if(s.alphaTest)++lastGenericDisabledAlpha;
        else ++lastGenericOther;
    }
    if(boundOpaque!=opaque){
        const DkShader *shaders[]{&vertexShader->shader,opaque?&opaqueFragmentShader->shader:&fragmentShader->shader};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);boundOpaque=opaque;
    }
    const auto key=pipelineKey(s,failurePass);
    if(boundDepth!=key.depth){
        DkDepthStencilState depthState;dkDepthStencilStateDefaults(&depthState);
        depthState.depthTestEnable=(key.depth&1)!=0;
        depthState.depthWriteEnable=(key.depth&2)!=0;
        depthState.depthCompareOp=DkCompareOp_Lequal;
        dkCmdBufBindDepthStencilState(commands,&depthState);boundDepth=key.depth;
    }
    if(boundMask!=key.mask){
        DkColorWriteState write;dkColorWriteStateDefaults(&write);
        dkColorWriteStateSetMask(&write,0,key.mask);
        dkCmdBufBindColorWriteState(commands,&write);boundMask=key.mask;
    }
    if(boundBlend!=int(s.blend)){
        DkColorState color;dkColorStateDefaults(&color);
        color.blendEnableMask=s.blend!=Blend::Opaque;
        dkCmdBufBindColorState(commands,&color);
        DkBlendState blend;dkBlendStateDefaults(&blend);
        if(s.blend==Blend::Additive || s.blend==Blend::Subtract)blend.dstColorBlendFactor=DkBlendFactor_One;
        if(s.blend==Blend::Subtract)blend.colorBlendOp=DkBlendOp_RevSub;
        dkCmdBufBindBlendState(commands,0,&blend);boundBlend=int(s.blend);
    }
    const auto material = materialFor(draw, failurePass);
    dkCmdBufPushConstants(commands, uniforms->gpu(), 512, 0, sizeof(material), &material);
    const auto images=draw.texture->descriptor->gpu();
    if(boundImages!=images){dkCmdBufBindImageDescriptorSet(commands,images,1);boundImages=images;}
    if(boundClamp!=int(s.clamp)){
        dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(0,s.clamp));boundClamp=int(s.clamp);
    }
    dkCmdBufBindVtxBuffer(commands, 0, draw.mesh->vertices->gpu()+draw.mesh->vertexOffset,
        draw.mesh->count * sizeof(Vertex));
    if (draw.mesh->indices) {
        dkCmdBufBindIdxBuffer(commands, DkIdxFormat_Uint32, draw.mesh->indices->gpu()+draw.mesh->indexOffset);
        dkCmdBufDrawIndexed(commands, DkPrimitive_Triangles, draw.mesh->indexCount, 1, 0, 0, 0);
    } else {
        dkCmdBufDraw(commands, DkPrimitive_Triangles, draw.mesh->count, 1, 0, 0);
    }
}
void DekoBackend::restoreShadowState() {
    const DkShader *normalShaders[]{&vertexShader->shader,&fragmentShader->shader};
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,normalShaders,2);
    boundOpaque=false;
    DkRasterizerState rasterizer;dkRasterizerStateDefaults(&rasterizer);
    rasterizer.cullMode=DkFace_None;
    dkCmdBufBindRasterizerState(commands,&rasterizer);
    // emitDraw rebinds depth/stencil, colour mask, blend and image/sampler state.
    boundDepth=boundMask=boundBlend=boundClamp=-1;boundImages=0;
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
    dkCmdBufBindImageDescriptorSet(commands,dummyTexture->descriptor->gpu(),1);
    dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(0,0));
    dkCmdBufBindVtxBuffer(commands,0,draw.mesh->vertices->gpu()+draw.mesh->vertexOffset,draw.mesh->count*sizeof(Vertex));
    if(draw.mesh->indices) {
        dkCmdBufBindIdxBuffer(commands,DkIdxFormat_Uint32,draw.mesh->indices->gpu()+draw.mesh->indexOffset);
        dkCmdBufDrawIndexed(commands,DkPrimitive_Triangles,draw.mesh->indexCount,1,0,0,0);
    }else dkCmdBufDraw(commands,DkPrimitive_Triangles,draw.mesh->count,1,0,0);
    restoreShadowState();
}
void DekoBackend::resolveShadows(float amount) {
    if(boundOpaque){
        const DkShader *shaders[]{&vertexShader->shader,&fragmentShader->shader};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);boundOpaque=false;
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
    dkCmdBufBindImageDescriptorSet(commands,dummyTexture->descriptor->gpu(),1);
    dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(0,0));
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
        lastOpaqueDraws=lastGenericDraws=lastShadowDraws=0;
        lastGenericAlphaModes.fill(0);
        lastGenericBackdrop=lastGenericFresnel=lastGenericDisabledAlpha=lastGenericOther=0;
        if(!frame.gpuReports)frame.gpuReports=std::make_shared<Buffer>(device,128,allocationBudget,bufferPool);
        std::memset(frame.gpuReports->cpu(),0,128);
        // Bottom-of-pipeline reports exclude work belonging to the preceding frame.
        dkCmdBufReportCounter(commands,DkCounter_Timestamp,frame.gpuReports->gpu());
        frame.gpuTimingFrame=submittedFrames+1;frame.gpuTimingPending=true;
        frame.gpuCountersPending=(++presentedNativeFrames%30)==0;
        frame.gpuCounterFrame=frame.gpuCountersPending?frame.gpuTimingFrame:0;
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
        configureTargets(slot);
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
        ShadowBatch shadows;
        for (const auto &draw : frame.draws) {
            if(shadows.resolveBefore(draw.state.order)){resolveShadows(shadows.amount);shadows.clear();}
            if(draw.state.shadowVolume){emitShadowVolume(draw);shadows.add(draw.state.order,draw.state.shadow);continue;}
            emitDraw(draw);
            if (needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha))
                emitDraw(draw, true);
        }
        if(shadows.pending)resolveShadows(shadows.amount);
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
    if (!held) {markRender(RenderPhase::Present);dkQueuePresentImage(queue, swapchain, slot);latestSlot=slot;}
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
            const bool timings=std::fprintf(file,"sample_end_ns=%llu\ntotal_native_frames=%u\nlast_begin_wait_ms=%.3f\nlast_upload_ms=%.3f\nlast_acquire_ms=%.3f\nlast_record_ms=%.3f\nlast_submit_ms=%.3f\nlast_end_ms=%.3f\n",
                static_cast<unsigned long long>(sampleEndNs),submittedFrames,lastBeginWaitNs/1000000.0,uploadNs/1000000.0,acquireNs/1000000.0,recordNs/1000000.0,
                (submitEnd-submitStart)/1000000.0,(submitEnd-cpuStart)/1000000.0)>0;
            const bool gpuTimings=std::fprintf(file,"gpu_timing_frame=%u\nlast_gpu_ms=%.3f\ngpu_samples=%llu\ngpu_average_ms=%.3f\ngpu_peak_ms=%.3f\n",
                lastGpuTimingFrame,lastGpuNs/1000000.0,static_cast<unsigned long long>(gpuTimeWindow.count),gpuTimeWindow.averageNs()/1000000.0,gpuTimeWindow.peakNs/1000000.0)>0;
            const bool counterStats=std::fprintf(file,"last_opaque_draws=%u\nlast_generic_draws=%u\nlast_shadow_draws=%u\ncounter_samples=%llu\ncounter_invalid_samples=%llu\ncounter_last_frame=%u\ncounter_avg_vertices=%.3f\ncounter_avg_fragments=%.3f\ncounter_avg_primitives=%.3f\ncounter_fragments_per_native_pixel_estimate=%.6f\ncounter_zero_policy=unproven_or_unsupported\n",
                lastOpaqueDraws,lastGenericDraws,lastShadowDraws,static_cast<unsigned long long>(gpuCounterWindow.count),
                static_cast<unsigned long long>(gpuCounterWindow.rejected),lastGpuCounterFrame,gpuCounterWindow.averageVertices(),
                gpuCounterWindow.averageFragments(),gpuCounterWindow.averagePrimitives(),gpuCounterWindow.fragmentInvocationsPerPixel(uint64_t(Width)*Height))>0;
            const bool alphaStats=std::fprintf(file,
                "generic_backdrop_draws=%u\ngeneric_fresnel_draws=%u\ngeneric_disabled_alpha_draws=%u\ngeneric_other_draws=%u\ngeneric_alpha_modes=%u,%u,%u,%u,%u,%u,%u,%u\n",
                lastGenericBackdrop,lastGenericFresnel,lastGenericDisabledAlpha,lastGenericOther,lastGenericAlphaModes[0],lastGenericAlphaModes[1],
                lastGenericAlphaModes[2],lastGenericAlphaModes[3],lastGenericAlphaModes[4],lastGenericAlphaModes[5],lastGenericAlphaModes[6],lastGenericAlphaModes[7])>0;
            const bool flushed=std::fflush(file)==0 && ::fsync(::fileno(file))==0;
            if(std::fclose(file)!=0 || !written || !timings || !gpuTimings || !counterStats || !alphaStats || !flushed)std::fputs("[hardware] status write failed\n",stderr);
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
