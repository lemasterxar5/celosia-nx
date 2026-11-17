#version 460
// Lean variants of the scene shaders (v104-v116). The build compiles 256 variants of this file, each with
// a subset of LEAN_ALPHA (alpha test), LEAN_BGFOG (background-image fade), LEAN_MIP (mip chain),
// LEAN_ZEROADD (zero-add discard), LEAN_EARLY (early tests: only for draws without depth writes),
// LEAN_NEARCULL (the background layer's near cull), LEAN_UNTEXTURED (no texture) and LEAN_OPAQUE
// (opaque.frag.glsl: no discards at all). The CPU picks the variant whose paths match the draw's
// uniforms; every path is the same arithmetic as native.frag.glsl / opaque.frag.glsl, so the output
// is identical; the paths that are off are gone (measured v115: the full shaders cost ~4x the lean
// one per full-screen pass).
#ifdef LEAN_EARLY
layout(early_fragment_tests) in;
#endif
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
layout(binding=0) uniform sampler2D image;
layout(binding=1) uniform sampler2D background;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 shade;
layout(location=2) in float viewDepth;
layout(location=3) noperspective in float fogAmount;
layout(location=4) in float nearCull;
layout(location=0) out vec4 outputColour;
#if defined(LEAN_ALPHA) && !defined(LEAN_OPAQUE)
bool alphaPass(float a,int test,float reference) {
    float A=floor(a*128.0+0.001);
    if(test==0)return false; if(test==1)return true;
    if(test==2)return A<reference; if(test==3)return A<=reference;
    if(test==4)return A==reference; if(test==5)return A>=reference;
    if(test==6)return A>reference; return A!=reference;
}
#endif
void main() {
#if defined(LEAN_NEARCULL) && !defined(LEAN_OPAQUE)
    if(nearCull<0.0)discard;
#endif
#ifdef LEAN_UNTEXTURED
    vec4 t=vec4(vec3(128.0/255.0),1.0);
    float a=clamp(t.a*1.0*shade.a,0.0,1.0);
#else
#ifdef LEAN_MIP
    float mip=clamp(log2(max(viewDepth,0.001))*u.lod.y+u.lod.x,0.0,u.lod.z);
#else
    float mip=0.0;
#endif
    vec4 t=textureLod(image,uv,mip);
    float a=clamp(t.a*2.0*shade.a,0.0,1.0);
#endif
#ifndef LEAN_OPAQUE
#ifdef LEAN_ALPHA
#ifdef LEAN_UNTEXTURED
    if(alphaPass(t.a*1.0*shade.a,int(u.alpha.x),u.alpha.y)==(u.alpha.z!=0.0))discard;
#else
    if(alphaPass(t.a*(255.0/128.0)*shade.a,int(u.alpha.x),u.alpha.y)==(u.alpha.z!=0.0))discard;
#endif
#endif
    if(a<0.02)discard;
#endif
    vec3 rgb=clamp(t.rgb*shade.rgb,0.0,1.0);
    rgb=mix(u.fogColour.rgb,rgb,fogAmount);
#ifdef LEAN_BGFOG
    {
        float f=clamp((viewDepth-u.bgFog.x)/u.bgFog.y,0.0,1.0);
        if(u.bgFog.z>0.0)f=floor(f*u.bgFog.z)/u.bgFog.z;
        rgb=mix(rgb,textureLod(background,gl_FragCoord.xy*vec2(1.0/1280.0,1.0/720.0),0.0).rgb,f);
    }
#endif
#if defined(LEAN_ZEROADD) && !defined(LEAN_OPAQUE)
    if(max(rgb.r,max(rgb.g,rgb.b))*a<0.5/255.0)discard;
#endif
    outputColour=vec4(rgb,a);
}
