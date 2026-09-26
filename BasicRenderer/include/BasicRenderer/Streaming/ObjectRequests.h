#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include "BasicRenderer/Extensions/Buffers/DynamicBuffer.h"
#include "BasicRenderer/Extensions/Buffers/SortedUnsignedIntBuffer.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"
#include <BasicRenderer/Pipeline/DrawWorkload.h>
#include <BasicRenderer/Streaming/ArtifactTypes.h>

class Material;
class Mesh;

namespace br::render {

using DesiredBufferStateReadyCallback = std::function<void()>;

struct ObjectBuildInfo {
	PerObjectCB perObjectCB{};
	const Components::MeshInstances* meshInstances = nullptr;
	const Components::InstanceTransforms* instanceTransforms = nullptr;
};

struct StaticMeshTemplateRef {
	std::uint32_t meshTemplateIndex = 0;
	std::uint32_t clodOffsetIndex = 0;
	std::uint64_t meshIdentity = 0;
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
	std::uint64_t meshIdentity = 0;
	std::vector<DrawWorkloadKey> workloadKeys;
	std::span<const DrawWorkloadKey> mappedWorkloadKeys;
	std::uint32_t skinnedAssemblyTypeSlot = 0xFFFFFFFFu;
	BoundingSphere skinnedAssemblyBounds{};
	float skinnedBoundsScale = 1.0f;
	[[nodiscard]] std::span<const DrawWorkloadKey> WorkloadKeys() const {
		return mappedWorkloadKeys.empty() ? std::span<const DrawWorkloadKey>{ workloadKeys } : mappedWorkloadKeys;
	}
};

struct PreparedStaticGroupInfo {
	struct WorkloadRouteRange {
		std::uint32_t first = 0;
		std::uint32_t count = 0;
	};
	std::uint64_t stableGroupID = 0;
	std::uint64_t allocationScopeID = 0;
	std::vector<PerObjectCB> perObjectCBs;
	std::vector<DirectX::XMFLOAT4X4> normalMatrices;
	std::vector<PreparedStaticMeshTemplateRef> meshTemplates;
	std::vector<std::vector<DrawWorkloadKey>> workloadKeysByMeshTemplate;
	// Immutable compact routing: each template range contains indices into
	// uniqueWorkloadKeys. Materialization binds each unique key once instead
	// of hashing and deduplicating every template occurrence again.
	std::vector<DrawWorkloadKey> uniqueWorkloadKeys;
	std::vector<std::uint32_t> workloadRouteIndices;
	std::vector<WorkloadRouteRange> workloadRouteRanges;
	std::vector<std::uint32_t> workloadRouteOccurrences;
	// Scatter/gather publication batches retain the source artifact and map
	// these immutable route tables instead of copying four vectors per group.
	std::span<const DrawWorkloadKey> mappedUniqueWorkloadKeys;
	std::span<const std::uint32_t> mappedWorkloadRouteIndices;
	std::span<const WorkloadRouteRange> mappedWorkloadRouteRanges;
	std::span<const std::uint32_t> mappedWorkloadRouteOccurrences;
	// Recipe-backed groups keep immutable transform rows in their mapped pack.
	// Runtime bindings remain owning because their indices are renderer-assigned.
	std::span<const PerObjectCB> mappedPerObjectCBs;
	std::span<const DirectX::XMFLOAT4X4> mappedNormalMatrices;
	std::span<const PreparedStaticMeshTemplateRef> mappedMeshTemplates;
	std::shared_ptr<const void> mappedRecipeOwner;
	std::shared_ptr<const void> mappedTemplateOwner;
	// Mapping storage and recipe publication semantics are independent. Static
	// import scatter/gather batches also map immutable rows, but still require
	// legacy draw-info/removal payload construction.
	bool mappedRecipeSemantics = false;

	[[nodiscard]] std::span<const PerObjectCB> PerObjectRows() const {
		return mappedPerObjectCBs.empty() ? std::span<const PerObjectCB>{ perObjectCBs } : mappedPerObjectCBs;
	}
	[[nodiscard]] std::span<const DirectX::XMFLOAT4X4> NormalRows() const {
		return mappedNormalMatrices.empty() ? std::span<const DirectX::XMFLOAT4X4>{ normalMatrices } : mappedNormalMatrices;
	}
	[[nodiscard]] std::span<const PreparedStaticMeshTemplateRef> MeshTemplates() const {
		return mappedMeshTemplates.empty() ? std::span<const PreparedStaticMeshTemplateRef>{ meshTemplates } : mappedMeshTemplates;
	}
	[[nodiscard]] std::span<const DrawWorkloadKey> UniqueWorkloadKeys() const {
		return mappedUniqueWorkloadKeys.empty() ? std::span<const DrawWorkloadKey>{ uniqueWorkloadKeys } : mappedUniqueWorkloadKeys;
	}
	[[nodiscard]] std::span<const std::uint32_t> WorkloadRouteIndices() const {
		return mappedWorkloadRouteIndices.empty() ? std::span<const std::uint32_t>{ workloadRouteIndices } : mappedWorkloadRouteIndices;
	}
	[[nodiscard]] std::span<const WorkloadRouteRange> WorkloadRouteRanges() const {
		return mappedWorkloadRouteRanges.empty() ? std::span<const WorkloadRouteRange>{ workloadRouteRanges } : mappedWorkloadRouteRanges;
	}
	[[nodiscard]] std::span<const std::uint32_t> WorkloadRouteOccurrences() const {
		return mappedWorkloadRouteOccurrences.empty() ? std::span<const std::uint32_t>{ workloadRouteOccurrences } : mappedWorkloadRouteOccurrences;
	}
	[[nodiscard]] bool IsRecipeView() const { return mappedRecipeSemantics; }
};

struct StaticRecipeTemplateBinding {
	PreparedStaticMeshTemplateRef rendererTemplate;
};

struct StaticRecipeView {
	std::vector<PreparedStaticGroupInfo> groups;
	std::uint64_t transformRows = 0;
	std::uint64_t drawRecords = 0;
	std::uint64_t mappedBytes = 0;
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

struct ObjectStorageStats {
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
	std::vector<org::DynamicBuffer::PagedAllocation> perObjectPages;
	std::vector<org::DynamicBuffer::PagedAllocation> instanceTransformPages;
	std::vector<org::DynamicBuffer::PagedAllocation> normalMatrixPages;
	std::vector<org::DynamicBuffer::PagedAllocation> instanceDrawRecordPages;
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
		std::uint64_t meshIdentity = 0;
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
	// Transaction-wide workload slots and each group's local-to-transaction
	// mapping. Materialization can bind vector destinations directly instead
	// of hashing every workload key for every group.
	std::vector<DrawWorkloadKey> activeWorkloadKeys;
	std::vector<std::vector<std::uint32_t>> activeWorkloadRoutesByGroup;
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
	org::DynamicBuffer::AllocationProbe normalMatrix;
	org::DynamicBuffer::AllocationProbe perObject;
	org::DynamicBuffer::AllocationProbe instanceTransform;
	org::DynamicBuffer::AllocationProbe instanceDrawRecord;
};

struct StaticImportReservation {
	std::uint64_t id = 0;
	StaticImportBuildBatch build;
	std::vector<std::size_t> transformCounts;
	std::vector<std::size_t> drawRecordCounts;
	std::vector<org::DynamicBuffer::PagedAllocation> perObjectRanges;
	std::vector<org::DynamicBuffer::PagedAllocation> instanceTransformRanges;
	std::vector<org::DynamicBuffer::PagedAllocation> normalMatrixRanges;
	std::vector<org::DynamicBuffer::PagedAllocation> instanceDrawRecordRanges;
	std::size_t visibilityDirtyStart = std::numeric_limits<std::size_t>::max();
	std::size_t visibilityDirtyEnd = 0;
	std::shared_ptr<org::DynamicBuffer> perObjectBuffer;
	std::shared_ptr<org::DynamicBuffer> instanceTransformBuffer;
	std::shared_ptr<org::DynamicBuffer> normalMatrixBuffer;
	std::shared_ptr<org::DynamicBuffer> instanceDrawRecordBuffer;
	std::uint64_t groupCount = 0;
	std::uint64_t preparedBytes = 0;
	std::uint64_t drawRecords = 0;
};

struct RemoveObjectsBulkOptions {
	bool deferBufferRangeRetirement = false;
	bool retireInstanceDrawRecordRanges = true;
	std::uint64_t retireFrame = 0;
};

struct StaticObjectRemovalResult {
	std::uint64_t mutationCoverageGeneration = 0;
};

struct StaticObjectRemovalPayload {
	enum class BufferKind : std::uint8_t {
		PerObject,
		InstanceTransform,
		InstanceDrawRecord,
		NormalMatrix
	};

	struct BufferRetireRange {
		std::shared_ptr<org::DynamicBuffer> buffer;
		Components::ObjectDrawInfo::BufferRange range;
		BufferKind kind = BufferKind::PerObject;
	};
	struct ActiveDrawSetRemovalRange {
		std::uint32_t workloadSlot = UINT32_MAX;
		std::uint32_t firstIndex = 0;
		std::uint32_t indexCount = 0;
	};
	struct ActiveDrawSetRemovalStorage {
		// Workload identity is transaction-wide. Intern it once instead of
		// copying RenderPhase strings into every per-group removal range.
		std::vector<DrawWorkloadKey> workloadKeys;
		std::vector<ActiveDrawSetRemovalRange> ranges;
		std::unique_ptr<std::uint32_t[]> indices;
		std::size_t indexCapacity = 0;
		std::size_t nextIndex = 0;
	};

	std::array<BufferRetireRange, 4> inlineBufferRanges;
	std::uint8_t inlineBufferRangeCount = 0;
	std::vector<BufferRetireRange> bufferRanges;
	std::vector<Components::ObjectDrawInfo::ActiveDrawSetRemovalBucket> activeDrawSetRemovals;
	std::shared_ptr<const ActiveDrawSetRemovalStorage> sharedActiveDrawSetRemovals;
	std::uint32_t firstActiveDrawSetRemovalRange = 0;
	std::uint32_t activeDrawSetRemovalRangeCount = 0;
	std::vector<std::uint32_t> drawRecordIndices;
	std::vector<std::uint32_t> skinnedAssemblyPlacementIndices;
	std::size_t drawInfoCount = 0;
};

struct StaticObjectResidencyHandle {
	StaticObjectRemovalPayload destructionPayload;
	bool visible = true;
};

using StaticVisibilityUpdateResult = std::unordered_map<
	DrawWorkloadKey,
	std::uint32_t,
	DrawWorkloadKey::Hasher>;

struct MaterializedStaticImportTransaction {
	struct PendingSkinnedAssemblyPlacement {
		std::size_t groupIndex = 0;
		SkinnedAssemblyPlacementGPU placement{};
		std::vector<std::size_t> drawRecordRowIndices;
	};
	StaticImportReservation reservation;
	std::vector<PerObjectCB> perObjectRows;
	std::vector<DirectX::XMFLOAT4X4> normalRows;
	std::vector<InstanceDrawRecordCB> drawRecordRows;
	std::vector<Components::ObjectDrawInfo> drawInfos;
	std::vector<std::uint32_t> drawInfoIndicesByGroup;
	std::vector<StaticObjectRemovalPayload> removalPayloads;
	std::shared_ptr<StaticObjectRemovalPayload::ActiveDrawSetRemovalStorage> activeDrawSetRemovalStorage;
	std::vector<PendingSkinnedAssemblyPlacement> skinnedAssemblyPlacements;
	std::unordered_map<DrawWorkloadKey, std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>, DrawWorkloadKey::Hasher> activeDrawSetInserts;
	std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher> activeDrawSetSpans;
	std::uint64_t materializeUs = 0;
	bool transformRowsStaged = false;
	bool drawRecordRowsStaged = false;
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
	// Logical mutation epoch covered by this bulk publication. Physical journal
	// capture is deferred until the DrawRecords root is admissible; a graph cut
	// is compatible only when it covers at least this generation.
	std::uint64_t mutationCoverageGeneration = 0;
};

struct DesiredObjectBufferStateCut {
	br::render::ArtifactVersionHandle version;
	std::uint64_t coveredMutationGeneration = 0;
	// Exact buffer versions the draw-records root pairs. Work that needs the
	// uploaded object data waits on these: the root itself additionally waits
	// for resident geometry, which static scene publication must not depend on.
	std::vector<br::render::ArtifactVersionHandle> bufferVersions;

	explicit operator bool() const noexcept { return static_cast<bool>(version); }
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

} // namespace br::render
