#version 460
// v163: bloom.frag.glsl's composite (mode 18) alone, the same code: the full-screen pass without the
// other modes' loops. render.cpp bloomQuad picks it for mode 18.
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
layout(binding=0) uniform sampler2D image;
layout(binding=1) uniform sampler2D second;
layout(location=0) out vec4 outputColour;
void main() {
    vec2 frag=gl_FragCoord.xy;
    // 18: the composite
    vec2 x=frag/u.params.xy-0.5;
    vec2 uv=(0.5+x*vec2(256.0/512.0,256.0/448.0))/256.0;
    // v158: blended One + One (alpha Zero, One): a zero colour leaves the pixel as it is, so it is not
    // written at all (no read-modify-write of the 4x samples where the bloom is black).
    const vec3 add=texture(image,uv).rgb*u.fogColour.rgb*u.fogColour.a;
    if(add.r<=0.0 && add.g<=0.0 && add.b<=0.0)discard;
    outputColour=vec4(add,0.0);
}
