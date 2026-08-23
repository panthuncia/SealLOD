#pragma once
#include <vector>
#include <unordered_map>
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

struct RenderPhase; // forward

// Hash for MaterialCompileFlags
struct MaterialCompileFlagsHash {
    size_t operator()(MaterialCompileFlags f) const noexcept {
        return std::hash<uint64_t>()(static_cast<uint64_t>(f));
    }
};

struct IndirectWorkload {
    std::shared_ptr<DynamicGloballyIndexedResource> buffer;
    unsigned int count = 0;
    unsigned int activeDrawCount = 0;
    std::shared_ptr<SortedUnsignedIntBuffer> activeDrawSetIndices;
};

struct IndirectBufferEntry {
    uint64_t viewID;
    DrawWorkloadKey key;
    IndirectWorkload workload;
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
    void CommitGpuVisibleSnapshot(ObjectManager& objectManager);

    // Set growth granularity
    void SetIncrementSize(unsigned int incrementSize);

    // Query: which (per-view) indirect command buffers participate in a render pass?
    // Order is unspecified; returns empty if none registered.
    std::vector<std::pair<MaterialCompileFlags, IndirectWorkload>>
        GetBuffersForRenderPhase(uint64_t viewID, const RenderPhase& phase, bool clodOnly = false) const;

    // per-view version of phase query, but returning viewID too
    std::vector<IndirectBufferEntry> GetViewIndirectBuffersForRenderPhase(uint64_t viewID, const RenderPhase& phase, bool clodOnly = false) const;

	// Iterate over all indirect buffers (all views, all flags):
    template<class F>
    void ForEachIndirectBuffer(F&& f) const {
        for (auto const& [viewID, perView] : m_viewIDToBuffers) {
            for (auto const& [key, wl] : perView.buffersByWorkload) {
                std::forward<F>(f)(viewID, key, wl);
            }
        }
    }

private:
    IndirectCommandBufferManager();

    // Per-view buffer set
    struct PerViewBuffers {
        // One buffer per unique draw workload
        std::unordered_map<DrawWorkloadKey,
            IndirectWorkload,
            DrawWorkloadKey::Hasher> buffersByWorkload;
    };

    // Per-workload published capacity (rounded to increment)
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToCapacity;

    // Per-workload requested and published draw count (unrounded)
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToRequestedCount;
    std::unordered_map<DrawWorkloadKey, unsigned int, DrawWorkloadKey::Hasher> m_workloadToPublishedCount;

    // Single group that owns all indirect command buffers (regardless of flags)
    std::shared_ptr<ResourceGroup> m_indirectCommandsResourceGroup;

    // ViewID -> buffers
    std::unordered_map<uint64_t, PerViewBuffers> m_viewIDToBuffers;

    // Growth granularity
    unsigned int m_incrementSize = 1000;

    // Helpers
    unsigned int RoundUp(unsigned int x) const {
        return ((x + m_incrementSize - 1) / m_incrementSize) * m_incrementSize;
    }
    void EnsureWorkloadRegistered(const DrawWorkloadKey& workloadKey);
};
