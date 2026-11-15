#pragma once
#include "runtime/native_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
namespace native_renderer::detail::deko {
inline uint8_t minimumVertexAlpha(const Vertex *vertices,uint32_t count){
    if(!vertices || !count)throw std::invalid_argument("Invalid vertex alpha range");
    uint8_t result=255;
    for(uint32_t i=0;i<count;++i)result=std::min(result,vertices[i].a);
    return result;
}
// bytes covers every uploaded mip, not just level zero. Linear filtering and
// mip interpolation are convex combinations, so their alpha cannot be lower.
inline uint8_t minimumTextureAlpha(const uint8_t *rgba,size_t bytes){
    if(!rgba || !bytes || bytes%4)throw std::invalid_argument("Invalid texture alpha range");
    uint8_t result=255;
    for(size_t i=3;i<bytes;i+=4)result=std::min(result,rgba[i]);
    return result;
}
inline bool alphaTestAlwaysPasses(const DrawState &state,uint8_t vertexAlpha,uint8_t textureAlpha){
    if(!(state.alphaTest&1))return true;
    const unsigned mode=(state.alphaTest>>1)&7;
    if(mode==1)return true;
    if((mode!=5 && mode!=6 && mode!=7) || state.fresnel || !std::isfinite(state.opacity) || state.opacity<=0 ||
       !vertexAlpha || (state.texture && !textureAlpha))return false;
    // Match alphaPass's pre-clamp GS value, not the output's 2/255 alpha
    // normalization. Its shader floor(raw+0.001) has a full-unit safety margin.
    const double raw=double(vertexAlpha)*double(state.opacity)*(state.texture?double(textureAlpha)/128.0:1.0);
    const unsigned reference=(state.alphaTest>>4)&255;
    return raw>=double(reference)+(mode==5?1.0:2.0);
}
inline bool needsAlphaFailurePass(const DrawState &state,uint8_t vertexAlpha,uint8_t textureAlpha){
    return (state.alphaTest&1) && ((state.alphaTest>>12)&3) && !alphaTestAlwaysPasses(state,vertexAlpha,textureAlpha);
}
inline bool canUseOpaqueFragment(const DrawState &state,uint8_t vertexAlpha,uint8_t textureAlpha,bool failurePass){
    if(state.screen || state.background || state.fresnel || failurePass || state.shadowVolume ||
       state.reflectionSource || state.reflectionSample || state.refractionSample || state.screenCopy || state.previousFrame ||
       (state.blend!=Blend::Normal && state.blend!=Blend::Opaque) || !std::isfinite(state.opacity) || state.opacity<=0)
        return false;
    if(!alphaTestAlwaysPasses(state,vertexAlpha,textureAlpha))return false;
    // The vertex shader only scales alpha by opacity when Fresnel is absent.
    // Keep a margin above the original fragment shader's 0.02 discard edge.
    const double shade=double(vertexAlpha)/128.0*double(state.opacity);
    const double alpha=shade*(state.texture?2.0*double(textureAlpha)/255.0:1.0);
    return alpha>=0.03;
}
}
