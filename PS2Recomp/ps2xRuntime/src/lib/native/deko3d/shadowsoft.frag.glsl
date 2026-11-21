#version 460
// v162: bloom.frag.glsl's soft-shadow modes (7, 19, 9) alone, the same code: a smaller shader (no bloom
// chain loops), so more of the passes' pixels in flight. render.cpp bloomQuad picks it for those modes.
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
layout(binding=0) uniform sampler2D image;
layout(binding=1) uniform sampler2D second;
layout(location=0) out vec4 outputColour;
void main() {
    int mode=int(u.alpha.x);
    vec2 frag=gl_FragCoord.xy;
    if(mode==7){
        ivec2 size=ivec2(u.params.xy);
        ivec2 base=ivec2(floor(frag))*4;
        float sum=0.0;
        if(base.x+3<size.x && base.y+3<size.y){
            // v131: four gathers of 2 x 2 (the block lies inside the mask: no clamping). The mask holds 0 or
            // 1, so the sum of the 16 texels is the same integer in any order.
            vec2 inv=1.0/vec2(size);
            for(int y=0;y<4;y+=2)for(int x=0;x<4;x+=2){
                vec4 g=textureGather(image,(vec2(base+ivec2(x,y))+1.0)*inv,0);
                sum+=g.x+g.y+g.z+g.w;
            }
        }else{
            for(int y=0;y<4;++y)for(int x=0;x<4;++x)sum+=texelFetch(image,min(base+ivec2(x,y),size-1),0).r;
        }
        outputColour=vec4(vec3(sum/16.0),1.0);
        return;
    }
    if(mode==19){
        vec2 uv=frag*u.params.zw,s=u.params.xy;
        vec4 c=texture(image,uv)*0.30;
        c+=(texture(image,uv+s)+texture(image,uv-s))*0.235;
        c+=(texture(image,uv+s*2.0)+texture(image,uv-s*2.0))*0.12;
        c+=(texture(image,uv+s*3.0)+texture(image,uv-s*3.0))*0.05;
        outputColour=c;
        return;
    }
    {outputColour=vec4(0.0,0.0,0.0,clamp(texture(image,frag*u.params.zw).r*u.params.x,0.0,1.0));return;}
}
