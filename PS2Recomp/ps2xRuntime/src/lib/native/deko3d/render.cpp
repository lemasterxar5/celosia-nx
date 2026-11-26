#include "backend.hpp"
namespace native_renderer::threads{void noteForBoost();} // v170 (native_renderer.cpp)
#include "../../../../platform/switch/hybrid/live_settings.hpp"
#include "render_progress.hpp"
extern "C" int celosia_effective_docked(int real);
#include "late_mesh.hpp"
#include "opaque_policy.hpp"
#include "prepass_policy.hpp"
// Optional present observer (platform/switch/hybrid/saltynx_fps.cpp: SaltyNX FPS counter block).
extern "C" void celosia_frame_presented(void) __attribute__((weak));
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sys/stat.h>
#include <vector>
#include <cstddef>
#include <iterator>
#include <unistd.h>
#include <condition_variable>
#include <cstdio>
#include <sys/stat.h>
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
            // v167: BC3 rows of 4 x 4 blocks (16 bytes each), in bytes as the RGBA ones.
            const uint32_t rowBytes = upload.bc3 ? (w + 3) / 4 * 16 : packedImageLayout(w, h).rowBytes;
            const uint32_t layerBytes = upload.bc3 ? rowBytes * ((h + 3) / 4) : w * h * 4;
            const DkCopyBuf source{upload.staging->gpu() + offset, rowBytes, layerBytes};
            const DkImageRect region{0, 0, 0, w, h, 1};
            dkCmdBufCopyBufferToImage(commands, &source, &view, &region, 0);
            offset += layerBytes;
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
    bindSamples(msaa>1 && ((scene && &colour==&scene->image) || (bloomMaskMs && &colour==&bloomMaskMs->image) ||
                           (glowMs && &colour==&glowMs->image))); // v177: the glow layer has the scene's samples
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
    pushMaterial(material);
    const auto set=imageDescriptors->storage->gpu();
    if(boundImages!=set){dkCmdBufBindImageDescriptorSet(commands,set,ImageDescriptorSlots);boundImages=set;++lastDescriptorSetBinds;}
    dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(slot,3));
    bindVtxFormat(0);dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
}
// One full-target pass of the bloom chain (bloom.frag.glsl, mode in alpha.x) on the bound target.
// add: one-one on the colour, alpha kept. maskDepth >= 0: the mask quad at that window depth,
// tested (Lequal, no write) against the bound depth. Leaves every cached pipeline binding invalid.
void DekoBackend::bloomQuad(int mode,uint32_t source,uint32_t second,const float params[4],const float colour[4],int blendMode,float maskDepth,bool stencil){
    const bool add=blendMode!=0;
    // v162: mode 1 (the masks: 1 where the tests pass) on one.frag.glsl, early tests (bloom.frag has discards).
    const bool oneMask=mode==1 && earlyMask && oneFragmentShader && !quadShaderOverride && diagGpuMode==0;
    const bool softMode=(mode==7 || mode==19 || mode==9) && shadowSoft && shadowSoftShader && !quadShaderOverride && diagGpuMode==0;
    const bool split=bloomSplit && bloomChainShader && bloomCompShader && !quadShaderOverride && diagGpuMode==0;
    const bool chainMode=split && (mode==15 || mode==17 || mode==16),compMode=split && mode==18;
    const DkShader *shaders[]{&vertexShader->shader,oneMask?&oneFragmentShader->shader:softMode?&shadowSoftShader->shader:
                              chainMode?&bloomChainShader->shader:compMode?&bloomCompShader->shader:
                              quadShaderOverride?quadShaderOverride:&bloomFragmentShader->shader};
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
    // v130: the blended quads keep the picture's alpha by their alpha factors (Zero, One: dst.a exactly)
    // and write all four channels: an RGB-only mask made the compressed target's writes read-modify-write
    // (v129 capture: composite 0.90 ms, darken 0.77 ms, a plain full-screen pass ~0.2 ms).
    DkColorWriteState write;dkColorWriteStateDefaults(&write);
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colourState;dkColorStateDefaults(&colourState);colourState.blendEnableMask=add?1:0;
    dkCmdBufBindColorState(commands,&colourState);
    if(add){
        DkBlendState blend;dkBlendStateDefaults(&blend);
        if(blendMode==2)dkBlendStateSetFactors(&blend,DkBlendFactor_Zero,DkBlendFactor_InvSrcAlpha,DkBlendFactor_Zero,DkBlendFactor_One);
        else if(blendMode==3)dkBlendStateSetFactors(&blend,DkBlendFactor_SrcAlpha,DkBlendFactor_InvSrcAlpha,DkBlendFactor_Zero,DkBlendFactor_One); // v177: GL QuadBlend::Over
        else dkBlendStateSetFactors(&blend,DkBlendFactor_One,DkBlendFactor_One,DkBlendFactor_Zero,DkBlendFactor_One);
        dkCmdBufBindBlendState(commands,0,&blend);
    }
    Material material{};
    // The vertex shader writes z = w - 0.1: with w = 0.1 / (1 - depth) the quad lies at `depth`.
    const float w=maskDepth>=0?0.1f/std::max(1.0f-maskDepth,1e-6f):1.0f;
    material.clip[0]=material.clip[5]=material.clip[15]=w;material.clip[10]=1;
    material.alpha[0]=float(mode);
    if(params)std::copy_n(params,4,material.params);
    if(colour)std::copy_n(colour,4,material.fogColour);
    pushMaterial(material);
    const auto set=imageDescriptors->storage->gpu();
    if(boundImages!=set){dkCmdBufBindImageDescriptorSet(commands,set,ImageDescriptorSlots);boundImages=set;++lastDescriptorSetBinds;}
    if(source!=DescriptorSlots::None)dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(source,3));
    if(second!=DescriptorSlots::None)dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(second,3));
    bindVtxFormat(0);dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
}
// The bloom at its layer (GL doBloom's GS chain, same-frame), then the picture rebound.
void DekoBackend::runBloom(int slot){
    if(diagBloom==2)return;
    const auto none=DescriptorSlots::None;
    float dt=1.0f;
    if(bloom.zB>0.0f)dt=std::clamp(1.0f-0.1f*(bloom.threshold+1.0f-bloom.zA)/bloom.zB,0.0f,1.0f);
    bindTarget(maskImage(),&depth->image,rw,rh);
    dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,0);
    bloomQuad(1,none,none,nullptr,nullptr,false,dt);
    resolveMask();
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    markDrawTime(-10);
    auto pass=[&](int to,int mode,uint32_t source,uint32_t second,float px,float py,const float *colour,bool barrier=true){
        const uint32_t size=(to==0 || to==6)?256u:((to==1 || to==5)?128u:64u);
        bindTarget(bloomChain[to]->image,nullptr,size,size);
        const float params[4]{px,py,1.0f/float(size),1.0f/float(size)};
        bloomQuad(mode,source,second,params,colour,false,-1.0f);
        if(barrier)tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    };
    const float maskColour[4]{bloom.maskColor[0],bloom.maskColor[1],bloom.maskColor[2],1.0f};
    pass(0,15,bloomMaskSlot,none,float(rw),float(rh),maskColour);
    pass(1,0,bloomChainSlot[0],none,0,0,nullptr);
    pass(2,0,bloomChainSlot[1],none,0,0,nullptr);
    pass(3,17,bloomChainSlot[2],bloomChainSlot[4],bloomHistory?bloom.temporal:0.0f,0,nullptr);
    const float taps=native_renderer::bloomTapAlpha(bloom.temporal);
    pass(4,16,bloomChainSlot[3],none,64.0f,taps,nullptr,false); // v127: 5 does not read 4
    pass(5,16,bloomChainSlot[3],none,128.0f,taps,nullptr);
    pass(6,0,bloomChainSlot[5],none,0,0,nullptr);
    markDrawTime(-11);
    bloomHistory=true;
    bindTarget(sceneImage(slot),&depth->image,rw,rh);
    const float scale[4]{float(rw)/512.0f,float(rh)/448.0f,0,0};
    const float colour[4]{bloom.color[0],bloom.color[1],bloom.color[2],bloom.color[3]};
    // v159: with only the resolved tail after it, added on the resolved (1x) picture in postQuad: one-one
    // blending of a value the same for the pixel's four samples, so the sum of the average is the average
    // of the sums (a sample clamped at white aside).
    if(diagBloom!=1){
        if(deferComposite){compositePending=true;std::copy_n(scale,4,compositeScale);std::copy_n(colour,4,compositeColour);}
        else bloomQuad(18,bloomChainSlot[6],none,scale,colour,true,-1.0f);
    }
    deferComposite=false;
    // Binding 1 back to the reduced background image for the scene's draws (configureTargets).
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
    ++lastBloomRuns;
}
// v176: the screen copy (GL `small`: the picture so far to 256 x 256, through 128 x 128 and back), made once a
// frame before its first draw; then the picture rebound. False when the picture is the output image itself
// (no separate scene image to read): those draws are left out, as before v176.
bool DekoBackend::makeScreenCopy(int slot,uint32_t use){
    if(screenCopyMade && screenCopyUse==use)return true;
    screenCopyUse=use;
    const uint32_t source=sceneReadSlot(slot); // v177: the output image too (no separate scene)
    if(source==DescriptorSlots::None || !screenCopy)return false;
    const auto none=DescriptorSlots::None;
    tcBarrier(DkBarrier_Fragments,0);
    if(msaa>1){const auto from=scene->view(),to=sceneResolved->view();tcFlush();dkCmdBufResolveImage(commands,&from,&to);}
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    const float full[4]{0,0,1.0f/256.0f,1.0f/256.0f},half[4]{0,0,1.0f/128.0f,1.0f/128.0f};
    bindTarget(screenCopy->image,nullptr,256,256);
    bloomQuad(0,source,none,full,nullptr,false,-1.0f);
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    bindTarget(screenCopyHalf->image,nullptr,128,128);
    bloomQuad(0,screenCopySlot,none,half,nullptr,false,-1.0f);
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    bindTarget(screenCopy->image,nullptr,256,256);
    bloomQuad(20,screenCopyHalfSlot,none,full,nullptr,false,-1.0f);
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    bindTarget(sceneImage(slot),&depth->image,rw,rh);
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3)); // as runBloom leaves it
    screenCopyMade=true;++lastScreenCopies;
    return true;
}
// v178: GL's mirror pass: the reflectionSource draws (not the sky's under the fog, as GL's pass) into the
// 256 x 256 mirror on a depth of its own, cleared to the fog colour (reflectionBegin); then the picture rebound.
bool DekoBackend::drawMirror(int slot,const std::vector<Draw> &all){
    if(!mirror || !mirrorDepth)return false;
    tcBarrier(DkBarrier_Fragments,0);
    bindTarget(mirror->image,&mirrorDepth->image,256,256);
    dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,fog.colour[0],fog.colour[1],fog.colour[2],1);
    dkCmdBufClearDepthStencil(commands,true,1,0xFF,0);
    const bool fogOn=fog.enabled && fog.width>0.0f;
    const bool sky=skyActive;skyActive=false; // (no fade to the background image: it is the picture's size)
    for(const auto &d:all){
        if(!d.state.reflectionSource || d.state.shadowVolume || (fogOn && d.state.background))continue;
        emitDraw(d,false,false);
        if(needsAlphaFailurePass(d.state,d.mesh->minAlpha,d.texture->minAlpha))emitDraw(d,true,false);
        ++lastMirrorDraws;
    }
    skyActive=sky;
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    bindTarget(sceneImage(slot),&depth->image,rw,rh);
    ++lastMirrors;
    return true;
}
// v177: the picture so far as a texture: the scene image (resolved by the caller at 4x), or, with no
// separate scene, the output image itself (its descriptor made once per swapchain).
uint32_t DekoBackend::sceneReadSlot(int slot){
    if(scaled())return sceneSlot;
    if(slot<0 || slot>=int(Frames) || !colors[slot])return DescriptorSlots::None;
    if(colorReadSlot[slot]==DescriptorSlots::None)colorReadSlot[slot]=imageDescriptors->slots.acquire();
    if(colorReadSlot[slot]==DescriptorSlots::None)return DescriptorSlots::None;
    if(colorReadGen[slot]!=swapGeneration){
        const auto view=colors[slot]->view();
        dkImageDescriptorInitialize(imageDescriptors->entries()+colorReadSlot[slot],&view,false,false);
        colorReadGen[slot]=swapGeneration;
    }
    return colorReadSlot[slot];
}
// v177: the blur's full-size copy (1x: the picture cannot be read while it is drawn into).
bool DekoBackend::ensureFullCopy(){
    if(fullCopy)return true;
    if(blurFailed)return false;
    try{
        if(fullCopySlot==DescriptorSlots::None)fullCopySlot=imageDescriptors->slots.acquire();
        if(fullCopySlot==DescriptorSlots::None)throw std::runtime_error("no descriptor slot");
        fullCopy=std::make_shared<Image>(device,rw,rh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender,allocationBudget);
        const auto view=fullCopy->view();
        dkImageDescriptorInitialize(imageDescriptors->entries()+fullCopySlot,&view,false,false);
        return true;
    }catch(const std::exception &e){
        std::fprintf(stderr,"[fx177] blur copy not made: %s\n",e.what());
        fullCopy.reset();blurFailed=true;return false;
    }
}
// v177: the glow layer at the scene's size and samples (drawn on the scene's depth), its resolve, its steps.
bool DekoBackend::ensureGlowTargets(){
    if(glowMs && glowSmall[3])return true;
    if(glowFailed)return false;
    try{
        if(glowSlot==DescriptorSlots::None)glowSlot=imageDescriptors->slots.acquire();
        for(auto &s:glowSmallSlot)if(s==DescriptorSlots::None)s=imageDescriptors->slots.acquire();
        if(glowSlot==DescriptorSlots::None || glowSmallSlot[3]==DescriptorSlots::None)throw std::runtime_error("no descriptor slot");
        const DkMsMode ms=msaa>=4?DkMsMode_4x:msaa>=2?DkMsMode_2x:DkMsMode_1x;
        glowMs=std::make_shared<Image>(device,rw,rh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|targetFlags,allocationBudget,1,
            std::shared_ptr<BufferPool>{},ms);
        if(msaa>1)glowResolved=std::make_shared<Image>(device,rw,rh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender,allocationBudget);
        const auto view=(msaa>1?glowResolved:glowMs)->view();
        dkImageDescriptorInitialize(imageDescriptors->entries()+glowSlot,&view,false,false);
        for(int i=0;i<4;++i){
            if(glowSmall[i])continue;
            const uint32_t size=i==0?256u:i==1?128u:64u;
            glowSmall[i]=std::make_shared<Image>(device,size,size,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender,allocationBudget);
            const auto smallView=glowSmall[i]->view();
            dkImageDescriptorInitialize(imageDescriptors->entries()+glowSmallSlot[i],&smallView,false,false);
        }
        return true;
    }catch(const std::exception &e){
        std::fprintf(stderr,"[fx177] glow layer not made: %s (layer 8 drawn plainly)\n",e.what());
        glowMs.reset();glowResolved.reset();glowFailed=true;return false;
    }
}
// v177: GL doBlur: the picture so far laid over itself `steps` times, turned, scaled and moved by
// (steps - i) / steps of the blur, at alpha 1 / (i + 1) (QuadBlend::Over), from a copy taken before.
void DekoBackend::runBlur(const ScreenBlur &b,int slot){
    if(b.steps<=0 || !effectsShader){++lastEffectDrops;return;}
    const auto none=DescriptorSlots::None;
    uint32_t source=none;
    tcBarrier(DkBarrier_Fragments,0);
    if(msaa>1){
        const auto from=scene->view(),to=sceneResolved->view();tcFlush();dkCmdBufResolveImage(commands,&from,&to);
        tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
        source=sceneSlot;
    }else{
        const uint32_t picture=sceneReadSlot(slot);
        if(picture==none || !ensureFullCopy()){++lastEffectDrops;return;}
        tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
        bindTarget(fullCopy->image,nullptr,rw,rh);
        const float whole[4]{0,0,1.0f/float(rw),1.0f/float(rh)};
        bloomQuad(0,picture,none,whole,nullptr,0,-1.0f);
        tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
        source=fullCopySlot;
    }
    bindTarget(sceneImage(slot),&depth->image,rw,rh);
    const DkShader *saved=quadShaderOverride;quadShaderOverride=&effectsShader->shader;
    // v183 (report: the camera blur cost ~25% of the frame rate while turning): the steps in one pass (effects
    // mode 25: the same turns and the same Over weights, one blend instead of `steps` full-screen ones).
    // CELOSIA_DIAG_BLUR183=0: one pass a step (v182).
    static const bool onePass=[]{const char *v=std::getenv("CELOSIA_DIAG_BLUR183");return !(v && std::atoi(v)==0);}();
    if(onePass && b.steps<=64){
        const float params[4]{float(b.steps),0,1.0f/float(rw),1.0f/float(rh)};
        const float turn[4]{b.scale-1.0f,std::fabs(b.angle)>=0.01570796f?b.angle:0.0f,b.dx/256.0f,b.dy/224.0f};
        bloomQuad(25,source,none,params,turn,3,-1.0f);
    }else
    for(int i=0;i<b.steps;++i){
        const float f=float(b.steps-i)/float(b.steps);
        // GL's turn (-angle) and move (dx, -dy) are y-up; here rows run down: angle, (dx, dy).
        const float params[4]{1.0f/float(i+1),0,1.0f/float(rw),1.0f/float(rh)};
        const float turn[4]{(b.scale-1.0f)*f+1.0f,std::fabs(b.angle)>=0.01570796f?b.angle*f:0.0f,b.dx*f/256.0f,b.dy*f/224.0f};
        bloomQuad(22,source,none,params,turn,3,-1.0f);
    }
    quadShaderOverride=saved;
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3)); // as runBloom leaves it
    ++lastBlurs;
}
// v177: GL doDof: the picture to 256 x 256, blurred `rounds` times through 128 x 128, laid over in
// `slices` passes: pass i where the GS Z is at most focusZ >> i (a quad at that depth, tested), alpha
// (i + 1) / slices; the passes' product is GL's single-pass keep.
void DekoBackend::runDof(int slot){
    const uint32_t source=sceneReadSlot(slot);
    if(!effectsShader || source==DescriptorSlots::None || !screenCopy || !(dof.zB>0.0f) || dof.slices<=0){++lastEffectDrops;return;}
    const auto none=DescriptorSlots::None;
    tcBarrier(DkBarrier_Fragments,0);
    if(msaa>1){const auto from=scene->view(),to=sceneResolved->view();tcFlush();dkCmdBufResolveImage(commands,&from,&to);}
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    const float full[4]{0,0,1.0f/256.0f,1.0f/256.0f},half[4]{0,0,1.0f/128.0f,1.0f/128.0f};
    bindTarget(screenCopy->image,nullptr,256,256);
    bloomQuad(0,source,none,full,nullptr,0,-1.0f);
    for(int i=0;i<dof.rounds;++i){
        tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
        bindTarget(screenCopyHalf->image,nullptr,128,128);
        bloomQuad(0,screenCopySlot,none,half,nullptr,0,-1.0f);
        tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
        bindTarget(screenCopy->image,nullptr,256,256);
        bloomQuad(0,screenCopyHalfSlot,none,full,nullptr,0,-1.0f);
    }
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    screenCopyMade=false; // (the copy now holds the blurred picture: a later screen copy is made again)
    bindTarget(sceneImage(slot),&depth->image,rw,rh);
    const DkShader *saved=quadShaderOverride;quadShaderOverride=&effectsShader->shader;
    const float whole[4]{0,0,1.0f/float(rw),1.0f/float(rh)};
    for(int i=0;i<dof.slices;++i){
        const float z=float(dof.focusZ>>unsigned(i));
        const float at=std::clamp(1.0f-0.1f*(z-dof.zA)/dof.zB,0.0f,1.0f); // GS Z = zA + zB / w; depth = 1 - 0.1 / w
        const float alpha[4]{0,0,0,float(i+1)/float(dof.slices)};
        bloomQuad(21,screenCopySlot,none,whole,alpha,3,at);
    }
    quadShaderOverride=saved;
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
    ++lastDofs;
}
// v177: GL doGlow: layer `glow.order` drawn apart (cleared to nothing, on the scene's depth) ...
bool DekoBackend::beginGlow(){
    if(!effectsShader || !ensureGlowTargets())return false;
    tcBarrier(DkBarrier_Fragments,0);
    bindTarget(glowMs->image,&depth->image,rw,rh);
    dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,0);
    ++lastGlows;
    return true;
}
// ... then reduced to 256, 128, 64 and blurred (GL buildGlow's reductions, mode 13 with their half-texel
// shifts), and laid over the picture sharp (amountA) and blurred (amountB) in the tint (mode 14, Over).
void DekoBackend::endGlow(int slot){
    const auto none=DescriptorSlots::None;
    tcBarrier(DkBarrier_Fragments,0);
    if(msaa>1){const auto from=glowMs->view(),to=glowResolved->view();tcFlush();dkCmdBufResolveImage(commands,&from,&to);}
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    const DkShader *saved=quadShaderOverride;quadShaderOverride=&effectsShader->shader;
    const bool blurred=glow.amountB>0.0f;
    if(blurred){
        const uint32_t sizes[4]{256,128,64,64};
        auto reduce=[&](uint32_t from,uint32_t fromW,uint32_t fromH,int to,float shift){
            bindTarget(glowSmall[to]->image,nullptr,sizes[to],sizes[to]);
            const float p[4]{shift/float(fromW),shift/float(fromH),1.0f/float(sizes[to]),1.0f/float(sizes[to])};
            bloomQuad(23,from,none,p,nullptr,0,-1.0f);
            tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
        };
        reduce(glowSlot,rw,rh,0,0.0f);
        if(glow.rounds>0){
            reduce(glowSmallSlot[0],256,256,1,0.0f);
            if(glow.rounds>1){
                reduce(glowSmallSlot[1],128,128,2,0.0f);
                if(glow.rounds>2)
                    for(int i=0;i<=glow.rounds;++i){reduce(glowSmallSlot[2],64,64,3,0.5f);reduce(glowSmallSlot[3],64,64,2,-0.5f);}
                reduce(glowSmallSlot[2],64,64,1,0.0f);
            }
            reduce(glowSmallSlot[1],128,128,0,0.0f);
        }
    }
    bindTarget(sceneImage(slot),&depth->image,rw,rh);
    const float whole[4]{0,0,1.0f/float(rw),1.0f/float(rh)};
    if(glow.amountA>0.0f){
        const float tint[4]{glow.tint[0],glow.tint[1],glow.tint[2],glow.amountA};
        bloomQuad(24,glowSlot,none,whole,tint,3,-1.0f);
    }
    if(blurred){
        const float tint[4]{glow.tint[0],glow.tint[1],glow.tint[2],glow.amountB};
        bloomQuad(24,glowSmallSlot[0],none,whole,tint,3,-1.0f);
    }
    quadShaderOverride=saved;
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
}
// v150: every target of the scene's size made again at width x height (every frame's fence waited: no
// GPU work reads the old ones), their descriptors rewritten in their slots.
// v151: the swapchain rebuilt at width x height: the queue idle (no presented image still in use by
// this process's commands), every frame's fence waited, then the scene's targets for the new scale.
void DekoBackend::resizeOutput(uint32_t width,uint32_t height){
    presentDrain(); // v164
    for(auto &f:frames)
        if(f.pending && dkFenceWait(&f.fence,-1)!=DkResult_Success)throw std::runtime_error("deko3d: resize fence wait failed");
    dkQueueWaitIdle(queue);
    makeSwapchain(width,height);
    latestSlot=-1;latestCapture.reset();
    std::fprintf(stderr,"[resolution] output %ux%u\n",ow,oh);
    rw=0;rh=0; // the scene's targets made again (separate or not) by resizeScene this frame
}
void DekoBackend::resizeScene(uint32_t width,uint32_t height,uint32_t samples,bool post){
    for(auto &f:frames)
        if(f.pending && dkFenceWait(&f.fence,-1)!=DkResult_Success)throw std::runtime_error("deko3d: resize fence wait failed");
    rw=width;rh=height;msaa=samples;postOn=post;
    separate=rw!=ow || rh!=oh || msaa>1 || postOn;
    const DkMsMode ms=msaa>=4?DkMsMode_4x:msaa>=2?DkMsMode_2x:DkMsMode_1x;
    scene.reset();sceneResolved.reset();bloomMaskMs.reset();depth.reset();
    fullCopy.reset();glowMs.reset();glowResolved.reset();blurFailed=glowFailed=false; // v177 (made again on use)
    depth=std::make_shared<Image>(device,rw,rh,DkImageFormat_ZF32_X24S8,DkImageFlags_UsageRender|targetFlags,allocationBudget,1,
        std::shared_ptr<BufferPool>{},ms);
    if(msaa>1)bloomMaskMs=std::make_shared<Image>(device,rw,rh,maskFormatNow,DkImageFlags_UsageRender,allocationBudget,1,
        std::shared_ptr<BufferPool>{},ms);
    const auto describe=[&](const std::shared_ptr<Image> &image,uint32_t slot){
        const auto view=image->view();
        dkImageDescriptorInitialize(imageDescriptors->entries()+slot,&view,false,false);
    };
    bloomMask=std::make_shared<Image>(device,rw,rh,maskFormatNow,DkImageFlags_UsageRender,allocationBudget);
    describe(bloomMask,bloomMaskSlot);
    for(int i=0;i<2;++i){
        shadowSmall[i]=std::make_shared<Image>(device,rw/4,rh/4,maskFormatNow,DkImageFlags_UsageRender,allocationBudget);
        describe(shadowSmall[i],shadowSmallSlot[i]);
    }
    if(separate){
        scene=std::make_shared<Image>(device,rw,rh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|targetFlags,allocationBudget,1,
            std::shared_ptr<BufferPool>{},ms);
        if(msaa>1){
            sceneResolved=std::make_shared<Image>(device,rw,rh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender,allocationBudget);
            describe(sceneResolved,sceneSlot);
        }else describe(scene,sceneSlot);
    }
    // v160: the compute post's output and its 16 constant sets (FXAA x 4 sharpening levels x debanding),
    // written here, every frame's fence waited above.
    postOut.reset();
    if(separate && rw==ow && rh==oh && postComputeShader && postConstants && postOutSlot!=DescriptorSlots::None && postOutTexSlot!=DescriptorSlots::None){
        postOut=std::make_shared<Image>(device,ow,oh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageLoadStore,allocationBudget);
        const auto view=postOut->view();
        dkImageDescriptorInitialize(imageDescriptors->entries()+postOutSlot,&view,true,false);
        dkImageDescriptorInitialize(imageDescriptors->entries()+postOutTexSlot,&view,false,false);
        static const float sharpness[4]{0.0f,0.35f,0.7f,1.0f};
        for(int i=0;i<16;++i){
            float *c=reinterpret_cast<float *>(static_cast<uint8_t *>(postConstants->cpu())+i*256);
            c[0]=(i&8)?1.0f:0.0f;c[1]=sharpness[(i>>1)&3];c[2]=0.0f;c[3]=(i&1)?1.0f:0.0f;
            c[4]=1.0f/float(rw);c[5]=1.0f/float(rh);c[6]=c[7]=0.0f;
        }
    }
    bloomHistory=false;
    std::fprintf(stderr,"[resolution] scene %ux%u msaa %u post %d (output %ux%u)\n",rw,rh,msaa,int(postOn),ow,oh);
}
void DekoBackend::bindSamples(bool multi){
    if(samplesBound && multi==(boundSamplesMulti!=0))return;
    DkMultisampleState state;dkMultisampleStateDefaults(&state);
    if(multi){state.mode=state.rasterizerMode=msaa>=4?DkMsMode_4x:DkMsMode_2x;}
    dkCmdBufBindMultisampleState(commands,&state);
    boundSamplesMulti=multi?1:0;samplesBound=true;
}
// v152: the multisampled mask averaged into the sampled one (the coverage of an edge as a fraction).
void DekoBackend::resolveMask(){
    if(!bloomMaskMs)return;
    tcBarrier(DkBarrier_Fragments,0);
    const auto from=bloomMaskMs->view(),to=bloomMask->view();
    tcFlush();dkCmdBufResolveImage(commands,&from,&to);
}
// v152: the scene onto the swapchain image (scaled, linear) with the post options; the menu after it.
void DekoBackend::bindSceneSamplers(){
    const int aniso=celosia::live::settings().anisotropy.load(std::memory_order_relaxed);
    const uint32_t set=aniso>=16?4u:aniso>=8?3u:aniso>=4?2u:aniso>=2?1u:0u;
    dkCmdBufBindSamplerDescriptorSet(commands, samplers->gpu()+(set*8+(diagGpuMode==3?4:0))*sizeof(DkSamplerDescriptor), 4);
}
// v164: the present thread (backend.hpp PresentJob). Each job: its display images ready on the render queue
// (fence), then for the generated (if any) and the real image: acquire, copy, present with its interval;
// then the pair's slot is free again (the copies' fence, waited on this thread before the slot's next use).
void DekoBackend::presentWorker(){
    ::native_renderer::threads::noteForBoost(); // v170: moved to core 3 by the 4-core boost
    for(;;){
        PresentJob job;
        {
            std::unique_lock<std::mutex> lock(presentMutex);
            presentCv.wait(lock,[&]{return presentStop || !presentJobs.empty();});
            if(presentStop && presentJobs.empty())return;
            job=presentJobs.front();
        }
        const uint32_t k=job.k;
        if(presentDonePending[k] && dkFenceWait(&presentDone[k],-1)!=DkResult_Success)std::abort();
        presentDonePending[k]=false;
        if(presentEndPending[k] && dkFenceWait(&presentEnd[k],-1)!=DkResult_Success)std::abort();
        presentEndPending[k]=false;
        dkCmdBufClear(presentCommands);
        dkCmdBufAddMemory(presentCommands,presentCmdMem[k]->handle(),presentCmdMem[k]->offset,presentCmdMem[k]->bytes);
        // v173p CELOSIA_DIAG_PACE173=1 (diagnosis): each real frame's way to the display, the frames over 34 ms logged.
        static const bool pace=std::getenv("CELOSIA_DIAG_PACE173")!=nullptr;
        uint64_t tJob=armTicksToNs(armGetSystemTick()),tGpu=0,tGenAcq=0,tGenPres=0,tFree=0,tRealAcq=0,tRealPres=0;
        if(pace){dkFenceWait(&presentReady[k],-1);tGpu=armTicksToNs(armGetSystemTick());}
        dkQueueWaitFence(presentQueue,&presentReady[k]);
        static const bool genOnlyDiag=std::getenv("CELOSIA_DIAG_LSFG_GENONLY")!=nullptr;
        static const bool stageOn=std::getenv("CELOSIA_DIAG_NO_PRESENT_STAGE")==nullptr;
        const Image &realSource=job.real?*job.real:*dispReal[k];
        bool slotFreed=false;
        const bool staged=stageOn && !genOnlyDiag && presentStage;
        if(staged){
            // v173n: the real picture into the stage first (no display image needed for it).
            dkCmdBufBarrier(presentCommands,DkBarrier_Full,DkInvalidateFlags_Image);
            const DkImageView src=realSource.view(),dst=presentStage->view();
            const DkImageRect rect{0,0,0,dispW,dispH,1};
            dkCmdBufCopyImage(presentCommands,&src,&rect,&dst,&rect,0);
            dkCmdBufBarrier(presentCommands,DkBarrier_Full,DkInvalidateFlags_Image);
        }
        auto freeSlot=[&]{
            dkCmdBufSignalFence(presentCommands,&presentDone[k],true);
            dkQueueSubmitCommands(presentQueue,dkCmdBufFinishList(presentCommands));
            dkQueueFlush(presentQueue);
            presentDonePending[k]=true;
            {
                std::lock_guard<std::mutex> lock(presentMutex);
                presentBusy[k]=false;
            }
            presentCv.notify_all();
            slotFreed=true;
            tFree=armTicksToNs(armGetSystemTick());
        };
        auto show=[&](const Image &from,int interval,bool generated){
            const int target=dkQueueAcquireImage(presentQueue,swapchain);
            if(target<0 || target>=int(Frames))std::abort();
            (generated?tGenAcq:tRealAcq)=armTicksToNs(armGetSystemTick());
            dkCmdBufBarrier(presentCommands,DkBarrier_Full,DkInvalidateFlags_Image);
            const DkImageView src=from.view(),dst=colors[target]->view();
            const DkImageRect rect{0,0,0,dispW,dispH,1};
            dkCmdBufCopyImage(presentCommands,&src,&rect,&dst,&rect,0);
            dkCmdBufBarrier(presentCommands,DkBarrier_Full,DkInvalidateFlags_Image);
            dkQueueSubmitCommands(presentQueue,dkCmdBufFinishList(presentCommands));
            if(interval!=swapIntervalNow){dkSwapchainSetSwapInterval(swapchain,interval);swapIntervalNow=interval;}
            dkQueuePresentImage(presentQueue,swapchain,target);latestSlot=target;
            (generated?tGenPres:tRealPres)=armTicksToNs(armGetSystemTick());
            if(celosia_frame_presented)celosia_frame_presented(); // v173k: every image shown (the overlay counted pairs: 30)
            // v169: the shown frames' pacing (acquire returns as the display frees an image: the cadence the
            // eye sees). Every 600 presents: the mean interval before a generated / a real frame (even pacing:
            // equal), their spread, p99 and the share off the median by more than 25 %. Only this thread.
            {
                static uint64_t lastNs=0;static std::vector<float> iv[2];static std::vector<float> all;
                const uint64_t t=armTicksToNs(armGetSystemTick());
                if(lastNs){const float ms=float(t-lastNs)/1e6f;if(ms<250.0f){iv[generated?1:0].push_back(ms);all.push_back(ms);}}
                lastNs=t;
                if(all.size()>=600){
                    std::vector<float> sorted=all;std::sort(sorted.begin(),sorted.end());
                    const float med=sorted[sorted.size()/2],p99=sorted[sorted.size()*99/100];
                    double mean=0,sd=0;for(const float v:all)mean+=v;mean/=double(all.size());
                    for(const float v:all)sd+=(v-mean)*(v-mean);sd=std::sqrt(sd/double(all.size()));
                    size_t off=0;for(const float v:all)off+=std::fabs(v-med)>0.25f*med;
                    const auto avg=[](const std::vector<float> &v){double a=0;for(const float x:v)a+=x;return v.empty()?0.0:a/double(v.size());};
                    std::fprintf(stderr,"[lsfg pace] shown %zu median %.2f ms sd %.2f p99 %.2f off25 %.1f%% before-gen %.2f before-real %.2f\n",
                        all.size(),med,sd,p99,100.0*double(off)/double(all.size()),avg(iv[1]),avg(iv[0]));
                    all.clear();iv[0].clear();iv[1].clear();
                }
            }
        };
        // v173f CELOSIA_DIAG_LSFG_GENONLY=1 (diagnosis): the generated picture shown in place of the real one too, so
        // a screenshot shows what the generation makes (ghosting check).
        static const bool genOnly=std::getenv("CELOSIA_DIAG_LSFG_GENONLY")!=nullptr;
        if(job.gen)show(job.genImage?*job.genImage:*dispGen[k],job.intervalGen,true);
        // v173n: the slot's images are all read by now (gen copied, real staged): the render may use the slot again.
        if(staged)freeSlot();
        if(genOnly && job.gen)show(job.genImage?*job.genImage:*dispGen[k],job.intervalReal,false);
        else show(staged?*presentStage:realSource,job.intervalReal,false);
        // v173 diagnosis (user: generation stops when the camera turns, the external overlay agrees): each second,
        // the images this thread really presented, generated and real, and the frames held by the render (no
        // image at all), when a second is short of generated ones (or every 10 s).
        {
            static uint64_t start=0;static uint32_t g=0,r=0,secs=0;
            g+=job.gen?1u:0u;++r;
            const uint64_t t=armTicksToNs(armGetSystemTick());
            if(!start)start=t;
            else if(t-start>=1000000000ull){
                const uint32_t heldNow=fgHeldFrames.exchange(0,std::memory_order_relaxed);
                if(g+2<r || heldNow || ++secs%10==0)
                    std::fprintf(stderr,"[fgsec173] %.1f s: presented gen %u real %u, held frames %u\n",double(t-start)/1e9,g,r,heldNow);
                g=r=0;start=t;
            }
        }
        if(pace && job.cpuStart){
            static uint64_t lastStart=0,lastRealPres=0;static unsigned lines=0,jobs=0;
            const double interval=lastStart?double(job.cpuStart-lastStart)/1e6:0.0;
            const auto at=[&](uint64_t t){return t?double(int64_t(t-job.cpuStart))/1e6:-1.0;};
            if((interval>34.0 || ++jobs%300==0) && lines<3000){
                ++lines;
                std::fprintf(stderr,"[pace173] iv %.1f | gate %.1f | render wait %.1f to %.1f submit %.1f to %.1f push %.1f | present job %.1f gpu %.1f gen acq %.1f pres %.1f free %.1f real acq %.1f pres %.1f | prev real pres %.1f gen %d\n",
                    interval,job.gate/1e6,at(job.waitA),at(job.waitB),at(job.subA),at(job.subB),at(job.push),at(tJob),at(tGpu),at(tGenAcq),at(tGenPres),at(tFree),at(tRealAcq),at(tRealPres),at(lastRealPres),int(job.gen));
            }
            lastStart=job.cpuStart;lastRealPres=tRealPres;
        }
        if(!slotFreed){
            dkCmdBufSignalFence(presentCommands,&presentDone[k],true);
            dkQueueSubmitCommands(presentQueue,dkCmdBufFinishList(presentCommands));
            presentDonePending[k]=true;
        }
        dkCmdBufSignalFence(presentCommands,&presentEnd[k],false);
        dkQueueSubmitCommands(presentQueue,dkCmdBufFinishList(presentCommands));
        dkQueueFlush(presentQueue);
        presentEndPending[k]=true;
        {
            std::lock_guard<std::mutex> lock(presentMutex);
            presentJobs.pop_front();
            if(!slotFreed)presentBusy[k]=false; // v173o: not again once freed early (the render may hold slot k for its next job by now)
        }
        presentCv.notify_all();
    }
}
// v164: every job handed over presented and its copies done (before the render queue presents itself,
// rebuilds the swapchain or the display images).
void DekoBackend::presentDrain(){
    if(!presentUsed)return;
    {
        std::unique_lock<std::mutex> lock(presentMutex);
        presentCv.wait(lock,[&]{return presentJobs.empty() && !presentBusy[0] && !presentBusy[1];});
    }
    dkQueueWaitIdle(presentQueue);
    presentDonePending[0]=presentDonePending[1]=false;
    presentEndPending[0]=presentEndPending[1]=false;
    presentUsed=false;
}
void DekoBackend::ensureDisplayImages(){
    if(dispW==ow && dispH==oh && dispGen[0])return;
    presentDrain();
    for(uint32_t k=0;k<2;++k){
        dispGen[k]=std::make_shared<Image>(device,ow,oh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|targetFlags,allocationBudget);
        dispReal[k]=std::make_shared<Image>(device,ow,oh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|targetFlags,allocationBudget);
    }
    presentStage=std::make_shared<Image>(device,ow,oh,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|targetFlags,allocationBudget);
    dispW=ow;dispH=oh;
}
// v153: an image of the frame generation copied onto a newly acquired swapchain image, the menu drawn
// over it, then submitted and presented (the frame's fence is signalled by the frame's last submission).
void DekoBackend::presentCopy(const lsfg::Img &image,Frame &frame,int interval){
    // v156: the work so far goes to the GPU before waiting for a free display image (that wait blocks the
    // CPU; with the work still unsubmitted the GPU sat idle through it).
    dkQueueSubmitCommands(queue,tcFinish());
    dkQueueFlush(queue);
    const int target=dkQueueAcquireImage(queue,swapchain);
    if(target<0 || target>=int(Frames))throw std::runtime_error("deko3d: acquire failed");
    tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
    if(blitCopies && frameGen && image.w==ow && image.h==oh){
        // v161: drawn (one tap per pixel through the ROPs) instead of copied: v160's copy+menu stage 5-9 ms.
        bindTarget(colors[target]->image,nullptr,ow,oh);
        blitTexture(dkMakeTextureHandle(image.tex,2),frameGen->imageSetGpu(),frameGen->imageSlots(),frameGen->samplerSetGpu(),3);
    }else{
        const DkImageView from=image.image->view(),to=colors[target]->view();
        const DkImageRect rect{0,0,0,ow,oh,1};
        tcFlush();dkCmdBufCopyImage(commands,&from,&rect,&to,&rect,0);
        tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
        bindTarget(colors[target]->image,nullptr,ow,oh);
    }
    boundImages=0;boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
    bindSceneSamplers();
    for(const std::size_t i:menuDraws){
        const auto &draw=frame.draws[i];
        emitDraw(draw,false,false);
        if(needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha))emitDraw(draw,true,false);
    }
    dkQueueSubmitCommands(queue,tcFinish());
    if(interval!=swapIntervalNow){dkSwapchainSetSwapInterval(swapchain,interval);swapIntervalNow=interval;}
    dkQueuePresentImage(queue,swapchain,target);latestSlot=target;
    if(celosia_frame_presented)celosia_frame_presented();
}
void DekoBackend::blitTexture(DkResHandle handle,DkGpuAddr imageSet,uint32_t imageSlots,DkGpuAddr samplerSet,uint32_t samplerSlots){
    const DkShader *shaders[]{&vertexShader->shader,&bloomFragmentShader->shader};
    dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);
    DkDepthStencilState depthState;dkDepthStencilStateDefaults(&depthState);
    depthState.depthTestEnable=false;depthState.depthWriteEnable=false;
    dkCmdBufBindDepthStencilState(commands,&depthState);
    DkColorWriteState write;dkColorWriteStateDefaults(&write);
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colourState;dkColorStateDefaults(&colourState);colourState.blendEnableMask=0;
    dkCmdBufBindColorState(commands,&colourState);
    Material material{};
    material.clip[0]=material.clip[5]=material.clip[15]=1;material.clip[10]=1;
    material.alpha[0]=0.0f; // bloom.frag mode 0: texture(image, gl_FragCoord.xy x params.zw).rgb, alpha 1
    material.params[2]=1.0f/float(ow);material.params[3]=1.0f/float(oh);
    pushMaterial(material);
    if(imageSet){dkCmdBufBindImageDescriptorSet(commands,imageSet,imageSlots);boundImages=0;}
    else{
        const auto set=imageDescriptors->storage->gpu();
        if(boundImages!=set){dkCmdBufBindImageDescriptorSet(commands,set,ImageDescriptorSlots);boundImages=set;++lastDescriptorSetBinds;}
    }
    if(samplerSet)dkCmdBufBindSamplerDescriptorSet(commands,samplerSet,samplerSlots);
    dkCmdBufBindTexture(commands,DkStage_Fragment,0,handle);
    bindVtxFormat(0);dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    if(samplerSet)bindSceneSamplers();
    boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
}
// v159: a screen draw that may go on the resolved picture: no depth, no failure pass, normal or no blending,
// and every vertex on or outside the picture's border (within half a pixel): its triangles cover the
// whole picture or lie along its edges, so no edge crosses a pixel inside it. Its fragments run once per
// pixel at the centre either way (no sample shading), and a normal blend of the four samples' average is
// the average of the four blends (linear), so the resolved picture is the same (8-bit rounding aside).
bool DekoBackend::resolvedEligible(const Draw &draw) const{
    const auto &s=draw.state;const Mesh &m=*draw.mesh;
    if(!s.screen || s.shadowVolume || s.background || s.screenCopy || s.previousFrame || s.screenDepthTest || s.overlay)return false;
    if(s.blend!=Blend::Normal && s.blend!=Blend::Opaque)return false;
    if(pipelineKey(s,false).depth!=0 || needsAlphaFailurePass(s,m.minAlpha,draw.texture->minAlpha))return false;
    if(draw.faint)return true;
    if(m.floatColour || !m.vertices || !m.count || m.count>64 || m.indices)return false;
    const uint8_t *vb=static_cast<const uint8_t *>(m.vertices->cpu())+m.vertexOffset;const uint32_t vs=m.stride();
    struct P{float x,y,z;};
    const auto at=[&](uint32_t i){P p;std::memcpy(&p,vb+size_t(i)*vs,sizeof(p));return p;};
    const float *c=draw.clip.data();
    const float tx=1.0f-1.0f/float(rw),ty=1.0f-1.0f/float(rh);
    bool corners=true;
    float px[64],py[64];
    for(uint32_t i=0;i<m.count;++i){
        const P q=at(i);
        const float x=c[0]*q.x+c[4]*q.y+c[8]*q.z+c[12],y=c[1]*q.x+c[5]*q.y+c[9]*q.z+c[13];
        const float w=c[3]*q.x+c[7]*q.y+c[11]*q.z+c[15];
        if(!(w>0.0f) || !std::isfinite(x) || !std::isfinite(y))return false;
        if(std::fabs(x/w)<tx || std::fabs(y/w)<ty)corners=false;
        px[i]=(x/w*0.5f+0.5f)*float(rw);py[i]=(y/w*0.5f+0.5f)*float(rh);
    }
    if(corners)return true;
    // v167 (CELOSIA_DIAG_NO_TAIL_RECTS=1: off): rectangles on the pixel grid (the cinematic's letterbox bars):
    // every vertex on a pixel corner and the triangles in pairs, each pair one axis-aligned rectangle split
    // along a diagonal. Every pixel is then wholly in or out (no partial coverage): at 1x after the resolve
    // the same pixels, as the full-screen draws.
    if(!tailRects || m.count%6)return false;
    for(uint32_t i=0;i<m.count;++i)
        if(std::fabs(px[i]-std::round(px[i]))>0.01f || std::fabs(py[i]-std::round(py[i]))>0.01f)return false;
    for(uint32_t q=0;q<m.count;q+=6){
        float xs[2],ys[2];int nx=0,ny=0;
        for(uint32_t i=q;i<q+6;++i){
            const float x=std::round(px[i]),y=std::round(py[i]);
            if(!(nx>0 && xs[0]==x) && !(nx>1 && xs[1]==x)){if(nx==2)return false;xs[nx++]=x;}
            if(!(ny>0 && ys[0]==y) && !(ny>1 && ys[1]==y)){if(ny==2)return false;ys[ny++]=y;}
        }
        if(nx!=2 || ny!=2)return false;
        // each triangle three distinct corners of the rectangle; the two share the diagonal (two corners)
        int shared=0;uint8_t seen[4]{};
        for(uint32_t t=0;t<2;++t){
            uint8_t mask=0;
            for(uint32_t v=0;v<3;++v){
                const uint32_t i=q+t*3+v;
                const int k=(std::round(px[i])==xs[1]?1:0)|(std::round(py[i])==ys[1]?2:0);
                if(mask&(1u<<k))return false;
                mask|=uint8_t(1u<<k);
            }
            for(int k=0;k<4;++k)if(mask&(1u<<k))++seen[k];
        }
        for(int k=0;k<4;++k)shared+=seen[k]==2;
        if(shared!=2 || seen[0]+seen[1]+seen[2]+seen[3]!=6 || !seen[0] || !seen[1] || !seen[2] || !seen[3])return false;
        // the shared corners are opposite (a diagonal), so the two triangles tile the rectangle
        const bool diag03=seen[0]==2 && seen[3]==2,diag12=seen[1]==2 && seen[2]==2;
        if(!diag03 && !diag12)return false;
    }
    ++lastTailRects;
    return true;
}
void DekoBackend::postQuad(const DkImage &target,uint32_t directSlot){
    auto &live=celosia::live::settings();
    // v169 (GPU rewrite #2): multisampled scene, no post pass and the output's size (the 60 FPS bench's
    // configuration: MSAA 2x, 1280 x 720): the samples resolved straight into the target (RGBA8, the same size)
    // instead of into sceneResolved and then a full-screen one-tap quad onto the target: the same texels, one
    // full-screen pass less. The bloom's composite (it reads only the chain) added on the target itself. Not
    // with screen draws after the resolve (they may read the resolved picture). CELOSIA_DIAG_NO_RESOLVEDIRECT169=1.
    static const bool resolveDirectOff=std::getenv("CELOSIA_DIAG_NO_RESOLVEDIRECT169")!=nullptr;
    if(msaa>1 && !postOn && rw==ow && rh==oh && resolvedDraws.empty() && directSlot==DescriptorSlots::None && diagGpuMode==0 &&
       !resolveDirectOff){
        tcBarrier(DkBarrier_Fragments,0);
        DkImageView to;dkImageViewDefaults(&to,&target);
        const auto from=scene->view();
        tcFlush();dkCmdBufResolveImage(commands,&from,&to);
        markDrawTime(-6);
        tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
        bindTarget(target,nullptr,ow,oh);
        if(compositePending){
            markSegment(GpuSegment::Screen);
            bloomQuad(18,bloomChainSlot[6],DescriptorSlots::None,compositeScale,compositeColour,true,-1.0f);
            markDrawTime(-5);
        }
        resolvedDraws.clear();resolvedFrom=nullptr;compositePending=false;
        markDrawTime(-7);
        boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
        return;
    }
    if(msaa>1){
        tcBarrier(DkBarrier_Fragments,0);
        const auto from=scene->view(),to=sceneResolved->view();
        tcFlush();dkCmdBufResolveImage(commands,&from,&to);
        markDrawTime(-6);
        if(compositePending){
            tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(sceneResolved->image,nullptr,rw,rh);
            markSegment(GpuSegment::Screen);
            bloomQuad(18,bloomChainSlot[6],DescriptorSlots::None,compositeScale,compositeColour,true,-1.0f);
            markDrawTime(-5);
        }
        if(!resolvedDraws.empty() && resolvedFrom){
            tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(sceneResolved->image,nullptr,rw,rh);
            boundImages=0;boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
            bindSceneSamplers();
            markSegment(GpuSegment::Screen);
            for(const std::size_t i:resolvedDraws){emitDraw((*resolvedFrom)[i],false,false);markDrawTime(int(i)*4);}
        }
    }
    resolvedDraws.clear();resolvedFrom=nullptr;compositePending=false;
    {
        // v160: FXAA / sharpening without upscaling as a compute pass (post.comp.glsl: each texel read once
        // per tile instead of 9-13 sampler taps per pixel), then copied onto the target.
        const int aa=live.antiAliasing.load(std::memory_order_relaxed),sharp=std::clamp(live.sharpen.load(std::memory_order_relaxed),0,3);
        const bool fx=aa==1 || aa==4,deband=live.deband.load(std::memory_order_relaxed)!=0;
        if(computePost && postOut && rw==ow && rh==oh && diagGpuMode==0 && (fx || sharp>0)){
            tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
            const auto set=imageDescriptors->storage->gpu();
            if(boundImages!=set){dkCmdBufBindImageDescriptorSet(commands,set,ImageDescriptorSlots);boundImages=set;++lastDescriptorSetBinds;}
            const DkShader *cs=&postComputeShader->shader;
            dkCmdBufBindShaders(commands,DkStageFlag_Compute,&cs,1);
            const int index=(fx?8:0)|(sharp<<1)|(deband?1:0);
            dkCmdBufBindUniformBuffer(commands,DkStage_Compute,0,postConstants->gpu()+DkGpuAddr(index)*256,256);
            dkCmdBufBindTexture(commands,DkStage_Compute,0,dkMakeTextureHandle(sceneSlot,3));
            dkCmdBufBindImage(commands,DkStage_Compute,0,dkMakeImageHandle(postOutSlot));
            // v168 (GPU rewrite #2): with frame generation the target (LSFG's input, the output's size, RGBA8,
            // load/store) written by the compute pass itself: no postOut copy and one full barrier less a real
            // frame; the same texels. CELOSIA_DIAG_NO_POSTDIRECT168=1: through postOut as before.
            static const bool direct168Off=std::getenv("CELOSIA_DIAG_NO_POSTDIRECT168")!=nullptr;
            if(directSlot!=DescriptorSlots::None && !direct168Off){
                dkCmdBufBindImage(commands,DkStage_Compute,0,dkMakeImageHandle(directSlot));
                dkCmdBufDispatchCompute(commands,(ow+15)/16,(oh+15)/16,1);
                tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
                bindTarget(target,nullptr,ow,oh);
                markDrawTime(-7);
                boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
                return;
            }
            dkCmdBufDispatchCompute(commands,(ow+15)/16,(oh+15)/16,1);
            tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
            if(blitCopies){
                bindTarget(target,nullptr,ow,oh);
                blitTexture(dkMakeTextureHandle(postOutTexSlot,3),0,0,0,0);
            }else{
                DkImageView from=postOut->view(),to;dkImageViewDefaults(&to,&target);
                const DkImageRect rect{0,0,0,ow,oh,1};
                tcFlush();dkCmdBufCopyImage(commands,&from,&rect,&to,&rect,0);
                tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
                bindTarget(target,nullptr,ow,oh);
            }
            markDrawTime(-7);
            boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
            return;
        }
    }
    tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
    bindTarget(target,nullptr,ow,oh);
    DkDepthStencilState depthState;dkDepthStencilStateDefaults(&depthState);
    depthState.depthTestEnable=false;depthState.depthWriteEnable=false;
    dkCmdBufBindDepthStencilState(commands,&depthState);
    DkColorWriteState write;dkColorWriteStateDefaults(&write);
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colourState;dkColorStateDefaults(&colourState);colourState.blendEnableMask=0;
    dkCmdBufBindColorState(commands,&colourState);
    Material material{};for(unsigned i=0;i<4;++i)material.clip[i*5]=1;
    material.params[0]=1.0f/float(ow);material.params[1]=1.0f/float(oh);material.alpha[0]=-1;
    const int aa=live.antiAliasing.load(std::memory_order_relaxed);
    static const float sharpness[4]{0.0f,0.35f,0.7f,1.0f};
    material.debug[0]=(aa==1 || aa==4)?1.0f:0.0f;
    material.debug[1]=sharpness[std::clamp(live.sharpen.load(std::memory_order_relaxed),0,3)];
    material.debug[2]=(rw!=ow || rh!=oh) && live.upscaleFilter.load(std::memory_order_relaxed)==1?1.0f:0.0f;
    material.debug[3]=live.deband.load(std::memory_order_relaxed)?1.0f:0.0f;
    material.bgFog[0]=1.0f/float(rw);material.bgFog[1]=1.0f/float(rh);material.bgFog[2]=float(rw);material.bgFog[3]=float(rh);
    {
        // v164: the post shader for the options on: FXAA (postfx: post.frag's FXAA path), none (postplain:
        // its one tap), else post.frag itself.
        const bool variants=postVariants && postFxShader && postPlainShader && diagGpuMode==0;
        const bool fxOn=material.debug[0]>0.5f,plain=!fxOn && material.debug[1]<=0.0f && material.debug[2]<=0.5f && material.debug[3]<=0.5f;
        const DkShader *fragment=variants && fxOn?&postFxShader->shader:variants && plain?&postPlainShader->shader:&postFragmentShader->shader;
        const DkShader *shaders[]{&vertexShader->shader,fragment};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);
    }
    pushMaterial(material);
    const auto set=imageDescriptors->storage->gpu();
    if(boundImages!=set){dkCmdBufBindImageDescriptorSet(commands,set,ImageDescriptorSlots);boundImages=set;++lastDescriptorSetBinds;}
    dkCmdBufBindTexture(commands,DkStage_Fragment,0,dkMakeTextureHandle(sceneSlot,3));
    bindVtxFormat(0);dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    markDrawTime(-7);
    boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
}
void DekoBackend::configureTargets(int slot, bool clearToFog) {
    boundDepth=boundMask=boundBlend=-1;boundImages=0;boundHandleValid=false;boundFragment=0;
    lastDescriptorSetBinds=lastTextureHandleBinds=0;
    DkImageView colour; dkImageViewDefaults(&colour, &sceneImage(slot));
    const auto z = depth->view();
    dkCmdBufBindRenderTarget(commands, &colour, &z);
    samplesBound=false;bindSamples(msaa>1);
    const DkViewport viewport{0, 0, float(rw), float(rh), 0, 1};
    const DkScissor scissor{0, 0, rw, rh};
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
    vtxFormat=-1;bindVtxFormat(0);
    dkCmdBufBindUniformBuffer(commands, DkStage_Vertex, 0, uniforms->gpu(), 512);
    dkCmdBufBindUniformBuffer(commands, DkStage_Fragment, 0, uniforms->gpu(), 512);
    bindSceneSamplers();
    // Binding 1: the reduced background image; sampled only by draws whose bgFog is on, and only
    // while the shared pool is bound (emitDraw turns bgFog off for privately described textures).
    dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
}
// v126: every scene fragment shader but the opaque one discards a = clamp(t.a * k * shade.a) < 0.02
// (k = 2 textured, 1 untextured with t.a = 1; shade.a = vertex alpha / 128 x opacity, times a fresnel
// factor <= 1). Texture filtering and the interpolation of shade.a are convex combinations, so no
// fragment of a triangle exceeds k x (texture max alpha / 255) x (its largest vertex alpha / 128) x
// opacity. A triangle under 0.0199 by that bound writes nothing (no colour, and these draws write no
// depth or stencil): it is left out, the others keep their order. Same picture, fewer fragments.
void DekoBackend::cullFaint(Draw &draw){
    const auto &s=draw.state;const Mesh &m=*draw.mesh;
    if(!faintCull || diagGpuMode!=0 || s.shadowVolume)return;
    if(!std::isfinite(s.opacity))return;
    const bool textured=s.texture!=0;
    const float scale=(textured?2.0f*float(draw.texture->maxAlpha)/255.0f:1.0f)*std::max(s.opacity,0.0f)/128.0f;
    const auto faintBelow=[&](uint8_t alpha){return float(alpha)*scale<0.0199f;};
    const bool whole=faintBelow(m.maxAlpha);
    if(!whole && (m.triangleAlpha.empty() || m.indices || !faintBelow(m.triangleFloor)))return;
    if(canUseOpaqueFragment(s,m.minAlpha,draw.texture->minAlpha,false) ||
       canUseOpaqueFragment(s,m.minAlpha,draw.texture->minAlpha,true))return;
    // v136: depth-writing draws too: no depth pre-pass for them (opaque fragments only), and every
    // non-opaque shader discards a < 0.02 (both passes) before its late depth write.
    if(whole){draw.faint=true;++lastFaintDraws;return;}
    const uint32_t triangles=uint32_t(m.triangleAlpha.size());
    uint32_t kept=0;
    for(uint32_t t=0;t<triangles;++t)kept+=faintBelow(m.triangleAlpha[t])?0u:1u;
    if(kept==triangles)return;
    if(!kept){draw.faint=true;++lastFaintDraws;lastFaintTriangles+=triangles;return;}
    keepScratch.clear();
    for(uint32_t t=0;t<triangles;++t)if(!faintBelow(m.triangleAlpha[t]))keepScratch.push_back(t);
    draw.mesh=subMesh(m,keepScratch,true);
    ++lastFaintPartial;lastFaintTriangles+=triangles-kept;
}
// v134: where a texel of alpha >= need can reach a filtered sample. Bilinear filtering reads texels
// i and i + 1 around p = u x W - 0.5, a convex mix, so a sample of alpha >= need needs one such texel:
// u x W in (i - 0.5, i + 1.5) of a passing texel. Taken as sixteen half-planes a u + b v <= c holding
// those squares (plus 1/16 texel). Null: a passing texel on the border (repeat reaches across), so
// no bound; empty: no texel passes.
const std::vector<float> *DekoBackend::alphaRegion(Texture &t,int need){
    if(t.alphaGrid.empty())return nullptr;
    need=std::clamp(need,1,255);
    if(t.regionState.empty()){t.regionState.assign(256,0);t.regions.resize(256);}
    uint8_t &state=t.regionState[need];
    if(state==1)return nullptr;
    auto &r=t.regions[need];
    if(state==2)return &r;
    const int W=t.gridW,H=t.gridH;
    float best[16];bool any=false;
    static const float dirs[16][2]={{1,0},{0.92388f,0.38268f},{0.70711f,0.70711f},{0.38268f,0.92388f},{0,1},{-0.38268f,0.92388f},
        {-0.70711f,0.70711f},{-0.92388f,0.38268f},{-1,0},{-0.92388f,-0.38268f},{-0.70711f,-0.70711f},{-0.38268f,-0.92388f},
        {0,-1},{0.38268f,-0.92388f},{0.70711f,-0.70711f},{0.92388f,-0.38268f}};
    for(int j=0;j<H;++j){
        int lo=-1,hi=-1;const uint8_t *row=&t.alphaGrid[size_t(j)*W];
        for(int i=0;i<W;++i)if(row[i]>=need){if(lo<0)lo=i;hi=i;}
        if(lo<0)continue;
        if(j==0 || j==H-1 || lo==0 || hi==W-1){state=1;return nullptr;}
        const float m=0.0625f;
        const float xs[2]{float(lo)-0.5f-m,float(hi)+1.5f+m},ys[2]{float(j)-0.5f-m,float(j)+1.5f+m};
        for(float x:xs)for(float y:ys)for(int k=0;k<16;++k){
            const float d=dirs[k][0]*x+dirs[k][1]*y;
            if(!any || d>best[k])best[k]=d;
            if(k==15)any=true;
        }
    }
    r.clear();
    if(any)for(int k=0;k<16;++k){r.push_back(dirs[k][0]*float(W));r.push_back(dirs[k][1]*float(H));r.push_back(best[k]);}
    state=2;return &r;
}
// v134: a fragment of a textured draw outside every alpha test writes only when
// a = T / 255 x 2 x va / 128 x opacity (x the fresnel factor, at most 1) >= 0.02, T the filtered texel
// alpha and va the interpolated vertex alpha: with one colour on a triangle, T >= X = 0.02 x 255 x 128
// / (2 va opacity). Each such triangle is clipped (Sutherland-Hodgman, in its plane: u and v are linear
// there) to the region of texels >= X (less a margin) and drawn as a fan in its place, the triangles
// in their order; the cut-away pixels are the ones the shader discards. Triangles with differing vertex
// colours, or spanning a texture repeat, are kept as they are.
// v135: the largest texel alpha over texels [i0, i1] x [j0, j1] (any integers: the texture repeats),
// from the smallest level of 2 x 2 maxima where the range spans at most four cells a side.
uint8_t DekoBackend::alphaMax(Texture &t,int i0,int i1,int j0,int j1,uint8_t clamp){
    const int W=t.gridW,H=t.gridH;
    // v137: a clamped axis reads its edge texels outside the texture: the range clamped to it.
    if(clamp&1){i0=std::clamp(i0,0,W-1);i1=std::clamp(i1,0,W-1);}
    if(clamp&2){j0=std::clamp(j0,0,H-1);j1=std::clamp(j1,0,H-1);}
    if(i1-i0+1>=W && j1-j0+1>=H)return t.maxAlpha;
    if(t.maxLevels.empty()){
        int w=W,h=H;const std::vector<uint8_t> *prev=&t.alphaGrid;
        while(w>1 || h>1){
            const int nw=std::max(w/2,1),nh=std::max(h/2,1);
            std::vector<uint8_t> next(size_t(nw)*nh);
            for(int y=0;y<nh;++y)for(int x=0;x<nw;++x){
                uint8_t m=0;
                for(int dy=0;dy<(h>1?2:1);++dy)for(int dx=0;dx<(w>1?2:1);++dx)
                    m=std::max(m,(*prev)[size_t(std::min(2*y+dy,h-1))*w+std::min(2*x+dx,w-1)]);
                next[size_t(y)*nw+x]=m;
            }
            t.maxLevels.push_back(std::move(next));prev=&t.maxLevels.back();w=nw;h=nh;
        }
    }
    const auto floorShift=[](int v,int l){return v>=0?(v>>l):-((-v+(1<<l)-1)>>l);};
    int level=0;
    while((floorShift(i1,level)-floorShift(i0,level)>3 || floorShift(j1,level)-floorShift(j0,level)>3) &&
          level<int(t.maxLevels.size()))++level;
    const int lw=std::max(W>>level,1),lh=std::max(H>>level,1);
    const std::vector<uint8_t> &g=level?t.maxLevels[level-1]:t.alphaGrid;
    const int ci0=floorShift(i0,level),ci1=floorShift(i1,level),cj0=floorShift(j0,level),cj1=floorShift(j1,level);
    uint8_t m=0;
    for(int cj=cj0;cj<=cj1 && cj<cj0+lh;++cj)for(int ci=ci0;ci<=ci1 && ci<ci0+lw;++ci)
        m=std::max(m,g[size_t(((cj%lh)+lh)%lh)*lw+((ci%lw)+lw)%lw]);
    return m;
}
// v135: a fragment of a textured draw outside every alpha test writes only when
// a = T / 255 x 2 x va / 128 x opacity (x the fresnel factor, at most 1) >= 0.02, T the filtered texel
// alpha (bilinear, convex: at most the largest of the texels it reads) and va the interpolated vertex
// alpha (at most the triangle's largest). A triangle whose largest vertex alpha with the largest texel
// its uv rectangle can read (one texel more on each side) stays below that writes no pixel: it is left
// out of the index list, the others drawn from the same vertices in their order. The same picture.
void DekoBackend::clipToTexture(Draw &draw){
    const auto &s=draw.state;
    if(!textureClip || diagGpuMode!=0 || draw.faint || s.shadowVolume)return;
    // v138 (diagnostics): which test stops the draws whose mesh is faint (largest vertex alpha < 64)
    const bool watch=draw.mesh->maxAlpha<64;
    auto gate=[&](int g){if(watch)++clipGate[g];};
    if(!s.texture || !draw.texture){gate(0);return;}
    Texture &tex=*draw.texture;const Mesh &m=*draw.mesh;
    if(tex.alphaGrid.empty()){gate(1);return;}
    // v169: the vertex copy is made later (kept meshes: once, on their first clipped draw).
    if(!m.cpuVertices && !m.vertices){gate(2);return;} // v170: frame meshes copied below too (when they get there)
    // v168 (GPU rewrite #1; the LSFG cinematic's heaviest frames: 238 alpha-tested foliage draws, 30.5 ms, most
    // of their fragments discarded): alpha-tested draws clipped too. Every fragment shader that runs them
    // (native, lean) discards a < 0.02 in the main pass and in the failure pass alike, before any write, so
    // the cut-away pixels (texels below the faint need) wrote nothing in either pass: the same picture and
    // depth. Not for a failure that writes depth (ZB_ONLY), whose pass may run another path. Small meshes
    // (foliage cards: a few triangles, a few % of the screen each) taken from 2 % coverage (else 15 %).
    // CELOSIA_DIAG_NO_CLIP168=1: alpha-tested draws left as before.
    static const bool clip168Off=std::getenv("CELOSIA_DIAG_NO_CLIP168")!=nullptr;
    const bool alphaTested=(s.alphaTest&1)!=0;
    if(alphaTested && (clip168Off || ((s.alphaTest>>12)&3)==2)){gate(4);return;}
    if(!std::isfinite(s.opacity) || !(s.opacity>0) || !std::isfinite(s.uvOffset[0]) || !std::isfinite(s.uvOffset[1])){gate(5);return;}
    if(!alphaTested && (canUseOpaqueFragment(s,m.minAlpha,tex.minAlpha,false) || canUseOpaqueFragment(s,m.minAlpha,tex.minAlpha,true))){gate(6);return;}
    if(!alphaTested && needsAlphaFailurePass(s,m.minAlpha,tex.minAlpha)){gate(7);return;}
    {
        // v169b: the mesh's own counts (the CPU copy may not exist yet: v169 read it here, a null pointer).
        const uint32_t vertices=m.cpuVertices?(m.cpuIndices.empty()?uint32_t(m.cpuVertices->size()):uint32_t(m.cpuIndices.size()))
                                             :(m.indices?m.indexCount:m.count);
        const float least=alphaTested && vertices<=96u?0.02f:0.15f;
        if(screenCoverage(draw.clip.data(),m.bounds,s.screen)<least){gate(8);return;}
    }
    // v169 (GPU rewrite #1): a kept mesh drawn with an alpha grid texture gets its CPU vertex copy here, once
    // (v168: copies only for faint meshes, so the foliage, vertex alpha 128, was never clipped: 3-7 draws a
    // frame). Read from its (uncached) vertex memory: float vertices as they are, packed ones with the normals
    // the vertex shader decodes. At most 48 MB of copies; meshes over 16384 vertices left as before.
    // CELOSIA_DIAG_NO_CLIPCOPY169=1: as v168.
    if(!m.cpuVertices){
        static const bool copyOff=std::getenv("CELOSIA_DIAG_NO_CLIPCOPY169")!=nullptr;
        static size_t copiedBytes=0;
        Mesh &mm=*draw.mesh;
        const uint32_t count=mm.count;
        // v170: a frame mesh (transient: rebuilt each frame) too, outside the 48 MB budget of the kept ones.
        if(copyOff || !mm.vertices || count==0 || count>16384u || (!mm.transient && copiedBytes>(48u<<20)) ||
           (mm.indices && mm.indexCount==0)){gate(2);return;}
        auto copy=std::make_shared<std::vector<Vertex>>(count);
        const uint8_t *base=static_cast<const uint8_t *>(mm.vertices->cpu())+mm.vertexOffset;
        if(mm.packed){
            const auto snorm=[](uint32_t q){const int32_t v=int32_t(q<<22)>>22;return std::max(-1.0f,float(v)/511.0f);};
            for(uint32_t i=0;i<count;++i){
                PackedVertex p;std::memcpy(&p,base+size_t(i)*sizeof(PackedVertex),sizeof(p));
                (*copy)[i]=Vertex{p.x,p.y,p.z,snorm(p.n&0x3FFu),snorm((p.n>>10)&0x3FFu),snorm((p.n>>20)&0x3FFu),p.u,p.v,p.r,p.g,p.b,p.a};
            }
        }else if(mm.floatColour){gate(2);return;}
        else std::memcpy(copy->data(),base,size_t(count)*sizeof(Vertex));
        if(mm.indices){
            mm.cpuIndices.resize(mm.indexCount);
            std::memcpy(mm.cpuIndices.data(),static_cast<const uint8_t *>(mm.indices->cpu())+mm.indexOffset,size_t(mm.indexCount)*sizeof(uint32_t));
        }
        if(!mm.transient)copiedBytes+=size_t(count)*sizeof(Vertex)+mm.cpuIndices.size()*4u;
        mm.cpuVertices=std::move(copy);
        ++lastClipCopies;
    }
    gate(9);
    const float W=float(tex.gridW),H=float(tex.gridH);
    const std::vector<Vertex> &cv=*m.cpuVertices;const bool listed=m.cpuIndices.empty();
    const auto idx=[&](uint32_t k){return listed?k:m.cpuIndices[k];};
    const uint32_t n=listed?uint32_t(cv.size()):uint32_t(m.cpuIndices.size());
    keepScratch.clear();
    for(uint32_t k=0;k+2<n;k+=3){
        const Vertex &a=cv[idx(k)],&b=cv[idx(k+1)],&c=cv[idx(k+2)];
        const uint8_t va=std::max({a.a,b.a,c.a});
        bool drop=va==0;
        if(!drop){
            const float x=0.02f*255.0f*128.0f/(2.0f*float(va)*s.opacity);
            const int need=int(std::floor(x*0.99f))-1;
            if(need>int(tex.maxAlpha))drop=true;
            else if(need>=1){
                const float u0=std::min({a.u,b.u,c.u})+s.uvOffset[0],u1=std::max({a.u,b.u,c.u})+s.uvOffset[0];
                const float v0=std::min({a.v,b.v,c.v})+s.uvOffset[1],v1=std::max({a.v,b.v,c.v})+s.uvOffset[1];
                if(std::isfinite(u0) && std::isfinite(u1) && std::isfinite(v0) && std::isfinite(v1) &&
                   u1-u0<1.0f && v1-v0<1.0f && std::fabs(u0)<1e5f && std::fabs(v0)<1e5f){
                    const int i0=int(std::floor(u0*W-0.5f))-1,i1=int(std::floor(u1*W-0.5f))+2;
                    const int j0=int(std::floor(v0*H-0.5f))-1,j1=int(std::floor(v1*H-0.5f))+2;
                    if(int(alphaMax(tex,i0,i1,j0,j1,s.clamp))<need)drop=true;
                }
            }
        }
        if(!drop)keepScratch.push_back(k);
    }
    const uint32_t triangles=n/3,kept=uint32_t(keepScratch.size());
    if(clipLogged<40 && n>=60){
        ++clipLogged;
        float su=0,sv=0,mu=0,mv=0;uint8_t vlo=255,vhi=0;
        for(uint32_t k=0;k+2<n;k+=3){
            const Vertex &a=cv[idx(k)],&b=cv[idx(k+1)],&c=cv[idx(k+2)];
            const float du=std::max({a.u,b.u,c.u})-std::min({a.u,b.u,c.u}),dv=std::max({a.v,b.v,c.v})-std::min({a.v,b.v,c.v});
            su+=du;sv+=dv;mu=std::max(mu,du);mv=std::max(mv,dv);
            const uint8_t va=std::max({a.a,b.a,c.a});vlo=std::min(vlo,va);vhi=std::max(vhi,va);
        }
        unsigned hist[8]{};for(const uint8_t v:tex.alphaGrid)++hist[v>>5];
        unsigned border=0;for(int i=0;i<tex.gridW;++i)border=std::max({border,unsigned(tex.alphaGrid[i]),unsigned(tex.alphaGrid[size_t(tex.gridH-1)*tex.gridW+i])});
        std::fprintf(stderr,"[clip] tex=%llx %ux%u clamp=%u op=%.3f tris=%u kept=%u uvspan avg %.3f %.3f max %.3f %.3f va %u..%u texmax %u border %u hist %u %u %u %u %u %u %u %u off %.3f %.3f\n",
            static_cast<unsigned long long>(s.texture),unsigned(tex.gridW),unsigned(tex.gridH),unsigned(s.clamp),s.opacity,triangles,kept,
            su/float(std::max(triangles,1u)),sv/float(std::max(triangles,1u)),mu,mv,unsigned(vlo),unsigned(vhi),unsigned(tex.maxAlpha),border,
            hist[0],hist[1],hist[2],hist[3],hist[4],hist[5],hist[6],hist[7],s.uvOffset[0],s.uvOffset[1]);
    }
    // v140: the kept triangles of one colour clipped to the region of texels that can pass (alphaRegion),
    // each drawn as a fan in its place; the cut-away pixels are ones the shader discards.
    clipFloat.clear();bool clippedAny=false;uint32_t clippedTris=0;
    ClipVertex poly[2][40];
    const auto toFloat=[](const Vertex &q){return ClipVertex{q.x,q.y,q.z,q.nx,q.ny,q.nz,q.u,q.v,float(q.r),float(q.g),float(q.b),float(q.a)};};
    for(const uint32_t k:keepScratch){
        const Vertex &a=cv[idx(k)],&b=cv[idx(k+1)],&c=cv[idx(k+2)];
        const auto keep=[&]{clipFloat.push_back(toFloat(a));clipFloat.push_back(toFloat(b));clipFloat.push_back(toFloat(c));};
        // v141: the interpolated vertex alpha is at most the triangle's largest.
        const uint8_t va=std::max({a.a,b.a,c.a});
        const float x=0.02f*255.0f*128.0f/(2.0f*float(va)*s.opacity);
        const int need=int(std::floor(x*0.99f))-1;
        if(need<1){keep();continue;}
        const float u0=std::min({a.u,b.u,c.u})+s.uvOffset[0],u1=std::max({a.u,b.u,c.u})+s.uvOffset[0];
        const float v0=std::min({a.v,b.v,c.v})+s.uvOffset[1],v1=std::max({a.v,b.v,c.v})+s.uvOffset[1];
        if(!std::isfinite(u0) || !std::isfinite(u1) || !std::isfinite(v0) || !std::isfinite(v1) || std::fabs(u0)>1e5f || std::fabs(v0)>1e5f){keep();continue;}
        const float ku=std::floor(u0),kv=std::floor(v0);
        if(u1>ku+1.0f || v1>kv+1.0f || ((s.clamp&1) && ku!=0.0f) || ((s.clamp&2) && kv!=0.0f)){keep();continue;}
        const auto *region=alphaRegion(tex,need);
        if(!region){keep();continue;}
        if(region->empty()){clippedAny=true;++clippedTris;continue;}
        const float ou=s.uvOffset[0]-ku,ov=s.uvOffset[1]-kv;
        const auto inside=[&](const auto &q,size_t h){return (*region)[h+2]-((*region)[h]*(q.u+ou)+(*region)[h+1]*(q.v+ov));};
        bool all=true;
        for(size_t h=0;h<region->size() && all;h+=3)all=inside(a,h)>=0 && inside(b,h)>=0 && inside(c,h)>=0;
        if(all){keep();continue;}
        int count=3,cur=0;poly[0][0]=toFloat(a);poly[0][1]=toFloat(b);poly[0][2]=toFloat(c);
        for(size_t h=0;h<region->size() && count>=3;h+=3){
            const ClipVertex *in=poly[cur];ClipVertex *out=poly[cur^1];int outCount=0;
            for(int i=0;i<count && outCount<38;++i){
                const ClipVertex &p=in[i],&q=in[(i+1)%count];
                const float fp=inside(p,h),fq=inside(q,h);
                if(fp>=0)out[outCount++]=p;
                if((fp>=0)!=(fq>=0)){
                    const float t=fp/(fp-fq);
                    const float *pp=&p.x,*qq=&q.x;ClipVertex r;float *rr=&r.x;
                    for(int f=0;f<12;++f)rr[f]=pp[f]+(qq[f]-pp[f])*t;
                    out[outCount++]=r;
                }
            }
            if(outCount>=38){count=-1;break;}
            count=outCount;cur^=1;
        }
        if(count<0){keep();continue;}
        clippedAny=true;++clippedTris;
        for(int i=1;i+1<count;++i){clipFloat.push_back(poly[cur][0]);clipFloat.push_back(poly[cur][i]);clipFloat.push_back(poly[cur][i+1]);}
    }
    if(clippedAny){
        ++lastClipDraws;lastClipTriangles+=triangles-kept+clippedTris;
        if(clipFloat.empty()){draw.faint=true;return;}
        auto mesh=std::make_shared<Mesh>();
        const size_t bytes=clipFloat.size()*sizeof(ClipVertex);
        mesh->vertices=vertexStorage(bytes,mesh->vertexOffset,true);
        std::memcpy(static_cast<uint8_t *>(mesh->vertices->cpu())+mesh->vertexOffset,clipFloat.data(),bytes);
        mesh->count=uint32_t(clipFloat.size());mesh->floatColour=true;
        mesh->minAlpha=m.minAlpha;mesh->maxAlpha=m.maxAlpha;mesh->bounds=m.bounds;mesh->transient=true;
        draw.mesh=std::move(mesh);
        return;
    }
    if(kept==triangles)return;
    ++lastClipDraws;lastClipTriangles+=triangles-kept;
    if(!kept){draw.faint=true;return;}
    auto mesh=std::make_shared<Mesh>();
    mesh->vertices=m.vertices;mesh->vertexOffset=m.vertexOffset;mesh->count=m.count;mesh->packed=m.packed;
    mesh->minAlpha=m.minAlpha;mesh->maxAlpha=m.maxAlpha;mesh->bounds=m.bounds;
    mesh->indexCount=kept*3;
    mesh->indices=vertexStorage(size_t(mesh->indexCount)*sizeof(uint32_t),mesh->indexOffset,true);
    auto *out=reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(mesh->indices->cpu())+mesh->indexOffset);
    for(const uint32_t k:keepScratch){*out++=idx(k);*out++=idx(k+1);*out++=idx(k+2);}
    mesh->transient=true;
    draw.mesh=std::move(mesh);
}
// The mesh's vertices with the listed triangles of its triangle list, in their order (indexed).
std::shared_ptr<Mesh> DekoBackend::subMesh(const Mesh &from,const std::vector<uint32_t> &triangles,bool transient){
    auto mesh=std::make_shared<Mesh>();
    mesh->vertices=from.vertices;mesh->vertexOffset=from.vertexOffset;mesh->count=from.count;mesh->packed=from.packed;
    mesh->minAlpha=from.minAlpha;mesh->maxAlpha=from.maxAlpha;mesh->bounds=from.bounds;
    mesh->indexCount=uint32_t(triangles.size())*3;
    mesh->indices=vertexStorage(size_t(mesh->indexCount)*sizeof(uint32_t),mesh->indexOffset,transient);
    auto *out=reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(mesh->indices->cpu())+mesh->indexOffset);
    for(const uint32_t t:triangles){*out++=3*t;*out++=3*t+1;*out++=3*t+2;}
    mesh->transient=transient;
    if(from.cpuVertices && from.cpuIndices.empty()){
        mesh->cpuVertices=from.cpuVertices;mesh->cpuIndices.reserve(triangles.size()*3);
        for(const uint32_t t:triangles){mesh->cpuIndices.push_back(3*t);mesh->cpuIndices.push_back(3*t+1);mesh->cpuIndices.push_back(3*t+2);}
    }
    return mesh;
}
// v128: the alpha test, by triangle. The shaders compute A = floor(a x 128 + 0.001) with a x 128 =
// T x va x opacity / 128 (T the texel alpha, 0..255; va the interpolated vertex alpha; untextured:
// va x opacity) and, for GEQUAL/GREATER, keep the fragment in the main pass when A >= need and in
// the failure pass otherwise. Filtering and interpolation are convex, so a triangle whose least
// vertex alpha gives A >= need with the texture's least alpha passes at every fragment (nothing in
// the failure pass), and one whose largest gives A < need with the texture's largest fails at
// every fragment (nothing in the main pass). Each pass draws only the triangles that can write in
// it, in their order: the same picture and depth, with fewer fragments shaded and discarded.
void DekoBackend::splitAlphaTest(Draw &draw){
    const auto &s=draw.state;
    static const bool bgSplit170=!std::getenv("CELOSIA_DIAG_NO_BGSPLIT170"); // v170a: background draws too
    if(!alphaSplit || diagGpuMode!=0 || draw.faint || s.shadowVolume || (s.background && !bgSplit170) || s.fresnel || !(s.alphaTest&1))return;
    const unsigned mode=(s.alphaTest>>1)&7;
    if(mode!=5 && mode!=6)return;
    const auto &mesh=draw.mesh;const Mesh &m=*mesh;
    if(m.triangleMin.empty() || m.indices || m.triangleMin.size()!=m.triangleAlpha.size())return;
    if(!std::isfinite(s.opacity) || s.opacity<=0)return;
    const unsigned ref=(s.alphaTest>>4)&255;
    const float need=float(mode==6?ref+1:ref);
    const bool textured=s.texture!=0;
    const float lo=(textured?float(draw.texture->minAlpha)/128.0f:1.0f)*s.opacity;
    const float hi=(textured?float(draw.texture->maxAlpha)/128.0f:1.0f)*s.opacity;
    const bool failure=needsAlphaFailurePass(s,m.minAlpha,draw.texture->minAlpha);
    const auto passes=[&](uint8_t va){return float(va)*lo*0.995f>=need;};
    const auto fails=[&](uint8_t va){return float(va)*hi*1.005f+0.001f<need;};
    // v167: with the texture's alpha grid, each triangle's own largest readable texel alpha T (its uv
    // rectangle and one texel more each side, as clipToTexture): it fails the main pass where
    // va x T / 128 x opacity < need, and writes nothing in either pass where the shaders' a < 0.02
    // (a = T / 255 x 2 x va / 128 x opacity). Same picture: only fragments both passes discard.
    Texture *regionTex=(alphaRegionSplit && textured && draw.texture && !draw.texture->alphaGrid.empty() &&
        m.triangleUV.size()==m.triangleMin.size()*4 && std::isfinite(s.uvOffset[0]) && std::isfinite(s.uvOffset[1]))?draw.texture.get():nullptr;
    std::vector<uint8_t> &regionHi=regionScratch;
    if(regionTex){
        const uint32_t triangles=uint32_t(m.triangleMin.size());
        regionHi.resize(triangles);
        const float W=float(regionTex->gridW),H=float(regionTex->gridH);
        for(uint32_t t=0;t<triangles;++t){
            const float *r=&m.triangleUV[size_t(t)*4];
            const float u0=r[0]+s.uvOffset[0],u1=r[1]+s.uvOffset[0],v0=r[2]+s.uvOffset[1],v1=r[3]+s.uvOffset[1];
            uint8_t top=regionTex->maxAlpha;
            if(std::isfinite(u0) && std::isfinite(u1) && std::isfinite(v0) && std::isfinite(v1) && u1-u0<1.0f && v1-v0<1.0f &&
               std::fabs(u0)<1e5f && std::fabs(v0)<1e5f){
                const int i0=int(std::floor(u0*W-0.5f))-1,i1=int(std::floor(u1*W-0.5f))+2;
                const int j0=int(std::floor(v0*H-0.5f))-1,j1=int(std::floor(v1*H-0.5f))+2;
                top=std::min(top,alphaMax(*regionTex,i0,i1,j0,j1,s.clamp));
            }
            regionHi[t]=top;
        }
    }
    const auto failsAt=[&](uint32_t t){
        if(!regionTex)return fails(m.triangleAlpha[t]);
        return float(m.triangleAlpha[t])*(float(regionHi[t])/128.0f*s.opacity)*1.005f+0.001f<need;
    };
    const auto faintAt=[&](uint32_t t){
        return regionTex && float(regionHi[t])*2.0f/255.0f*float(m.triangleAlpha[t])/128.0f*s.opacity*1.01f<0.0199f;
    };
    std::shared_ptr<Mesh::AlphaSplit> result;
    if(!m.transient && m.split && m.split->lo==lo && m.split->hi==hi && m.split->need==need && m.split->failure==failure &&
       m.split->regionTexture==regionTex && (!regionTex || (m.split->regionU==s.uvOffset[0] && m.split->regionV==s.uvOffset[1] &&
       m.split->regionClamp==s.clamp)))
        result=m.split;
    else{
        // Kept meshes keep their first split (persistent index lists); other bounds (an animated
        // opacity) and frame meshes use this frame's storage.
        const bool keep=!m.transient && !m.split;
        result=std::make_shared<Mesh::AlphaSplit>();
        result->lo=lo;result->hi=hi;result->need=need;result->failure=failure;
        result->regionTexture=regionTex;result->regionU=s.uvOffset[0];result->regionV=s.uvOffset[1];result->regionClamp=s.clamp;
        const uint32_t triangles=uint32_t(m.triangleMin.size());
        uint32_t certainPass=0,certainFail=0,failDrop=0;
        for(uint32_t t=0;t<triangles;++t){
            const bool p=passes(m.triangleMin[t]);
            if(p)++certainPass;
            else if(failsAt(t))++certainFail;
            if(p || faintAt(t))++failDrop;
        }
        if(certainFail){
            if(certainFail==triangles)result->mainEmpty=true;
            else{
                keepScratch.clear();
                for(uint32_t t=0;t<triangles;++t)if(passes(m.triangleMin[t]) || !failsAt(t))keepScratch.push_back(t);
                result->main=subMesh(m,keepScratch,!keep);
            }
            result->mainDropped=certainFail;
        }
        if(failure && failDrop){
            if(failDrop==triangles)result->failEmpty=true;
            else{
                keepScratch.clear();
                for(uint32_t t=0;t<triangles;++t)if(!passes(m.triangleMin[t]) && !faintAt(t))keepScratch.push_back(t);
                result->fail=subMesh(m,keepScratch,!keep);
            }
            result->failDropped=failDrop;
        }
        if(keep)mesh->split=result;
    }
    if(!result->mainDropped && !result->failDropped)return;
    if(s.background)draw.wholeMesh=draw.mesh; // v170a: for the merged pass (before the halves replace it)
    // The failure pass keeps the original mesh when the main one changes.
    if(result->failEmpty)draw.failureEmpty=true;
    else draw.failureMesh=result->fail?result->fail:mesh;
    if(result->mainEmpty)draw.mainEmpty=true;
    else if(result->main)draw.mesh=result->main;
    ++lastSplitDraws;lastSplitMainDropped+=result->mainDropped;lastSplitFailDropped+=result->failDropped;
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
    // v152: texture detail: the mip chosen 0.5 or 1 level finer (only where the PS2 uses mips).
    if (s.maxLod > 0) result.lod[0] -= 0.5f * float(std::clamp(celosia::live::settings().textureDetail.load(std::memory_order_relaxed), 0, 2));
    // v167: a texture pack's texture (several times the PS2's size): the hardware's mip choice (anisotropic
    // as the sampler asks), biased finer by the texture detail setting; lod.z = -1 tells the shaders.
    if (draw.texture && draw.texture->replaced) {
        result.lod[0] = -0.5f * float(std::clamp(celosia::live::settings().textureDetail.load(std::memory_order_relaxed), 0, 2));
        result.lod[1] = 0; result.lod[2] = -1;
    }
    // v169: the draw distance option (user: "more texture detail farther", not drawn farther): the mip chosen as
    // if the surface were 1.25x / 1.5x nearer: lod.x - lod.y x log2(f) where the PS2's formula picks the mip
    // (mip = log2(depth) x lod.y + lod.x), the hardware's bias - log2(f) for a pack texture.
    // v173s: the models' steps became 1.5x / 2x / 3x; the textures keep v169's gentle shift (1.25x for the first
    // step, 1.5x above: a much finer far mip shimmers).
    if (const int dd = std::clamp(celosia::live::settings().drawDistance.load(std::memory_order_relaxed), 0, 3); dd != 0 && s.texture) {
        const float shift = dd == 1 ? 0.32193f : 0.58496f; // log2(1.25), log2(1.5)
        if (result.lod[2] < 0) result.lod[0] -= shift;
        else if (result.lod[2] > 0) result.lod[0] -= result.lod[1] * shift;
    }
    result.lod[3] = s.fresnel ? -1 : (s.lit ? 1 : 0);
    result.alpha[0] = s.alphaTest & 1 ? (s.alphaTest >> 1) & 7 : -1;
    result.alpha[1] = (s.alphaTest >> 4) & 255;
    result.alpha[2] = failurePass;
    if (s.debugColour) {
        result.debug[0] = ((s.debugColour >> 16) & 255) / 255.0f;
        result.debug[1] = ((s.debugColour >> 8) & 255) / 255.0f;
        result.debug[2] = (s.debugColour & 255) / 255.0f;
        result.debug[3] = 1;
    } else {
        result.debug[0] = 1.0f / float(rw); result.debug[1] = 1.0f / float(rh); // v150: background lookup
    }
    return result;
}
void DekoBackend::bindVtxFormat(int format) {
    if(vtxFormat==format)return;
    vtxFormat=format;
    if(format==2){
        const DkVtxAttribState a[]{
            {0, 0, offsetof(ClipVertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(ClipVertex, nx), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(ClipVertex, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(ClipVertex, r), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0}};
        const DkVtxBufferState b{sizeof(ClipVertex), 0};
        dkCmdBufBindVtxAttribState(commands, a, 4);dkCmdBufBindVtxBufferState(commands, &b, 1);
    }else if(format==1){
        const DkVtxAttribState a[]{
            {0, 0, offsetof(PackedVertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(PackedVertex, n), DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Snorm, 0},
            {0, 0, offsetof(PackedVertex, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(PackedVertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled, 0}};
        const DkVtxBufferState b{sizeof(PackedVertex), 0};
        dkCmdBufBindVtxAttribState(commands, a, 4);dkCmdBufBindVtxBufferState(commands, &b, 1);
    }else{
        const DkVtxAttribState a[]{
            {0, 0, offsetof(Vertex, x), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(Vertex, nx), DkVtxAttribSize_3x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(Vertex, u), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
            {0, 0, offsetof(Vertex, r), DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled, 0}};
        const DkVtxBufferState b{sizeof(Vertex), 0};
        dkCmdBufBindVtxAttribState(commands, a, 4);dkCmdBufBindVtxBufferState(commands, &b, 1);
    }
}
void DekoBackend::pushMaterial(const Material &material) {
    constexpr unsigned Rows=sizeof(Material)/16;
    pushRowsAll+=Rows;
    if(!pushDelta || !pushedValid){
        dkCmdBufPushConstants(commands,uniforms->gpu(),512,0,sizeof(material),&material);
        pushedMaterial=material;pushedValid=true;pushRowsSent+=Rows;return;
    }
    const auto *now=reinterpret_cast<const uint8_t *>(&material);
    auto *held=reinterpret_cast<uint8_t *>(&pushedMaterial);
    bool changed[Rows];
    for(unsigned r=0;r<Rows;++r)changed[r]=std::memcmp(now+r*16,held+r*16,16)!=0;
    // Runs of changed rows; a gap of fewer than 3 equal rows is pushed with them (one push's own command
    // words cost about as much as two rows).
    for(unsigned r=0;r<Rows;){
        if(!changed[r]){++r;continue;}
        unsigned end=r+1,last=r;
        while(end<Rows){
            if(changed[end]){last=end;++end;continue;}
            unsigned gap=end;while(gap<Rows && !changed[gap] && gap-last<=3)++gap;
            if(gap<Rows && changed[gap] && gap-last<=3){end=gap;continue;}
            break;
        }
        const unsigned rows=last+1-r;
        dkCmdBufPushConstants(commands,uniforms->gpu(),512,r*16,rows*16,now+r*16);
        std::memcpy(held+r*16,now+r*16,rows*16);pushRowsSent+=rows;
        r=last+1;
    }
}
void DekoBackend::emitDraw(const Draw &draw, bool failurePass, bool farBackdrop) {
    if(draw.faint || (failurePass?draw.failureEmpty:draw.mainEmpty))return;
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
    // v179 (the user's PCSX2 capture: the sigil added twice the light over the colossus's skin, +72/+89/+87 vs
    // +32/+39/+40): the additive draws of the glow layer (the weak points' sigils) add half their colour.
    if(s.order==8 && s.blend==Blend::Additive && !s.screen && material.debug[3]==0 &&
       celosia::live::settings().sigilPs2.load(std::memory_order_relaxed))material.debug[2]=0.5f;
    // v178: the water's reads (native.frag's textured 2 / 3; the generic shader, never a lean one).
    const bool waterRead=s.reflectionSample || s.refractionSample;
    if(waterRead){
        material.fogColour[3]=s.reflectionSample?2.0f:3.0f;
        std::copy_n(s.reflectionTint,4,material.bgFog);
        material.debug[0]=1.0f/float(rw);material.debug[1]=1.0f/float(rh);
        material.debug[2]=s.mirrorRipple[0];material.debug[3]=0;
        material.alpha[0]=-1;material.alpha[1]=s.reflectionSample?s.mirrorRipple[1]:0.0f;
        material.params[3]=0;
        if(fragment!=2)fragment=0;
    }
    // v105: a lean variant of the generic shader holding only the paths this draw's uniforms turn on
    // (lean.frag.glsl). Early tests only without depth writes, where they reject what late ones would.
    // v116: also the opaque shader's draws, the background layer's (near cull) and untextured ones.
    // v171c first person (fog.z < 0, the near plane): the generic shader, the one that discards (nearCull).
    if(material.fog[2]<0.0f && fragment<=2)fragment=0;
    if(leanEnabled && diagGpuMode==0 && fragment<=2 && material.debug[3]==0 && material.fog[2]>=0.0f && !waterRead){
        const bool textured=material.fogColour[3]!=0;
        int variant=(material.bgFog[3]>0?2:0)|(textured && material.lod[2]!=0?4:0)|(textured?0:64);
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
    pushMaterial(material);
    bindTexture(diagGpuMode==7 && diagAdditive?*dummyTexture:*draw.texture,(s.screenCopy || waterRead)?uint8_t(3):s.clamp); // v176: the copy clamped (v178: the mirror too)
    submitGeometry(failurePass && draw.failureMesh?*draw.failureMesh:*draw.mesh);
}
void DekoBackend::submitGeometry(const Mesh &mesh) {
    // v166: the mesh's own vertex format (Vertex / PackedVertex / ClipVertex), bound when it changes.
    bindVtxFormat(mesh.floatColour?2:mesh.packed?1:0);
    if(mesh.floatColour){
        dkCmdBufBindVtxBuffer(commands, 0, mesh.vertices->gpu()+mesh.vertexOffset, mesh.count * sizeof(ClipVertex));
        dkCmdBufDraw(commands, DkPrimitive_Triangles, mesh.count, 1, 0, 0);
        return;
    }
    dkCmdBufBindVtxBuffer(commands, 0, mesh.vertices->gpu()+mesh.vertexOffset, mesh.count * mesh.stride());
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
    if(draw.faint || draw.mainEmpty)return;
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
    pushMaterial(material);
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
    // v132: the volume's bounding box projected (all eight corners in front of the camera: the image of
    // the box is the hull of theirs); both row orientations are taken, so the rectangle holds it either way.
    if(!shadowRectFull){
        const auto &b=draw.mesh->bounds;const float *c=draw.clip.data();
        bool ok=b.valid;float x0=1e9f,x1=-1e9f,y0=1e9f,y1=-1e9f;
        for(unsigned corner=0;ok && corner<8;++corner){
            const float p[3]{corner&1?b.hi[0]:b.lo[0],corner&2?b.hi[1]:b.lo[1],corner&4?b.hi[2]:b.lo[2]};
            const float x=c[0]*p[0]+c[4]*p[1]+c[8]*p[2]+c[12];
            const float y=c[1]*p[0]+c[5]*p[1]+c[9]*p[2]+c[13];
            const float w=c[3]*p[0]+c[7]*p[1]+c[11]*p[2]+c[15];
            if(!(w>1e-3f) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w)){ok=false;break;}
            const float nx=x/w,ny=y/w;
            x0=std::min(x0,nx);x1=std::max(x1,nx);y0=std::min(y0,ny);y1=std::max(y1,ny);
        }
        if(!ok)shadowRectFull=true;
        else{
            const float px0=(x0+1.0f)*0.5f*float(rw),px1=(x1+1.0f)*0.5f*float(rw);
            const float ra=(y0+1.0f)*0.5f*float(rh),rb=(y1+1.0f)*0.5f*float(rh);
            const float py0=std::min(ra,float(rh)-rb),py1=std::max(rb,float(rh)-ra);
            if(!shadowRectAny){shadowRect[0]=px0;shadowRect[1]=py0;shadowRect[2]=px1;shadowRect[3]=py1;shadowRectAny=true;}
            else{shadowRect[0]=std::min(shadowRect[0],px0);shadowRect[1]=std::min(shadowRect[1],py0);
                 shadowRect[2]=std::max(shadowRect[2],px1);shadowRect[3]=std::max(shadowRect[3],py1);}
        }
    }
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
    pushMaterial(material);
    bindTexture(*dummyTexture,0);
    bindVtxFormat(draw.mesh->floatColour?2:draw.mesh->packed?1:0);
    dkCmdBufBindVtxBuffer(commands,0,draw.mesh->vertices->gpu()+draw.mesh->vertexOffset,draw.mesh->count*draw.mesh->stride());
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
    struct RectReset{DekoBackend &b;~RectReset(){b.shadowRectAny=b.shadowRectFull=false;}} rectReset{*this};
    if(softShadows && diagGpuMode!=28 && diagGpuMode!=29 && celosia::live::settings().softShadows.load(std::memory_order_relaxed)){
        const auto none=DescriptorSlots::None;
        const bool noBlur=diagGpuMode==25,darkenOnly=diagGpuMode==26,noBarrier=diagGpuMode==27;
        auto barrier=[&]{if(!noBarrier)tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);};
        const uint32_t sw=rw/4,sh=rh/4;
        // v132: the stencil is set only inside the volumes' rectangle, so every pass's output is 0 outside
        // its own region: the mask inside R (2 px margin), the reduce inside R / 4 (+1), both blurs inside
        // that +10 small texels (taps up to 3 x 2.5 texels, +1 for the filter), the darken inside the
        // blur's region x 4 (+4 px). Each pass is drawn inside its region over a target cleared to 0
        // (the darken: alpha 0 leaves the picture as it is), so every pixel gets the same value.
        const bool rect=shadowRectOn && shadowRectAny && !shadowRectFull && diagGpuMode==0;
        int R[4]{0,0,int(rw),int(rh)},S[4]{0,0,int(sw),int(sh)},Bl[4]{0,0,int(sw),int(sh)},Dk[4]{0,0,int(rw),int(rh)};
        if(rect){
            auto clampTo=[](int r[4],int w,int h){r[0]=std::clamp(r[0],0,w);r[2]=std::clamp(r[2],0,w);r[1]=std::clamp(r[1],0,h);r[3]=std::clamp(r[3],0,h);};
            R[0]=int(std::floor(shadowRect[0]))-2;R[1]=int(std::floor(shadowRect[1]))-2;
            R[2]=int(std::ceil(shadowRect[2]))+2;R[3]=int(std::ceil(shadowRect[3]))+2;clampTo(R,int(rw),int(rh));
            S[0]=R[0]/4-1;S[1]=R[1]/4-1;S[2]=(R[2]+3)/4+1;S[3]=(R[3]+3)/4+1;clampTo(S,int(sw),int(sh));
            Bl[0]=S[0]-10;Bl[1]=S[1]-10;Bl[2]=S[2]+10;Bl[3]=S[3]+10;clampTo(Bl,int(sw),int(sh));
            Dk[0]=Bl[0]*4-4;Dk[1]=Bl[1]*4-4;Dk[2]=Bl[2]*4+4;Dk[3]=Bl[3]*4+4;clampTo(Dk,int(rw),int(rh));
            lastShadowRectArea=uint32_t(int64_t(Dk[2]-Dk[0])*(Dk[3]-Dk[1])*1000/(int64_t(rw)*rh));
        }else lastShadowRectArea=1000;
        auto scissor=[&](const int r[4]){
            if(!rect)return;
            const DkScissor s{uint32_t(r[0]),uint32_t(r[1]),uint32_t(std::max(r[2]-r[0],0)),uint32_t(std::max(r[3]-r[1],0))};
            dkCmdBufSetScissors(commands,0,&s,1);
        };
        // The counted pixels as a mask (the picture's stencil tested and cleared).
        if(!darkenOnly){
        bindTarget(maskImage(),&depth->image,rw,rh);
        dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,0);
        scissor(R);
        bloomQuad(1,none,none,nullptr,nullptr,0,-1.0f,true);
        resolveMask();
        barrier();markDrawTime(-12);
        // Reduced 4 x 4, then blurred across and down (GL blur(1): steps of render width / 512 texels).
        bindTarget(shadowSmall[0]->image,nullptr,sw,sh);
        if(rect)dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,0);
        scissor(S);
        const float reduce[4]{float(rw),float(rh),1.0f/float(sw),1.0f/float(sh)};
        bloomQuad(7,bloomMaskSlot,none,reduce,nullptr,0,-1.0f);
        barrier();markDrawTime(-13);
        if(!noBlur){
        const float k=float(rw)/512.0f;
        bindTarget(shadowSmall[1]->image,nullptr,sw,sh);
        if(rect)dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,0);
        scissor(Bl);
        const float across[4]{k/float(sw),0,1.0f/float(sw),1.0f/float(sh)};
        bloomQuad(19,shadowSmallSlot[0],none,across,nullptr,0,-1.0f);
        barrier();
        bindTarget(shadowSmall[0]->image,nullptr,sw,sh);
        scissor(Bl);
        const float down[4]{0,k/float(sh),1.0f/float(sw),1.0f/float(sh)};
        bloomQuad(19,shadowSmallSlot[1],none,down,nullptr,0,-1.0f);
        barrier();markDrawTime(-14);
        }
        }
        // The picture darkened by the mask x the amount.
        bindTarget(sceneImage(slot),&depth->image,rw,rh);
        scissor(Dk);
        const float darken[4]{amount,0,1.0f/float(rw),1.0f/float(rh)};
        bloomQuad(9,shadowSmallSlot[0],none,darken,nullptr,2,-1.0f);
        if(rect){const DkScissor full{0,0,rw,rh};dkCmdBufSetScissors(commands,0,&full,1);}
        shadowRectAny=shadowRectFull=false;
        dkCmdBufBindTexture(commands,DkStage_Fragment,1,dkMakeTextureHandle(bgSmallSlot,3));
        ++lastSoftShadows;
        restoreShadowState();
        return;
    }
    // v160: a trivial early-test fragment (solid.frag.glsl): the blend takes nothing of the source colour,
    // so the pixels are the same, and the stencil test now rejects the unshadowed pixels before shading.
    if(solidResolve && solidFragmentShader && diagGpuMode==0){
        const DkShader *shaders[]{&vertexShader->shader,&solidFragmentShader->shader};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);boundFragment=-1;
    }else if(boundFragment!=0){
        const DkShader *shaders[]{&vertexShader->shader,&fragmentShader->shader};
        dkCmdBufBindShaders(commands,DkStageFlag_GraphicsMask,shaders,2);boundFragment=0;
    }
    DkDepthStencilState state;dkDepthStencilStateDefaults(&state);
    state.depthTestEnable=false;state.depthWriteEnable=false;state.stencilTestEnable=true;
    state.stencilFrontCompareOp=state.stencilBackCompareOp=DkCompareOp_NotEqual;
    state.stencilFrontPassOp=state.stencilBackPassOp=DkStencilOp_Zero;
    dkCmdBufBindDepthStencilState(commands,&state);
    dkCmdBufSetStencil(commands,DkFace_FrontAndBack,0xFF,0,0xFF);
    DkColorWriteState write;dkColorWriteStateDefaults(&write); // v130: RGBA (alpha kept by Zero, One)
    dkCmdBufBindColorWriteState(commands,&write);
    DkColorState colour;dkColorStateDefaults(&colour);colour.blendEnableMask=1;
    dkCmdBufBindColorState(commands,&colour);
    DkBlendState blend;dkBlendStateDefaults(&blend);
    dkBlendStateSetFactors(&blend,DkBlendFactor_Zero,DkBlendFactor_InvConstAlpha,DkBlendFactor_Zero,DkBlendFactor_One);
    dkCmdBufBindBlendState(commands,0,&blend);dkCmdBufSetBlendConst(commands,0,0,0,amount);
    Material material{};for(unsigned i=0;i<4;++i)material.clip[i*5]=1;
    material.params[0]=1;material.alpha[0]=-1;material.debug[3]=1;
    // Keep shader alpha opaque: even amount=0 must clear the counted stencil.
    pushMaterial(material);
    bindTexture(*dummyTexture,0);
    // v158: only inside the volumes' screen rectangle (v132's, +2 px): the volumes set the stencil only
    // there (outside it every pixel's stencil is 0: NotEqual fails, nothing written or cleared), so the
    // picture and the cleared stencil are the same, without reading the 4x Z/S of the whole screen.
    const bool rect=shadowRectOn && shadowRectAny && !shadowRectFull && diagGpuMode==0;
    if(rect){
        const int x0=std::clamp(int(std::floor(shadowRect[0]))-2,0,int(rw)),y0=std::clamp(int(std::floor(shadowRect[1]))-2,0,int(rh));
        const int x1=std::clamp(int(std::ceil(shadowRect[2]))+2,0,int(rw)),y1=std::clamp(int(std::ceil(shadowRect[3]))+2,0,int(rh));
        const DkScissor s{uint32_t(x0),uint32_t(y0),uint32_t(std::max(x1-x0,0)),uint32_t(std::max(y1-y0,0))};
        dkCmdBufSetScissors(commands,0,&s,1);
        lastShadowRectArea=uint32_t(int64_t(x1-x0)*(y1-y0)*1000/(int64_t(rw)*rh));
    }else lastShadowRectArea=1000;
    bindVtxFormat(0);dkCmdBufBindVtxBuffer(commands,0,shadowTriangle->gpu(),3*sizeof(Vertex));
    dkCmdBufDraw(commands,DkPrimitive_Triangles,3,1,0,0);
    if(rect){const DkScissor full{0,0,rw,rh};dkCmdBufSetScissors(commands,0,&full,1);}
    restoreShadowState();
}
void DekoBackend::endFrame(uint32_t width, uint32_t height) {
    const auto cpuStart=armTicksToNs(armGetSystemTick());
    if (!recording) throw std::logic_error("deko3d: endFrame outside frame");
    if (width != Width || height != Height)
        throw std::invalid_argument("deko3d: scene must be native 1280x720");
    const bool diagnosticCapture=diagnosticCaptureFrame && submittedFrames+1==diagnosticCaptureFrame && !held;
    if(diagnosticCapture)captureRequested=true;
    // v172 photo mode: while it is on every picture is copied back (the last one is the frozen picture saved).
    // v173b: once it is off, its copy dropped (else the host's present path keeps reading every frame back).
    {
        static bool photoCaptured=false;
        if(celosia::live::settings().photoMode.load(std::memory_order_relaxed)!=0){if(!held){captureRequested=true;photoCaptured=true;}}
        else if(photoCaptured && !diagnosticCapture){latestCapture.reset();photoCaptured=false;}
    }
    auto &frame = frames[frameIndex];
    startCommands(frame);
    // v123: swapped, not moved: the lists keep their storage between frames (a move left them empty, so
    // every frame regrew them, copying each Draw and its two shared_ptrs). The frame's previous lists
    // (retired: beginFrame waited for this frame's fence) are cleared here as the assignment did.
    frame.draws.swap(draws); draws.clear(); frame.uploads.swap(uploads); uploads.clear();
    frameBlurs.swap(blurs);blurs.clear(); // v177
    std::stable_sort(frameBlurs.begin(),frameBlurs.end(),[](const ScreenBlur &a,const ScreenBlur &b){return a.order<b.order;});
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
    if(held && frameGen)fgHeldFrames.fetch_add(1,std::memory_order_relaxed); // v173 diagnosis ([fgsec173])
    if (!held) {
        const auto acquireStart=armTicksToNs(armGetSystemTick());
        markRender(RenderPhase::Acquire);
        // The menu's frame limit (v149): 60 = every refresh, 30 = every image shown for two refreshes,
        // 45 = one, one and two refreshes in turn (three images in four refreshes).
        {
            static int swapNow=1;static uint32_t cadence=0;
            const int cap=celosia::live::settings().fpsCap.load(std::memory_order_relaxed);
            int wanted=cap<=30?2:(cap<=45?((cadence++%3)==2?2:1):1);
            // v179 (report: loads took twice as long at 30): the game thread waits on each held image, and the
            // game loads a step per frame. Frames without a scene (round 107's rule: on at 150 draws or more,
            // off under 40: loading screens, logos, menus over black) are shown every refresh.
            {
                static bool scene=true;
                const std::size_t n=frame.draws.size();
                if(n>=150)scene=true;else if(n<40)scene=false;
                if(!scene)wanted=1;
            }
            if(celosia::live::settings().frameGen.load(std::memory_order_relaxed)>0)wanted=swapIntervalNow; // v155: set before each of its presents
            if(wanted!=swapIntervalNow){dkSwapchainSetSwapInterval(swapchain,wanted);swapIntervalNow=wanted;}
        }
        // v150: the menu's render height for the console's mode (handheld / docked), 16:9.
        {
            auto &live=celosia::live::settings();
            // v151: the effective mode (ReverseNX-RT's forced mode when it controls it) and the output for it:
            // docked 1920x1080 (1280x720 when the TV reports a 720p default), handheld 1280x720.
            const bool real=appletGetOperationMode()==AppletOperationMode_Console;
            const bool docked=celosia_effective_docked(real)!=0;
            uint32_t outW=Width,outH=Height;
            if(docked){
                outW=1920;outH=1080;
                s32 dw=0,dh=0;
                if(real && R_SUCCEEDED(appletGetDefaultDisplayResolution(&dw,&dh)) && dh==720){outW=1280;outH=720;}
            }
            if(outW!=ow || outH!=oh)resizeOutput(outW,outH);
            if(docked!=dockedNow){dockedNow=docked;std::fprintf(stderr,"[resolution] mode %s (console %s)\n",docked?"docked":"handheld",real?"docked":"handheld");}
            live.docked=docked;
            uint32_t height=std::clamp<uint32_t>(uint32_t((docked?live.dockedHeight:live.handheldHeight).load(std::memory_order_relaxed)),360u,1080u);
            // v169/v170: dynamic resolution (its own menu mode: least 80 / 70 / 60 %). The GPU's frame time against
            // the real frame's period (the cap, or frame generation's real rate), averaged over the last second.
            // v169 stepped down after 20 frames over 87 % and back up only after 120 frames under 60 %: between
            // the two it never moved, so it ended at its least whatever the GPU's clock (user: "no reacciona al
            // overclock"). Now it aims at 80 % load: the scale that would give it (the GPU time follows the pixel
            // count: pct x sqrt(0.80 / load)), in 5 % steps; down at once (after 10 frames) when the average is
            // over 87 %, up when the larger size is predicted under 85 %. A faster GPU (overclock) gives a lower
            // load and so a larger size. One change per second at most (each reallocates the scene's targets).
            {
                static int pct=100;static uint64_t lastChangeNs=0,lastLogNs=0;static uint32_t seenGpuFrame=0;
                static double sum=0;static int samples=0;static double avg=0;static int overRun=0;
                const int least=std::clamp(live.drsMinPercent.load(std::memory_order_relaxed),0,100);
                if(least<=0 || least>=100){pct=100;sum=0;samples=0;overRun=0;}
                else{
                    pct=std::max(pct,least);
                    const int mode=std::clamp(live.frameGen.load(std::memory_order_relaxed),0,3);
                    const int cap=live.fpsCap.load(std::memory_order_relaxed);
                    const double periodNs=mode>0?1e9/(mode==1?30.0:mode==2?45.0:60.0):1e9/double(cap<=30?30:cap<=45?45:60);
                    if(lastGpuTimingFrame!=seenGpuFrame && lastGpuNs){
                        seenGpuFrame=lastGpuTimingFrame;
                        const double load=double(lastGpuNs)/periodNs;
                        sum+=load;++samples;
                        overRun=load>0.87?overRun+1:0;
                    }
                    const uint64_t now=armTicksToNs(armGetSystemTick());
                    const bool second=now-lastChangeNs>1000000000ull && samples>=20;
                    if(second || (overRun>=10 && now-lastChangeNs>500000000ull && samples>=10)){
                        avg=sum/double(samples);sum=0;samples=0;
                        int next=pct;
                        if(avg>0.87 || overRun>=10)next=int(std::floor(double(pct)*std::sqrt(0.80/avg)/5.0))*5;
                        else{
                            const int up=std::min(100,pct+5);
                            const double ratio=double(up)/double(pct);
                            if(avg*ratio*ratio<0.85)next=std::min(100,int(std::floor(double(pct)*std::sqrt(0.80/avg)/5.0))*5);
                        }
                        next=std::clamp(next,least,100);
                        if(next!=pct){
                            std::fprintf(stderr,"[drs] %d%% -> %d%% (load %.0f%% of %.1f ms)\n",pct,next,avg*100.0,periodNs/1e6);
                            pct=next;overRun=0;
                        }
                        lastChangeNs=now;
                        if(now-lastLogNs>5000000000ull){
                            lastLogNs=now;
                            std::fprintf(stderr,"[drs] scale %d%% (least %d%%), load %.0f%%\n",pct,least,avg*100.0);
                        }
                    }
                }
                live.drsPercentNow.store(uint32_t(pct),std::memory_order_relaxed);
                if(pct<100)height=std::max<uint32_t>(240u,(height*uint32_t(pct)/100u)&~7u);
            }
            const uint32_t width=celosia::live::widthFor(height);
            const int aa=std::clamp(live.antiAliasing.load(std::memory_order_relaxed),0,4);
            const uint32_t samples=aa==2?2u:(aa>=3?4u:1u);
            static int lastAa=-1;
            if(aa!=lastAa){lastAa=aa;live.msaaFailed=false;} // a new choice is tried again
            // v153: frame generation at the output's size; made again when the size or the flow quality changes.
            const int fgMode=std::clamp(live.frameGen.load(std::memory_order_relaxed),0,3);
            fgOn=false;
            if(fgMode>0){
                // v156: the motion estimate at a fixed height per quality (360 / 540 / 720 lines) whatever the
                // output: at 720p the same as before (1/2, 3/4, full), at 1080p 1/3, 1/2, 2/3. The picture
                // itself stays at the output's resolution.
                // v157: the motion estimate at 1/4 of the output (performance, NaGaa95's default), 1/2, 3/4.
                // v173h: Calidad 1/1.25 (was 1/1.33: 720 / 1.3333334 truncated to 539 rows, the generated frames showed
                // the HUD and text doubled; 1.25 gives 1024x576 exactly, the same GPU cost, checked on hardware).
                static const float flowDivs[3]{4.0f,2.0f,1.0f}; // v173l: Cemu-nx 1.3.3's choices (1/4, 1/2, full flow)
                // v173g CELOSIA_DIAG_LSFG_FLOW=<divisor> (diagnosis): the flow's divisor forced (1 to 4).
                static const float flowForced=[]{const char *v=std::getenv("CELOSIA_DIAG_LSFG_FLOW");return v?std::clamp(float(std::atof(v)),1.0f,4.0f):0.0f;}();
                const float flowRaw=flowForced>0.0f?flowForced:flowDivs[std::clamp(live.frameGenQuality.load(std::memory_order_relaxed),0,2)];
                const float flow=flowRaw>=3.0f?4.0f:flowRaw>=1.5f?2.0f:1.0f; // v173m: Cemu-nx's 1 / 2 / 4
                const bool perf=live.frameGenPerformance.load(std::memory_order_relaxed); // v173m: its performance mode
                static int failedMode=-1;static float failedFlow=0;static bool failedPerf=false;static uint32_t failedW=0;static uint64_t failedAt=0;
                // v170a: a failed creation tried again after 5 s (before: never, for the same mode / flow / width).
                if(failedMode>=0 && armTicksToNs(armGetSystemTick())-failedAt>5000000000ull){
                    std::fprintf(stderr,"[lsfg] trying again after a failure\n");
                    failedMode=-1;
                }
                if(frameGen && (frameGen->width()!=ow || frameGen->height()!=oh || frameGen->flow()!=flow || frameGen->performance()!=perf))frameGen.reset();
                if(!frameGen && !(failedMode==fgMode && failedFlow==flow && failedPerf==perf && failedW==ow)){
                    for(auto &f:frames)
                        if(f.pending && dkFenceWait(&f.fence,-1)!=DkResult_Success)throw std::runtime_error("deko3d: lsfg fence wait failed");
                    try{
                        if(!lsfg::FrameGen::shadersPresent(perf)){live.frameGenState=1;throw std::runtime_error("shaders missing");}
                        frameGen=std::make_unique<lsfg::FrameGen>(device,allocationBudget,ow,oh,flow,perf,1u);
                        // v168: its two inputs as storage images of the scene's set (every frame waited above).
                        for(uint32_t i=0;i<2;++i){
                            const lsfg::Img &in=frameGen->historyImage(i);
                            if(fgInSlot[i]==DescriptorSlots::None || in.w!=ow || in.h!=oh){fgInSlot[0]=fgInSlot[1]=DescriptorSlots::None;break;}
                            const auto view=in.image->view();
                            dkImageDescriptorInitialize(imageDescriptors->entries()+fgInSlot[i],&view,true,false);
                        }
                        fgFrame=0;live.frameGenState=0;
                        std::fprintf(stderr,"[lsfg] ready %ux%u flow 1/%.2f\n",ow,oh,flow);
                    }catch(const std::exception &e){
                        frameGen.reset();if(live.frameGenState.load()==0)live.frameGenState=2;
                        failedMode=fgMode;failedFlow=flow;failedPerf=perf;failedW=ow;failedAt=armTicksToNs(armGetSystemTick());
                        std::fprintf(stderr,"[lsfg] unavailable: %s\n",e.what());
                    }
                }
                fgOn=frameGen!=nullptr;
                // v157: the user's rule: frame generation when the game falls below the mode's rate, x2 of
                // what it gives; real frames as they are while it reaches it. Passthrough -> generation after
                // 16 frames in a row slower than the rate (+12 %); back after 90 frames whose own cost (the
                // game's CPU time, the scene's GPU time) fits in 85 % of the rate's period.
                if(fgOn){
                    const uint64_t entry=armTicksToNs(armGetSystemTick());
                    // v173k (as Cemu-nx 1.3.3): after a gap of more than 0.25 s between real frames (loading, a
                    // pause) the generation starts again from two new frames instead of joining two scenes.
                    {static uint64_t lastEntry=0;if(lastEntry && entry-lastEntry>250000000ull && fgFrame>=2){fgFrame=0;frameGen->reset();std::fprintf(stderr,"[lsfg] restart after a %.0f ms gap\n",double(entry-lastEntry)/1e6);}lastEntry=entry;}
                    const double period=1e9/double(fgMode==1?60:(fgMode==2?90:120));
                    // v173 (user, 2026-11-20: "el lsfg debe generar siempre que este activado"): no passthrough;
                    // generated frames from the first frame on, whatever the game's rate. CELOSIA_DIAG_LSFG_PASSTHROUGH=1:
                    // v157's switching back.
                    static const bool allowPass=std::getenv("CELOSIA_DIAG_LSFG_PASSTHROUGH")!=nullptr;
                    if(!allowPass){
                        if(fgPassthrough){
                            fgPassthrough=false;fgFrame=0;frameGen->reset();fgFast=0;fgRealEma=0;fgLastRealNs=0;fgShownStart=0;fgShownCount=0;
                            std::fprintf(stderr,"[lsfg] generating always (v173)\n");
                        }
                    }else if(fgPassthrough){
                        if(fgPassLast){const double d=double(entry-fgPassLast);fgSlow=(d>period*1.12 && d<250e6)?fgSlow+1:0;}
                        fgPassLast=entry;
                        if(fgSlow>=16){
                            fgPassthrough=false;fgFrame=0;frameGen->reset();fgFast=0;fgRealEma=0;fgLastRealNs=0;fgShownStart=0;fgShownCount=0;
                            std::fprintf(stderr,"[lsfg] generating (the game below %.0f FPS)\n",1e9/period);
                        }
                    }else{
                        const double cpu=fgLastExitNs?double(entry-fgLastExitNs):period;
                        const double gpu=double(lastGpuNs)-double(std::min<uint64_t>(fgLsfgNs,lastGpuNs));
                        // v161: never passthrough when the menu's frame limit is below the rate (30 x2 with the
                        // 30 limit: the user's choice is generated frames throughout).
                        const bool capReaches=double(celosia::live::settings().fpsCap.load(std::memory_order_relaxed))*period>=1e9*0.99;
                        static uint64_t lastEntry170a=0;
                        const double interval=lastEntry170a?double(entry-lastEntry170a):1e12;
                        lastEntry170a=entry;
                        fgFast=capReaches && std::max(cpu,gpu)<period*0.85 && interval<period*1.10?fgFast+1:0; // v170a: the real rate too
                        if(fgFast>=90){
                            fgPassthrough=true;fgSlow=0;fgPassLast=0;
                            std::fprintf(stderr,"[lsfg] passthrough (the game reaches %.0f FPS: cpu %.1f gpu %.1f ms, real interval %.1f ms)\n",1e9/period,cpu/1e6,gpu/1e6,interval/1e6);
                        }
                    }
                    if(fgPassthrough){
                        fgOn=false;live.frameGenShown=0;live.frameGenReal=0;
                        const double refreshNs=1000.0*double(std::max<uint32_t>(4000u,live.refreshUs.load(std::memory_order_relaxed)));
                        const int interval=std::max(1,int(period/refreshNs+0.5));
                        if(interval!=swapIntervalNow){dkSwapchainSetSwapInterval(swapchain,interval);swapIntervalNow=interval;}
                    }
                }
            }else{
                if(frameGen){for(auto &f:frames)if(f.pending)dkFenceWait(&f.fence,-1);frameGen.reset();}
                live.frameGenState=0;live.frameGenReal=0;live.frameGenShown=0;
            }
            const bool post=aa==1 || aa==4 || live.sharpen.load(std::memory_order_relaxed)>0 || live.deband.load(std::memory_order_relaxed) || fgOn;
            const uint32_t wantSamples=live.msaaFailed.load(std::memory_order_relaxed)&&samples>1?1u:samples;
            if(width!=rw || height!=rh || wantSamples!=msaa || post!=postOn){
                const uint64_t resizeStart=armTicksToNs(armGetSystemTick());
                try{resizeScene(width,height,wantSamples,post);}
                catch(const std::exception &e){
                    // Not enough GPU memory for the multisampled targets: shown in the menu, 1x kept.
                    std::fprintf(stderr,"[resolution] msaa %u failed (%s); 1x\n",wantSamples,e.what());
                    live.msaaFailed=true;resizeScene(width,height,1,post);
                }
                dkCmdBufBarrier(commands,DkBarrier_None,DkInvalidateFlags_Descriptors);
                std::fprintf(stderr,"[resolution] %ux%u x%u: resize took %.2f ms\n",rw,rh,msaa,
                    double(armTicksToNs(armGetSystemTick())-resizeStart)/1e6);
            }
            live.renderWidthNow=rw;live.renderHeightNow=rh;
        }
        if(fgOn)slot=0; // v153: no swapchain image yet (the scene has its own target; presentCopy acquires)
        else{
        presentDrain(); // v164: the present thread's images shown before this queue presents again
        slot = dkQueueAcquireImage(queue, swapchain);
        if (slot < 0 || slot >= int(Frames)) throw std::runtime_error("deko3d: acquire failed");
        }
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
        lastFaintDraws=lastFaintPartial=lastFaintTriangles=0;
        lastSplitDraws=lastSplitMainDropped=lastSplitFailDropped=0;
        lastClipDraws=lastClipTriangles=0;
        if(++clipGateFrames>=300){
            clipGateFrames=0;
            std::fprintf(stderr,"[clip-gates] notex %llu nogrid %llu novtx %llu noidx %llu atest %llu op %llu opaque %llu fail %llu small %llu reached %llu\n",
                (unsigned long long)clipGate[0],(unsigned long long)clipGate[1],(unsigned long long)clipGate[2],(unsigned long long)clipGate[3],
                (unsigned long long)clipGate[4],(unsigned long long)clipGate[5],(unsigned long long)clipGate[6],(unsigned long long)clipGate[7],
                (unsigned long long)clipGate[8],(unsigned long long)clipGate[9]);
            for(auto &g:clipGate)g=0;
        }
        // v168 (GPU rewrite #2; LSFG cinematic frame 6071: a 9-vertex background quad, its texture and vertex
        // alpha full, 0.98 ms with the alpha test's discard and late depth): a background draw whose GEQUAL /
        // GREATER test passes at every fragment (least texel alpha x least vertex alpha x opacity at or above
        // the reference, as splitAlphaTest's `passes`; filtering never goes below the least texel) drawn
        // without it: no fragment was discarded by it, so the same colour and depth. The other discards stay.
        // CELOSIA_DIAG_NO_BGPASS168=1: as before.
        static const bool bgPass168Off=std::getenv("CELOSIA_DIAG_NO_BGPASS168")!=nullptr;
        lastBgAlwaysPass=0;
        if(!bgPass168Off && diagGpuMode==0)
            for(auto &draw:frame.draws){
                auto &st=draw.state;
                if(!st.background || st.shadowVolume || st.fresnel || !(st.alphaTest&1) || draw.faint || !draw.mesh)continue;
                const unsigned mode=(st.alphaTest>>1)&7;
                if(mode!=5 && mode!=6)continue;
                if(!std::isfinite(st.opacity) || st.opacity<=0)continue;
                if(st.texture && !draw.texture)continue;
                const unsigned ref=(st.alphaTest>>4)&255;
                const float need=float(mode==6?ref+1:ref);
                const float lo=(st.texture?float(draw.texture->minAlpha)/128.0f:1.0f)*st.opacity;
                if(float(draw.mesh->minAlpha)*lo*0.995f>=need){st.alphaTest=0;++lastBgAlwaysPass;}
            }
        for(auto &draw:frame.draws){cullFaint(draw);clipToTexture(draw);splitAlphaTest(draw);}
        // v170a (GPU rewrite #1): background draws whose GEQUAL / GREATER test fails everywhere (see bgnever170).
        static const bool bgNever170Off=std::getenv("CELOSIA_DIAG_NO_BGNEVER170")!=nullptr;
        lastBgNeverPass=0;
        if(!bgNever170Off && diagGpuMode==0)
            for(auto &draw:frame.draws){
                const auto &st=draw.state;
                if(!st.background || st.shadowVolume || st.fresnel || !(st.alphaTest&1) || draw.faint || !draw.mesh)continue;
                const unsigned mode=(st.alphaTest>>1)&7;
                if(mode!=5 && mode!=6)continue;
                if(!std::isfinite(st.opacity) || st.opacity<=0)continue;
                if(st.texture && !draw.texture)continue;
                const unsigned ref=(st.alphaTest>>4)&255;
                const float need=float(mode==6?ref+1:ref);
                const float hi=(st.texture?float(draw.texture->maxAlpha)/128.0f:1.0f)*st.opacity;
                if(float(draw.mesh->maxAlpha)*hi*1.005f+0.001f<need){draw.bgMainEmpty=true;++lastBgNeverPass;}
            }
        if(submittedFrames%1800u==0u)
            std::fprintf(stderr,"[gpu168] frame %u: background always-pass %u, never-pass %u, clipped draws %u, vertex copies %u\n",submittedFrames,lastBgAlwaysPass,lastBgNeverPass,lastClipDraws,lastClipCopies);
        drawTiming=diagnosticCapture;
        if(drawTiming){
            if(!drawTimeReports)drawTimeReports=std::make_shared<Buffer>(device,MaxDrawTimes*16,allocationBudget);
            std::memset(drawTimeReports->cpu(),0,MaxDrawTimes*16);
            drawTimeOwners.clear();markDrawTime(-1);
        }
        if(skyActive){
            // Layer 1 into the background image, in the frame's painter order.
            markSegment(GpuSegment::Backdrop);
            tcBarrier(DkBarrier_Fragments,0);
            bindTarget(bgColour->image,&bgDepth->image,512,448);
            if(backgroundClearFog)dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,fog.colour[0],fog.colour[1],fog.colour[2],1);
            else dkCmdBufClearColorFloat(commands,0,DkColorMask_RGBA,0,0,0,1);
            dkCmdBufClearDepthStencil(commands,true,1,0xFF,0);
            inBackgroundPass=true;
            for(std::size_t i=0;i<frame.draws.size();++i){
                const auto &draw=frame.draws[i];
                if(!draw.state.background || draw.state.shadowVolume || draw.state.reflectionSource)continue; // (v178: the mirror's)
                if(!draw.bgMainEmpty)emitDraw(draw,false,false); // v170a: not when it fails everywhere
                if(needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha))emitDraw(draw,true,false);
                markDrawTime(int(i)*4+2);
            }
            inBackgroundPass=false;
            // bgEnd: reduced to 256 x 256, then laid behind the scene, stretched back.
            tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(bgSmall->image,nullptr,256,256);
            drawImageQuad(bgColourSlot,1.0f/256.0f,1.0f/256.0f);
            tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(sceneImage(slot),&depth->image,rw,rh);
            drawImageQuad(bgSmallSlot,1.0f/float(rw),1.0f/float(rh));
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
        menuDraws.clear();resolvedDraws.clear();resolvedFrom=nullptr;compositePending=deferComposite=false;
        screenCopyMade=false; // v176
        // v177: the effects at their layers, in GL's event order (each before the first draw at or past its
        // layer, after the soft shadows and the bloom of lower layers): the blurs, the depth of field, and the
        // glow layer drawn apart then laid over. The resolved tail starts past them all.
        const bool fxOn=effectsShader!=nullptr;
        const bool dofOn=fxOn && dof.enabled && dof.slices>0 && dof.zB>0.0f && screenCopy;
        const bool glowOn=fxOn && glow.enabled && !glowFailed;
        if(!fxOn){lastEffectDrops+=uint32_t(frameBlurs.size());frameBlurs.clear();}
        std::size_t nextBlur=0;bool dofDone=!dofOn;int glowState=glowOn?0:2; // glow: 0 to come, 1 drawing apart, 2 done, 3 layer left out
        uint32_t effectLast=0;bool anyEffect=false;
        for(const auto &b:frameBlurs){effectLast=std::max(effectLast,b.order);anyEffect=true;}
        if(dofOn){effectLast=std::max(effectLast,dof.order);anyEffect=true;}
        if(glowOn){effectLast=std::max(effectLast,glow.order);anyEffect=true;}
        bool haveMirrorSource=false,mirrorTried=false,mirrorOk=false; // v178
        for(const auto &d:frame.draws)if(d.state.reflectionSource){haveMirrorSource=true;break;}
        auto effectsBefore=[&](uint32_t o){
            if(glowState>=2 && dofDone && nextBlur>=frameBlurs.size())return;
            auto prior=[&](uint32_t x){
                if(shadows.resolveBefore(x)){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount,slot);shadows.clear();markDrawTime(-4);}
                if(!bloomDone && bloom.order<x){
                    if(shadows.pending){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount,slot);shadows.clear();markDrawTime(-4);}
                    markSegment(GpuSegment::Screen);runBloom(slot);bloomDone=true;markDrawTime(-5);
                }
            };
            for(;;){
                const uint32_t bo=nextBlur<frameBlurs.size()?frameBlurs[nextBlur].order:~0u,d=dofDone?~0u:dof.order,x=std::min(bo,d);
                if(x==~0u || x>o)break;
                if(glowState==1 && x>glow.order){markSegment(GpuSegment::Screen);endGlow(slot);glowState=2;}
                prior(x);
                markSegment(GpuSegment::Screen);
                if(d<=bo){runDof(slot);dofDone=true;}
                else runBlur(frameBlurs[nextBlur++],slot);
                markDrawTime(-5);
            }
            if(glowState==1 && o>glow.order){markSegment(GpuSegment::Screen);endGlow(slot);glowState=2;markDrawTime(-5);}
            if(glowState==0 && o>=glow.order){
                prior(glow.order);
                if(o!=glow.order)glowState=2; // no draw in the layer: nothing to lay over
                else if(!(glow.amountA>0.0f) && !(glow.amountB>0.0f))glowState=3; // laid over at 0: not seen
                else glowState=beginGlow()?1:2;
            }
        };
        // v159: the tail of eligible draws (menu draws aside, drawn after the picture anyway), from where the
        // bloom has already run (its source is the picture before them); diagnostics keep 4x.
        std::size_t tailStart=frame.draws.size();
        if(resolvedTail && msaa>1 && scaled() && diagGpuMode==0){
            for(std::size_t i=frame.draws.size();i-->0;){
                const auto &d=frame.draws[i];
                if(d.state.order>=MenuOrder && d.state.screen && !d.state.shadowVolume)continue;
                if(!resolvedEligible(d))break;
                tailStart=i;
            }
            while(tailStart<frame.draws.size() && bloomOn && frame.draws[tailStart].state.order<bloom.order)++tailStart;
            if(anyEffect)while(tailStart<frame.draws.size() && frame.draws[tailStart].state.order<=effectLast)++tailStart; // v177
        }
        bool tailOpen=false;
        for (std::size_t drawIndex=0;drawIndex<frame.draws.size();++drawIndex) {
            const auto &draw=frame.draws[drawIndex];
            if(scaled() && draw.state.order>=MenuOrder && draw.state.screen && !draw.state.shadowVolume){menuDraws.push_back(drawIndex);continue;}
            effectsBefore(draw.state.order); // v177
            if(glowState==3 && draw.state.order==glow.order)continue;
            if(draw.state.reflectionSource)continue; // v178: drawn into the mirror (drawMirror)
            if(draw.state.reflectionSample){
                if(!mirrorTried){mirrorTried=true;markSegment(GpuSegment::Screen);mirrorOk=haveMirrorSource && drawMirror(slot,frame.draws);}
                if(!mirrorOk)continue; // GL: no mirror drawn, its reads left out
            }
            if(draw.state.refractionSample){markSegment(GpuSegment::Screen);if(!makeScreenCopy(slot,2)){++lastScreenCopyDrops;continue;}}
            if(drawIndex>=tailStart){
                // Shadows still pending here are resolved before this draw only if the original order does so
                // (resolveBefore); otherwise they would darken the tail too: then the tail stays on the 4x picture.
                if(!tailOpen){
                    tailOpen=true;
                    if(shadows.pending && !shadows.resolveBefore(draw.state.order))tailStart=frame.draws.size();
                    else if(shadows.pending){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount,slot);shadows.clear();markDrawTime(-4);}
                    if(tailStart<frame.draws.size() && !bloomDone){deferComposite=true;markSegment(GpuSegment::Screen);runBloom(slot);bloomDone=true;markDrawTime(-5);}
                }
                if(drawIndex>=tailStart){resolvedDraws.push_back(drawIndex);resolvedFrom=&frame.draws;continue;}
            }
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
                // v170a: the merged pass draws every triangle (the split halves are for the two-pass forms).
                if(merged.wholeMesh)merged.mesh=merged.wholeMesh;
                merged.mainEmpty=merged.failureEmpty=merged.bgMainEmpty=false;merged.failureMesh=nullptr;
                markSegment(GpuSegment::Backdrop);
                emitDraw(merged,false,prepass.farBackdrop(drawIndex,draw.state));
                markDrawTime(int(drawIndex)*4);
                continue;
            }
            if(draw.state.screenCopy){ // v176: the picture so far, copied before the first of them
                markSegment(GpuSegment::Screen);
                if(!makeScreenCopy(slot)){++lastScreenCopyDrops;continue;}
            }
            markSegment(draw.state.background?GpuSegment::Backdrop:draw.state.screen?GpuSegment::Screen:
                canUseOpaqueFragment(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha,false)?GpuSegment::Opaque:GpuSegment::Generic);
            if(!draw.bgMainEmpty)emitDraw(draw,false,prepass.farBackdrop(drawIndex,draw.state)); // v170a
            markDrawTime(int(drawIndex)*4);
            // v113: 19 (j) each screen draw 4 more times, the same state, in the prepass segment (measures only).
            if(diagGpuMode==19 && draw.state.screen){
                markSegment(GpuSegment::Prepass);
                for(int q=0;q<4;++q)emitDraw(draw,false,prepass.farBackdrop(drawIndex,draw.state));
                markSegment(GpuSegment::Screen);
            }
            if (failure && diagGpuMode!=1) {markSegment(GpuSegment::Failure);emitDraw(draw, true);markDrawTime(int(drawIndex)*4+1);}
        }
        effectsBefore(~0u); // v177: the effects past the last draw
        {
            static uint32_t fxFrames=0;
            if(++fxFrames%600u==0u)
                std::fprintf(stderr,"[fx177] blurs %u, depth of field %u, glow %u, left out %u; screen copies %u (%u left out); mirrors %u (%u draws)\n",
                             lastBlurs,lastDofs,lastGlows,lastEffectDrops,lastScreenCopies,lastScreenCopyDrops,lastMirrors,lastMirrorDraws);
        }
        if(attributeCoverage)lastCoverage=coverage;
        if(shadows.pending){markSegment(GpuSegment::Shadow);resolveShadows(shadows.amount,slot);markDrawTime(-4);}
        if(!bloomDone){deferComposite=resolvedTail && msaa>1 && scaled() && diagGpuMode==0;markSegment(GpuSegment::Screen);runBloom(slot);markDrawTime(-5);}
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
            bindTarget(bloomMask->image,diagGpuMode==16?&depth->image:nullptr,rw,rh);
            const float copy[4]{0,0,1.0f/float(rw),1.0f/float(rh)};
            for(int q=0;q<8;++q)
                bloomQuad(diagGpuMode==15?0:1,diagGpuMode==15?bgSmallSlot:DescriptorSlots::None,DescriptorSlots::None,copy,nullptr,
                    diagGpuMode==14?1:0,diagGpuMode==16?0.999f:-1.0f);
            tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            bindTarget(sceneImage(slot),&depth->image,rw,rh);
        }
        // v150: a scene below the output scaled onto the swapchain image (linear), then the menu.
        if(fgOn){
            auto &live=celosia::live::settings();
            const lsfg::Img &input=frameGen->input();
            const uint32_t fgIn=frameGen->inputIndex(); // v173m: the history image of this phase (Cemu-nx's m_frame % 2)
            // v167: the image the post pass writes next may still be shown from (its present job two real
            // frames back): that job's copies done first (normally long since).
            // v170: the same for the generation's output written this frame (shown two real frames back).
            for(int *jp:{&inputJobSlot[fgIn],&genJobSlot[fgIn]})if(int &j=*jp;j>=0){
                {
                    std::unique_lock<std::mutex> lock(presentMutex);
                    presentCv.wait(lock,[&]{return !presentBusy[j];});
                }
                if(presentDonePending[j] && dkFenceWait(&presentDone[j],-1)!=DkResult_Success)throw std::runtime_error("deko3d: present fence wait failed");
                j=-1;
            }
            frameGen->stamp(commands,fgFrame,0);
            postQuad(input.image->image,fgInSlot[fgIn]);
            tcBarrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            frameGen->stamp(commands,fgFrame,1);
            const bool fgReady=frameGen->recordInput(commands,fgFrame); // v173m: past the schedule's warmup
            frameGen->stamp(commands,fgFrame,2);
            // Dynamic (the user's rule): always one generated frame per real one, so the picture runs at twice
            // the real rate whatever it is (25 real -> 50 shown); nothing fills up to the target.
            const uint64_t nowNs=armTicksToNs(armGetSystemTick());
            fgCount=1;
            if(fgLastRealNs){
                const double d=std::min(100e6,double(nowNs-fgLastRealNs));
                fgRealEma=fgRealEma>0?fgRealEma*0.85+d*0.15:d;
                live.frameGenReal=uint32_t(1e11/fgRealEma);
            }
            fgLastRealNs=nowNs;
            // v155: pacing on the display's measured refresh. Each image is shown for the mode's period (60 /
            // 90 / 120 per second: on a 120 Hz display a 60 mode image stays two refreshes, so the real frames
            // stay at 30); when the real frames come slower, the generated one gets half of the real interval.
            const double refreshNs=1000.0*double(std::max<uint32_t>(4000u,live.refreshUs.load(std::memory_order_relaxed)));
            const int mode=std::clamp(live.frameGen.load(std::memory_order_relaxed),1,2); // v173s: no 120 mode
            const int fgInterval=std::max(1,int(1e9/double(mode==1?60:(mode==2?90:120))/refreshNs+0.5));
            const int fgIntervalGen=std::clamp(int(fgRealEma*0.5/refreshNs+0.5),fgInterval,4);
            // Its GPU stages, two real frames back (done: the frame before waited for it), every 300 frames.
            if(fgFrame>=2){
                uint64_t st[4]{};
                const uint64_t g=fgFrame-2;
                if(frameGen->stageNs(g,0,1,st[0]) && frameGen->stageNs(g,1,2,st[1]) && frameGen->stageNs(g,2,3,st[2]) && frameGen->stageNs(g,3,4,st[3])){
                    for(int k=0;k<4;++k)fgStageSum[k]+=st[k];
                    fgLsfgNs=st[0]+st[1]+st[2]+st[3];
                    if(++fgStageCount==300){
                        const double n=1e6*fgStageCount;
                        std::fprintf(stderr,"[lsfg] gpu ms post %.2f input %.2f generate %.2f copy+menu %.2f whole frame %.2f | real %.1f shown %.1f refresh %.2f Hz interval %d/%d\n",
                            fgStageSum[0]/n,fgStageSum[1]/n,fgStageSum[2]/n,fgStageSum[3]/n,lastGpuNs/1e6,
                            live.frameGenReal.load()/100.0,live.frameGenShown.load()/100.0,1e9/refreshNs,fgIntervalGen,fgInterval);
                        for(auto &v:fgStageSum)v=0;
                        fgStageCount=0;
                    }
                }
            }
            frameGen->stamp(commands,fgFrame,3);
            boundImages=0;boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
            bindSceneSamplers();
            if(presentAsync && fgCount==1){
                // v164: the pictures into the display pair k (menu over each); the present thread shows them.
                ensureDisplayImages();
                const uint32_t k=presentSlot;presentSlot^=1u;
                const uint64_t paceWaitA=armTicksToNs(armGetSystemTick());
                {
                    std::unique_lock<std::mutex> lock(presentMutex);
                    presentCv.wait(lock,[&]{return !presentBusy[k];});
                    // v173n: the render queue waits (on the GPU, not here) for the present thread's copies out of slot k
                    // (with the slot freed before the real picture is shown, they may still be running).
                    if(presentDonePending[k])dkQueueWaitFence(queue,&presentDone[k]);
                }
                const uint64_t paceWaitB=armTicksToNs(armGetSystemTick());
                const bool gen=fgReady;
                auto menuOver=[&](const DkImage &target){
                    bindTarget(target,nullptr,ow,oh);
                    boundImages=0;boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
                    bindSceneSamplers();
                    for(const std::size_t i:menuDraws){
                        const auto &draw=frame.draws[i];
                        emitDraw(draw,false,false);
                        if(needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha))emitDraw(draw,true,false);
                    }
                };
                tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
                // v170 (LSFG improvement): with no menu over it, the generated picture is shown from the
                // generation's output itself (the present thread copies it to the swapchain image, as it did
                // dispGen): one full-screen pass less a generated frame; the same pixels.
                const lsfg::Img &genOut=frameGen->outputFor(fgFrame,0);
                const bool directGen=false && gen && lsfgDirectGen && // v173m: one output image (Cemu-nx graph), copied
                     menuDraws.empty() && genOut.w==ow && genOut.h==oh;
                if(gen && !directGen){
                    bindTarget(dispGen[k]->image,nullptr,ow,oh);
                    blitTexture(dkMakeTextureHandle(genOut.tex,2),frameGen->imageSetGpu(),frameGen->imageSlots(),frameGen->samplerSetGpu(),3);
                    menuOver(dispGen[k]->image);
                }
                // v167: with no menu to draw over it, the real picture is shown from the input image itself (the
                // present thread copies it to the swapchain image): one full-screen copy less a real frame.
                const bool direct=lsfgDirectReal && menuDraws.empty() && input.w==ow && input.h==oh;
                if(!direct){
                    {
                        const DkImageView from=input.image->view(),to=dispReal[k]->view();
                        const DkImageRect rect{0,0,0,ow,oh,1};
                        tcFlush();dkCmdBufCopyImage(commands,&from,&rect,&to,&rect,0);
                        tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
                    }
                    menuOver(dispReal[k]->image);
                }
                tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
                frameGen->stamp(commands,fgFrame,4);
                {
                    static const unsigned dumpMax=[]{const char *v=std::getenv("CELOSIA_DIAG_LSFG_DUMP");return v?unsigned(std::atoi(v)):0u;}();
                    // v173l: only while the trigger file exists (the test makes it in gameplay), every 20th frame.
                    static unsigned dumpPoll=0;
                    bool dumpGo=false;
                    if(dumpMax && lsfgDumpCount<dumpMax && gen && fgFrame%20==0 && genOut.w==ow && genOut.h==oh && ++dumpPoll){
                        struct stat st;dumpGo=stat("sdmc:/switch/celosia_nx/lsfgdump/go",&st)==0;
                    }
                    if(dumpGo){
                        const auto packed=packedImageLayout(ow,oh);
                        lsfgDump=std::make_shared<Buffer>(device,size_t(ow)*oh*12u,allocationBudget,uint32_t(DkMemBlockFlags_CpuCached|DkMemBlockFlags_GpuCached));lsfgDumpW=ow;lsfgDumpH=oh;
                        const lsfg::Img *imgs[3]={&frameGen->input(fgFrame+1),&genOut,&input};
                        tcFlush();
                        for(int i=0;i<3;++i){
                            const DkImageView v=imgs[i]->image->view();const DkImageRect r{0,0,0,ow,oh,1};
                            const DkCopyBuf d{lsfgDump->gpu()+uint64_t(i)*ow*oh*4u,packed.rowBytes,packed.layerBytes};
                            dkCmdBufCopyImageToBuffer(commands,&v,&r,&d,0);
                        }
                        tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
                    }
                }
                dkCmdBufSignalFence(commands,&presentReady[k],true);
                {std::lock_guard<std::mutex> lock(presentMutex);presentBusy[k]=true;}
                pendingJob=PresentJob{k,fgIntervalGen,fgInterval,gen,direct?input.image:nullptr,directGen?genOut.image:nullptr};pendingJobValid=true;
                pendingJob.cpuStart=cpuStart;pendingJob.waitA=paceWaitA;pendingJob.waitB=paceWaitB;
                if(direct)inputJobSlot[fgIn]=int(k); // handed over after the submit below
                if(directGen)genJobSlot[fgIn]=int(k);
                slot=latestSlot>=0?latestSlot:0;
            }else{
            if(fgReady)for(uint32_t p=0;p<fgCount;++p)presentCopy(frameGen->outputFor(fgFrame,p),frame,fgIntervalGen);
            // The real frame: acquired last; the tail below presents it. Submitted first (v156, as above).
            dkQueueSubmitCommands(queue,tcFinish());
            dkQueueFlush(queue);
            slot=dkQueueAcquireImage(queue,swapchain);
            if (slot < 0 || slot >= int(Frames)) throw std::runtime_error("deko3d: acquire failed");
            tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
            {
                const DkImageView from=input.image->view(),to=colors[slot]->view();
                const DkImageRect rect{0,0,0,ow,oh,1};
                tcFlush();dkCmdBufCopyImage(commands,&from,&rect,&to,&rect,0);
            }
            tcBarrier(DkBarrier_Full,DkInvalidateFlags_Image);
            if(fgInterval!=swapIntervalNow){dkSwapchainSetSwapInterval(swapchain,fgInterval);swapIntervalNow=fgInterval;}
            bindTarget(colors[slot]->image,nullptr,ow,oh);
            boundImages=0;boundFragment=-1;boundDepth=boundMask=boundBlend=-1;boundHandleValid=false;
            bindSceneSamplers();
            for(const std::size_t i:menuDraws){
                const auto &draw=frame.draws[i];
                emitDraw(draw,false,false);
                if(needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha))emitDraw(draw,true,false);
            }
            frameGen->stamp(commands,fgFrame,4);
            }
            fgShownCount+=(fgReady?fgCount:0)+1;
            static uint32_t fgRealCount=0;++fgRealCount;
            if(!fgShownStart){fgShownStart=nowNs;fgRealCount=0;}
            else if(nowNs-fgShownStart>=1'000'000'000ULL){
                live.frameGenShown=uint32_t(double(fgShownCount)*1e11/double(nowNs-fgShownStart));
                // v162 (benchmarks, CELOSIA_DIAG_PRESENT_LOG): each second's presented and real frames, so a run
                // shows whether every second of it was doubled (the present log holds the real frames only).
                static const bool perSecond=std::getenv("CELOSIA_DIAG_PRESENT_LOG")!=nullptr;
                if(perSecond)std::fprintf(stderr,"[fgsec] %.3f shown %.1f real %.1f\n",nowNs/1e9,
                    double(fgShownCount)*1e9/double(nowNs-fgShownStart),double(fgRealCount)*1e9/double(nowNs-fgShownStart));
                fgRealCount=0;
                fgShownStart=nowNs;fgShownCount=0;
            }
            ++fgFrame;
        }else if(scaled()){
            postQuad(colors[slot]->image);
            for(const std::size_t i:menuDraws){
                const auto &draw=frame.draws[i];
                markSegment(GpuSegment::Screen);
                emitDraw(draw,false,false);
                if(needsAlphaFailurePass(draw.state,draw.mesh->minAlpha,draw.texture->minAlpha))emitDraw(draw,true,false);
            }
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
        // v173h: with frame generation the real picture is read from the image its present job shows (the
        // generation's input or dispReal), not the swapchain image (v172: no capture, photo mode saved nothing).
        if (captureRequested && (!pendingJobValid || pendingJob.real || dispReal[pendingJob.k])) {
            frame.capture = std::make_shared<Buffer>(device, ow * oh * 4, allocationBudget);
            const auto view = !pendingJobValid ? colors[slot]->view() : pendingJob.real ? pendingJob.real->view() : dispReal[pendingJob.k]->view();
            const DkImageRect region{0, 0, 0, ow, oh, 1};
            const auto packed = packedImageLayout(ow, oh);
            captureW=ow;captureH=oh;
            const DkCopyBuf destination{frame.capture->gpu(), packed.rowBytes, packed.layerBytes};
            dkCmdBufBarrier(commands, DkBarrier_Full, 0);
            tcFlush();dkCmdBufCopyImageToBuffer(commands, &view, &region, &destination, 0);
            latestCapture = frame.capture; captureFrameIndex = frameIndex;
            captureRequested = false;
        }
    }
    // v173r tried waiting for the previous frame's fence before the submit (less latency, ~66 vs ~88 ms, but a
    // 1 % low of 21.6 vs 27.4 FPS in the same settings); v173s: back to v173q's behaviour (the user's choice).
    // CELOSIA_DIAG_SUBMIT_GATE173=1: the v173r wait (A/B).
    uint64_t gateNs=0;
    if(fgOn && presentAsync){
        static const bool noGate=std::getenv("CELOSIA_DIAG_SUBMIT_GATE173")==nullptr;
        auto &previous=frames[(frameIndex+Frames-1)%Frames];
        if(!noGate && previous.pending){
            const uint64_t g0=armTicksToNs(armGetSystemTick());
            if(dkFenceWait(&previous.fence,-1)!=DkResult_Success)throw std::runtime_error("deko3d: frame fence wait failed");
            gateNs=armTicksToNs(armGetSystemTick())-g0;
        }
    }
    const auto submitStart=armTicksToNs(armGetSystemTick());
    markRender(RenderPhase::Submit);
    dkCmdBufSignalFence(commands, &frame.fence, true);
    const auto subA=armTicksToNs(armGetSystemTick());
    dkQueueSubmitCommands(queue, tcFinish());
    const auto subB=armTicksToNs(armGetSystemTick());
    frame.pending = true;
    if(pendingJobValid){
        // v164: the display pair goes to the present thread once its fence is submitted (above).
        dkQueueFlush(queue);
        pendingJob.subA=subA;pendingJob.subB=subB;pendingJob.gate=gateNs;
        pendingJob.push=armTicksToNs(armGetSystemTick());
        {std::lock_guard<std::mutex> lock(presentMutex);presentJobs.push_back(pendingJob);}
        presentCv.notify_all();
        presentUsed=true;pendingJobValid=false;
        if(lsfgDump){
            // v173k CELOSIA_DIAG_LSFG_DUMP: the previous real, the generated and the new real picture of this frame.
            dkQueueWaitIdle(queue);
            mkdir("sdmc:/switch/celosia_nx/lsfgdump",0777);
            char path[96];std::snprintf(path,sizeof(path),"sdmc:/switch/celosia_nx/lsfgdump/t%02u-%ux%u.rgba",lsfgDumpCount,lsfgDumpW,lsfgDumpH);
            armDCacheFlush(lsfgDump->cpu(),size_t(lsfgDumpW)*lsfgDumpH*12u); // CPU-cached: drop stale lines before reading the GPU's copy
            if(FILE *f=std::fopen(path,"wb")){static std::vector<char> vb(4u<<20);std::setvbuf(f,vb.data(),_IOFBF,vb.size());std::fwrite(lsfgDump->cpu(),1,size_t(lsfgDumpW)*lsfgDumpH*12u,f);std::fclose(f);}
            std::fprintf(stderr,"[lsfgdump] %s\n",path);
            lsfgDump.reset();++lsfgDumpCount;
        }
    }else
    if (!held) {markRender(RenderPhase::Present);dkQueuePresentImage(queue, swapchain, slot);latestSlot=slot;
        if(celosia_frame_presented)celosia_frame_presented();}
    markRender(RenderPhase::Flush);
    dkQueueFlush(queue);
    const auto submitEnd=armTicksToNs(armGetSystemTick());
    activeFrame = nullptr;
    markRender(RenderPhase::Idle);
    // v154: low latency with frame generation: at most one frame queued behind the one on the GPU. The
    // game reads its input for the next frame only once the previous frame's GPU work (its generated and
    // real images queued for display) is done, instead of running up to two real frames ahead.
    // v165 (user: "un punto medio con la latencia"): the wait is bounded by the real frame's budget. A frame
    // whose game + render time since the last exit leaves room under 33 ms still waits (low latency as v154);
    // one already near 33 ms waits only what fits (at most 1 ms), so CPU and GPU overlap again and it keeps
    // 30 real frames. The frame ring's own fence wait (beginFrame, 3 frames) is untouched.
    // CELOSIA_DIAG_FG_LATENCY=full: v154's unbounded wait; =bounded: v165's.
    // v173b (user: smoothness very poor, the picture drops to ~35 shown when the camera turns, Performance too;
    // [fgsec173]: those seconds had 17-19 real frames and no held ones): the wait made the game's CPU frame and the
    // GPU's run one after the other in heavy views (real rate = 1 / (cpu + gpu)); by default now no wait, they
    // overlap again and the frames queued for display absorb the real frames' jitter (more latency).
    if(fgOn){
        static const int mode=[]{const char *v=std::getenv("CELOSIA_DIAG_FG_LATENCY");
            return v==nullptr?0:std::strcmp(v,"full")==0?2:std::strcmp(v,"bounded")==0?1:0;}();
        auto &previous=frames[(frameIndex+Frames-1)%Frames];
        if(previous.pending && mode!=0){
            int64_t timeout=-1;
            if(mode==1){
                const uint64_t now=armTicksToNs(armGetSystemTick());
                const int64_t used=fgLastExitNs?int64_t(now-fgLastExitNs):0;
                timeout=std::max<int64_t>(int64_t(1000000),int64_t(33000000)-used);
            }
            const DkResult r=dkFenceWait(&previous.fence,timeout);
            if(r!=DkResult_Success && r!=DkResult_Timeout)
                throw std::runtime_error("deko3d: low-latency fence wait failed");
        }
    }
    fgLastExitNs=armTicksToNs(armGetSystemTick());
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
    // v180 (report: the on-screen counter changed every 5 s): it counts the images shown over half a second;
    // the 5 s window below stays for the logs.
    {
        static uint64_t counterStart=0;static uint32_t counterFrames=0;
        if(!held)++counterFrames;
        if(!counterStart || sampleEndNs<counterStart){counterStart=sampleEndNs;counterFrames=0;}
        else if(sampleEndNs-counterStart>=500000000ull){
            const double fps=double(counterFrames)*1e9/double(sampleEndNs-counterStart);
            celosia::live::settings().fpsCenti.store(uint32_t(fps*100.0+0.5),std::memory_order_relaxed);
            counterStart=sampleEndNs;counterFrames=0;
        }
    }
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
                std::fprintf(log," | %.2f %.2f %.2f %.2f %.2f %.2f %u",gpuCounterWindow.fragmentInvocationsPerPixel(uint64_t(rw)*rh),
                    lastCoverage.opaque,lastCoverage.generic,lastCoverage.failure,lastCoverage.backdrop,lastCoverage.shadow,lastCoverage.fullScreenDraws);
                std::fprintf(log," | mode %d bgFogged %u bgTarget %u lean %u generic %u faint %u %u %u split %u %u %u clip %u %u",diagGpuMode,lastBgFogged,lastBgTarget,lastLeanDraws,lastGenericDraws,
                    lastFaintDraws,lastFaintPartial,lastFaintTriangles,lastSplitDraws,lastSplitMainDropped,lastSplitFailDropped,lastClipDraws,lastClipTriangles);
                if(rt.n){const double k=1.0/rt.n;
                    std::fprintf(log," | rt out %.2f draw %.2f mesh %.2f tex %.2f fence %.2f up %.2f rec %.2f sub %.2f draws %.0f sort %.2f",
                        rt.out*k,rt.draw*k,rt.mesh*k,rt.tex*k,rt.fence*k,rt.up*k,rt.rec*k,rt.sub*k,rt.draws*k,rt.sort*k);rt=RtSums{};}
                // v165: the share of material rows pushed (the rest unchanged since the previous push).
                if(pushRowsAll){std::fprintf(log," | push165 %.1f",100.0*double(pushRowsSent)/double(pushRowsAll));pushRowsSent=pushRowsAll=0;}
                if(!slowFrames.empty()){std::fprintf(log," | slow%s",slowFrames.c_str());slowFrames.clear();}
                std::fputc(10,log);
                std::fclose(log);
                sdWriter().post({"sdmc:/switch/celosia_nx/present-times.txt",append,true,std::string(logBuf,logLen)});
                std::free(logBuf);
            }
            presentTenths.clear();
        }
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
                gpuCounterWindow.averageFragments(),gpuCounterWindow.averagePrimitives(),gpuCounterWindow.fragmentInvocationsPerPixel(uint64_t(rw)*rh))>0;
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
            else sdWriter().post({"sdmc:/switch/celosia_nx/hardware-status.txt",false,false,std::string(statusBuf,statusLen)});
            std::free(statusBuf);
        }else std::fputs("[hardware] status file unavailable\n",stderr);
        gpuTimeWindow.reset();
        gpuCounterWindow.reset();
        if(!diagGpuCycle.empty()){diagGpuIndex=(diagGpuIndex+1)%diagGpuCycle.size();diagGpuMode=diagGpuCycle[diagGpuIndex];}
    }
    if(diagnosticCapture) {
        std::vector<uint8_t> rgba;uint32_t w=0,h=0;
        // v168: with frame generation the present thread owns the swapchain images (no picture to read here):
        // the draw list and its timings written without the picture (v167e: the capture threw, std::terminate).
        const bool picture=latestFrame(rgba,w,h);
        char capturePath[128];
        if(picture){
        std::snprintf(capturePath,sizeof(capturePath),"sdmc:/switch/celosia_nx/native-frame-%06u.ppm",submittedFrames);
        auto *file=std::fopen(capturePath,"wb");
        if(!file)throw std::runtime_error("deko3d: diagnostic capture file unavailable");
        bool okay=std::fprintf(file,"P6\n%u %u\n255\n",w,h)>0;
        std::vector<uint8_t> rgb(size_t(w)*h*3);
        for(size_t i=0;i<size_t(w)*h;++i)std::copy_n(rgba.data()+i*4,3,rgb.data()+i*3);
        okay=std::fwrite(rgb.data(),1,rgb.size(),file)==rgb.size() && okay;
        okay=std::fclose(file)==0 && okay;
        if(!okay)throw std::runtime_error("deko3d: diagnostic capture write failed");
        }
        std::fprintf(stderr,"[deko3d] captured native frame %u: %ux%u, %zu draws%s\n",submittedFrames,w,h,frame.draws.size(),picture?"":" (no picture: frame generation)");
        // The frame's draws in recorded order, to see which ones a heavy view is made of.
        std::snprintf(capturePath,sizeof(capturePath),"sdmc:/switch/celosia_nx/native-frame-%06u.csv",submittedFrames);
        std::vector<double> usColour(frame.draws.size()),usFailure(frame.draws.size()),usBackground(frame.draws.size());
        double usOther[16]{};
        if(drawTimeReports && drawTimeOwners.size()>1){
            const auto *reports=static_cast<const GpuReport *>(drawTimeReports->cpu());
            for(std::size_t k=1;k<drawTimeOwners.size();++k){
                const auto ns=gpuIntervalNs(reports[k-1],reports[k]);if(!ns)continue;
                const double us=double(*ns)/1000.0;const int owner=drawTimeOwners[k];
                if(owner<0){if(-owner<16)usOther[-owner]+=us;continue;}
                const std::size_t index=std::size_t(owner/4);if(index>=frame.draws.size())continue;
                (owner%4==0?usColour:owner%4==1?usFailure:usBackground)[index]+=us;
            }
        }
        if(auto *list=std::fopen(capturePath,"w")){
            std::fprintf(list,"# gpu_us other: sky_copies=%.1f prepass=%.1f shadow_resolve=%.1f bloom_composite=%.1f bloom_mask=%.1f bloom_chain=%.1f shadow_mask=%.1f shadow_reduce=%.1f shadow_blur=%.1f resolve=%.1f post=%.1f\n",
                usOther[2],usOther[3],usOther[4],usOther[5],usOther[10],usOther[11],usOther[12],usOther[13],usOther[14],usOther[6],usOther[7]);
            std::fprintf(list,"index,order,background,screen,shadow,blend,alpha_test,fogged,overlay,texture,tex_min_alpha,vertices,indices,coverage,max_w,opaque,failure,opacity,lit,fresnel,us_colour,us_failure,us_background,tex_w,tex_h,tex_levels,lod_k,lod_scale,max_lod,diag_mode,tex_max_alpha,tex_full_pm,tex_clear_pm,mesh_min_alpha,mesh_max_alpha,faint,draw_indices\n");
            for(std::size_t i=0;i<frame.draws.size();++i){
                const auto &d=frame.draws[i];const auto &s=d.state;
                std::fprintf(list,"%zu,%u,%d,%d,%d,%d,0x%04x,%d,%d,%llx,%u,%u,%u,%.4f,%.1f,%d,%d,%.3f,%d,%d,%.1f,%.1f,%.1f,%u,%u,%u,%.3f,%.3f,%.3f,%d,%u,%u,%u,%u,%u,%d,%u\n",
                    i,s.order,int(s.background),int(s.screen),int(s.shadowVolume),int(s.blend),unsigned(s.alphaTest),int(s.fogged),int(s.overlay),
                    static_cast<unsigned long long>(s.texture),unsigned(d.texture->minAlpha),d.mesh->count,d.mesh->indexCount,
                    screenCoverage(d.clip.data(),d.mesh->bounds,s.screen),maxClipW(d.clip.data(),d.mesh->bounds),
                    int(canUseOpaqueFragment(s,d.mesh->minAlpha,d.texture->minAlpha,false)),
                    int(needsAlphaFailurePass(s,d.mesh->minAlpha,d.texture->minAlpha)),s.opacity,int(s.lit),int(s.fresnel),
                    usColour[i],usFailure[i],usBackground[i],unsigned(d.texture->width),unsigned(d.texture->height),
                    unsigned(d.texture->levels),s.lodK,s.lodScale,s.maxLod,diagGpuMode,
                    unsigned(d.texture->maxAlpha),unsigned(d.texture->fullTexels),unsigned(d.texture->clearTexels),
                    unsigned(d.mesh->minAlpha),unsigned(d.mesh->maxAlpha),int(d.faint),d.mesh->indexCount);
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
    // v173b: asked mid-frame (the host's present path reads it whenever a capture exists: the photo mode's free
    // camera draws frames while the game is frozen): no picture this time instead of std::terminate.
    if (recording) return false;
    // No commands access an image that has already been handed to the compositor.
    // A reused frame fence is safe too: waiting for newer queue work also orders
    // the earlier capture. The immutable readback allocation is retained separately.
    if (dkFenceWait(&frames[captureFrameIndex].fence, -1) != DkResult_Success)
        throw std::runtime_error("deko3d: readback fence wait failed");
    const auto *bytes = static_cast<const uint8_t *>(latestCapture->cpu());
    rgba.assign(bytes, bytes + captureW * captureH * 4); w = captureW; h = captureH;
    return true;
}
}
