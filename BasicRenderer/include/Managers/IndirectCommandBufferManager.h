#pragma once
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <span>

#include "Scene/Components.h"
#include "Materials/TechniqueDescriptor.h"

namespace org { class DynamicGloballyIndexedResource; }
using org::DynamicGloballyIndexedResource;
namespace org { class ResourceGroup; }
using org::ResourceGroup;
class ObjectManager;
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
    void PublishDesiredState(ObjectManager& objectManager);
    void SetRendererStateServices(br::render::RendererStateRequestService* requests,
        org::runtime::IUploadService* uploads);

    // Set growth granularity
    void SetIncrementSize(unsigned int incrementSize);

private:
    IndirectCommandBufferManager();

    // Per-view buffer set
    // Per-workload published capacity (rounded to increment)
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToCapacity;

    // Per-workload requested and published draw count (unrounded)
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToRequestedCount;
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToPublishedCount;

    std::unordered_set<std::uint64_t> m_viewIDs;

    // Growth granularity
    unsigned int m_incrementSize = 1000;
    br::render::RendererStateRequestService* m_rendererStateRequests = nullptr;
    org::runtime::IUploadService* m_uploadService = nullptr;
    std::uint64_t m_graphInputFingerprint = 0;
    std::uint64_t m_graphPendingFingerprint = 0;
    std::uint32_t m_graphStableTicks = 0;
    std::uint64_t m_graphRevision = 0;
    std::uint32_t m_graphDiagnosticTicks = 0;

    // Helpers
    unsigned int RoundUp(unsigned int x) const {
        return ((x + m_incrementSize - 1) / m_incrementSize) * m_incrementSize;
    }
    void EnsureWorkloadRegistered(const DrawWorkloadKey& workloadKey);
};
