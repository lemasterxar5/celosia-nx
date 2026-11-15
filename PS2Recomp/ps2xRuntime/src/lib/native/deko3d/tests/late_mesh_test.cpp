#include "../late_mesh.hpp"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using native_renderer::detail::deko::resolvePendingMeshes;
namespace {
struct Mesh {float headY,torsoY;};
struct Draw {uint64_t meshId;std::shared_ptr<Mesh> mesh;};
}
int main(){
    unsigned checks=0;
    auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("Late pose regression");};
    auto poseA=std::make_shared<Mesh>(Mesh{1,0});
    auto poseB=std::make_shared<Mesh>(Mesh{3,2});
    std::unordered_map<uint64_t,std::shared_ptr<Mesh>> meshes{{42,poseA}};
    std::vector<Draw> submitted{{42,poseA}};
    resolvePendingMeshes(submitted,meshes);
    // The initial draw precedes a skin-at-kick rewrite of the same mesh ID.
    std::vector<Draw> pending{{42,meshes.at(42)},{42,meshes.at(42)}};
    meshes[42]=poseB;
    resolvePendingMeshes(pending,meshes);
    check(pending[0].mesh==poseB && pending[1].mesh==poseB);
    check(pending[0].mesh->headY-pending[0].mesh->torsoY==1);
    check(submitted[0].mesh==poseA && submitted[0].mesh->headY==1);
    // Once submitted, resource retention must not depend on the cache entry.
    meshes.clear();poseA.reset();poseB.reset();
    check(pending[0].mesh->headY==3 && submitted[0].mesh->headY==1);
    bool rejected=false;
    try{resolvePendingMeshes(pending,meshes);}catch(const std::logic_error &){rejected=true;}
    check(rejected);
    std::vector<Draw> empty;
    resolvePendingMeshes(empty,meshes);check(empty.empty());
    std::printf("late mesh: %u checks passed\n",checks);
}
