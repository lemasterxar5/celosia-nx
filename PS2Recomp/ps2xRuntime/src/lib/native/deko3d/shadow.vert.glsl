#version 460
layout(location=0) in vec3 position;
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
void main() {
    vec4 c=u.clip*vec4(position,1.0);
    // Match native geometry's zero-to-one depth; the volume pass controls
    // depth clamping and stencil operations through its rasterizer state.
    gl_Position=vec4(c.xy,c.w-0.1,c.w);
}
