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

// What CLod streaming uses of one frame graph: the feedback buffers its passes write, the queue its uploads go on, and
// how it submits its after-frame work through the graph. A build makes one (the opaque CLod variant, in its gather) and
// records it in its FrameBuildOutputs. The renderer attaches it to the persistent streaming service after the build
// (CLodStreamingSystem::AttachGraph), so a build never changes the service the attached graph runs with. The old graph's
// passes keep the old binding's buffers alive until that graph retires.
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

    // The async upload queue in this graph's queue registry. It is made in the opaque variant's Initialize, when the
    // registry has its primary queues.
    org::QueueSlotIndex uploadQueueSlot{};

    // Set by the gathers: submit the async uploads (on the upload queue) and the feedback readback through the graph's
    // external client, after each frame.
    std::function<void(org::ExternalClient&, org::QueueSlotIndex)> submitAsyncUploads;
    std::function<void(org::ExternalClient&)> submitStreamingReadback;

    // From the shadow map variant, when the graph has one: the upload slots streaming writes page upgrades into, and the
    // fallback dependency list the readback copies back.
    std::vector<std::shared_ptr<org::Buffer>> virtualShadowUpgradeUploadBuffers;
    std::shared_ptr<org::Buffer> virtualShadowFallbackDependencies;
    std::shared_ptr<org::Buffer> virtualShadowFallbackDependencyCount;

    // The CLOD_BRANCH_* the graph's views take, or 0 to leave the service's as they are.
    uint32_t activeBranches = 0u;

    // Makes the buffers above (unmaterialized; nothing is placed until a graph registers them).
    static std::shared_ptr<CLodStreamingGraphBinding> Create();

    void SubmitExternalWork(org::ExternalClient& client) const {
        if (submitAsyncUploads) submitAsyncUploads(client, uploadQueueSlot);
        if (submitStreamingReadback) submitStreamingReadback(client);
    }
};
