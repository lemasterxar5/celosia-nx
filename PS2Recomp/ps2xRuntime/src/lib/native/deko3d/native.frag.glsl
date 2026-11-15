#version 460
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug;
} u;
layout(binding=0) uniform sampler2D image;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 shade;
layout(location=2) in float viewDepth;
layout(location=3) noperspective in float fogAmount;
layout(location=4) in float nearCull;
layout(location=0) out vec4 outputColour;
bool alphaPass(float a,int test,float reference) {
    float A=floor(a*128.0+0.001);
    if(test==0)return false; if(test==1)return true;
    if(test==2)return A<reference; if(test==3)return A<=reference;
    if(test==4)return A==reference; if(test==5)return A>=reference;
    if(test==6)return A>reference; return A!=reference;
}
void main() {
    if(nearCull<0.0)discard;
    bool textured=u.fogColour.w!=0.0;
    float mip=u.lod.z>0.0?clamp(log2(max(viewDepth,0.001))*u.lod.y+u.lod.x,0.0,u.lod.z):0.0;
    vec4 t=textured?textureLod(image,uv,mip):vec4(vec3(128.0/255.0),1.0);
    float a=clamp(t.a*(textured?2.0:1.0)*shade.a,0.0,1.0);
    if(u.alpha.x>=0.0 && alphaPass(t.a*(textured?255.0/128.0:1.0)*shade.a,int(u.alpha.x),u.alpha.y)==(u.alpha.z!=0.0))discard;
    if(a<0.02)discard;
    vec3 rgb=clamp(t.rgb*shade.rgb,0.0,1.0);
    rgb=mix(u.fogColour.rgb,rgb,fogAmount);
    outputColour=u.debug.w>0.0?vec4(u.debug.rgb,1.0):vec4(rgb,a);
}
