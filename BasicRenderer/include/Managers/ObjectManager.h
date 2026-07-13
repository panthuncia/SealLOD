#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <limits>
#include <optional>
#include <mutex>
#include <cstdint>
#include <deque>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/Buffers/DynamicBuffer.h"
#include "ShaderBuffers.h"
#include "Resources/Buffers/SortedUnsignedIntBuffer.h"
#include "Render/IndirectCommand.h"
#include "Scene/Components.h"
#include "Interfaces/IResourceProvider.h"
#include "Materials/TechniqueDescriptor.h"
#include "Render/Runtime/BufferUploadPolicy.h"

class BufferView;
class DynamicBuffer;
class Material;
class Mesh;

class ObjectManager : public IResourceProvider {
public:
	static std::unique_ptr<ObjectManager> CreateUnique() {
		return std::unique_ptr<ObjectManager>(new ObjectManager());
	}
	~ObjectManager();

	struct ObjectBuildInfo {
		PerObjectCB perObjectCB{};
		const Components::MeshInstances* meshInstances = nullptr;
		const Components::InstanceTransforms* instanceTransforms = nullptr;
	};

	struct StaticMeshTemplateRef {
		std::uint32_t meshTemplateIndex = 0;
		std::uint32_t clodOffsetIndex = 0;
		std::shared_ptr<Mesh> mesh;
		std::shared_ptr<Material> material;
		std::vector<DrawWorkloadKey> workloadKeys;
		std::uint32_t skinnedAssemblyTypeSlot = 0xFFFFFFFFu;
		BoundingSphere skinnedAssemblyBounds{};
		float skinnedBoundsScale = 1.0f;
	};

	struct StaticGroupBuildInfo {
		std::uint64_t stableGroupID = 0;
		std::uint64_t allocationScopeID = 0;
		std::vector<DirectX::XMMATRIX> instanceTransforms;
		std::vector<StaticMeshTemplateRef> meshTemplates;
	};
	struct PreparedStaticMeshTemplateRef {
		std::uint32_t meshTemplateIndex = 0;
		std::uint32_t clodOffsetIndex = 0;
		std::vector<DrawWorkloadKey> workloadKeys;
		std::uint32_t skinnedAssemblyTypeSlot = 0xFFFFFFFFu;
		BoundingSphere skinnedAssemblyBounds{};
		float skinnedBoundsScale = 1.0f;
	};

	struct PreparedStaticGroupInfo {
		std::uint64_t stableGroupID = 0;
		std::uint64_t allocationScopeID = 0;
		std::vector<PerObjectCB> perObjectCBs;
		std::vector<DirectX::XMFLOAT4X4> normalMatrices;
		std::vector<PreparedStaticMeshTemplateRef> meshTemplates;
		std::vector<std::vector<DrawWorkloadKey>> workloadKeysByMeshTemplate;
	};

	struct PreparedStaticGroupsBulkPlan {
		std::vector<PreparedStaticGroupInfo> groups;
		std::uint64_t transformRows = 0;
		std::uint64_t drawRecords = 0;
		std::uint64_t preparedBytes = 0;
		std::uint64_t prepareUs = 0;
		std::uint64_t transformBuildUs = 0;
		std::uint64_t workloadBuildUs = 0;
		std::uint64_t drawRecordBuildUs = 0;
	};

	struct Stats {
		std::uint64_t bulkAddCalls = 0;
		std::uint64_t objectsSubmitted = 0;
		std::uint64_t staticDirectBulkAddCalls = 0;
		std::uint64_t staticDirectGroupsSubmitted = 0;
		std::uint64_t staticDirectGroupsImported = 0;
		std::uint64_t staticDirectTransformRows = 0;
		std::uint64_t staticDirectDrawRecords = 0;
		std::uint64_t staticDirectImportUs = 0;
		std::uint64_t staticDirectTransformBuildUs = 0;
		std::uint64_t staticDirectPageUploadUs = 0;
		std::uint64_t staticDirectWorkloadBuildUs = 0;
		std::uint64_t staticDirectDrawRecordBuildUs = 0;
		std::uint64_t staticDirectDrawRecordUploadUs = 0;
		std::uint64_t staticDirectFinalizeUs = 0;
		std::uint64_t staticDirectResizePublishUs = 0;
		std::uint64_t staticDirectScopeBuildUs = 0;
		std::uint64_t staticDirectNormalPatchUs = 0;
		std::uint64_t staticDirectPacketBuildUs = 0;
		std::uint64_t staticDirectPacketPublishUs = 0;
		std::uint64_t staticDirectReserveHeadroomCalls = 0;
		std::uint64_t staticDirectReservedHeadroomBytes = 0;
		std::uint64_t staticDirectWorkloadCacheHits = 0;
		std::uint64_t staticDirectWorkloadCacheMisses = 0;
		std::uint64_t perObjectRowsAllocated = 0;
		std::uint64_t perInstanceTransformRowsAllocated = 0;
		std::uint64_t normalMatrixRowsAllocated = 0;
		std::uint64_t meshTemplateRowsReferenced = 0;
		std::uint64_t instanceDrawRecordsAllocated = 0;
		std::uint64_t activeDrawSetInsertCalls = 0;
		std::uint64_t activeDrawSetInsertIndices = 0;
		std::uint64_t activeDrawSetInsertUs = 0;
		std::uint64_t bulkRemoveCalls = 0;
		std::uint64_t bulkRemoveObjects = 0;
		std::uint64_t bulkRemoveUs = 0;
		std::uint64_t bulkRemovePageDeallocUs = 0;
		std::uint64_t bulkRemoveCollectUs = 0;
		std::uint64_t activeDrawSetRemoveCalls = 0;
		std::uint64_t activeDrawSetRemoveIndices = 0;
		std::uint64_t activeDrawSetRemoveUs = 0;
		std::uint64_t activeDrawSetCompactionJobsQueued = 0;
		std::uint64_t activeDrawSetCompactionJobsBuilt = 0;
		std::uint64_t activeDrawSetCompactionJobsPublished = 0;
		std::uint64_t activeDrawSetCompactionJobsStale = 0;
		std::uint64_t activeDrawSetCompactionInputEntries = 0;
		std::uint64_t activeDrawSetCompactionOutputEntries = 0;
		std::uint64_t activeDrawSetCompactionWorkerUs = 0;
		std::uint64_t activeDrawSetCompactionPublishUs = 0;
		std::uint64_t maxDrawRecordIndex = 0;
		std::uint64_t bulkReserveCalls = 0;
		std::uint64_t bulkReserveUs = 0;
		std::uint64_t bulkReservedPerObjectBytes = 0;
		std::uint64_t bulkReservedInstanceTransformBytes = 0;
		std::uint64_t bulkReservedDrawRecordBytes = 0;
		std::uint64_t bulkReservedNormalMatrixRows = 0;
		std::uint64_t deferredRetireRangesQueued = 0;
		std::uint64_t deferredRetireRangesRetired = 0;
		std::uint64_t deferredRetireBytesQueued = 0;
		std::uint64_t deferredRetireBytesRetired = 0;
		std::uint64_t deferredRetireQueueDepth = 0;
		std::uint64_t deferredRetireWorkerUs = 0;
	};

	struct StaticImportPacketPlan {
		PreparedStaticGroupsBulkPlan prepared;
	};

	struct StaticImportPacketAllocation {
		std::vector<DynamicBuffer::PagedAllocation> perObjectPages;
		std::vector<DynamicBuffer::PagedAllocation> instanceTransformPages;
		std::vector<DynamicBuffer::PagedAllocation> normalMatrixPages;
		std::vector<DynamicBuffer::PagedAllocation> instanceDrawRecordPages;
	};

	struct StaticImportPacket {
		struct GroupTransformRange {
			std::size_t first = 0;
			std::size_t count = 0;
		};

		struct PatchableDrawRecord {
			std::size_t groupIndex = 0;
			std::size_t scopeTransformOrdinal = 0;
			std::uint32_t meshTemplateIndex = 0;
			std::uint32_t clodOffsetIndex = 0;
			std::uint32_t skinnedAssemblyTypeSlot = 0xFFFFFFFFu;
			BoundingSphere skinnedAssemblyBounds{};
			float skinnedBoundsScale = 1.0f;
			std::vector<DrawWorkloadKey> workloadKeys;
		};

		struct Scope {
			std::uint64_t id = 0;
			std::vector<std::size_t> groupIndices;
			std::vector<PerObjectCB> perObjectCBs;
			std::vector<DirectX::XMFLOAT4X4> normalMatrices;
			std::vector<PatchableDrawRecord> drawRecords;
			StaticImportPacketAllocation allocation;
		};

		std::vector<Scope> scopes;
		std::vector<GroupTransformRange> transformRanges;
		std::vector<Components::ObjectDrawInfo> drawInfos;
		std::uint64_t groupCount = 0;
		std::uint64_t transformRows = 0;
		std::uint64_t drawRecords = 0;
		std::uint64_t preparedBytes = 0;
		std::uint64_t prepareUs = 0;
		std::uint64_t transformBuildUs = 0;
		std::uint64_t workloadBuildUs = 0;
		std::uint64_t scopeBuildUs = 0;
		std::uint64_t drawRecordBuildUs = 0;
		std::uint64_t packetBuildUs = 0;
	};

	struct StaticImportBuildBatch {
		PreparedStaticGroupsBulkPlan prepared;
		std::vector<std::size_t> transformCounts;
		std::vector<std::size_t> drawRecordCounts;
		std::unordered_map<DrawWorkloadKey, std::uint64_t, DrawWorkloadKey::Hasher> activeReserveCounts;
		std::uint64_t drawRecords = 0;
		std::uint64_t activeInsertIndices = 0;
		std::uint64_t preparedBytes = 0;
		std::uint64_t buildUs = 0;
		bool finalized = false;
	};

	enum class StaticImportReservationStatus {
		Ready,
		PendingResources,
		Empty
	};

	enum class StaticImportResourceProbeStatus {
		Ready,
		PendingNormalMatrix,
		PendingPerObject,
		PendingInstanceTransform,
		PendingDrawRecord,
		Empty
	};

	struct StaticImportResourceProbe {
		DynamicBuffer::AllocationProbe normalMatrix;
		DynamicBuffer::AllocationProbe perObject;
		DynamicBuffer::AllocationProbe instanceTransform;
		DynamicBuffer::AllocationProbe instanceDrawRecord;
	};

	struct StaticImportReservation {
		std::uint64_t id = 0;
		StaticImportBuildBatch build;
		std::vector<std::size_t> transformCounts;
		std::vector<std::size_t> drawRecordCounts;
		std::vector<DynamicBuffer::PagedAllocation> perObjectRanges;
		std::vector<DynamicBuffer::PagedAllocation> instanceTransformRanges;
		std::vector<DynamicBuffer::PagedAllocation> normalMatrixRanges;
		std::vector<DynamicBuffer::PagedAllocation> instanceDrawRecordRanges;
		std::size_t visibilityDirtyStart = std::numeric_limits<std::size_t>::max();
		std::size_t visibilityDirtyEnd = 0;
		std::shared_ptr<DynamicBuffer> perObjectBuffer;
		std::shared_ptr<DynamicBuffer> instanceTransformBuffer;
		std::shared_ptr<DynamicBuffer> normalMatrixBuffer;
		std::shared_ptr<DynamicBuffer> instanceDrawRecordBuffer;
		std::uint64_t groupCount = 0;
		std::uint64_t preparedBytes = 0;
		std::uint64_t drawRecords = 0;
	};

	struct RemoveObjectsBulkOptions {
		bool deferBufferRangeRetirement = false;
		bool retireInstanceDrawRecordRanges = true;
		std::uint64_t retireFrame = 0;
	};

	struct StaticObjectRemovalPayload {
		enum class BufferKind : std::uint8_t {
			PerObject,
			InstanceTransform,
			InstanceDrawRecord,
			NormalMatrix
		};

		struct BufferRetireRange {
			std::shared_ptr<DynamicBuffer> buffer;
			Components::ObjectDrawInfo::BufferRange range;
			BufferKind kind = BufferKind::PerObject;
		};

		std::vector<BufferRetireRange> bufferRanges;
		std::vector<Components::ObjectDrawInfo::ActiveDrawSetRemovalBucket> activeDrawSetRemovals;
		std::vector<std::uint32_t> drawRecordIndices;
		std::vector<std::uint32_t> skinnedAssemblyPlacementIndices;
		std::size_t drawInfoCount = 0;
	};

	struct MaterializedStaticImportTransaction {
		struct PendingSkinnedAssemblyPlacement {
			std::size_t groupIndex = 0;
			SkinnedAssemblyPlacementGPU placement{};
		};
		StaticImportReservation reservation;
		std::vector<PerObjectCB> perObjectRows;
		std::vector<DirectX::XMFLOAT4X4> normalRows;
		std::vector<InstanceDrawRecordCB> drawRecordRows;
		std::vector<Components::ObjectDrawInfo> drawInfos;
		std::vector<StaticObjectRemovalPayload> removalPayloads;
		std::vector<PendingSkinnedAssemblyPlacement> skinnedAssemblyPlacements;
		std::unordered_map<DrawWorkloadKey, std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>, DrawWorkloadKey::Hasher> activeDrawSetInserts;
		std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher> activeDrawSetSpans;
		std::uint64_t materializeUs = 0;
	};

	struct StaticImportPublishResult {
		std::vector<Components::ObjectDrawInfo> drawInfos;
		std::vector<StaticObjectRemovalPayload> removalPayloads;
		std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher> activeDrawSetSpans;
		std::uint64_t transactionID = 0;
		std::uint64_t groupsImported = 0;
		std::uint64_t drawRecords = 0;
		std::uint64_t preparedBytes = 0;
	};

	struct StaticImportTransactionPublishRecord {
		std::vector<StaticObjectRemovalPayload> removalPayloads;
		std::uint64_t transactionID = 0;
		std::uint64_t groupsImported = 0;
		std::uint64_t drawRecords = 0;
		std::uint64_t preparedBytes = 0;
	};

	struct StaticImportBulkPublishResult {
		std::vector<StaticImportTransactionPublishRecord> transactions;
		std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher> activeDrawSetSpans;
		std::uint64_t transactionID = 0;
		std::uint64_t groupsImported = 0;
		std::uint64_t drawRecords = 0;
		std::uint64_t preparedBytes = 0;
	};

	struct ActiveDrawSetCompactionPublishResult {
		DrawWorkloadKey workloadKey{};
		std::uint32_t activeSpan = 0;
		std::uint64_t inputEntries = 0;
		std::uint64_t outputEntries = 0;
	};

	struct ActiveDrawSetDebugStats {
		DrawWorkloadKey workloadKey{};
		std::uint64_t span = 0;
		std::uint64_t liveSize = 0;
		std::uint64_t tombstoneEstimate = 0;
		std::uint64_t cpuGenerationMatches = 0;
		std::uint64_t cpuGenerationStale = 0;
		std::uint64_t cpuGenerationOutOfRange = 0;
	};

	Components::ObjectDrawInfo AddObject(const PerObjectCB& perObjectCB, const Components::MeshInstances* meshInstances);
	std::vector<Components::ObjectDrawInfo> AddObjectsBulk(const std::vector<ObjectBuildInfo>& objects);
	std::vector<Components::ObjectDrawInfo> AddStaticGroupsBulk(const std::vector<StaticGroupBuildInfo>& groups);
	static PreparedStaticGroupsBulkPlan PrepareStaticGroupsBulkPlan(const std::vector<StaticGroupBuildInfo>& groups);
	static StaticImportPacketPlan PrepareStaticImportPacketPlan(const std::vector<StaticGroupBuildInfo>& groups);
	static StaticImportPacket BuildStaticImportPacket(StaticImportPacketPlan plan);
	static StaticImportBuildBatch PrepareStaticImportBuildBatch(const std::vector<StaticGroupBuildInfo>& groups);
	static void FinalizeStaticImportBuildBatch(StaticImportBuildBatch& build);
	void PrepareStaticGroupCommitResourcesAsync(const PreparedStaticGroupsBulkPlan& plan);
	void RequestStaticImportPacketResources(const StaticImportPacketPlan& plan);
	void RequestStaticImportTransactionResources(const StaticImportBuildBatch& build);
	StaticImportResourceProbe CreateStaticImportResourceProbe() const;
	StaticImportResourceProbeStatus ProbeStaticImportTransactionResources(
		StaticImportBuildBatch& build,
		StaticImportResourceProbe& probe);
	StaticImportReservationStatus TryReserveStaticImportTransaction(
		StaticImportBuildBatch build,
		StaticImportReservation& reservation);
	std::vector<StaticImportReservationStatus> TryReserveStaticImportTransactionsBatch(
		std::vector<StaticImportBuildBatch>& builds,
		std::vector<StaticImportReservation>& reservations);
	std::vector<StaticImportReservationStatus> TryReserveStaticImportTransactionsInPlace(
		std::span<StaticImportBuildBatch*> builds,
		std::vector<StaticImportReservation>& reservations);
	MaterializedStaticImportTransaction MaterializeStaticImportTransaction(StaticImportReservation reservation) const;
	MaterializedStaticImportTransaction MaterializeStaticImportTransaction(
		StaticImportReservation&& reservation,
		StaticImportBuildBatch& buildScratch) const;
	StaticImportPublishResult PublishStaticImportTransaction(MaterializedStaticImportTransaction transaction);
	StaticImportBulkPublishResult PublishStaticImportTransactionsBulk(std::span<MaterializedStaticImportTransaction*> transactions);
	void CancelStaticImportTransaction(StaticImportReservation reservation, std::uint64_t retireFrame = 0);
	std::vector<Components::ObjectDrawInfo> PublishStaticImportPacket(StaticImportPacket packet);
	std::vector<Components::ObjectDrawInfo> CommitPreparedStaticGroupsBulk(const PreparedStaticGroupsBulkPlan& plan);
	StaticObjectRemovalPayload BuildStaticObjectRemovalPayload(std::span<const Components::ObjectDrawInfo> drawInfos) const;
	void RemoveObject(const Components::ObjectDrawInfo* drawInfo);
	void RemoveObjectsBulk(
		const std::vector<const Components::ObjectDrawInfo*>& drawInfos,
		const RemoveObjectsBulkOptions& options);
	void RemoveObjectsBulk(const std::vector<const Components::ObjectDrawInfo *> &drawInfos);
	void RemoveStaticObjectsBulk(
		std::span<const StaticObjectRemovalPayload> payloads,
		const RemoveObjectsBulkOptions& options);
	void RemoveStaticObjectsBulk(std::span<const StaticObjectRemovalPayload> payloads);
	void UpdatePerObjectBuffer(BufferView*, PerObjectCB& data);
	void UpdateNormalMatrixBuffer(BufferView* view, void* data);
	void PublishDeferredRetireCompletedFrame(std::uint64_t completedFrame, std::uint64_t retireDelayFrames);
	std::uint64_t MakeDeferredRetireFrame() const;
	std::vector<ActiveDrawSetCompactionPublishResult> PublishActiveDrawSetCompactionResults(std::size_t maxResults = 1);
	std::vector<ActiveDrawSetDebugStats> SnapshotActiveDrawSetDebugStats() const;

	rg::runtime::BulkWriteHandle BeginPerObjectBulkWrite();
	void EndPerObjectBulkWrite(size_t dirtyOffset, size_t dirtySize);
	rg::runtime::BulkWriteHandle BeginPerInstanceTransformBulkWrite();
	void EndPerInstanceTransformBulkWrite(size_t dirtyOffset, size_t dirtySize);
	rg::runtime::BulkWriteHandle BeginNormalMatrixBulkWrite();
	void EndNormalMatrixBulkWrite(size_t dirtyOffset, size_t dirtySize);

	std::shared_ptr<DynamicBuffer>& GetPerObjectBuffers() {
		return m_perObjectBuffers;
	}

	std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>>& GetDrawRecordVisibilityGenerationBuffer() {
		return m_drawRecordVisibilityGenerationSidecar;
	}

	std::uint64_t GetResidentInstanceDrawRecordCount() const {
		return m_instanceDrawRecordBuffers
			? m_instanceDrawRecordBuffers->GetBufferSize() / sizeof(InstanceDrawRecordCB)
			: 0u;
	}
	std::uint64_t GetResidentInstanceTransformCount() const {
		return m_perInstanceTransformBuffers
			? m_perInstanceTransformBuffers->GetBufferSize() / sizeof(PerInstanceTransformCB)
			: 0u;
	}
	std::span<const std::uint32_t> GetDrawRecordVisibilityGenerations() const {
		return m_drawRecordVisibilityGenerations;
	}
	std::shared_ptr<DynamicStructuredBuffer<SkinnedAssemblyPlacementGPU>>& GetSkinnedAssemblyPlacements() { return m_skinnedAssemblyPlacements; }
	std::shared_ptr<SortedUnsignedIntBuffer>& GetActiveSkinnedAssemblyPlacements() { return m_activeSkinnedAssemblyPlacements; }

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::shared_ptr<SortedUnsignedIntBuffer> TryGetActiveDrawSetIndices(const DrawWorkloadKey& workloadKey) {
		auto it = m_activeDrawSetIndices.find(workloadKey);
		return it != m_activeDrawSetIndices.end() ? it->second : nullptr;
	}
	std::shared_ptr<SortedUnsignedIntBuffer> GetActiveDrawSetIndices(const DrawWorkloadKey& workloadKey) {
		auto buffer = TryGetActiveDrawSetIndices(workloadKey);
		if (!buffer) {
			throw std::runtime_error("Active draw set indices for given flags not found");
		}
		return buffer;
	}
	std::shared_ptr<SortedUnsignedIntBuffer> GetActiveDrawSetIndices(MaterialCompileFlags flags, const RenderPhase& renderPhase, bool clodOnly = false) {
        return GetActiveDrawSetIndices(DrawWorkloadKey { flags, renderPhase, clodOnly });
    }
    uint64_t GetDrawSetDeclarationRevision() const { return m_drawSetDeclarationRevision; }
	Stats GetStats() const;

private:
	void PublishSkinnedAssemblyPlacements(MaterializedStaticImportTransaction& transaction);
	ObjectManager();

	struct DeferredBufferRangeRetire {
		std::shared_ptr<DynamicBuffer> buffer;
		std::uint64_t offset = 0;
		std::uint64_t size = 0;
		std::uint64_t retireFrame = 0;
	};

	struct ActiveDrawSetCompactionJob {
		DrawWorkloadKey workloadKey;
		std::shared_ptr<SortedUnsignedIntBuffer> buffer;
		std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> entries;
		std::vector<std::uint32_t> visibilityGenerations;
		std::uint64_t activeSetRevision = 0;
		std::uint64_t visibilityRevision = 0;
	};

	struct ActiveDrawSetCompactionResult {
		DrawWorkloadKey workloadKey;
		std::shared_ptr<SortedUnsignedIntBuffer> buffer;
		std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> entries;
		std::uint64_t activeSetRevision = 0;
		std::uint64_t visibilityRevision = 0;
		std::size_t inputEntries = 0;
		std::uint64_t buildUs = 0;
	};

	void StartDeferredRetireWorker();
	void StopDeferredRetireWorker();
	void DeferredRetireWorkerMain();
	void EnqueueDeferredBufferRangeRetire(
		const std::shared_ptr<DynamicBuffer>& buffer,
		std::uint64_t offset,
		std::uint64_t size,
		std::uint64_t retireFrame);
	void EnqueueDeferredBufferRangeRetires(
		const std::shared_ptr<DynamicBuffer>& buffer,
		const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges,
		std::uint64_t retireFrame);
	void EnqueueDeferredBufferRangeRetires(std::vector<DeferredBufferRangeRetire> retires);
	void StartActiveDrawSetCompactionWorker();
	void StopActiveDrawSetCompactionWorker();
	void ActiveDrawSetCompactionWorkerMain();
	void PumpActiveDrawSetCompactionRequests(std::size_t maxRequests);
	void MaybeQueueActiveDrawSetCompaction(
		const DrawWorkloadKey& workloadKey,
		const std::shared_ptr<SortedUnsignedIntBuffer>& buffer);

	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::shared_ptr<DynamicBuffer> m_perObjectBuffers; // Per object constant buffer
	std::shared_ptr<DynamicBuffer> m_perInstanceTransformBuffers; // Per instance transform/object data
	std::shared_ptr<DynamicBuffer> m_instanceDrawRecordBuffers; // Compact draw records consumed by GPU culling
	// Absolute-index sidecar for append-only active draw entries.
	// This is deliberately not a DynamicBuffer allocation pool: draw-record index N
	// must always read generation[N], and backing growth replays the CPU mirror.
	std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> m_drawRecordVisibilityGenerationSidecar;
	std::shared_ptr<DynamicBuffer> m_masterIndirectCommandsBuffer; // Indirect draw command buffer
	std::shared_ptr<DynamicBuffer> m_normalMatrixBuffer; // Normal matrices for each object
	std::unordered_map<DrawWorkloadKey, std::shared_ptr<SortedUnsignedIntBuffer>, DrawWorkloadKey::Hasher> m_activeDrawSetIndices; // Indices into m_drawSetCommandsBuffer for active objects per workload
	std::vector<std::uint32_t> m_drawRecordVisibilityGenerations;
	std::shared_ptr<DynamicStructuredBuffer<SkinnedAssemblyPlacementGPU>> m_skinnedAssemblyPlacements;
	std::shared_ptr<SortedUnsignedIntBuffer> m_activeSkinnedAssemblyPlacements;
	std::vector<SkinnedAssemblyPlacementGPU> m_skinnedAssemblyPlacementCPU;
	std::uint64_t m_drawRecordVisibilityRevision = 1;
	std::uint64_t m_nextStaticImportTransactionID = 1;
	std::shared_ptr<LazyDynamicStructuredBuffer<PerMeshInstanceCB>> m_perMeshInstanceBuffers; // Indices into m_perObjectBuffers for each mesh instance in each object
    uint64_t m_drawSetDeclarationRevision = 1u;
	Stats m_stats{};
	std::mutex m_deferredRetireMutex;
	std::condition_variable m_deferredRetireCv;
	std::deque<DeferredBufferRangeRetire> m_deferredRetireQueue;
	std::thread m_deferredRetireWorker;
	std::atomic_bool m_deferredRetireStop{ false };
	std::atomic<std::uint64_t> m_deferredRetireCompletedFrame{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireDelayFrames{ 4 };
	std::atomic<std::uint64_t> m_deferredRetireRangesQueued{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireRangesRetired{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireBytesQueued{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireBytesRetired{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireQueueDepth{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireWorkerUs{ 0 };
	std::mutex m_activeDrawSetCompactionMutex;
	std::condition_variable m_activeDrawSetCompactionCv;
	std::deque<DrawWorkloadKey> m_activeDrawSetCompactionRequests;
	std::deque<ActiveDrawSetCompactionJob> m_activeDrawSetCompactionJobs;
	std::deque<ActiveDrawSetCompactionResult> m_activeDrawSetCompactionResults;
	std::unordered_set<DrawWorkloadKey, DrawWorkloadKey::Hasher> m_activeDrawSetCompactionQueued;
	std::thread m_activeDrawSetCompactionWorker;
	std::atomic_bool m_activeDrawSetCompactionStop{ false };
	std::mutex m_objectUpdateMutex; // Mutex for thread safety
	std::mutex m_normalMatrixUpdateMutex; // Mutex for thread safety

	std::shared_ptr<SortedUnsignedIntBuffer> EnsureActiveDrawSetIndices(const DrawWorkloadKey& workloadKey, std::size_t initialCapacity = 1);
	std::uint32_t ActivateDrawRecordCPU(std::uint32_t drawRecordIndex);
	std::uint32_t AdvanceDrawRecordVisibilityGenerationCPU(std::uint32_t drawRecordIndex);
	std::uint32_t ActivateDrawRecord(std::uint32_t drawRecordIndex);
	void TombstoneDrawRecord(std::uint32_t drawRecordIndex);
	void TombstoneDrawRecords(std::span<const std::uint32_t> drawRecordIndices);
	void AppendActiveDrawSetEntries(const DrawWorkloadKey& workloadKey, const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>& entries);
	void AssignStaticImportTransactionGenerations(MaterializedStaticImportTransaction& transaction);
	void AssignStaticImportTransactionGenerations(std::span<MaterializedStaticImportTransaction*> transactions);
};
