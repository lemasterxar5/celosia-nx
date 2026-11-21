#version 460
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
layout(binding=0) uniform sampler2D image;
layout(location=0) out vec4 outputColour;
// v152: the scene's last pass onto the swapchain image (render.cpp postQuad), all optional:
// params.xy = 1 / the output's size (uv), bgFog.xy = 1 / the scene's size (texel steps).
// debug.x = FXAA (0 / 1), debug.y = sharpening (0..1, contrast-adaptive, limited to the neighbours'
// range: no halos), debug.z = bicubic (Catmull-Rom) upscaling, debug.w = debanding; bgFog.zw = the scene's size.
const vec3 Luma=vec3(0.299,0.587,0.114);
vec3 tap(vec2 uv){return textureLod(image,uv,0.0).rgb;}
vec3 fxaa(vec2 uv,vec2 px){
    const vec3 nw=tap(uv+vec2(-1.0,-1.0)*px),ne=tap(uv+vec2(1.0,-1.0)*px);
    const vec3 sw=tap(uv+vec2(-1.0,1.0)*px),se=tap(uv+vec2(1.0,1.0)*px),m=tap(uv);
    const float lnw=dot(nw,Luma),lne=dot(ne,Luma),lsw=dot(sw,Luma),lse=dot(se,Luma),lm=dot(m,Luma);
    const float lmin=min(lm,min(min(lnw,lne),min(lsw,lse))),lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));
    if(lmax-lmin<max(0.0312,lmax*0.125))return m; // no edge: the pixel as drawn
    vec2 dir=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));
    const float reduce=max((lnw+lne+lsw+lse)*(0.25/8.0),1.0/128.0);
    dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2(-8.0),vec2(8.0))*px;
    const vec3 a=0.5*(tap(uv+dir*(1.0/3.0-0.5))+tap(uv+dir*(2.0/3.0-0.5)));
    const vec3 b=a*0.5+0.25*(tap(uv-dir*0.5)+tap(uv+dir*0.5));
    const float lb=dot(b,Luma);
    return (lb<lmin || lb>lmax)?a:b;
}
// Catmull-Rom from five bilinear taps (the weights of the middle two texels merged per axis).
vec3 bicubic(vec2 uv,vec2 size){
    const vec2 p=uv*size,tc=floor(p-0.5)+0.5,f=p-tc;
    const vec2 w0=f*(-0.5+f*(1.0-0.5*f)),w1=1.0+f*f*(-2.5+1.5*f),w2=f*(0.5+f*(2.0-1.5*f)),w3=f*f*(-0.5+0.5*f);
    const vec2 w12=w1+w2,t0=(tc-1.0)/size,t3=(tc+2.0)/size,t12=(tc+w2/w12)/size;
    const float a=w12.x*w0.y,b=w0.x*w12.y,c=w12.x*w12.y,d=w3.x*w12.y,e=w12.x*w3.y;
    const vec3 sum=tap(vec2(t12.x,t0.y))*a+tap(vec2(t0.x,t12.y))*b+tap(t12)*c+tap(vec2(t3.x,t12.y))*d+tap(vec2(t12.x,t3.y))*e;
    return clamp(sum/(a+b+c+d+e),0.0,1.0);
}
float hash(vec2 p){return fract(sin(dot(p,vec2(12.9898,78.233)))*43758.5453);}
void main() {
    const vec2 uv=gl_FragCoord.xy*u.params.xy,px=u.bgFog.xy;
    vec3 c=u.debug.x>0.5?fxaa(uv,px):(u.debug.z>0.5?bicubic(uv,u.bgFog.zw):tap(uv));
    if(u.debug.y>0.0){
        const vec3 n=tap(uv+vec2(0.0,-1.0)*px),s=tap(uv+vec2(0.0,1.0)*px),e=tap(uv+vec2(1.0,0.0)*px),w=tap(uv+vec2(-1.0,0.0)*px);
        const vec3 lo=min(c,min(min(n,s),min(e,w))),hi=max(c,max(max(n,s),max(e,w)));
        // Weaker where the neighbourhood already has contrast (as AMD's CAS), clamped to it.
        const vec3 amp=sqrt(clamp(min(lo,1.0-hi)/max(hi,1e-4),0.0,1.0));
        const vec3 k=-amp*mix(0.125,0.2,u.debug.y);
        c=clamp((c+k*(n+s+e+w))/(1.0+4.0*k),lo,hi);
    }
    // Triangular noise of one 8-bit step: gradients (the sky, the fog) without visible bands.
    if(u.debug.w>0.5)c+=vec3((hash(gl_FragCoord.xy)+hash(gl_FragCoord.yx+17.0)-1.0)/255.0);
    outputColour=vec4(c,1.0);
}
