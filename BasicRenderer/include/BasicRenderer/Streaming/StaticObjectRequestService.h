#pragma once

#include <BasicRenderer/Streaming/ObjectRequests.h>

class ObjectManager;

namespace br::render {

// Serialized scene-ingestion boundary for persistent object storage.  The
// service deliberately preserves the existing transaction value types while
// keeping the mutable ObjectManager out of producer capability sets.
class StaticObjectRequestService {
public:
    static PreparedStaticGroupsBulkPlan PrepareStaticGroupsBulkPlan(
        const std::vector<StaticGroupBuildInfo>& groups);
    static StaticImportPacket BuildStaticImportPacket(StaticImportPacketPlan plan);
    static void FinalizeStaticImportBuildBatch(StaticImportBuildBatch& build);
    void Configure(ObjectManager* objects) noexcept;
    [[nodiscard]] bool Available() const noexcept;

    void SetDesiredBufferStateReadyCallback(DesiredBufferStateReadyCallback callback) const;
    StaticVisibilityUpdateResult SetStaticObjectsVisibleBulk(
        std::span<StaticObjectResidencyHandle*> handles, bool visible) const;
    std::shared_ptr<SortedUnsignedIntBuffer> TryGetActiveDrawSetIndices(const DrawWorkloadKey& workload) const;
    void RequestStaticImportGraphCapacityHint(std::uint64_t transformRows, std::uint64_t drawRecords) const;
    std::vector<StaticImportReservationStatus> TryReserveStaticImportTransactionsInPlace(
        std::span<StaticImportBuildBatch*> builds,
        std::vector<StaticImportReservation>& reservations) const;
    MaterializedStaticImportTransaction MaterializeStaticImportTransaction(
        StaticImportReservation&& reservation,
        StaticImportBuildBatch& buildScratch) const;
    void StageStaticImportTransactionUploads(
        MaterializedStaticImportTransaction& transaction,
        bool includeDrawRecords = true) const;
    void RequestStaticImportTransactionResources(const StaticImportBuildBatch& build) const;
    std::uint64_t PublishDesiredBufferState() const;
    DesiredObjectBufferStateCut DesiredBufferStateCut() const;
    // Geometry mutation sequence a Geometry root must cover before the newest
    // static draw records can publish.
    std::uint64_t RequiredGeometryCoverage() const;
    StaticImportBulkPublishResult PublishStaticImportTransactionsBulk(
        std::span<MaterializedStaticImportTransaction*> transactions) const;
    std::uint64_t MakeDeferredRetireFrame() const;
    StaticObjectRemovalResult RemoveStaticObjectsBulk(
        std::span<const StaticObjectRemovalPayload> payloads,
        const RemoveObjectsBulkOptions& options) const;
    void CancelStaticImportTransaction(
        StaticImportReservation reservation, std::uint64_t retireFrame = 0) const;
    StaticImportResourceProbe CreateStaticImportResourceProbe() const;
    StaticImportResourceProbeStatus ProbeStaticImportTransactionResources(
        StaticImportBuildBatch& build,
        StaticImportResourceProbe& probe) const;
    ObjectStorageStats GetStats() const;

private:
    ObjectManager* m_objects = nullptr;
};

}
