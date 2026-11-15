#version 460
// Shadow volumes update stencil only. Material alpha, textures and fog must
// never discard their fragments or contribute a colour attachment output.
void main() {}
