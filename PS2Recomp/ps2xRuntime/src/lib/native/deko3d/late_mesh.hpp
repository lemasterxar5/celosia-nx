#pragma once
#include <stdexcept>
namespace native_renderer::detail::deko {
// Skin-at-kick can replace a mesh after its draw was recorded. Resolve only
// this frame's draws; submitted frames keep their existing shared ownership.
template<class DrawRange,class MeshMap>
void resolvePendingMeshes(DrawRange &pending,const MeshMap &meshes) {
    for(auto &draw:pending){
        const auto found=meshes.find(draw.meshId);
        if(found==meshes.end() || !found->second)
            throw std::logic_error("deko3d: pending draw lost its mesh");
        draw.mesh=found->second;
    }
}
}
