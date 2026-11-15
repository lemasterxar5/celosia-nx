#pragma once
// Depth pre-pass planning. The backdrop (sky, distant layers) is drawn first in painter order
// without depth testing, so every opaque world pixel in front of it is shaded twice. The plan
// lays down the depth of provably opaque world draws first, then lets the leading backdrop
// draws test against it at the far plane. Rejected backdrop fragments are exactly the ones a
// later opaque world draw overwrites, so the final image is unchanged.
#include "opaque_policy.hpp"
#include "pipeline.hpp"
#include <cstddef>
#include <vector>
namespace native_renderer::detail::deko {
// Replaces every covered pixel's RGBA and writes depth with the ordinary Lequal test.
inline bool prepassEligible(const DrawState &s,uint8_t vertexAlpha,uint8_t textureAlpha){
    if(s.debugColour || !canUseOpaqueFragment(s,vertexAlpha,textureAlpha,false) ||
       needsAlphaFailurePass(s,vertexAlpha,textureAlpha))return false;
    const auto key=pipelineKey(s,false);
    if(key.depth!=3 || key.mask!=15)return false;
    if(s.blend==Blend::Opaque)return true;
    // Normal blending only replaces the destination when the shader alpha saturates at 1.
    const double shade=double(vertexAlpha)/128.0*double(s.opacity);
    return shade*(s.texture?2.0*double(textureAlpha)/255.0:1.0)>=1.0;
}
inline bool writesDepth(const DrawState &s,bool failure){
    return (pipelineKey(s,false).depth&2) || (failure && (pipelineKey(s,true).depth&2));
}
// Stops the plan: stencil users, framebuffer readers, and depth writers outside the plan
// would observe depth (or colour) the original painter order never produced at that point.
inline bool prepassBarrier(const DrawState &s,bool eligible,bool failure){
    return s.shadowVolume || s.screenCopy || s.previousFrame || s.reflectionSource ||
        s.reflectionSample || s.refractionSample || (!eligible && writesDepth(s,failure));
}
struct PrepassPlan {
    std::size_t first=0,barrier=0;   // eligible draws in [first,barrier) are pre-passed
    bool active=false;
    void clear(){first=barrier=0;active=false;}
    // Leading backdrops (before any depth write) test against the planned depth.
    bool farBackdrop(std::size_t i,const DrawState &s)const{
        return active && i<first && s.background && !s.screenDepthTest;
    }
};
// flags[i] is set for pre-passed draws. Access is through callbacks so the host tests do not
// need GPU meshes.
template<class StateAt,class AlphaAt>
PrepassPlan planPrepass(std::size_t count,StateAt stateAt,AlphaAt alphaAt,std::vector<uint8_t> &flags){
    PrepassPlan plan;flags.assign(count,0);
    std::size_t first=count,barrier=count,eligibleCount=0;
    for(std::size_t i=0;i<count;++i){
        const DrawState &s=stateAt(i);
        uint8_t va=0,ta=0;alphaAt(i,va,ta);
        const bool eligible=!s.shadowVolume && prepassEligible(s,va,ta);
        if(prepassBarrier(s,eligible,!s.shadowVolume && needsAlphaFailurePass(s,va,ta))){barrier=i;break;}
        if(eligible){if(first==count)first=i;flags[i]=1;++eligibleCount;}
    }
    if(!eligibleCount){flags.assign(count,0);return plan;}
    plan.first=first;plan.barrier=barrier;plan.active=true;
    // Any leading backdrop must precede the first depth write (eligible writers are the only
    // writers before the barrier), which holds by construction of `first`.
    return plan;
}
}
