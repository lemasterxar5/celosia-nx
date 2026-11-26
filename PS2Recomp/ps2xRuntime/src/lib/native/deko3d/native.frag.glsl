#version 460
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
bool alphaPass(float a,int test,float reference) {
    float A=floor(a*128.0+0.001);
    if(test==0)return false; if(test==1)return true;
    if(test==2)return A<reference; if(test==3)return A<=reference;
    if(test==4)return A==reference; if(test==5)return A>=reference;
    if(test==6)return A>reference; return A!=reference;
}
void main() {
    if(nearCull<0.0)discard;
    // v178: GL textured 2 (the mirror, rippled in 32 bands; rgb x tint x vertex colour, alpha vertex x tint)
    // and 3 (refraction: the picture's copy x tint, alpha the tint's) at the screen position (debug.xy =
    // 1 / the target's size, debug.z the ripple's t, alpha.y its H; bgFog the tint). Rows run down here.
    if(u.fogColour.w>1.5){
        vec2 s=gl_FragCoord.xy*u.debug.xy;
        if(u.fogColour.w>2.5){outputColour=vec4(clamp(texture(image,s).rgb*u.bgFog.rgb,0.0,1.0),clamp(u.bgFog.a,0.0,1.0));return;}
        if(u.alpha.y>0.0){
            float band=floor(clamp(s.y,0.0,0.999)*32.0);
            s.x+=(sin(2.0*(12.0*band+u.debug.z)*75.39822/u.alpha.y)+1.0)*(6.4*band/u.alpha.y+0.2)/256.0;
        }
        outputColour=vec4(clamp(texture(image,s).rgb*u.bgFog.rgb*shade.rgb,0.0,1.0),clamp(shade.a*u.bgFog.a,0.0,1.0));
        return;
    }
    bool textured=u.fogColour.w!=0.0;
    float mip=u.lod.z>0.0?clamp(log2(max(viewDepth,0.001))*u.lod.y+u.lod.x,0.0,u.lod.z):0.0;
    vec4 t=textured?(u.lod.z<0.0?texture(image,uv,u.lod.x):textureLod(image,uv,mip)):vec4(vec3(128.0/255.0),1.0);
    float a=clamp(t.a*(textured?2.0:1.0)*shade.a,0.0,1.0);
    if(u.alpha.x>=0.0 && alphaPass(t.a*(textured?255.0/128.0:1.0)*shade.a,int(u.alpha.x),u.alpha.y)==(u.alpha.z!=0.0))discard;
    if(a<0.02)discard;
    vec3 rgb=clamp(t.rgb*shade.rgb,0.0,1.0);
    rgb=mix(u.fogColour.rgb,rgb,fogAmount);
    // Fades to the background image (the sky as the PS2 shows it) by view depth: fogged draws and,
    // before the fog's layer, the zFog's slices (bgFog = start, width, steps, on).
    if(u.bgFog.w>0.0){
        float f=clamp((viewDepth-u.bgFog.x)/u.bgFog.y,0.0,1.0);
        if(u.bgFog.z>0.0)f=floor(f*u.bgFog.z)/u.bgFog.z;
        rgb=mix(rgb,textureLod(background,gl_FragCoord.xy*u.debug.xy,0.0).rgb,f);
    }
    // Additive / subtractive draws without depth writes (params.w = 2): a fragment adding less than
    // half an 8-bit step leaves the picture's colour as it is; nothing reads the picture's alpha.
    rgb*=1.0-u.debug.z; // v179: the weak points' glow scaled (render.cpp; 0 elsewhere)
    if(u.params.w>1.5 && max(rgb.r,max(rgb.g,rgb.b))*a<0.5/255.0)discard;
    outputColour=u.debug.w>0.0?vec4(u.debug.rgb,1.0):vec4(rgb,a);
}
