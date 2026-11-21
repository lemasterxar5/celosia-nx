#version 460
// v163: bloom.frag.glsl's chain modes (15 mask, 17 temporal, 16 blur) alone, the same code: a smaller shader
// for those passes. render.cpp bloomQuad picks it for them.
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
    if(mode==17){
        vec2 uv=frag*u.params.zw;
        vec3 cd=floor(texture(image,uv).rgb*255.0+0.001),cs=floor(texture(second,uv).rgb*255.0+0.001);
        outputColour=vec4((cd+floor((cs-cd)*u.params.x))/255.0,1.0);
        return;
    }
    {
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
}
