#version 460
// v160: post.frag.glsl's FXAA + sharpening + debanding as a compute pass, for a scene the size of the
// output (no upscaling): every tap of the fragment version lands on a texel centre except FXAA's four
// directional ones, so the 3 x 3 neighbourhood is read once per 16 x 16 tile (18 x 18 texels, clamped at
// the edges as the sampler's ClampToEdge) into shared memory instead of 9 texture reads per pixel; the
// directional taps still go through the sampler. The same arithmetic in the same order as post.frag.glsl.
layout(local_size_x=16,local_size_y=16) in;
layout(std140,binding=0) uniform Post {
    vec4 opt; // x FXAA, y sharpening (0..1), w debanding
    vec4 px;  // xy = 1 / the size
} u;
layout(binding=0) uniform sampler2D image;
layout(binding=0,rgba8) uniform writeonly image2D outputImage;
shared vec3 tile[18*18];
const vec3 Luma=vec3(0.299,0.587,0.114);
vec3 tap(vec2 uv){return textureLod(image,uv,0.0).rgb;}
float hash(vec2 p){return fract(sin(dot(p,vec2(12.9898,78.233)))*43758.5453);}
void main() {
    const ivec2 size=textureSize(image,0);
    const ivec2 base=ivec2(gl_WorkGroupID.xy)*16-1;
    for(uint i=gl_LocalInvocationIndex;i<324u;i+=256u)
        tile[i]=texelFetch(image,clamp(base+ivec2(int(i%18u),int(i/18u)),ivec2(0),size-1),0).rgb;
    barrier();
    const ivec2 g=ivec2(gl_GlobalInvocationID.xy);
    if(g.x>=size.x || g.y>=size.y)return;
    const int l=(int(gl_LocalInvocationID.y)+1)*18+int(gl_LocalInvocationID.x)+1;
    const vec2 px=u.px.xy,frag=vec2(g)+0.5,uv=frag*px;
    const vec3 m=tile[l];
    vec3 c=m;
    if(u.opt.x>0.5){
        const vec3 nw=tile[l-19],ne=tile[l-17],sw=tile[l+17],se=tile[l+19];
        const float lnw=dot(nw,Luma),lne=dot(ne,Luma),lsw=dot(sw,Luma),lse=dot(se,Luma),lm=dot(m,Luma);
        const float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))),lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));
        if(!(lmax-lmin<max(0.0312,lmax*0.125))){
            vec2 dir=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));
            const float reduce=max((lnw+lne+lsw+lse)*(0.25/8.0),1.0/128.0);
            dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2(-8.0),vec2(8.0))*px;
            const vec3 a=0.5*(tap(uv+dir*(1.0/3.0-0.5))+tap(uv+dir*(2.0/3.0-0.5)));
            const vec3 b=a*0.5+0.25*(tap(uv-dir*0.5)+tap(uv+dir*0.5));
            const float lb=dot(b,Luma);
            c=(lb<lmin || lb>lmax)?a:b;
        }
    }
    if(u.opt.y>0.0){
        const vec3 n=tile[l-18],s=tile[l+18],e=tile[l+1],w=tile[l-1];
        const vec3 lo=min(c,min(min(n,s),min(e,w))),hi=max(c,max(max(n,s),max(e,w)));
        const vec3 amp=sqrt(clamp(min(lo,1.0-hi)/max(hi,1e-4),0.0,1.0));
        const vec3 k=-amp*mix(0.125,0.2,u.opt.y);
        c=clamp((c+k*(n+s+e+w))/(1.0+4.0*k),lo,hi);
    }
    if(u.opt.w>0.5)c+=vec3((hash(frag)+hash(frag.yx+17.0)-1.0)/255.0);
    imageStore(outputImage,g,vec4(c,1.0));
}
