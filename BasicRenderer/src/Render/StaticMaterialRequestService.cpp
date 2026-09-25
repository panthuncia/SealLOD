#include "Render/StaticMaterialRequestService.h"
#include <stdexcept>

namespace br::render {
namespace { MaterialManager& Storage(MaterialManager* value) { if (!value) throw std::logic_error("StaticMaterialRequestService is not configured"); return *value; } }
void StaticMaterialRequestService::MarkMaterialDirty(Material& material) const { Storage(m_materials).MarkMaterialDirty(material); }
std::uint64_t StaticMaterialRequestService::DesiredPublishedStateRevision() const { return Storage(m_materials).DesiredPublishedStateRevision(); }
ArtifactVersionHandle StaticMaterialRequestService::DesiredPublishedStateHandle() const { return Storage(m_materials).DesiredPublishedStateHandle(); }
void StaticMaterialRequestService::RegisterMaterialSource(const std::shared_ptr<Material>& material) const { Storage(m_materials).RegisterMaterialSource(material); }
MaterialManager::MaterialUsageCapture StaticMaterialRequestService::CaptureMaterialUsage(Material& material, unsigned int count, bool refresh) const { return Storage(m_materials).CaptureMaterialUsage(material, count, refresh); }
std::shared_ptr<const MaterialUsageReservation> StaticMaterialRequestService::ReserveMaterialUsage(const std::vector<MaterialManager::MaterialUsageCapture>& captures) const { return Storage(m_materials).ReserveMaterialUsage(captures); }
void StaticMaterialRequestService::DecrementMaterialUsageCount(const Material& material) const { Storage(m_materials).DecrementMaterialUsageCount(material); }
unsigned int StaticMaterialRequestService::AcquireCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count) const { return Storage(m_materials).AcquireCompileFlagsSlot(flags, count); }
bool StaticMaterialRequestService::ReleaseCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count) const { return Storage(m_materials).ReleaseCompileFlagsSlot(flags, count); }
unsigned int StaticMaterialRequestService::AcquireRasterBucket(MaterialRasterFlags flags, unsigned int count) const { return Storage(m_materials).AcquireRasterBucket(flags, count); }
void StaticMaterialRequestService::ReleaseRasterBucket(MaterialRasterFlags flags) const { Storage(m_materials).ReleaseRasterBucket(flags); }
}
