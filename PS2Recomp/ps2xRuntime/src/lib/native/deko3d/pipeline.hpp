#pragma once
#include "runtime/native_renderer.h"
#include <cstdint>
namespace native_renderer::detail::deko {
struct PipelineKey {uint8_t depth,mask;};
inline PipelineKey pipelineKey(const DrawState &state,bool failurePass){
    const unsigned action=(state.alphaTest>>12)&3;
    // Direct background draws use painter order, matching GL/Metal without
    // the separate zFog background pass. Explicit backdrop tests never write Z.
    const bool backdrop=state.screen || state.background;
    const bool test=!backdrop || state.screenDepthTest;
    bool write=!backdrop && !state.screenDepthTest && !state.overlay && !state.noDepthWrite &&
        (state.blend==Blend::Normal || state.blend==Blend::Opaque || state.writeDepth);
    if(failurePass && action!=2)write=false;
    const uint8_t mask=failurePass && action==2?0:failurePass && action==3?7:15;
    return {uint8_t((test?1:0)|(write?2:0)),mask};
}
// An FB_ONLY alpha test on a draw whose passing pixels write no depth: failing pixels are then
// written exactly like passing ones (same colour mask, same blend, no depth), so the test selects
// nothing and the draw is one pass, as on the GS. Splitting it into a passing and a failing pass
// rasterised the draw twice (additive light shafts: ~23 ms of a 59 ms frame). The background
// image pass writes depth for layer 1, so background draws keep both passes.
inline bool alphaTestWithoutEffect(const DrawState &state){
    return (state.alphaTest&1) && ((state.alphaTest>>12)&3)==1 && !state.background && !state.shadowVolume &&
        !(pipelineKey(state,false).depth&2);
}
}
