#version 460
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
layout(binding=0) uniform sampler2D image;
layout(location=0) out vec4 outputColour;
// Background image copies (render.cpp): the reduction to 256 x 256 (bgEnd) and the sky laid
// behind the scene. params.xy = 1 / the target's size; linear filtering does the resampling.
void main() {
    outputColour=vec4(texture(image,gl_FragCoord.xy*u.params.xy).rgb,1.0);
}
