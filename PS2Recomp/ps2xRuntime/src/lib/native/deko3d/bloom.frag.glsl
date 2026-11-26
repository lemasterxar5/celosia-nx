#version 460
// SotC's bloom, the GS chain as the GL/Metal renderers do it (native_renderer_gl.cpp modes 15-18),
// with the deko3d target origin (rows from the top, no flips). alpha.x selects the pass:
//  1  the mask: 1 where the quad's depth test passed (the scene at or beyond the threshold)
//  15 the chain's mask at 256 x 256: bilinear over the PS2 frame's texels (p + 0.5) x (2, 1.75),
//     each 1 where the full-size mask is set; fogColour.rgb the mask colour, params.xy its size
//  0  copy (resampled by linear filtering); params.zw = 1 / the target's size
//  17 the temporal step: Cd + ((Cs - Cd) x params.x) truncated, Cs the history (binding 1)
//  16 the blur: the 64 x 64 source onto params.x pixels, 12 shifted 8-bit lerps, params.y the taps' alpha
//  18 the composite: UV 0.5..256.5 over the PS2 frame; params.xy render pixels per PS2 pixel,
//     fogColour the colour (rgb x a), added to the picture
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
    if(mode==1){outputColour=vec4(1.0);return;}
    if(mode==15){
        vec2 size=u.params.xy;
        vec2 p=floor(frag);
        vec2 t=(p+0.5)*vec2(2.0,1.75)-0.5;
        vec2 i0=floor(t),f=t-i0;
        float m[4];
        for(int k=0;k<4;++k){
            vec2 at=clamp(i0+vec2(k&1,k>>1),vec2(0.0),vec2(511.0,447.0));
            vec2 px=min((at+0.5)*size/vec2(512.0,448.0),size-1.0);
            m[k]=texelFetch(image,ivec2(px),0).r>=0.5?1.0:0.0;
        }
        outputColour=vec4(u.fogColour.rgb*mix(mix(m[0],m[1],f.x),mix(m[2],m[3],f.x),f.y),1.0);
        return;
    }
    if(mode==0){outputColour=vec4(texture(image,frag*u.params.zw).rgb,1.0);return;}
    if(mode==20){outputColour=vec4(texture(image,frag*u.params.zw).rgb,128.0/255.0);return;} // v176: screen copy (alpha 0x80)
    if(mode==17){
        vec2 uv=frag*u.params.zw;
        vec3 cd=floor(texture(image,uv).rgb*255.0+0.001),cs=floor(texture(second,uv).rgb*255.0+0.001);
        outputColour=vec4((cd+floor((cs-cd)*u.params.x))/255.0,1.0);
        return;
    }
    if(mode==16){
        float r=u.params.x,k=64.0/r;
        vec2 p=floor(frag);
        vec2 t0=(p+0.5)*k;
        vec3 acc=floor(texture(image,t0/64.0).rgb*255.0+0.001);
        const vec2 d[12]=vec2[12](vec2(-1,0),vec2(1,0),vec2(0,-1),vec2(0,1),vec2(-2,0),vec2(2,0),
                                  vec2(0,-2),vec2(0,2),vec2(-3,0),vec2(3,0),vec2(0,-3),vec2(0,3));
        vec3 w=floor(vec3(64.0,32.0,16.0)*u.params.y/128.0)/128.0;
        for(int i=0;i<12;++i){
            float a=i<4?w.x:(i<8?w.y:w.z);
            vec2 q=p-d[i];
            if(any(lessThan(q,vec2(0.0))) || any(greaterThanEqual(q,vec2(r))))continue;
            vec2 t=0.5+q*k;
            vec3 c=floor(texture(image,t/64.0).rgb*255.0+0.001);
            acc+=floor((c-acc)*a);
        }
        outputColour=vec4(acc/255.0,1.0);
        return;
    }
    // The soft shadows (GL quad modes 7, 1, 9): the stencil count's mask reduced 4 x 4 (7, params.xy
    // the mask's size), blurred along params.xy (19, a step in UV; params.zw = 1 / the target's size)
    // and the picture darkened by it (9: alpha = mask x params.x; params.zw = 1 / the picture's size).
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
    if(mode==9){outputColour=vec4(0.0,0.0,0.0,clamp(texture(image,frag*u.params.zw).r*u.params.x,0.0,1.0));return;}
    // 18: the composite
    vec2 x=frag/u.params.xy-0.5;
    vec2 uv=(0.5+x*vec2(256.0/512.0,256.0/448.0))/256.0;
    // v158: blended One + One (alpha Zero, One): a zero colour leaves the pixel as it is, so it is not
    // written at all (no read-modify-write of the 4x samples where the bloom is black).
    const vec3 add=texture(image,uv).rgb*u.fogColour.rgb*u.fogColour.a;
    if(add.r<=0.0 && add.g<=0.0 && add.b<=0.0)discard;
    outputColour=vec4(add,0.0);
}
