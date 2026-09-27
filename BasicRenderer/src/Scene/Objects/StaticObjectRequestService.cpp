#include <BasicRenderer/Streaming/StaticObjectRequestService.h>
#include "Scene/Objects/ObjectManager.h"

#include <stdexcept>

namespace br::render {
PreparedStaticGroupsBulkPlan StaticObjectRequestService::PrepareStaticGroupsBulkPlan(
    const std::vector<StaticGroupBuildInfo>& groups) {
    return ObjectManager::PrepareStaticGroupsBulkPlan(groups);
}

StaticImportPacket StaticObjectRequestService::BuildStaticImportPacket(StaticImportPacketPlan plan) {
    return ObjectManager::BuildStaticImportPacket(std::move(plan));
}

void StaticObjectRequestService::FinalizeStaticImportBuildBatch(StaticImportBuildBatch& build) {
    ObjectManager::FinalizeStaticImportBuildBatch(build);
}

namespace {
ObjectManager& RequireObjects(ObjectManager* objects) {
    if (!objects) throw std::logic_error("StaticObjectRequestService is not configured");
    return *objects;
}
}

void StaticObjectRequestService::Configure(ObjectManager* objects) noexcept { m_objects = objects; }
bool StaticObjectRequestService::Available() const noexcept { return m_objects != nullptr; }
void StaticObjectRequestService::SetDesiredBufferStateReadyCallback(DesiredBufferStateReadyCallback callback) const { RequireObjects(m_objects).SetDesiredBufferStateReadyCallback(std::move(callback)); }
StaticVisibilityUpdateResult StaticObjectRequestService::SetStaticObjectsVisibleBulk(std::span<StaticObjectResidencyHandle*> handles, bool visible) const { return RequireObjects(m_objects).SetStaticObjectsVisibleBulk(handles, visible); }
std::shared_ptr<SortedUnsignedIntBuffer> StaticObjectRequestService::TryGetActiveDrawSetIndices(const DrawWorkloadKey& workload) const { return RequireObjects(m_objects).TryGetActiveDrawSetIndices(workload); }
void StaticObjectRequestService::RequestStaticImportGraphCapacityHint(std::uint64_t transformRows, std::uint64_t drawRecords) const { RequireObjects(m_objects).RequestStaticImportGraphCapacityHint(transformRows, drawRecords); }
std::vector<StaticImportReservationStatus> StaticObjectRequestService::TryReserveStaticImportTransactionsInPlace(std::span<StaticImportBuildBatch*> builds, std::vector<StaticImportReservation>& reservations) const { return RequireObjects(m_objects).TryReserveStaticImportTransactionsInPlace(builds, reservations); }
MaterializedStaticImportTransaction StaticObjectRequestService::MaterializeStaticImportTransaction(StaticImportReservation&& reservation, StaticImportBuildBatch& buildScratch) const { return RequireObjects(m_objects).MaterializeStaticImportTransaction(std::move(reservation), buildScratch); }
void StaticObjectRequestService::StageStaticImportTransactionUploads(MaterializedStaticImportTransaction& transaction, bool includeDrawRecords) const { RequireObjects(m_objects).StageStaticImportTransactionUploads(transaction, includeDrawRecords); }
void StaticObjectRequestService::RequestStaticImportTransactionResources(const StaticImportBuildBatch& build) const { RequireObjects(m_objects).RequestStaticImportTransactionResources(build); }
std::uint64_t StaticObjectRequestService::PublishDesiredBufferState() const { return RequireObjects(m_objects).PublishDesiredBufferState(); }
std::uint64_t StaticObjectRequestService::RequiredGeometryCoverage() const { return RequireObjects(m_objects).RequiredGeometryCoverage(); }
DesiredObjectBufferStateCut StaticObjectRequestService::DesiredBufferStateCut() const { return RequireObjects(m_objects).DesiredBufferStateCut(); }
StaticImportBulkPublishResult StaticObjectRequestService::PublishStaticImportTransactionsBulk(std::span<MaterializedStaticImportTransaction*> transactions) const { return RequireObjects(m_objects).PublishStaticImportTransactionsBulk(transactions); }
std::uint64_t StaticObjectRequestService::MakeDeferredRetireFrame() const { return RequireObjects(m_objects).MakeDeferredRetireFrame(); }
StaticObjectRemovalResult StaticObjectRequestService::RemoveStaticObjectsBulk(std::span<const StaticObjectRemovalPayload> payloads, const RemoveObjectsBulkOptions& options) const { return RequireObjects(m_objects).RemoveStaticObjectsBulk(payloads, options); }
void StaticObjectRequestService::CancelStaticImportTransaction(StaticImportReservation reservation, std::uint64_t retireFrame) const { RequireObjects(m_objects).CancelStaticImportTransaction(std::move(reservation), retireFrame); }
StaticImportResourceProbe StaticObjectRequestService::CreateStaticImportResourceProbe() const { return RequireObjects(m_objects).CreateStaticImportResourceProbe(); }
StaticImportResourceProbeStatus StaticObjectRequestService::ProbeStaticImportTransactionResources(StaticImportBuildBatch& build, StaticImportResourceProbe& probe) const { return RequireObjects(m_objects).ProbeStaticImportTransactionResources(build, probe); }
ObjectStorageStats StaticObjectRequestService::GetStats() const { return RequireObjects(m_objects).GetStats(); }

}
