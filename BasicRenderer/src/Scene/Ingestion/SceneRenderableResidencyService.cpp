#include "BasicRenderer/Runtime/Detail/SceneRenderableResidencyService.h"

#include <algorithm>
#include <stdexcept>

#include "Materials/MaterialManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"

namespace br::render {
namespace {
template <typename T> T& Storage(T* value, const char* name) {
    if (!value) throw std::logic_error(name);
    return *value;
}
}

void SceneRenderableResidencyService::Configure(
    MeshManager* meshes, MaterialManager* materials) noexcept {
    m_meshes = meshes;
    m_materials = materials;
}

bool SceneRenderableResidencyService::Available() const noexcept {
    return m_meshes != nullptr && m_materials != nullptr;
}

unsigned int SceneRenderableResidencyService::AcquireMaterial(
    Material& material, unsigned int count) const {
    return Storage(m_materials, "Renderable residency material storage is unavailable")
        .IncrementMaterialUsageCount(material, true, count);
}

void SceneRenderableResidencyService::ReleaseMaterial(const Material& material) const {
    Storage(m_materials, "Renderable residency material storage is unavailable")
        .DecrementMaterialUsageCount(material);
}

void SceneRenderableResidencyService::MergeReyesUvDensity(
    Mesh& mesh, Material& material) const {
    const uint32_t heightUvSetIndex = material.GetData().heightUvSetIndex;
    auto density = mesh.EstimateReyesUvDensity(heightUvSetIndex);
    if ((material.GetMaterialFlags() & MaterialFlags::MATERIAL_TERRAIN)
        != MaterialFlags::MATERIAL_FLAGS_NONE) {
        density.x = (std::max)(density.x, 1.0f);
        density.y = (std::max)(density.y, 1.0f);
    }
    const auto previous = material.GetReyesUvDensity();
    material.MergeReyesUvDensity(density);
    const auto updated = material.GetReyesUvDensity();
    if (updated.x != previous.x || updated.y != previous.y) {
        Storage(m_materials, "Renderable residency material storage is unavailable")
            .MarkMaterialDirty(material);
    }
}

unsigned int SceneRenderableResidencyService::AcquireCompileFlags(
    MaterialCompileFlags flags, unsigned int count) const {
    return Storage(m_materials, "Renderable residency material storage is unavailable")
        .AcquireCompileFlagsSlot(flags, count);
}

bool SceneRenderableResidencyService::ReleaseCompileFlags(
    MaterialCompileFlags flags, unsigned int count) const {
    return Storage(m_materials, "Renderable residency material storage is unavailable")
        .ReleaseCompileFlagsSlot(flags, count);
}

unsigned int SceneRenderableResidencyService::AcquireRasterBucket(
    MaterialRasterFlags flags, unsigned int count) const {
    return Storage(m_materials, "Renderable residency material storage is unavailable")
        .AcquireRasterBucket(flags, count);
}

void SceneRenderableResidencyService::ReleaseRasterBucket(MaterialRasterFlags flags) const {
    Storage(m_materials, "Renderable residency material storage is unavailable")
        .ReleaseRasterBucket(flags);
}

std::unique_ptr<org::BufferView> SceneRenderableResidencyService::AllocateMeshOverride(
    const PerMeshCB& data) const {
    return Storage(m_meshes, "Renderable residency geometry storage is unavailable")
        .AllocatePerMeshOverrideBuffer(data);
}

void SceneRenderableResidencyService::ReleaseMeshOverride(
    std::unique_ptr<org::BufferView>& view) const {
    Storage(m_meshes, "Renderable residency geometry storage is unavailable")
        .ReleasePerMeshOverrideBuffer(view);
}

bool SceneRenderableResidencyService::MaterializeMesh(
    std::shared_ptr<Mesh>& mesh, bool reordered) const {
    return Storage(m_meshes, "Renderable residency geometry storage is unavailable")
        .AddMesh(mesh, reordered);
}

bool SceneRenderableResidencyService::MaterializeInstance(
    MeshInstance& instance, bool reordered) const {
    return Storage(m_meshes, "Renderable residency geometry storage is unavailable")
        .AddMeshInstance(&instance, reordered);
}

void SceneRenderableResidencyService::ReleaseInstance(MeshInstance& instance) const {
    Storage(m_meshes, "Renderable residency geometry storage is unavailable")
        .RemoveMeshInstance(&instance);
}

} // namespace br::render
