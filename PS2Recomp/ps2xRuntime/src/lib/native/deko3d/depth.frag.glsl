#version 460
// Depth-only pre-pass: colour writes are masked off; early tests let the rasterizer write Z.
layout(early_fragment_tests) in;
void main() {}
