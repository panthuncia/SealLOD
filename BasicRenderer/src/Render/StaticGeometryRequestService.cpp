#include "Render/StaticGeometryRequestService.h"
#include <stdexcept>

namespace br::render {
namespace { MeshManager& Storage(MeshManager* value) { if (!value) throw std::logic_error("StaticGeometryRequestService is not configured"); return *value; } }
void StaticGeometryRequestService::AddMeshesBulk(const std::vector<std::shared_ptr<Mesh>>& meshes, bool reordered) const { Storage(m_meshes).AddMeshesBulk(meshes, reordered); }
std::vector<MeshManager::StaticMeshTemplateRegistration> StaticGeometryRequestService::AddStaticMeshTemplatesBulk(const std::vector<MeshManager::StaticMeshTemplateRequest>& requests) const { return Storage(m_meshes).AddStaticMeshTemplatesBulk(requests); }
void StaticGeometryRequestService::PrepareStaticMeshTemplateResourcesAsync(const std::vector<MeshManager::StaticMeshTemplateRequest>& requests) const { Storage(m_meshes).PrepareStaticMeshTemplateResourcesAsync(requests); }
std::uint64_t StaticGeometryRequestService::PublishDesiredBufferState() const { return Storage(m_meshes).PublishDesiredBufferState(); }
std::optional<ArtifactRequirement> StaticGeometryRequestService::DesiredBufferStateRequirement() const { return Storage(m_meshes).DesiredBufferStateRequirement(); }
std::optional<ArtifactRequirement> StaticGeometryRequestService::DesiredBufferStateRequirement(std::uint64_t& coverage) const { return Storage(m_meshes).DesiredBufferStateRequirement(coverage); }
}
