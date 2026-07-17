#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Resources/Buffers/DynamicBuffer.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "ShaderBuffers.h"

class Skeleton; // base skeleton asset or instance
class BufferView;

class SkeletonManager : public IResourceProvider {
public:
	struct TransientWindRegion {
		uint32_t transformBaseMatrices = 0;
		uint32_t previousTransformBaseMatrices = 0;
		uint32_t inverseSkinBaseMatrices = 0;
		uint32_t capacityMatrices = 0;
		bool valid = false;
	};
	struct ActiveInstanceView {
		Skeleton* skeleton = nullptr;
		uint32_t instanceSlot = 0xFFFFFFFFu;
		uint32_t transformOffsetMatrices = 0u;
		uint32_t inverseSkinOffsetMatrices = 0u;
		uint32_t boneCount = 0u;
	};

    static std::unique_ptr<SkeletonManager> CreateUnique() {
        return std::unique_ptr<SkeletonManager>(new SkeletonManager());
    }
    ~SkeletonManager();

    // Called when a renderable becomes active/inactive and references a skinning instance.
    // Multiple renderables may call Acquire/Release for the same instance.
    uint32_t AcquireSkinningInstance(const std::shared_ptr<Skeleton>& skinningInstance);
    void     ReleaseSkinningInstance(Skeleton* skinningInstance);
    std::weak_ptr<std::atomic_bool> GetLifetimeToken() const noexcept { return m_lifetimeToken; }

    // Tick animations for all active skeletons
    void TickAnimations(float elapsedSeconds);
	// Advances palette history once per rendered frame. CPU-driven instances
	// collapse previous to current until a new pose is uploaded; procedural wind
	// flips between its two fixed-capacity matrix regions.
	void BeginFrame(uint64_t frameNumber);

    // Upload pose for a specific instance (or call UpdateAllDirtyInstances once per frame).
    void UpdateInstanceTransforms(Skeleton& skinningInstance);
    void UpdateAllDirtyInstances();
	std::vector<ActiveInstanceView> GetActiveInstanceViews() const;
	uint64_t GetActiveInstanceRevision() const noexcept { return m_activeInstanceRevision; }
	TransientWindRegion ReserveTransientWindRegion(uint32_t matrixCapacity);
	void EnsureTransientWindInstanceSlots(uint32_t drawRecordCapacity);

    // IResourceProvider
    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
    std::vector<ResourceIdentifier> GetSupportedKeys() override;

private:
    SkeletonManager();

    struct BaseRecord {
        std::unique_ptr<BufferView> invBindView;
        uint32_t boneCount = 0;
        uint32_t refCount = 0;
        // cached matrix offset (index, not bytes)
        uint32_t invBindOffsetMatrices = 0;
    };

    struct InstanceRecord {
        std::unique_ptr<BufferView> transformsView;
        std::unique_ptr<BufferView> inverseSkinView;
        uint32_t boneCount = 0;
        uint32_t refCount = 0;

        uint32_t instanceSlot = 0xFFFFFFFF;
        bool dirty = true;

        const Skeleton* base = nullptr;

        uint32_t transformOffsetMatrices = 0;
		uint32_t previousTransformOffsetMatrices = 0;
		uint32_t transformOffsetsMatrices[2]{};
		uint32_t currentTransformIndex = 0;
		bool hasTransformHistory = false;
        uint32_t invBindOffsetMatrices = 0;
        uint32_t inverseSkinOffsetMatrices = 0;
    };

private:
    void RebuildIterationList();

    // Global packed buffers
    std::shared_ptr<DynamicBuffer> m_inverseBindMatrices;  // float4x4[]
    std::shared_ptr<DynamicBuffer> m_boneTransforms;       // float4x4[]
    std::shared_ptr<DynamicBuffer> m_inverseSkinMatrices;  // float4x4[]
	std::shared_ptr<DynamicStructuredBuffer<SkinningInstanceGPUInfo>> m_instanceInfo; // slot -> offsets/count
	std::unique_ptr<BufferView> m_transientWindTransformsView;
	std::unique_ptr<BufferView> m_transientWindInverseSkinView;
	TransientWindRegion m_transientWindRegion{};
	uint32_t m_transientWindAllocationBaseMatrices = 0;
	uint64_t m_lastBegunFrame = std::numeric_limits<uint64_t>::max();
	uint64_t m_activeInstanceRevision = 0;

    // Resource provider map
    std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
    std::shared_ptr<std::atomic_bool> m_lifetimeToken;

    // Records
    std::unordered_map<const Skeleton*, BaseRecord>    m_bases;
    std::unordered_map<const Skeleton*, InstanceRecord> m_instances;

    // Flat iteration list for parallel-for access
    struct InstanceEntry {
        Skeleton* skeleton;
        InstanceRecord* record;
    };
    std::vector<InstanceEntry> m_iterationList;
    bool m_iterationListDirty = true;

    // Free-list for instance slots
    std::vector<uint32_t> m_freeInstanceSlots;
    uint32_t m_slotsUsed = 0;

private:
    BaseRecord& AcquireBase(const std::shared_ptr<Skeleton>& baseSkeleton);
    void           ReleaseBase(const Skeleton* baseSkeleton);

    uint32_t       AllocateInstanceSlot();
    void           FreeInstanceSlot(uint32_t slot);

    static constexpr uint32_t kInvalidSlot = 0xFFFFFFFF;
};
