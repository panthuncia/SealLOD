#pragma once

#include <memory>

#include "Materials/Material.h"
#include "ShaderBuffers.h"

namespace org { class BufferView; }
class MaterialManager;
class Mesh;
class MeshInstance;
class MeshManager;

namespace br::render {

// Serialized dynamic-scene residency boundary. It coordinates the related
// geometry and material allocations as operations, without exposing their
// backing stores to scene ingestion.
class SceneRenderableResidencyService {
public:
    void Configure(MeshManager* meshes, MaterialManager* materials) noexcept;
    [[nodiscard]] bool Available() const noexcept;

    unsigned int AcquireMaterial(Material& material, unsigned int count = 1u) const;
    void ReleaseMaterial(const Material& material) const;
    void MergeReyesUvDensity(Mesh& mesh, Material& material) const;
    unsigned int AcquireCompileFlags(MaterialCompileFlags flags, unsigned int count = 1u) const;
    bool ReleaseCompileFlags(MaterialCompileFlags flags, unsigned int count = 1u) const;
    unsigned int AcquireRasterBucket(MaterialRasterFlags flags, unsigned int count = 1u) const;
    void ReleaseRasterBucket(MaterialRasterFlags flags) const;

    std::unique_ptr<org::BufferView> AllocateMeshOverride(const PerMeshCB& data) const;
    void ReleaseMeshOverride(std::unique_ptr<org::BufferView>& view) const;
    bool MaterializeMesh(std::shared_ptr<Mesh>& mesh, bool reordered) const;
    bool MaterializeInstance(MeshInstance& instance, bool reordered) const;
    void ReleaseInstance(MeshInstance& instance) const;

private:
    MeshManager* m_meshes = nullptr;
    MaterialManager* m_materials = nullptr;
};

} // namespace br::render
