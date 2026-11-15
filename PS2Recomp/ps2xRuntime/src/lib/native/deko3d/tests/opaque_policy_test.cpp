#include "../opaque_policy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace native_renderer;
using namespace native_renderer::detail::deko;
namespace {
uint16_t testBits(unsigned mode,unsigned reference,unsigned fail=1){return uint16_t(1u|(mode<<1)|(reference<<4)|(fail<<12));}
bool shaderAlphaPass(const DrawState &state,unsigned vertexAlpha,unsigned textureAlpha){
    if(!(state.alphaTest&1))return true;
    const float shade=(float(vertexAlpha)/128.0f)*state.opacity;
    const float texture=state.texture?float(textureAlpha)/255.0f:1.0f;
    const float input=texture*(state.texture?255.0f/128.0f:1.0f)*shade;
    const float alpha=std::floor(input*128.0f+0.001f);
    const float ref=float((state.alphaTest>>4)&255);
    switch((state.alphaTest>>1)&7){
    case 0:return false;case 1:return true;case 2:return alpha<ref;case 3:return alpha<=ref;
    case 4:return alpha==ref;case 5:return alpha>=ref;case 6:return alpha>ref;default:return alpha!=ref;
    }
}
}
int main(){
    unsigned checks=0;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Opaque fragment regression");};
    DrawState d;
    d.alphaTest=0x140A;check(alphaTestAlwaysPasses(d,0,0)); // ATE disabled, other TEST bits retained
    check(canUseOpaqueFragment(d,128,0,false));
    for(unsigned fail=0;fail<4;++fail){
        d.alphaTest=testBits(5,64,fail);
        check(!needsAlphaFailurePass(d,128,0));
        check(needsAlphaFailurePass(d,64,0)==(fail!=0));
    }
    for(unsigned fail=0;fail<4;++fail){
        d.alphaTest=testBits(1,255,fail);
        check(alphaTestAlwaysPasses(d,0,0));check(!needsAlphaFailurePass(d,0,0));
        check(canUseOpaqueFragment(d,128,0,false));
    }
    d={};d.alphaTest=0x140B;
    check(!alphaTestAlwaysPasses(d,64,0));check(needsAlphaFailurePass(d,64,0));
    check(alphaTestAlwaysPasses(d,65,0));check(!needsAlphaFailurePass(d,65,0));
    check(canUseOpaqueFragment(d,128,0,false));
    for(unsigned mode:{6u,7u}){
        d.alphaTest=testBits(mode,64);check(!alphaTestAlwaysPasses(d,65,0));
        check(alphaTestAlwaysPasses(d,66,0));
    }
    for(unsigned mode:{0u,2u,3u,4u}){d.alphaTest=testBits(mode,64);check(!alphaTestAlwaysPasses(d,128,128));}
    d.alphaTest=0x140B;d.texture=1;
    check(!alphaTestAlwaysPasses(d,128,64));check(alphaTestAlwaysPasses(d,128,65));
    check(!alphaTestAlwaysPasses(d,128,0));check(!alphaTestAlwaysPasses(d,0,128));
    d.fresnel=true;check(!alphaTestAlwaysPasses(d,128,128));d.fresnel=false;
    for(float opacity:{0.0f,-1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        d.opacity=opacity;check(!alphaTestAlwaysPasses(d,128,128));
    }
    d={};
    check(canUseOpaqueFragment(d,128,0,false)); // untextured ignores dummy alpha
    check(!canUseOpaqueFragment(d,0,255,false));
    d.texture=1;check(canUseOpaqueFragment(d,128,128,false));
    check(!canUseOpaqueFragment(d,128,0,false));
    check(!canUseOpaqueFragment(d,0,128,false));
    d.opacity=0.02f;check(!canUseOpaqueFragment(d,128,128,false));
    d.opacity=0.04f;check(canUseOpaqueFragment(d,128,128,false));
    d.opacity=0;check(!canUseOpaqueFragment(d,128,128,false));
    d.opacity=-1;check(!canUseOpaqueFragment(d,128,128,false));
    d.opacity=std::numeric_limits<float>::infinity();check(!canUseOpaqueFragment(d,128,128,false));
    d.opacity=std::numeric_limits<float>::quiet_NaN();check(!canUseOpaqueFragment(d,128,128,false));
    d={};d.texture=1;
    for(auto member:{&DrawState::screen,&DrawState::background,&DrawState::fresnel,&DrawState::shadowVolume,
                     &DrawState::reflectionSource,&DrawState::reflectionSample,&DrawState::refractionSample,
                     &DrawState::screenCopy,&DrawState::previousFrame}){
        d.*member=true;check(!canUseOpaqueFragment(d,128,128,false));d.*member=false;
    }
    d.alphaTest=1;check(!canUseOpaqueFragment(d,128,128,false));
    d.alphaTest=2;check(canUseOpaqueFragment(d,128,128,false));d.alphaTest=0;
    check(!canUseOpaqueFragment(d,128,128,true));
    d.blend=Blend::Opaque;check(canUseOpaqueFragment(d,128,128,false));
    d.blend=Blend::Additive;check(!canUseOpaqueFragment(d,128,128,false));
    d.blend=Blend::Subtract;check(!canUseOpaqueFragment(d,128,128,false));
    std::array<Vertex,3> vertices{};vertices[0].a=255;vertices[1].a=128;vertices[2].a=32;
    check(minimumVertexAlpha(vertices.data(),3)==32);
    vertices[2].a=0;check(minimumVertexAlpha(vertices.data(),3)==0);
    // RGBA stream includes the last mip: a transparent final texel must win.
    const uint8_t rgba[]={0,0,0,255,0,0,0,128,0,0,0,0};
    check(minimumTextureAlpha(rgba,sizeof(rgba))==0);
    check(minimumTextureAlpha(rgba,8)==128);
    d={};d.texture=1;d.alphaTest=0x140B;
    check(!alphaTestAlwaysPasses(d,128,minimumTextureAlpha(rgba,sizeof(rgba))));
    check(needsAlphaFailurePass(d,128,minimumTextureAlpha(rgba,sizeof(rgba))));
    check(alphaTestAlwaysPasses(d,128,minimumTextureAlpha(rgba,8)));
    bool rejected=false;try{minimumTextureAlpha(rgba,3);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    rejected=false;try{minimumVertexAlpha(nullptr,1);}catch(const std::invalid_argument &){rejected=true;}check(rejected);
    // Exhaust byte minima around the shader's discard boundary for several opacities.
    d={};d.texture=1;
    for(float opacity:{0.01f,0.03f,0.1f,1.0f,2.0f}){
        d.opacity=opacity;
        for(unsigned va=0;va<256;++va)for(unsigned ta=0;ta<256;++ta){
            if(!canUseOpaqueFragment(d,va,ta,false))continue;
            const float shade=(float(va)/128.0f)*opacity;
            const float alpha=std::clamp((float(ta)/255.0f)*2.0f*shade,0.0f,1.0f);
            check(alpha>=0.02f);
        }
    }
    // Compare the proof against the shader's exact operation order and floor.
    for(bool textured:{false,true})for(float opacity:{0.03f,0.25f,0.5f,1.0f,2.0f}){
        d={};d.texture=textured;d.opacity=opacity;
        for(unsigned ref:{64u,128u,255u})for(unsigned mode=0;mode<8;++mode){
            d.alphaTest=testBits(mode,ref);
            for(unsigned va=0;va<256;++va)for(unsigned ta=0;ta<256;++ta){
                if(alphaTestAlwaysPasses(d,va,ta)){
                    check(shaderAlphaPass(d,va,ta));check(!needsAlphaFailurePass(d,va,ta));
                    // Linear interpolation may sample anything above the minima.
                    check(shaderAlphaPass(d,255,ta));check(shaderAlphaPass(d,va,255));
                }
                if(canUseOpaqueFragment(d,va,ta,false)){
                    const float alpha=std::clamp((textured?float(ta)/255.0f*2.0f:1.0f)*(float(va)/128.0f*opacity),0.0f,1.0f);
                    check(alpha>=0.02f && shaderAlphaPass(d,va,ta));
                }
            }
        }
    }
    std::printf("opaque fragment: %u checks passed\n",checks);
}
