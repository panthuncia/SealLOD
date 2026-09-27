#pragma once

#include <BasicRenderer/Streaming/MaterialRequests.h>

class MaterialManager;
class Material;

namespace br::render {

class StaticMaterialRequestService {
public:
    void Configure(MaterialManager* materials) noexcept { m_materials = materials; }
    [[nodiscard]] bool Available() const noexcept { return m_materials != nullptr; }
    void MarkMaterialDirty(Material& material) const;
    std::uint64_t DesiredPublishedStateRevision() const;
    ArtifactVersionHandle DesiredPublishedStateHandle() const;
    void RegisterMaterialSource(const std::shared_ptr<Material>& material) const;
    MaterialUsageCapture CaptureMaterialUsage(Material& material, unsigned int count, bool refresh) const;
    std::shared_ptr<const MaterialUsageReservation> ReserveMaterialUsage(
        const std::vector<MaterialUsageCapture>& captures) const;
    void DecrementMaterialUsageCount(const Material& material) const;
    unsigned int AcquireCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count = 1u) const;
    bool ReleaseCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count = 1u) const;
    unsigned int AcquireRasterBucket(MaterialRasterFlags flags, unsigned int count = 1u) const;
    void ReleaseRasterBucket(MaterialRasterFlags flags) const;
private:
    MaterialManager* m_materials = nullptr;
};

}
