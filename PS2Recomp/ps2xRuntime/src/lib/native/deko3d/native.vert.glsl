#version 460
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texcoord;
layout(location=3) in vec4 colour;
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug;
} u;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 shade;
layout(location=2) out float viewDepth;
layout(location=3) noperspective out float fogAmount;
layout(location=4) out float nearCull;
void main() {
    vec4 c=u.clip*vec4(position,1.0);
    // alpha.w>0 pins planned backdrops to the far plane so they test against the depth pre-pass.
    gl_Position=vec4(c.xy,u.alpha.w>0.0?c.w:c.w-0.1,c.w);
    uv=texcoord+u.params.yz;
    shade=colour/128.0;
    shade.a*=u.params.x;
    if(u.lod.w<0.0) {
        vec3 p=(u.directions*vec4(position,1.0)).xyz;
        vec3 n=(u.directions*vec4(0.0,1.0,0.0,0.0)).xyz;
        shade.a*=0.2+0.8*(1.0-abs(dot(normalize(p),n)));
    } else if(u.lod.w>0.0) {
        vec4 d=max(u.directions*vec4(normal,1.0),vec4(0.0));
        d.w=1.0;
        shade.rgb=min(shade.rgb*(u.colours*d).rgb,vec3(255.0/128.0));
    }
    viewDepth=c.w;
    fogAmount=u.fog.w!=0.0?clamp(u.fog.x+u.fog.y/max(c.w,0.001),0.0,255.0)/255.0:1.0;
    nearCull=u.fog.z>0.0?c.w-u.fog.z:1.0;
}
