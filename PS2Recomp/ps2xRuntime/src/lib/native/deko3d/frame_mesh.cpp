#include "backend.hpp"
#include "opaque_policy.hpp"

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
        mesh->bounds=meshBounds(scratchVertices.data(),count);
        const auto bytes = vertexBytes(count);
        mesh->vertices = vertexStorage(bytes,mesh->vertexOffset,true);
        std::memcpy(static_cast<uint8_t *>(mesh->vertices->cpu())+mesh->vertexOffset, scratchVertices.data(), bytes);
        const auto indexBytes=size_t(indexCount)*sizeof(uint32_t);
        mesh->indices=vertexStorage(indexBytes,mesh->indexOffset,true);
        std::memcpy(static_cast<uint8_t *>(mesh->indices->cpu())+mesh->indexOffset,scratchIndices.data(),indexBytes);
        mesh->count = count; mesh->indexCount = indexCount; mesh->transient = true;
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
