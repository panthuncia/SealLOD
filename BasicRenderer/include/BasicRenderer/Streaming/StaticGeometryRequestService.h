#pragma once

#include <BasicRenderer/Streaming/GeometryRequests.h>
#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <optional>

class MeshManager;

namespace br::render {

class StaticGeometryRequestService {
public:
    void Configure(MeshManager* meshes) noexcept { m_meshes = meshes; }
    [[nodiscard]] bool Available() const noexcept { return m_meshes != nullptr; }
    void AddMeshesBulk(const std::vector<std::shared_ptr<Mesh>>& meshes, bool reordered) const;
    std::vector<StaticMeshTemplateRegistration> AddStaticMeshTemplatesBulk(
        const std::vector<StaticMeshTemplateRequest>& requests) const;
    void PrepareStaticMeshTemplateResourcesAsync(
        const std::vector<StaticMeshTemplateRequest>& requests) const;
	std::uint64_t PublishDesiredBufferState() const;
	std::optional<ArtifactRequirement> DesiredBufferStateRequirement() const;
	std::optional<ArtifactRequirement> DesiredBufferStateRequirement(std::uint64_t& coverage) const;
private:
    MeshManager* m_meshes = nullptr;
};

}
