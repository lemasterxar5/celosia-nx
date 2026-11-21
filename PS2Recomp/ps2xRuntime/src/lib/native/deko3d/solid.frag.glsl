#version 460
// v160: the hard shadow resolve's fragment (render.cpp resolveShadows). Its blend is Zero x source,
// InvConstAlpha x destination (alpha Zero, One), so the source colour never reaches the picture: any
// output that is not discarded gives the same pixels. No discard and early tests: the stencil test runs
// before the shader (the generic shader it replaces could discard, so its tests ran late, for every pixel).
layout(early_fragment_tests) in;
layout(location=0) out vec4 outputColour;
void main(){outputColour=vec4(0.0,0.0,0.0,1.0);}
