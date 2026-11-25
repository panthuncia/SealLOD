#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Render/QueueKind.h"

namespace org {
class Buffer;
class ExternalClient;
}

// What CLod streaming uses of the frame graphs: the feedback buffers their passes write. A build that declares Visibility makes
// one (the opaque CLod variant owns streaming) and records it in its FrameBuildOutputs. The renderer attaches it to the
// persistent streaming service after the build (CLodStreamingSystem::AttachGraph), so a build never changes the service the
// attached graph runs with. The old graph's passes keep the old binding's buffers alive until that graph retires.
// The buffers cross epochs: Visibility's CLod passes write them and Shading's (the alpha variant, the streaming tail) write
// and read them. So a Visibility rebuild takes the attached binding's buffers again (CreateSharing) when it can, and
// Shading's graph keeps working with them (SharesBuffersWith).
struct CLodStreamingGraphBinding {
    // Written by the CLod passes each frame and copied back by the streaming readback. Aliased: placed by the graph that
    // registers them.
    std::shared_ptr<org::Buffer> loadRequestKeys;
    std::shared_ptr<org::Buffer> loadRequests;
    std::shared_ptr<org::Buffer> loadCounter;
    std::shared_ptr<org::Buffer> runtimeState;
    std::shared_ptr<org::Buffer> usedGroupsCounter;
    std::shared_ptr<org::Buffer> usedGroupsBuffer;
    std::shared_ptr<org::Buffer> usedGroupsBits;
    std::shared_ptr<org::Buffer> sourceGroupMismatchCounter;
    std::shared_ptr<org::Buffer> sourceGroupMismatchDetails;

    // Scratch of the feedback sort (a radix sort of the load requests by priority).
    struct SortBuffers {
        std::shared_ptr<org::Buffer> keyScratch;
        std::shared_ptr<org::Buffer> payloadScratch;
        std::shared_ptr<org::Buffer> sumTable;
        std::shared_ptr<org::Buffer> reduceTable;
        std::shared_ptr<org::Buffer> constants;
        std::shared_ptr<org::Buffer> countScatterArgs;
        std::shared_ptr<org::Buffer> reduceScanArgs;
    } sort;

    // From the shadow map variant, when the graph has one: the upload slots streaming writes page upgrades into, and the
    // fallback dependency list the readback copies back.
    std::vector<std::shared_ptr<org::Buffer>> virtualShadowUpgradeUploadBuffers;
    std::shared_ptr<org::Buffer> virtualShadowFallbackDependencies;
    std::shared_ptr<org::Buffer> virtualShadowFallbackDependencyCount;

    // The CLOD_BRANCH_* the graph's views take, or 0 to leave the service's as they are.
    uint32_t activeBranches = 0u;

    // Makes the buffers above (unmaterialized; nothing is placed until a graph registers them).
    static std::shared_ptr<CLodStreamingGraphBinding> Create();
    // A binding of a new build over `previous`'s feedback and sort buffers (the same objects); the shadow variant's and the
    // branches are the new build's to set.
    static std::shared_ptr<CLodStreamingGraphBinding> CreateSharing(const CLodStreamingGraphBinding& previous);
    // Whether `other` writes and reads the same feedback buffers (it is this binding or one made by CreateSharing).
    bool SharesBuffersWith(const CLodStreamingGraphBinding& other) const noexcept { return loadRequests && loadRequests == other.loadRequests; }
    bool HasVirtualShadowResources() const noexcept {
        return !virtualShadowUpgradeUploadBuffers.empty() || virtualShadowFallbackDependencies || virtualShadowFallbackDependencyCount;
    }
};

// The streaming tail's after-execution work in one graph (CLodStreamingSystem::GatherStructuralTailPasses): the async uploads
// and the feedback readback, submitted through that graph's external client.
struct CLodStreamingTailWork {
    std::function<void(org::ExternalClient&, org::QueueSlotIndex)> submitAsyncUploads;
    std::function<void(org::ExternalClient&)> submitStreamingReadback;
};
