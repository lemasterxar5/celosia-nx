#include "backend.hpp"
#include "../../../../platform/switch/hybrid/live_settings.hpp"
#include "render_progress.hpp"
#include "late_mesh.hpp"
#include "opaque_policy.hpp"
#include "prepass_policy.hpp"
// Optional present observer (platform/switch/hybrid/saltynx_fps.cpp: SaltyNX FPS counter block).
extern "C" void celosia_frame_presented(void) __attribute__((weak));
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <cstddef>
#include <iterator>
#include <unistd.h>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <switch.h>
// v106: status files are formatted in memory on the render thread and written to the SD by this
// writer thread (an fopen + fsync on the SD took tens of ms of the render thread every 5 s: a
// visible hitch in play). The writer also appends the clocks read at that moment to present-times.
namespace {
struct SdWriter {
    struct Job {std::string path;bool append=false;bool clocks=false;std::string text;};
    std::mutex lock;std::condition_variable wake;std::vector<Job> jobs;bool started=false,failed=false;Thread thread{};
    void post(Job job){
        std::lock_guard<std::mutex> guard(lock);
        if(failed)return;
        for(auto &queued:jobs)if(!queued.append && !job.append && queued.path==job.path){queued=std::move(job);wake.notify_one();return;}
        jobs.push_back(std::move(job));
        // libnx thread (std::thread terminated here: v97, v106). Low priority; it only waits on the SD.
        if(!started){started=true;
            if(R_FAILED(threadCreate(&thread,[](void *self){static_cast<SdWriter *>(self)->run();},this,nullptr,0x8000,0x3B,-2)) ||
               R_FAILED(threadStart(&thread))){std::fputs("[hardware] status writer thread unavailable\n",stderr);jobs.clear();started=false;failed=true;return;}
        }
        wake.notify_one();
    }
    static void appendClocks(std::string &text){
        static bool ready=R_SUCCEEDED(clkrstInitialize());
        u32 hz[2]{};
        const PcvModuleId ids[2]{PcvModuleId_CpuBus,PcvModuleId_GPU};
        for(int i=0;ready && i<2;++i){
            ClkrstSession session{};
            if(R_SUCCEEDED(clkrstOpenSession(&session,ids[i],3))){(void)clkrstGetClockRate(&session,&hz[i]);clkrstCloseSession(&session);}
        }
        char tail[64];std::snprintf(tail,sizeof(tail)," | clk %u %u",unsigned(hz[0]/1000000u),unsigned(hz[1]/1000000u));
        if(!text.empty() && text.back()=='\n')text.pop_back();
        text+=tail;text+='\n';
    }
    void run(){
        for(;;){
            std::vector<Job> batch;
            {std::unique_lock<std::mutex> guard(lock);wake.wait(guard,[this]{return !jobs.empty();});batch.swap(jobs);}
            for(auto &job:batch){
                if(job.clocks)appendClocks(job.text);
                auto *file=std::fopen(job.path.c_str(),job.append?"a":"w");
                if(!file){std::fputs("[hardware] status file unavailable\n",stderr);continue;}
                const bool okay=std::fwrite(job.text.data(),1,job.text.size(),file)==job.text.size();
                if(std::fclose(file)!=0 || !okay)std::fputs("[hardware] status write failed\n",stderr);
            }
        }
    }
};
SdWriter &sdWriter(){static SdWriter writer;return writer;}
}

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
void DekoBackend::bindTarget(const DkImage &colour,const DkImage *depthImage,uint32_t width,uint32_t height){
    DkImageView colourView;dkImageViewDefaults(&colourView,&colour);
    DkImageView depthView;if(depthImage)dkImageViewDefaults(&depthView,depthImage);
    dkCmdBufBindRenderTarget(commands,&colourView,depthImage?&depthView:nullptr);
    const DkViewport viewport{0,0,float(width),float(height),0,1};
    const DkScissor scissor{0,0,width,height};
    dkCmdBufSetViewports(commands,0,&viewport,1);
    dkCmdBufSetScissors(commands,0,&scissor,1);
}
// A full-target copy of a background image (shared descriptor `slot`, clamped, linear) with the
// sky shader: no depth, no blending. Leaves every cached pipeline binding invalid.
void DekoBackend::drawImageQuad(uint32_t slot,float invWidth,float invHeight){
    const DkShader *shaders[]{&vertexShader->shader,&skyFragmentShader->shader};
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);
    DkDepthStencilState depthState;dkDepthStencilStateDefaults(&depthState);
    depthState.depthTestEnable=false;depthState.depthWriteEnable=false;
    dkCmdBufBindDepthStencilState(commands,&depthState);
    DkColorWriteState write;dkColorWriteStateDefaults(&write);
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colourState;dkColorStateDefaults(&colourState);colourState.blendEnableMask=0;
    dkCmdBufBindColorState(commands,&colourState);
    Material material{};for(unsigned i=0;i<4;++i)material.clip[i*5]=1;
    material.params[0]=invWidth;material.params[1]=invHeight;material.alpha[0]=-1;
    dkCmdBufPushConstants(commands,uniforms->gpu(),512,0,sizeof(material),&material);
    const auto set=imageDescriptors->storage->gpu();
    if(boundImages!=set){dkCmdBufBindImageDescriptorSet(commands,set,ImageDescriptorSlots);boundImages=set;++lastDescriptorSetBinds;}
    dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(slot,3));
    dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
}
// One full-target pass of the bloom chain (bloom.frag.glsl, mode in alpha.x) on the bound target.
// add: one-one on the colour, alpha kept. maskDepth >= 0: the mask quad at that window depth,
// tested (Lequal, no write) against the bound depth. Leaves every cached pipeline binding invalid.
void DekoBackend::bloomQuad(int mode,uint32_t source,uint32_t second,const float params[4],const float colour[4],int blendMode,float maskDepth,bool stencil){
    const bool add=blendMode!=0;
    const DkShader *shaders[]{&vertexShader->shader,quadShaderOverride?quadShaderOverride:&bloomFragmentShader->shader};
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);
    DkDepthStencilState depthState;dkDepthStencilStateDefaults(&depthState);
    depthState.depthTestEnable=maskDepth>=0;depthState.depthWriteEnable=false;depthState.depthCompareOp=DkCompareOp_Lequal;
    if(stencil){
        depthState.stencilTestEnable=true;
        depthState.stencilFrontCompareOp=depthState.stencilBackCompareOp=DkCompareOp_NotEqual;
        depthState.stencilFrontPassOp=depthState.stencilBackPassOp=DkStencilOp_Zero;
    }
    dkCmdBufBindDepthStencilState(commands,&depthState);
    if(stencil)dkCmdBufSetStencil(commands,DkFace_FrontAndBack,0xFF,0,0xFF);
    DkColorWriteState write;dkColorWriteStateDefaults(&write);
    if(add)dkColorWriteStateSetMask(&write,0,DkColorMask_R|DkColorMask_G|DkColorMask_B);
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colourState;dkColorStateDefaults(&colourState);colourState.blendEnableMask=add?1:0;
    dkCmdBufBindColorState(commands,&colourState);
    if(add){
        DkBlendState blend;dkBlendStateDefaults(&blend);
        if(blendMode==2)dkBlendStateSetFactors(&blend,DkBlendFactor_Zero,DkBlendFactor_InvSrcAlpha,DkBlendFactor_Zero,DkBlendFactor_One);
        else{blend.srcColorBlendFactor=DkBlendFactor_One;blend.dstColorBlendFactor=DkBlendFactor_One;}
        dkCmdBufBindBlendState(commands,0,&blend);
    }
    Material material{};
    // The vertex shader writes z = w - 0.1: with w = 0.1 / (1 - depth) the quad lies at `depth`.
    const float w=maskDepth>=0?0.1f/std::max(1.0f-maskDepth,1e-6f):1.0f;
    material.clip[0]=material.clip[5]=material.clip[15]=w;material.clip[10]=1;
    material.alpha[0]=float(mode);
    if(params)std::copy_n(params,4,material.params);
    if(colour)std::copy_n(colour,4,material.fogColour);
    dkCmdBufPushConstants(commands,uniforms->gpu(),512,0,sizeof(material),&material);
    const auto set=imageDescriptors->storage->gpu();
    if(boundImages!=set){dkCmdBufBindImageDescriptorSet(commands,set,ImageDescriptorSlots);boundImages=set;++lastDescriptorSetBinds;}
    if(source!=DescriptorSlots::None)dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(source,3));
    if(second!=DescriptorSlots::None)dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(second,3));
    dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
}
// The bloom at its layer (GL doBloom's GS chain, same-frame), then the picture rebound.
void DekoBackend::runBloom(int slot){
    const auto none=DescriptorSlots::None;
    float dt=1.0f;
    if(bloom.zB>0.0f)dt=std::clamp(1.0f-0.1f*(bloom.threshold+1.0f-bloom.zA)/bloom.zB,0.0f,1.0f);
    bindTarget(bloomMask->image,&depth->image,Width,Height);
    dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,0);
    bloomQuad(1,none,none,nullptr,nullptr,false,dt);
    dkCmdBufBarrier(commands,DkBarrier_Fragments,DkInvalidateFlags_Image);
    auto pass=[&](int to,int mode,uint32_t source,uint32_t second,float px,float py,const float *colour){
        const uint32_t size=(to==0 || to==6)?256u:((to==1 || to==5)?128u:64u);
        bindTarget(bloomChain[to]->image,nullptr,size,size);
        const float params[4]{px,py,1.0f/float(size),1.0f/float(size)};
        bloomQuad(mode,source,second,params,colour,false,-1.0f);
        dkCmdBufBarrier(commands,DkBarrier_Fragments,DkInvalidateFlags_Image);
    };
    const float maskColour[4]{bloom.maskColor[0],bloom.maskColor[1],bloom.maskColor[2],1.0f};
    pass(0,15,bloomMaskSlot,none,float(Width),float(Height),maskColour);
    pass(1,0,bloomChainSlot[0],none,0,0,nullptr);
    pass(2,0,bloomChainSlot[1],none,0,0,nullptr);
    pass(3,17,bloomChainSlot[2],bloomChainSlot[4],bloomHistory?bloom.temporal:0.0f,0,nullptr);
    const float taps=native_renderer::bloomTapAlpha(bloom.temporal);
    pass(4,16,bloomChainSlot[3],none,64.0f,taps,nullptr);
    pass(5,16,bloomChainSlot[3],none,128.0f,taps,nullptr);
    pass(6,0,bloomChainSlot[5],none,0,0,nullptr);
    bloomHistory=true;
    bindTarget(colors[slot]->image,&depth->image,Width,Height);
    const float scale[4]{float(Width)/512.0f,float(Height)/448.0f,0,0};
    const float colour[4]{bloom.color[0],bloom.color[1],bloom.color[2],bloom.color[3]};
    bloomQuad(18,bloomChainSlot[6],none,scale,colour,true,-1.0f);
    // Binding 1 back to the reduced background image for the scene's draws (configureTargets).
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
    ++lastBloomRuns;
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
    dkCmdBufBindShaders(commands, DkStageFlag_GraphicsMask, shaders, 2);nativeVertexBound=true;
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
    dkCmdBufBindSamplerDescriptorSet(commands, samplers->gpu()+(diagGpuMode==3?4*sizeof(DkSamplerDescriptor):0), 4);
    // Binding 1: the reduced background image; sampled only by draws whose bgFog is on, and only
    // while the shared pool is bound (emitDraw turns bgFog off for privately described textures).
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
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
    if(failurePass){
        ++lastFailureActions[(s.alphaTest>>12)&3];
        if(unsigned(s.blend)<4)++lastFailureBlend[unsigned(s.blend)];
        const unsigned ref=(s.alphaTest>>4)&255;
        ++lastFailureRefs[ref<64?0:ref<128?1:ref==128?2:3];
        if(diagGpuMode==2)key.mask=0;
    }
    const bool diagAdditive=s.blend==Blend::Additive && !s.background && !s.screen && !inBackgroundPass;
    if(diagGpuMode==6 && diagAdditive)key.mask=0;
    // Planned backdrops test (never write) against the pre-pass depth at the far plane.
    if(farBackdrop){key.depth=1;++lastPrepassBackdrops;}
    // Background image pass: layer 1 depth tested into its own image (GL Tuning::backgroundDepth);
    // the failure half of an alpha-tested draw writes depth only for ZB_ONLY, as in the picture.
    if(inBackgroundPass && s.background && !s.screen)
        key.depth=uint8_t(failurePass && ((s.alphaTest>>12)&3)!=2?1:3);
    // Alpha-test failure pass (no depth write) of a draw whose passing pixels wrote depth: those
    // pixels hold this draw's exact depth (same vertex shader and matrix), and the failure shader
    // would discard them anyway. A strict Less test rejects them before shading instead of after.
    // Overlay draws rely on equal depth over earlier geometry and keep Lequal.
    if(failurePass && strictFailureDepth && !s.overlay && (key.depth&1) && !(key.depth&2) &&
       (pipelineKey(s,false).depth&2)){key.depth|=4;++lastStrictFailureDraws;}
    // Discarding shaders otherwise risk late depth testing. With depth writes off (and stencil
    // disabled in normal draws) an early test rejects exactly the fragments the late test would.
    // v102/v103 diagnosis (CELOSIA_DIAG_GPU_CYCLE 8 9 a b c) for generic Normal-blend draws without a depth
    // write (the haze / cloud layers): 8 no depth test, 9 no colour write, a (10) a shader that writes
    // nothing, b (11) blending off, c (12) no background-image read. Measures only (the picture changes).
    const bool diagTarget=diagGpuMode>=8 && diagGpuMode<=12 && !failurePass && !opaque && !(key.depth&2) && !s.background && !s.screen &&
       s.blend==Blend::Normal;
    if(diagTarget){
        if(diagGpuMode==8)key.depth&=uint8_t(~1u);else if(diagGpuMode==9 || diagGpuMode==10)key.mask=0;
    }
    const bool early=!opaque && (key.depth&1) && !(key.depth&2);
    if(early)++lastEarlyDraws;
    int fragment=(diagTarget && diagGpuMode==10)?3:(diagGpuMode==5 || (diagGpuMode==4 && failurePass))?0:opaque?1:early?2:0;
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
    const bool diagNoBlend=diagTarget && diagGpuMode==11;
    const int blendKey=int(s.blend)|((diagSky==5 && s.background)?8:0)|(diagNoBlend?16:0);
    if(boundBlend!=blendKey){
        DkColorState color;dkColorStateDefaults(&color);
        color.blendEnableMask=s.blend!=Blend::Opaque && !diagNoBlend;
        dkCmdBufBindColorState(commands,&color);
        DkBlendState blend;dkBlendStateDefaults(&blend);
        if(s.blend==Blend::Additive || s.blend==Blend::Subtract)blend.dstColorBlendFactor=DkBlendFactor_One;
        if(s.blend==Blend::Subtract && !(diagSky==5 && s.background))blend.colorBlendOp=DkBlendOp_RevSub;
        dkCmdBufBindBlendState(commands,0,&blend);boundBlend=blendKey;
    }
    auto material = materialFor(draw, failurePass);
    material.alpha[3]=farBackdrop;
    if(skyActive && !inBackgroundPass && !s.screen && !s.background && draw.texture->slot!=DescriptorSlots::None){
        const bool zFogged=fog.steps>0 && s.order<fog.order;
        // A draw lying wholly nearer than the fog's start fades by 0 at every pixel (the clamp of
        // a negative ramp): it skips the background read with an identical result (GL glNoFog98,
        // with the same margin for the GPU's rounding).
        const bool nearer=maxClipW(draw.clip.data(),draw.mesh->bounds)<fog.start*0.999f;
        if(nearer && (s.fogged || zFogged))++lastBgNear;
        if(diagTarget && (s.fogged || zFogged) && !nearer)++lastBgTarget;
        if((s.fogged || zFogged) && !nearer && !(diagTarget && diagGpuMode==12)){
            material.bgFog[0]=fog.start;material.bgFog[1]=fog.width;
            material.bgFog[2]=float(fog.steps);material.bgFog[3]=1;
            if(!failurePass)++lastBgFogged;
        }
    }
    if(zeroAddDiscard && celosia::live::settings().addDiscard.load(std::memory_order_relaxed) && !opaque && !failurePass && !(key.depth&2) && key.mask &&
       (s.blend==Blend::Additive || s.blend==Blend::Subtract)){material.params[3]=2;++lastZeroAddDraws;}
    // v105: a lean variant of the generic shader holding only the paths this draw's uniforms turn on
    // (lean.frag.glsl). Early tests only without depth writes, where they reject what late ones would.
    // v116: also the opaque shader's draws, the background layer's (near cull) and untextured ones.
    if(leanEnabled && diagGpuMode==0 && fragment<=2 && material.debug[3]==0){
        const bool textured=material.fogColour[3]!=0;
        int variant=(material.bgFog[3]>0?2:0)|(textured && material.lod[2]>0?4:0)|(textured?0:64);
        if(fragment==1)variant|=128;
        else variant|=(material.alpha[0]>=0?1:0)|(material.params[3]>1.5f?8:0)|((key.depth&2)?0:16)|(material.fog[2]>0?32:0);
        if(variant<32 || (leanVariants&1u)){fragment=8+variant;++lastLeanDraws;}
    }
    if(boundFragment!=fragment){
        const DkShader *selected=fragment>=8?&leanFragmentShaders[fragment-8]->shader:fragment==3?&depthFragmentShader->shader:fragment==1?&opaqueFragmentShader->shader:fragment==2?&earlyFragmentShader->shader:&fragmentShader->shader;
        // v124: the vertex shader is the same for every scene draw: only the fragment stage changes (the
        // 256 lean variants switch often), as long as nothing else replaced the vertex shader.
        if(nativeVertexBound)dkCmdBufBindShaders(commands,DkStageFlag_Fragment,&selected,1);
        else{
            const DkShader *shaders[]{&vertexShader->shader,selected};
            dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);nativeVertexBound=true;
        }
        boundFragment=fragment;
    }
    dkCmdBufPushConstants(commands, uniforms->gpu(), 512, 0, sizeof(material), &material);
    bindTexture(diagGpuMode==7 && diagAdditive?*dummyTexture:*draw.texture,s.clamp);
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
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,normalShaders,2);nativeVertexBound=true;
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
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,volumeShaders,2);nativeVertexBound=false;
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
void DekoBackend::resolveShadows(float amount,int slot) {
    // v119 measures (CELOSIA_DIAG_GPU_CYCLE; the picture changes): 25 (p) no blur passes, 26 (q) no
    // mask / reduce / blur (only the darken), 27 (r) no barriers, 28 (s) the hard (one-pass) resolve,
    // 29 (t) no resolve at all (the counted stencil cleared by the hard pass with amount 0).
    if(diagGpuMode==29)amount=0;
    if(softShadows && diagGpuMode!=28 && diagGpuMode!=29 && celosia::live::settings().softShadows.load(std::memory_order_relaxed)){
        const auto none=DescriptorSlots::None;
        const bool noBlur=diagGpuMode==25,darkenOnly=diagGpuMode==26,noBarrier=diagGpuMode==27;
        auto barrier=[&]{if(!noBarrier)dkCmdBufBarrier(commands,DkBarrier_Fragments,DkInvalidateFlags_Image);};
        const uint32_t sw=Width/4,sh=Height/4;
        // The counted pixels as a mask (the picture's stencil tested and cleared).
        if(!darkenOnly){
        bindTarget(bloomMask->image,&depth->image,Width,Height);
        dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,0);
        bloomQuad(1,none,none,nullptr,nullptr,0,-1.0f,true);
        barrier();
        // Reduced 4 x 4, then blurred across and down (GL blur(1): steps of render width / 512 texels).
        bindTarget(shadowSmall[0]->image,nullptr,sw,sh);
        const float reduce[4]{float(Width),float(Height),1.0f/float(sw),1.0f/float(sh)};
        bloomQuad(7,bloomMaskSlot,none,reduce,nullptr,0,-1.0f);
        barrier();
        if(!noBlur){
        const float k=float(Width)/512.0f;
        bindTarget(shadowSmall[1]->image,nullptr,sw,sh);
        const float across[4]{k/float(sw),0,1.0f/float(sw),1.0f/float(sh)};
        bloomQuad(19,shadowSmallSlot[0],none,across,nullptr,0,-1.0f);
        barrier();
        bindTarget(shadowSmall[0]->image,nullptr,sw,sh);
        const float down[4]{0,k/float(sh),1.0f/float(sw),1.0f/float(sh)};
        bloomQuad(19,shadowSmallSlot[1],none,down,nullptr,0,-1.0f);
        barrier();
        }
        }
        // The picture darkened by the mask x the amount.
        bindTarget(colors[slot]->image,&depth->image,Width,Height);
        const float darken[4]{amount,0,1.0f/float(Width),1.0f/float(Height)};
        bloomQuad(9,shadowSmallSlot[0],none,darken,nullptr,2,-1.0f);
        dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
        ++lastSoftShadows;
        restoreShadowState();
        return;
    }
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
    // v123: swapped, not moved: the lists keep their storage between frames (a move left them empty, so
    // every frame regrew them, copying each Draw and its two shared_ptrs). The frame's previous lists
    // (retired: beginFrame waited for this frame's fence) are cleared here as the assignment did.
    frame.draws.swap(draws); draws.clear(); frame.uploads.swap(uploads); uploads.clear();
    const uint32_t frameMergedAlpha=mergedAlphaDraws;mergedAlphaDraws=0;
    resolvePendingMeshes(frame.draws,meshes);
    if(!held){
        lastOpaqueDraws=lastGenericDraws=lastShadowDraws=lastEarlyDraws=lastLeanDraws=lastPrepassDraws=lastPrepassBackdrops=lastStrictFailureDraws=0;
        lastMergedAlphaDraws=frameMergedAlpha;lastMergedBgAlpha=0;
        diagSky=diagSkyRequested && std::chrono::steady_clock::now()>=diagSkyFrom?diagSkyRequested:0;
        lastGenericAlphaModes.fill(0);
        for(int i=0;i<4;++i)lastFailureActions[i]=lastFailureBlend[i]=lastFailureRefs[i]=0;
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
        // The menu's frame limit: 2 = every image shown for two refreshes (30 FPS).
        {
            static int swapNow=1;
            const int wanted=std::clamp(celosia::live::settings().swapInterval.load(std::memory_order_relaxed),1,2);
            if(wanted!=swapNow){dkSwapchainSetSwapInterval(swapchain,wanted);swapNow=wanted;}
        }
        slot = dkQueueAcquireImage(queue, swapchain);
        if (slot < 0 || slot >= int(Frames)) throw std::runtime_error("deko3d: acquire failed");
        bool clearToFog=false,hasBackground=false;
        for(const auto &draw:frame.draws)if(draw.state.background){hasBackground=true;break;}
        skyActive=skyImageEnabled && celosia::live::settings().skyImage.load(std::memory_order_relaxed) && hasBackground && fog.enabled && fog.width>0.0f && !diagSky;
        if(backgroundClearFog && !skyActive)clearToFog=hasBackground;
        lastClearToFog=clearToFog;lastSkyImage=skyActive;lastBgFogged=lastBgNear=lastBgTarget=0;
        configureTargets(slot,clearToFog);
        acquireNs=armTicksToNs(armGetSystemTick())-acquireStart;
        if(frame.gpuCountersPending){
            dkCmdBufReportCounter(commands,DkCounter_VertexShaderInvocations,frame.gpuReports->gpu()+32);
            dkCmdBufReportCounter(commands,DkCounter_FragmentShaderInvocations,frame.gpuReports->gpu()+64);
            dkCmdBufReportCounter(commands,DkCounter_InputPrimitives,frame.gpuReports->gpu()+96);
        }
        const auto recordStart=armTicksToNs(armGetSystemTick());
        markRender(RenderPhase::Record);
        {
            const auto sortStart=armGetSystemTick();
            const auto byOrder=[](const Draw &a, const Draw &b) { return a.state.order < b.state.order; };
            // v124: a stable sort leaves an ordered list as it is.
            // v125: otherwise the keys (order, index) are sorted (8 bytes each, the index keeps it stable)
            // and each Draw moved once into its place: the same order as std::stable_sort, which moved the
            // ~300-byte Draws n log n times (v124: 2 ms of the 4.2 ms record at 1040 draws).
            if(!std::is_sorted(frame.draws.begin(), frame.draws.end(), byOrder)){
                const std::size_t count=frame.draws.size();
                sortKeys.resize(count);
                for(std::size_t i=0;i<count;++i)sortKeys[i]=(uint64_t(frame.draws[i].state.order)<<32)|uint64_t(i);
                std::sort(sortKeys.begin(),sortKeys.end());
                sortedDraws.clear();sortedDraws.reserve(count);
                for(const uint64_t key:sortKeys)sortedDraws.push_back(std::move(frame.draws[uint32_t(key)]));
                frame.draws.swap(sortedDraws);sortedDraws.clear();
            }
            frameSortNs=armTicksToNs(armGetSystemTick()-sortStart);
        }
        drawTiming=diagnosticCapture;
        if(drawTiming){
            if(!drawTimeReports)drawTimeReports=std::make_shared<Buffer>(device,MaxDrawTimes*16,allocationBudget);
            std::memset(drawTimeReports->cpu(),0,MaxDrawTimes*16);
            drawTimeOwners.clear();markDrawTime(-1);
        }
        if(skyActive){
            // Layer 1 into the background image, in the frame's painter order.
            markSegment(GpuSegment::Backdrop);
            dkCmdBufBarrier(commands,DkBarrier_Fragments,0);
            bindTarget(bgColour->image,&bgDepth->image,512,448);
            if(backgroundClearFog)dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,fog.colour[0],fog.colour[1],fog.colour[2],1);
            else dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,1);
            dkCmdBufClearDepthStencil(commands,true,1,0xFF,0);
            inBackgroundPass=true;
            for(std::size_t i=0;i<frame.draws.size();++i){
                const auto &draw=frame.draws[i];
                if(!draw.state.background || draw.state.shadowVolume)continue;
                emitDraw(draw,false,false);
                if(needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha))emitDraw(draw,true,false);
                markDrawTime(int(i)*4+2);
            }
            inBackgroundPass=false;
            // bgEnd: reduced to 256 x 256, then laid behind the scene, stretched back.
            dkCmdBufBarrier(commands,DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(bgSmall->image,nullptr,256,256);
            drawImageQuad(bgColourSlot,1.0f/256.0f,1.0f/256.0f);
            dkCmdBufBarrier(commands,DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(colors[slot]->image,&depth->image,Width,Height);
            drawImageQuad(bgSmallSlot,1.0f/float(Width),1.0f/float(Height));
            markDrawTime(-2);
        }
        PrepassPlan prepass;
        if(prepassEnabled && !diagSky){
            prepass=planPrepass(frame.draws.size(),[&](std::size_t i)->const DrawState&{return frame.draws[i].state;},
                [&](std::size_t i,uint8_t &va,uint8_t &ta){va=frame.draws[i].mesh->minAlpha;ta=frame.draws[i].texture->minAlpha;},prepassFlags);
            if(prepass.active)
                for(std::size_t i=prepass.first;i<prepass.barrier;++i)if(prepassFlags[i]){markSegment(GpuSegment::Prepass);emitDepthOnly(frame.draws[i]);}
            markDrawTime(-3);
        }
        ShadowBatch shadows;
        // Coverage attribution is CPU-only bookkeeping on counter-sampled frames; the GPU
        // command stream is identical whether or not it runs.
        CoverageSample coverage;const bool attributeCoverage=frame.gpuCountersPending;
        if(attributeCoverage)coverage.frame=frame.gpuCounterFrame;
        const bool bloomOn=bloomAllowed && celosia::live::settings().bloom.load(std::memory_order_relaxed) && bloom.enabled && !diagSky;
        if(!bloomOn)bloomHistory=false;
        bool bloomDone=!bloomOn;
        for (std::size_t drawIndex=0;drawIndex<frame.draws.size();++drawIndex) {
            const auto &draw=frame.draws[drawIndex];
            if(shadows.resolveBefore(draw.state.order)){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount,slot);shadows.clear();markDrawTime(-4);}
            // The bloom before the first draw of its layer (GL: the event at bloom.order).
            if(!bloomDone && draw.state.order>=bloom.order){
                if(shadows.pending){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount,slot);shadows.clear();markDrawTime(-4);}
                markSegment(GpuSegment::Screen);runBloom(slot);bloomDone=true;markDrawTime(-5);
            }
            const float area=attributeCoverage?screenCoverage(draw.clip.data(),draw.mesh->bounds,draw.state.screen):0;
            if(attributeCoverage && area>=0.9f)++coverage.fullScreenDraws;
            if(draw.state.shadowVolume){
                markSegment(GpuSegment::Shadow);
                emitShadowVolume(draw);shadows.add(draw.state.order,draw.state.shadow);markDrawTime(int(drawIndex)*4);
                if(attributeCoverage)coverage.shadow+=area;
                continue;
            }
            if(skyActive && draw.state.background)continue;
            const bool failure=needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha);
            if(attributeCoverage){
                if(draw.state.screen || draw.state.background)coverage.backdrop+=area;
                else if(canUseOpaqueFragment(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha,false))coverage.opaque+=area;
                else coverage.generic+=area;
                if(failure)coverage.failure+=area;
            }
            // v121: a background draw's FB_ONLY alpha test in the main pass (no depth writes either half;
            // the background image pass, which writes layer 1's depth, keeps both): its passing and failing
            // pixels are written alike (same colour mask, blend and shader colour), so the draw is one pass
            // without the test, in primitive order as on the GS. Pixels the pre-pass covers are drawn over
            // later by the same opaque draws either way. CELOSIA_DIAG_NO_BG_ALPHA_MERGE=1 reverts.
            if(failure && mergeBgAlpha && diagGpuMode==0 && draw.state.background && !draw.state.shadowVolume && (draw.state.alphaTest&1) &&
               ((draw.state.alphaTest>>12)&3)==1 && !(pipelineKey(draw.state,false).depth&2) && !(pipelineKey(draw.state,true).depth&2)){
                Draw merged=draw;merged.state.alphaTest=0;++lastMergedBgAlpha;
                markSegment(GpuSegment::Backdrop);
                emitDraw(merged,false,prepass.farBackdrop(drawIndex,draw.state));
                markDrawTime(int(drawIndex)*4);
                continue;
            }
            markSegment(draw.state.background?GpuSegment::Backdrop:draw.state.screen?GpuSegment::Screen:
                canUseOpaqueFragment(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha,false)?GpuSegment::Opaque:GpuSegment::Generic);
            emitDraw(draw,false,prepass.farBackdrop(drawIndex,draw.state));
            markDrawTime(int(drawIndex)*4);
            // v113: 19 (j) each screen draw 4 more times, the same state, in the prepass segment (measures only).
            if(diagGpuMode==19 && draw.state.screen){
                markSegment(GpuSegment::Prepass);
                for(int q=0;q<4;++q)emitDraw(draw,false,prepass.farBackdrop(drawIndex,draw.state));
                markSegment(GpuSegment::Screen);
            }
            if (failure && diagGpuMode!=1) {markSegment(GpuSegment::Failure);emitDraw(draw, true);markDrawTime(int(drawIndex)*4+1);}
        }
        if(attributeCoverage)lastCoverage=coverage;
        if(shadows.pending){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount,slot);markDrawTime(-4);}
        if(!bloomDone){markSegment(GpuSegment::Screen);runBloom(slot);markDrawTime(-5);}
        // v112 fill calibration (CELOSIA_DIAG_GPU_CYCLE d e f g = modes 13-16; measures only): 8 full-screen
        // quads into the bloom mask image (not shown) in the prepass segment: 13 constant colour, 14 the
        // same blended (one-one), 15 a texture read, 16 constant with a depth test against the scene's depth.
        // v113: 17 (h) the constant quads into the picture itself, its depth bound and untested.
        // v114: 20 (k) the same blended (zero, 1 - source alpha), 21 (l) depth-tested where it passes (near).
        // v115: 22 (m) the lean shader (variant 0, a texture read), 23 (n) the opaque one, 24 (o) the generic one,
        // on the same quads (constant vertex colour 0: the material's shade; measures only).
        if(diagGpuMode>=22 && diagGpuMode<=24){
            markSegment(GpuSegment::Prepass);
            quadShaderOverride=diagGpuMode==22?&leanFragmentShaders[0]->shader:diagGpuMode==23?&opaqueFragmentShader->shader:&fragmentShader->shader;
            const float textured[4]{0,0,0,1};
            for(int q=0;q<8;++q){
                bindTexture(*dummyTexture,0);
                bloomQuad(-1,DescriptorSlots::None,DescriptorSlots::None,nullptr,textured,0,-1.0f);
            }
            quadShaderOverride=nullptr;
        }
        if(diagGpuMode==17 || diagGpuMode==20 || diagGpuMode==21){
            markSegment(GpuSegment::Prepass);
            for(int q=0;q<8;++q)bloomQuad(1,DescriptorSlots::None,DescriptorSlots::None,nullptr,nullptr,diagGpuMode==20?2:0,diagGpuMode==21?0.0f:-1.0f);
        }
        if(diagGpuMode>=13 && diagGpuMode<=16){
            markSegment(GpuSegment::Prepass);
            bindTarget(bloomMask->image,diagGpuMode==16?&depth->image:nullptr,Width,Height);
            const float copy[4]{0,0,1.0f/float(Width),1.0f/float(Height)};
            for(int q=0;q<8;++q)
                bloomQuad(diagGpuMode==15?0:1,diagGpuMode==15?bgSmallSlot:DescriptorSlots::None,DescriptorSlots::None,copy,nullptr,
                    diagGpuMode==14?1:0,diagGpuMode==16?0.999f:-1.0f);
            dkCmdBufBarrier(commands,DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(colors[slot]->image,&depth->image,Width,Height);
        }
        drawTiming=false;
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
    // v99 CELOSIA_DIAG_PRESENT_LOG=1 (benchmarks): every presentation interval, in 0.1 ms, kept here and
    // appended to present-times.txt once per status window (one line per window; no game-thread work).
    static const bool presentLog=std::getenv("CELOSIA_DIAG_PRESENT_LOG")!=nullptr;
    static std::vector<uint16_t> presentTenths;
    static uint64_t lastPresentNs=0;
    static std::string slowFrames;static uint64_t lastSubmitEnd=0;static uint32_t blocksBefore=0;
    // v123: the render thread per frame, averaged over the status window (ms): outside endFrame (of it draw()
    // calls, mesh and texture uploads; the rest waits for the game's batch), fence, uploads, record, submit.
    struct RtSums{double out=0,draw=0,mesh=0,tex=0,fence=0,up=0,rec=0,sub=0,draws=0,sort=0;uint32_t n=0;};
    static RtSums rt;
    if(presentLog&&!held){
        if(lastPresentNs){
            const uint64_t interval=sampleEndNs-lastPresentNs;
            presentTenths.push_back(uint16_t(std::min<uint64_t>(interval/100000u,65535u)));
            // v107: a presentation over 25 ms: where the render thread's time went (ms): outside endFrame
            // (waiting for / taking the game's frame), fence wait, uploads, record, submit, and the
            // textures / meshes created for it (count / ms).
            if(interval>34000000u && slowFrames.size()<4000){
                char item[192];
                std::snprintf(item,sizeof(item)," f%u:%.1f(out %.1f fence %.1f up %.1f rec %.1f sub %.1f tex %u/%.1f mesh %u/%.1f blocks %u draws %zu)",
                    submittedFrames,interval/1e6,lastSubmitEnd&&cpuStart>lastSubmitEnd?(cpuStart-lastSubmitEnd)/1e6:0.0,lastBeginWaitNs/1e6,
                    uploadNs/1e6,recordNs/1e6,(submitEnd-submitStart)/1e6,frameTexCount,frameTexNs/1e6,frameMeshCount,frameMeshNs/1e6,BufferBlock::created-blocksBefore,frame.draws.size());
                slowFrames+=item;
            }
        }
        lastPresentNs=sampleEndNs;
    }
    if(presentLog && !held && lastSubmitEnd && cpuStart>lastSubmitEnd){
        rt.out+=(cpuStart-lastSubmitEnd)/1e6;rt.draw+=frameDrawNs/1e6;rt.mesh+=frameMeshNs/1e6;rt.tex+=frameTexNs/1e6;
        rt.fence+=lastBeginWaitNs/1e6;rt.up+=uploadNs/1e6;rt.rec+=recordNs/1e6;rt.sub+=(submitEnd-submitStart)/1e6;
        rt.draws+=double(frame.draws.size());rt.sort+=frameSortNs/1e6;++rt.n;
    }
    lastSubmitEnd=submitEnd;frameTexNs=frameMeshNs=0;frameTexCount=frameMeshCount=0;blocksBefore=BufferBlock::created;frameDrawNs=0;
    if(const auto sample=presentationRate.record(sampleEndNs,!held)){
        if(presentLog&&!presentTenths.empty()){
            static bool fresh=true;
            char *logBuf=nullptr;size_t logLen=0;
            if(auto *log=open_memstream(&logBuf,&logLen)){
                const bool append=!fresh;fresh=false;
                std::fprintf(log,"%llu",static_cast<unsigned long long>(sampleEndNs/1000000u));
                for(uint16_t v:presentTenths)std::fprintf(log," %u",unsigned(v));
                // v100: the window's GPU time (average, peak) and per-segment averages, after a '|'.
                std::fprintf(log," | %.2f %.2f",gpuTimeWindow.averageNs()/1000000.0,gpuTimeWindow.peakNs/1000000.0);
                for(unsigned k=0;k<GpuSegmentCount;++k)std::fprintf(log," %.2f",segmentWindow.averageMs(k));
                // v101: fragments per pixel (window) and the last counted frame's screen coverage by class.
                std::fprintf(log," | %.2f %.2f %.2f %.2f %.2f %.2f %u",gpuCounterWindow.fragmentInvocationsPerPixel(uint64_t(Width)*Height),
                    lastCoverage.opaque,lastCoverage.generic,lastCoverage.failure,lastCoverage.backdrop,lastCoverage.shadow,lastCoverage.fullScreenDraws);
                std::fprintf(log," | mode %d bgFogged %u bgTarget %u lean %u generic %u",diagGpuMode,lastBgFogged,lastBgTarget,lastLeanDraws,lastGenericDraws);
                if(rt.n){const double k=1.0/rt.n;
                    std::fprintf(log," | rt out %.2f draw %.2f mesh %.2f tex %.2f fence %.2f up %.2f rec %.2f sub %.2f draws %.0f sort %.2f",
                        rt.out*k,rt.draw*k,rt.mesh*k,rt.tex*k,rt.fence*k,rt.up*k,rt.rec*k,rt.sub*k,rt.draws*k,rt.sort*k);rt=RtSums{};}
                if(!slowFrames.empty()){std::fprintf(log," | slow%s",slowFrames.c_str());slowFrames.clear();}
                std::fputc(10,log);
                std::fclose(log);
                sdWriter().post({"sdmc:/switch/celosia-native-boot/present-times.txt",append,true,std::string(logBuf,logLen)});
                std::free(logBuf);
            }
            presentTenths.clear();
        }
        celosia::live::settings().fpsCenti.store(uint32_t(sample->fps*100.0+0.5),std::memory_order_relaxed);
        celosia::live::settings().gpuUs.store(uint32_t(lastGpuNs/1000u),std::memory_order_relaxed);
        char *statusBuf=nullptr;size_t statusLen=0;
        auto *file=open_memstream(&statusBuf,&statusLen);
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
            const bool counterStats=std::fprintf(file,"last_opaque_draws=%u\nlast_generic_draws=%u\nlast_early_draws=%u\nlast_prepass_draws=%u\nlast_prepass_backdrops=%u\nprepass_enabled=%d\nlast_strict_failure_draws=%u\nalpha_noop_merged=%u\nbloom_runs=%u bloom_enabled=%d\nsoft_shadow_resolves=%u zero_add_draws=%u\nlast_shadow_draws=%u\ncounter_samples=%llu\ncounter_invalid_samples=%llu\ncounter_last_frame=%u\ncounter_avg_vertices=%.3f\ncounter_avg_fragments=%.3f\ncounter_avg_primitives=%.3f\ncounter_fragments_per_native_pixel_estimate=%.6f\ncounter_zero_policy=unproven_or_unsupported\n",
                lastOpaqueDraws,lastGenericDraws,lastEarlyDraws,lastPrepassDraws,lastPrepassBackdrops,int(prepassEnabled),lastStrictFailureDraws,lastMergedAlphaDraws,lastBloomRuns,int(bloom.enabled),lastSoftShadows,lastZeroAddDraws,lastShadowDraws,static_cast<unsigned long long>(gpuCounterWindow.count),
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
            std::fprintf(file,"merged_bg_alpha=%u\nfailure_actions=%u,%u,%u,%u\nfailure_blend=%u,%u,%u,%u\nfailure_refs=%u,%u,%u,%u\ndiag_gpu_mode=%d\nsky_image=%u\nbg_fogged_draws=%u\nbg_near_draws=%u\nfog=%d,%.1f,%.1f,%d,%u\n",
                lastMergedBgAlpha,lastFailureActions[0],lastFailureActions[1],lastFailureActions[2],lastFailureActions[3],
                lastFailureBlend[0],lastFailureBlend[1],lastFailureBlend[2],lastFailureBlend[3],
                lastFailureRefs[0],lastFailureRefs[1],lastFailureRefs[2],lastFailureRefs[3],diagGpuMode,
                lastSkyImage,lastBgFogged,lastBgNear,int(fog.enabled),fog.start,fog.width,fog.steps,fog.order);
            const bool flushed=true;
            if(std::fclose(file)!=0 || !written || !lows || !timings || !gpuTimings || !counterStats || !alphaStats || !descriptorStats || !coverageStats || !flushed)std::fputs("[hardware] status write failed\n",stderr);
            else sdWriter().post({"sdmc:/switch/celosia-native-boot/hardware-status.txt",false,false,std::string(statusBuf,statusLen)});
            std::free(statusBuf);
        }else std::fputs("[hardware] status file unavailable\n",stderr);
        gpuTimeWindow.reset();
        gpuCounterWindow.reset();
        if(!diagGpuCycle.empty()){diagGpuIndex=(diagGpuIndex+1)%diagGpuCycle.size();diagGpuMode=diagGpuCycle[diagGpuIndex];}
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
        // The frame's draws in recorded order, to see which ones a heavy view is made of.
        std::snprintf(capturePath,sizeof(capturePath),"sdmc:/switch/celosia-native-boot/native-frame-%06u.csv",submittedFrames);
        std::vector<double> usColour(frame.draws.size()),usFailure(frame.draws.size()),usBackground(frame.draws.size());
        double usOther[5]{};
        if(drawTimeReports && drawTimeOwners.size()>1){
            const auto *reports=static_cast<const GpuReport *>(drawTimeReports->cpu());
            for(std::size_t k=1;k<drawTimeOwners.size();++k){
                const auto ns=gpuIntervalNs(reports[k-1],reports[k]);if(!ns)continue;
                const double us=double(*ns)/1000.0;const int owner=drawTimeOwners[k];
                if(owner<0){if(-owner<5)usOther[-owner]+=us;continue;}
                const std::size_t index=std::size_t(owner/4);if(index>=frame.draws.size())continue;
                (owner%4==0?usColour:owner%4==1?usFailure:usBackground)[index]+=us;
            }
        }
        if(auto *list=std::fopen(capturePath,"w")){
            std::fprintf(list,"# gpu_us other: sky_copies=%.1f prepass=%.1f shadow_resolve=%.1f\n",usOther[2],usOther[3],usOther[4]);
            std::fprintf(list,"index,order,background,screen,shadow,blend,alpha_test,fogged,overlay,texture,tex_min_alpha,vertices,indices,coverage,max_w,opaque,failure,opacity,lit,fresnel,us_colour,us_failure,us_background,tex_w,tex_h,tex_levels,lod_k,lod_scale,max_lod,diag_mode\n");
            for(std::size_t i=0;i<frame.draws.size();++i){
                const auto &d=frame.draws[i];const auto &s=d.state;
                std::fprintf(list,"%zu,%u,%d,%d,%d,%d,0x%04x,%d,%d,%llx,%u,%u,%u,%.4f,%.1f,%d,%d,%.3f,%d,%d,%.1f,%.1f,%.1f,%u,%u,%u,%.3f,%.3f,%.3f,%d\n",
                    i,s.order,int(s.background),int(s.screen),int(s.shadowVolume),int(s.blend),unsigned(s.alphaTest),int(s.fogged),int(s.overlay),
                    static_cast<unsigned long long>(s.texture),unsigned(d.texture->minAlpha),d.mesh->count,d.mesh->indexCount,
                    screenCoverage(d.clip.data(),d.mesh->bounds,s.screen),maxClipW(d.clip.data(),d.mesh->bounds),
                    int(canUseOpaqueFragment(s,d.mesh->minAlpha,d.texture->minAlpha,false)),
                    int(needsAlphaFailurePass(s,d.mesh->minAlpha,d.texture->minAlpha)),s.opacity,int(s.lit),int(s.fresnel),
                    usColour[i],usFailure[i],usBackground[i],unsigned(d.texture->width),unsigned(d.texture->height),
                    unsigned(d.texture->levels),s.lodK,s.lodScale,s.maxLod,diagGpuMode);
            }
            std::fclose(list);
        }
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
