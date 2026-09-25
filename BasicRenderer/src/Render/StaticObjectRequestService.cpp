#include "Render/StaticObjectRequestService.h"

#include <stdexcept>

namespace br::render {
namespace {
ObjectManager& RequireObjects(ObjectManager* objects) {
    if (!objects) throw std::logic_error("StaticObjectRequestService is not configured");
    return *objects;
}
}

void StaticObjectRequestService::Configure(ObjectManager* objects) noexcept { m_objects = objects; }
bool StaticObjectRequestService::Available() const noexcept { return m_objects != nullptr; }
void StaticObjectRequestService::SetDesiredBufferStateReadyCallback(ObjectManager::DesiredBufferStateReadyCallback callback) const { RequireObjects(m_objects).SetDesiredBufferStateReadyCallback(std::move(callback)); }
ObjectManager::StaticVisibilityUpdateResult StaticObjectRequestService::SetStaticObjectsVisibleBulk(std::span<ObjectManager::StaticObjectResidencyHandle*> handles, bool visible) const { return RequireObjects(m_objects).SetStaticObjectsVisibleBulk(handles, visible); }
std::shared_ptr<SortedUnsignedIntBuffer> StaticObjectRequestService::TryGetActiveDrawSetIndices(const DrawWorkloadKey& workload) const { return RequireObjects(m_objects).TryGetActiveDrawSetIndices(workload); }
void StaticObjectRequestService::RequestStaticImportGraphCapacityHint(std::uint64_t transformRows, std::uint64_t drawRecords) const { RequireObjects(m_objects).RequestStaticImportGraphCapacityHint(transformRows, drawRecords); }
std::vector<ObjectManager::StaticImportReservationStatus> StaticObjectRequestService::TryReserveStaticImportTransactionsInPlace(std::span<ObjectManager::StaticImportBuildBatch*> builds, std::vector<ObjectManager::StaticImportReservation>& reservations) const { return RequireObjects(m_objects).TryReserveStaticImportTransactionsInPlace(builds, reservations); }
ObjectManager::MaterializedStaticImportTransaction StaticObjectRequestService::MaterializeStaticImportTransaction(ObjectManager::StaticImportReservation&& reservation, ObjectManager::StaticImportBuildBatch& buildScratch) const { return RequireObjects(m_objects).MaterializeStaticImportTransaction(std::move(reservation), buildScratch); }
void StaticObjectRequestService::StageStaticImportTransactionUploads(ObjectManager::MaterializedStaticImportTransaction& transaction, bool includeDrawRecords) const { RequireObjects(m_objects).StageStaticImportTransactionUploads(transaction, includeDrawRecords); }
void StaticObjectRequestService::RequestStaticImportTransactionResources(const ObjectManager::StaticImportBuildBatch& build) const { RequireObjects(m_objects).RequestStaticImportTransactionResources(build); }
std::uint64_t StaticObjectRequestService::PublishDesiredBufferState() const { return RequireObjects(m_objects).PublishDesiredBufferState(); }
std::uint64_t StaticObjectRequestService::RequiredGeometryCoverage() const { return RequireObjects(m_objects).RequiredGeometryCoverage(); }
ObjectManager::DesiredObjectBufferStateCut StaticObjectRequestService::DesiredBufferStateCut() const { return RequireObjects(m_objects).DesiredBufferStateCut(); }
ObjectManager::StaticImportBulkPublishResult StaticObjectRequestService::PublishStaticImportTransactionsBulk(std::span<ObjectManager::MaterializedStaticImportTransaction*> transactions) const { return RequireObjects(m_objects).PublishStaticImportTransactionsBulk(transactions); }
std::uint64_t StaticObjectRequestService::MakeDeferredRetireFrame() const { return RequireObjects(m_objects).MakeDeferredRetireFrame(); }
ObjectManager::StaticObjectRemovalResult StaticObjectRequestService::RemoveStaticObjectsBulk(std::span<const ObjectManager::StaticObjectRemovalPayload> payloads, const ObjectManager::RemoveObjectsBulkOptions& options) const { return RequireObjects(m_objects).RemoveStaticObjectsBulk(payloads, options); }
void StaticObjectRequestService::CancelStaticImportTransaction(ObjectManager::StaticImportReservation reservation, std::uint64_t retireFrame) const { RequireObjects(m_objects).CancelStaticImportTransaction(std::move(reservation), retireFrame); }
ObjectManager::StaticImportResourceProbe StaticObjectRequestService::CreateStaticImportResourceProbe() const { return RequireObjects(m_objects).CreateStaticImportResourceProbe(); }
ObjectManager::StaticImportResourceProbeStatus StaticObjectRequestService::ProbeStaticImportTransactionResources(ObjectManager::StaticImportBuildBatch& build, ObjectManager::StaticImportResourceProbe& probe) const { return RequireObjects(m_objects).ProbeStaticImportTransactionResources(build, probe); }
ObjectManager::Stats StaticObjectRequestService::GetStats() const { return RequireObjects(m_objects).GetStats(); }

}
