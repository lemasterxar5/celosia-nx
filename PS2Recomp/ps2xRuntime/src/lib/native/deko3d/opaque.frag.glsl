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
layout(location=0) out vec4 outputColour;
// Selected only when CPU metadata proves all three native fragment discard
// conditions false. Colour, alpha, LOD and fog arithmetic stay identical.
void main() {
    bool textured=u.fogColour.w!=0.0;
    float mip=u.lod.z>0.0?clamp(log2(max(viewDepth,0.001))*u.lod.y+u.lod.x,0.0,u.lod.z):0.0;
    vec4 t=textured?textureLod(image,uv,mip):vec4(vec3(128.0/255.0),1.0);
    float a=clamp(t.a*(textured?2.0:1.0)*shade.a,0.0,1.0);
    vec3 rgb=clamp(t.rgb*shade.rgb,0.0,1.0);
    rgb=mix(u.fogColour.rgb,rgb,fogAmount);
    // Fades to the background image (the sky as the PS2 shows it) by view depth: fogged draws and,
    // before the fog's layer, the zFog's slices (bgFog = start, width, steps, on).
    if(u.bgFog.w>0.0){
        float f=clamp((viewDepth-u.bgFog.x)/u.bgFog.y,0.0,1.0);
        if(u.bgFog.z>0.0)f=floor(f*u.bgFog.z)/u.bgFog.z;
        rgb=mix(rgb,textureLod(background,gl_FragCoord.xy*vec2(1.0/1280.0,1.0/720.0),0.0).rgb,f);
    }
    outputColour=u.debug.w>0.0?vec4(u.debug.rgb,1.0):vec4(rgb,a);
}
