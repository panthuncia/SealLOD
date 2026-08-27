#pragma once
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <span>
#include <atomic>

#include "Scene/Components.h"
#include "Materials/TechniqueDescriptor.h"
#include "Render/IndirectStateArtifacts.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Resources/Buffers/SortedUnsignedIntBuffer.h"

namespace org { class DynamicGloballyIndexedResource; }
using org::DynamicGloballyIndexedResource;
namespace org { class ResourceGroup; }
using org::ResourceGroup;
class ObjectManager;
class MaterialManager;
class SortedUnsignedIntBuffer;
namespace org::runtime { class IUploadService; }
namespace br::render { class RendererStateRequestService; }
namespace br::render { struct PublishedRendererState; }

struct RenderPhase; // forward

// Hash for MaterialCompileFlags
struct MaterialCompileFlagsHash {
    size_t operator()(MaterialCompileFlags f) const noexcept {
        return std::hash<uint64_t>()(static_cast<uint64_t>(f));
    }
};

struct WorkloadCountUpdate {
    DrawWorkloadKey workloadKey;
    unsigned int count = 0;
};

class IndirectCommandBufferManager {
public:
    ~IndirectCommandBufferManager();

    static std::unique_ptr<IndirectCommandBufferManager> CreateUnique() {
        return std::unique_ptr<IndirectCommandBufferManager>(new IndirectCommandBufferManager());
    }
    static std::shared_ptr<IndirectCommandBufferManager> CreateShared() {
        return std::shared_ptr<IndirectCommandBufferManager>(new IndirectCommandBufferManager());
    }

    // Tell the manager about a draw workload once. This builds the inverted index:
    // RenderPhase -> [workloadKey].
    void RegisterWorkload(const DrawWorkloadKey& workloadKey);

    // Ensure we have buffers for all known workloads for this view.
    void CreateBuffersForView(uint64_t viewID);

    // Remove buffers associated with a view
    void UnregisterBuffers(uint64_t viewID);

    // Update the buffer associated with the workload to accommodate numDraws.
    // Rounds up to increment size. Triggers per-view reallocation for that workload,
    // and resizes meshlet buffers (sum of all flags sizes).
    void UpdateBuffersForWorkload(const DrawWorkloadKey& workloadKey, unsigned int numDraws);
    void UpdateBuffersForWorkloads(std::span<const WorkloadCountUpdate> updates);
    void RequestWorkloadCount(const DrawWorkloadKey& workloadKey, unsigned int numDraws);
    void RequestWorkloadCounts(std::span<const WorkloadCountUpdate> updates);
    void PublishDesiredState(ObjectManager& objectManager, MaterialManager& materialManager);
    void SetRendererStateServices(br::render::RendererStateRequestService* requests,
        org::runtime::IUploadService* uploads);

    // Set growth granularity
    void SetIncrementSize(unsigned int incrementSize);

private:
    IndirectCommandBufferManager();

    struct ActiveJournal {
        std::uint64_t revision = 0;
        std::shared_ptr<const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>> base;
        std::vector<std::shared_ptr<const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>>> appends;
    };
    struct DesiredSnapshot {
        std::uint64_t revision = 0;
        std::uint64_t activeMaterialRevision = 0;
        std::optional<br::render::ArtifactRequirement> objectBufferRequirement;
        std::uint64_t residentDrawRecordCount = 0;
        unsigned int incrementSize = 1000;
        std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> requestedCounts;
        std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> capacities;
        std::unordered_map<DrawWorkloadKey, std::uint64_t, DrawWorkloadKey::Hasher> workloadIDs;
        std::unordered_map<DrawWorkloadKey, ActiveJournal, DrawWorkloadKey::Hasher> activeJournals;
        std::unordered_set<std::uint64_t> viewIDs;
        std::unordered_map<std::uint64_t, std::uint64_t> viewLifetimeRevisions;
    };

    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToCapacity;
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToRequestedCount;
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToPublishedCount;
    std::unordered_map<DrawWorkloadKey, std::uint64_t, DrawWorkloadKey::Hasher> m_workloadIDs;
    std::uint64_t m_nextWorkloadID = 1;
    std::unordered_set<std::uint64_t> m_viewIDs;
    std::unordered_map<std::uint64_t, std::uint64_t> m_viewLifetimeRevisions;
    unsigned int m_incrementSize = 1000;
    br::render::RendererStateRequestService* m_rendererStateRequests = nullptr;
    org::runtime::IUploadService* m_uploadService = nullptr;
    std::unordered_map<DrawWorkloadKey, ActiveJournal, DrawWorkloadKey::Hasher> m_activeJournals;
    mutable std::mutex m_desiredMutex;
    TaskScope m_buildScope;
    std::atomic_bool m_buildScheduled{ false };
    std::atomic_bool m_stopping{ false };
    bool m_activeObserverInstalled = false;
    ObjectManager* m_observedObjectManager = nullptr;
    std::uint64_t m_desiredMutationRevision = 1;
    std::uint64_t m_consumedMutationRevision = 0;
    std::uint64_t m_lastObjectBufferRevision = 0;
    std::optional<br::render::ArtifactRequirement> m_objectBufferRequirement;
    std::uint64_t m_lastMaterialRevision = 0;
    std::uint64_t m_lastResidentDrawRecordCount = 0;

    void OnActiveDrawSetMutation(const DrawWorkloadKey& workloadKey, bool replace,
        std::uint64_t revision,
        std::shared_ptr<const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>> entries);
    void ScheduleDesiredBuild();
    void DrainDesiredBuild(const br::TaskContext& context);
    void BuildDesiredState(DesiredSnapshot snapshot);
    [[nodiscard]] DesiredSnapshot CaptureDesiredSnapshotLocked() const;
    void EnsureWorkloadRegistered(const DrawWorkloadKey& workloadKey);
};
