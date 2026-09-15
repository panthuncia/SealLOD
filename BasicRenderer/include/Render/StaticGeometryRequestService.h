#pragma once

#include "Managers/MeshManager.h"

namespace br::render {

class StaticGeometryRequestService {
public:
    void Configure(MeshManager* meshes) noexcept { m_meshes = meshes; }
    [[nodiscard]] bool Available() const noexcept { return m_meshes != nullptr; }
    void AddMeshesBulk(const std::vector<std::shared_ptr<Mesh>>& meshes, bool reordered) const;
    std::vector<MeshManager::StaticMeshTemplateRegistration> AddStaticMeshTemplatesBulk(
        const std::vector<MeshManager::StaticMeshTemplateRequest>& requests) const;
    void PrepareStaticMeshTemplateResourcesAsync(
        const std::vector<MeshManager::StaticMeshTemplateRequest>& requests) const;
	std::uint64_t PublishDesiredBufferState() const;
	std::optional<ArtifactRequirement> DesiredBufferStateRequirement() const;
private:
    MeshManager* m_meshes = nullptr;
};

}
