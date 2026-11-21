#include "backend.hpp"
#include "opaque_policy.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace native_renderer::detail::deko {
Vertex *DekoBackend::frameMesh(uint32_t count) {
    vertexBytes(count);
    scratchVertices.resize(count);
    scratchVertexCount = count; scratchIndexCount = 0;
    return scratchVertices.data();
}
uint32_t *DekoBackend::frameMeshIndices(uint32_t count, uint32_t &base) {
    if (!scratchVertexCount || !count || count % 3 || count > 16 * 1024 * 1024)
        throw std::invalid_argument("deko3d: invalid pending indexed mesh");
    scratchIndices.resize(count); scratchIndexCount = count;
    base = 0; return scratchIndices.data();
}
void DekoBackend::commitFrameMesh(uint64_t id, uint32_t count, uint32_t indexCount) {
    if (!count || count > scratchVertexCount || indexCount > scratchIndexCount)
        throw std::invalid_argument("deko3d: commit exceeds pending mesh range");
    if (!indexCount) {
        uploadMesh(id, scratchVertices.data(), count, true);
    } else {
        validateIndices(scratchIndices.data(), indexCount, count);
        auto mesh = std::make_shared<Mesh>();
        mesh->minAlpha=minimumVertexAlpha(scratchVertices.data(),count);
        {uint8_t top=0;for(uint32_t i=0;i<count;++i)top=std::max(top,scratchVertices[i].a);mesh->maxAlpha=top;}
        mesh->bounds=meshBounds(scratchVertices.data(),count);
        const auto bytes = vertexBytes(count);
        bool pack=packNormals;
        for(uint32_t i=0;pack && i<count;++i){const Vertex &v=scratchVertices[i];pack=std::fabs(v.nx)<=1.0f && std::fabs(v.ny)<=1.0f && std::fabs(v.nz)<=1.0f;}
        mesh->packed=pack;(pack?packedMeshes:floatMeshes)++;
        mesh->vertices = vertexStorage(pack?size_t(count)*sizeof(PackedVertex):bytes,mesh->vertexOffset,true);
        if(pack){auto *p=reinterpret_cast<PackedVertex *>(static_cast<uint8_t *>(mesh->vertices->cpu())+mesh->vertexOffset);
            for(uint32_t i=0;i<count;++i)p[i]=packVertex(scratchVertices[i]);}
        else std::memcpy(static_cast<uint8_t *>(mesh->vertices->cpu())+mesh->vertexOffset, scratchVertices.data(), bytes);
        const auto indexBytes=size_t(indexCount)*sizeof(uint32_t);
        mesh->indices=vertexStorage(indexBytes,mesh->indexOffset,true);
        std::memcpy(static_cast<uint8_t *>(mesh->indices->cpu())+mesh->indexOffset,scratchIndices.data(),indexBytes);
        mesh->count = count; mesh->indexCount = indexCount; mesh->transient = true;
        // v170: the faint ones only, as v168. v169 copied every frame mesh up to 1024 vertices, every frame (a heap
        // vector each): +1.4 ms of the render thread's record (3.9 -> 5.3 ms) for 0-4 clipped draws a frame;
        // clipToTexture now copies a frame mesh itself, only when a draw of it passes every other test.
        if(mesh->maxAlpha<64 && count<=8192){
            mesh->cpuVertices=std::make_shared<const std::vector<Vertex>>(scratchVertices.begin(),scratchVertices.begin()+count);
            mesh->cpuIndices.assign(scratchIndices.begin(),scratchIndices.begin()+indexCount);
        }
        meshes[id] = std::move(mesh);
    }
    cancelFrameMesh();
}
void DekoBackend::uploadSkinnedMesh(uint64_t, const SkinVertex *, uint32_t,
                                    const uint32_t *, uint32_t) {
    throw std::runtime_error("deko3d: GPU skinning not implemented; capability is false");
}
uint32_t DekoBackend::framePalette(const float *, uint32_t) {
    throw std::runtime_error("deko3d: GPU palettes not implemented; capability is false");
}
void DekoBackend::rewriteFramePalette(uint32_t, const float *, uint32_t) {
    throw std::runtime_error("deko3d: GPU palettes not implemented; capability is false");
}
}
