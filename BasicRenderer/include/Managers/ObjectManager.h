#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <optional>
#include <mutex>
#include <cstdint>
#include <deque>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
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
	};

	struct StaticGroupBuildInfo {
		std::uint64_t stableGroupID = 0;
		std::uint64_t allocationScopeID = 0;
		std::vector<DirectX::XMMATRIX> instanceTransforms;
		std::vector<StaticMeshTemplateRef> meshTemplates;
	};

	struct PreparedStaticGroupInfo {
		std::uint64_t stableGroupID = 0;
		std::uint64_t allocationScopeID = 0;
		std::vector<PerObjectCB> perObjectCBs;
		std::vector<DirectX::XMFLOAT4X4> normalMatrices;
		std::vector<StaticMeshTemplateRef> meshTemplates;
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

	struct RemoveObjectsBulkOptions {
		bool deferBufferRangeRetirement = false;
		std::uint64_t retireFrame = 0;
	};

	Components::ObjectDrawInfo AddObject(const PerObjectCB& perObjectCB, const Components::MeshInstances* meshInstances);
	std::vector<Components::ObjectDrawInfo> AddObjectsBulk(const std::vector<ObjectBuildInfo>& objects);
	std::vector<Components::ObjectDrawInfo> AddStaticGroupsBulk(const std::vector<StaticGroupBuildInfo>& groups);
	static PreparedStaticGroupsBulkPlan PrepareStaticGroupsBulkPlan(const std::vector<StaticGroupBuildInfo>& groups);
	void PrepareStaticGroupCommitResourcesAsync(const PreparedStaticGroupsBulkPlan& plan);
	void PublishPreparedStaticGroupCommitResourceResizes(bool wait = false);
	std::vector<Components::ObjectDrawInfo> CommitPreparedStaticGroupsBulk(const PreparedStaticGroupsBulkPlan& plan);
	void RemoveObject(const Components::ObjectDrawInfo* drawInfo);
	void RemoveObjectsBulk(
		const std::vector<const Components::ObjectDrawInfo*>& drawInfos,
		const RemoveObjectsBulkOptions& options = {});
	void UpdatePerObjectBuffer(BufferView*, PerObjectCB& data);
	void UpdateNormalMatrixBuffer(BufferView* view, void* data);
	void PublishDeferredRetireCompletedFrame(std::uint64_t completedFrame, std::uint64_t retireDelayFrames);
	std::uint64_t MakeDeferredRetireFrame() const;

	rg::runtime::BulkWriteHandle BeginPerObjectBulkWrite();
	void EndPerObjectBulkWrite(size_t dirtyOffset, size_t dirtySize);
	rg::runtime::BulkWriteHandle BeginPerInstanceTransformBulkWrite();
	void EndPerInstanceTransformBulkWrite(size_t dirtyOffset, size_t dirtySize);
	rg::runtime::BulkWriteHandle BeginNormalMatrixBulkWrite();
	void EndNormalMatrixBulkWrite(size_t dirtyOffset, size_t dirtySize);

	std::shared_ptr<DynamicBuffer>& GetPerObjectBuffers() {
		return m_perObjectBuffers;
	}

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
	ObjectManager();

	struct DeferredBufferRangeRetire {
		std::shared_ptr<DynamicBuffer> buffer;
		std::uint64_t offset = 0;
		std::uint64_t size = 0;
		std::uint64_t retireFrame = 0;
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

	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::shared_ptr<DynamicBuffer> m_perObjectBuffers; // Per object constant buffer
	std::shared_ptr<DynamicBuffer> m_perInstanceTransformBuffers; // Per instance transform/object data
	std::shared_ptr<DynamicBuffer> m_instanceDrawRecordBuffers; // Compact draw records consumed by GPU culling
	std::shared_ptr<DynamicBuffer> m_masterIndirectCommandsBuffer; // Indirect draw command buffer
	std::shared_ptr<DynamicBuffer> m_normalMatrixBuffer; // Normal matrices for each object
	std::unordered_map<DrawWorkloadKey, std::shared_ptr<SortedUnsignedIntBuffer>, DrawWorkloadKey::Hasher> m_activeDrawSetIndices; // Indices into m_drawSetCommandsBuffer for active objects per workload
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
	std::mutex m_objectUpdateMutex; // Mutex for thread safety
	std::mutex m_normalMatrixUpdateMutex; // Mutex for thread safety

	std::shared_ptr<SortedUnsignedIntBuffer> EnsureActiveDrawSetIndices(const DrawWorkloadKey& workloadKey, std::size_t initialCapacity = 1);
};
