#version 460
layout(early_fragment_tests) in;
// v177: SotC's screen effects as the GL renderer draws them (native_renderer_gl.cpp), on deko3d's target
// origin (rows from the top). No discards: the depth-tested passes are rejected before shading.
// alpha.x selects the pass; params.zw = 1 / the target's size (uv = gl_FragCoord x params.zw):
//  21 depth of field (GL mode 11 split in slices): the blurred 256 x 256 copy, alpha fogColour.a, laid
//     over where the quad's depth test passes (the scene at or beyond the slice's GS Z)
//  22 screen blur (GL mode 3 on the turned quad): the picture as the quad turned by fogColour.y, scaled by
//     fogColour.x and moved by fogColour.zw (NDC, y down) would show it; alpha params.x inside, 0 outside
//  23 copy with alpha, the source read shifted by params.xy (GL mode 13: the glow's reductions)
//  24 glow (GL mode 14): rgb x fogColour.rgb, alpha x fogColour.a, clamped
//  25 v183 screen blur in one pass: mode 22's `steps` (params.x) passes, step i turned by fogColour.y x f,
//     scaled by (fogColour.x x f + 1), moved by fogColour.zw x f, f = (steps - i) / steps, each laid Over
//     with alpha 1 / (i + 1) inside; their product as one Over: alpha 1 - keep, rgb the weighted sum / alpha
layout(std140,binding=0) uniform Material {
    mat4 clip, directions, colours;
    vec4 params, fog, fogColour, lod, alpha, debug, bgFog;
} u;
layout(binding=0) uniform sampler2D image;
layout(location=0) out vec4 outputColour;
void main() {
    int mode=int(u.alpha.x);
    vec2 uv=gl_FragCoord.xy*u.params.zw;
    if(mode==21){outputColour=vec4(texture(image,uv).rgb,u.fogColour.a);return;}
    if(mode==22){
        vec2 p=uv*2.0-1.0-u.fogColour.zw;
        float s=sin(u.fogColour.y),c=cos(u.fogColour.y);
        vec2 q=vec2(c*p.x+s*p.y,-s*p.x+c*p.y)/u.fogColour.x;
        vec2 t=q*0.5+0.5;
        float inside=(t.x>=0.0 && t.x<=1.0 && t.y>=0.0 && t.y<=1.0)?1.0:0.0;
        outputColour=vec4(texture(image,t).rgb,u.params.x*inside);
        return;
    }
    if(mode==25){
        int steps=int(u.params.x);
        float keep=1.0;vec3 acc=vec3(0.0);
        for(int i=steps-1;i>=0;--i){
            float f=float(steps-i)/float(steps);
            vec2 p=uv*2.0-1.0-u.fogColour.zw*f;
            float s=sin(u.fogColour.y*f),c=cos(u.fogColour.y*f);
            vec2 q=vec2(c*p.x+s*p.y,-s*p.x+c*p.y)/(u.fogColour.x*f+1.0);
            vec2 t=q*0.5+0.5;
            float a=(t.x>=0.0 && t.x<=1.0 && t.y>=0.0 && t.y<=1.0)?1.0/float(i+1):0.0;
            acc+=keep*a*texture(image,t).rgb;
            keep*=1.0-a;
        }
        float alpha=1.0-keep;
        outputColour=vec4(alpha>0.0?acc/alpha:vec3(0.0),alpha);
        return;
    }
    if(mode==23){outputColour=texture(image,uv+u.params.xy);return;}
    vec4 c=texture(image,uv);
    outputColour=clamp(vec4(c.rgb*u.fogColour.rgb,c.a*u.fogColour.a),0.0,1.0);
}
