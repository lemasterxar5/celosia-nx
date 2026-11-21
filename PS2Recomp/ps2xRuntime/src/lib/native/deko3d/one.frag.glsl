#version 460
// v162: the bloom / soft-shadow mask pass (render.cpp bloomQuad mode 1): 1 where the depth (and stencil) tests
// pass, as bloom.frag.glsl mode 1 writes, without its other modes' discard, so the tests run before the shader.
layout(early_fragment_tests) in;
layout(location=0) out vec4 outputColour;
void main(){outputColour=vec4(1.0);}
