#pragma once

#include "Managers/ObjectManager.h"

namespace br::render {

// Serialized scene-ingestion boundary for persistent object storage.  The
// service deliberately preserves the existing transaction value types while
// keeping the mutable ObjectManager out of producer capability sets.
class StaticObjectRequestService {
public:
    void Configure(ObjectManager* objects) noexcept;
    [[nodiscard]] bool Available() const noexcept;

    void SetDesiredBufferStateReadyCallback(ObjectManager::DesiredBufferStateReadyCallback callback) const;
    ObjectManager::StaticVisibilityUpdateResult SetStaticObjectsVisibleBulk(
        std::span<ObjectManager::StaticObjectResidencyHandle*> handles, bool visible) const;
    std::shared_ptr<SortedUnsignedIntBuffer> TryGetActiveDrawSetIndices(const DrawWorkloadKey& workload) const;
    void RequestStaticImportGraphCapacityHint(std::uint64_t transformRows, std::uint64_t drawRecords) const;
    std::vector<ObjectManager::StaticImportReservationStatus> TryReserveStaticImportTransactionsInPlace(
        std::span<ObjectManager::StaticImportBuildBatch*> builds,
        std::vector<ObjectManager::StaticImportReservation>& reservations) const;
    ObjectManager::MaterializedStaticImportTransaction MaterializeStaticImportTransaction(
        ObjectManager::StaticImportReservation&& reservation,
        ObjectManager::StaticImportBuildBatch& buildScratch) const;
    void StageStaticImportTransactionUploads(
        ObjectManager::MaterializedStaticImportTransaction& transaction,
        bool includeDrawRecords = true) const;
    void RequestStaticImportTransactionResources(const ObjectManager::StaticImportBuildBatch& build) const;
    std::uint64_t PublishDesiredBufferState() const;
    ObjectManager::DesiredObjectBufferStateCut DesiredBufferStateCut() const;
    // Geometry mutation sequence a Geometry root must cover before the newest
    // static draw records can publish.
    std::uint64_t RequiredGeometryCoverage() const;
    ObjectManager::StaticImportBulkPublishResult PublishStaticImportTransactionsBulk(
        std::span<ObjectManager::MaterializedStaticImportTransaction*> transactions) const;
    std::uint64_t MakeDeferredRetireFrame() const;
    ObjectManager::StaticObjectRemovalResult RemoveStaticObjectsBulk(
        std::span<const ObjectManager::StaticObjectRemovalPayload> payloads,
        const ObjectManager::RemoveObjectsBulkOptions& options) const;
    void CancelStaticImportTransaction(
        ObjectManager::StaticImportReservation reservation, std::uint64_t retireFrame = 0) const;
    ObjectManager::StaticImportResourceProbe CreateStaticImportResourceProbe() const;
    ObjectManager::StaticImportResourceProbeStatus ProbeStaticImportTransactionResources(
        ObjectManager::StaticImportBuildBatch& build,
        ObjectManager::StaticImportResourceProbe& probe) const;
    ObjectManager::Stats GetStats() const;

private:
    ObjectManager* m_objects = nullptr;
};

}
