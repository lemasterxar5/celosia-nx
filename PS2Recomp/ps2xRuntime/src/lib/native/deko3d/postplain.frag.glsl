#version 460
// v164: post.frag.glsl with no option on (no FXAA, sharpening, bicubic or debanding: the frame generation's
// input, MSAA alone): the same one tap and alpha 1, without the other paths. render.cpp postQuad picks it then.
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
layout(binding=0) uniform sampler2D image;
layout(location=0) out vec4 outputColour;
void main(){outputColour=vec4(textureLod(image,gl_FragCoord.xy*u.params.xy,0.0).rgb,1.0);}
