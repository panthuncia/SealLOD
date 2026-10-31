#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <vector>

#include <rhi.h>

#include "Resources/PixelBuffer.h"

namespace org { class ExternalClient; }

namespace br {

// Saves textures to DDS files. Requests are queued with RequestReadback. The renderer calls Submit once per frame,
// after the frame graph executes, to copy the queued textures into readback buffers. ProcessReadbackRequests then
// saves each one once its copy has finished on the GPU.
class ReadbackManager {
public:
    ReadbackManager();

    void Initialize(rhi::Timeline readbackFence);

    void RequestReadback(std::shared_ptr<org::PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback, bool cubemap);

    // Copies every queued texture into a readback buffer, as one submission on `queueSlot` of `client`.
    // Call it after the frame graph's execution, so the copies read what the frame wrote.
    void Submit(org::ExternalClient& client, uint32_t queueSlot);

    void ProcessReadbackRequests();

    void Cleanup();

private:
    struct ReadbackInfo {
        bool cubemap = false;
        std::shared_ptr<org::PixelBuffer> texture;
        std::wstring outputFile;
        std::function<void()> callback;
    };

    struct ReadbackRequest {
        std::shared_ptr<org::Resource> readbackBuffer;
        std::vector<rhi::CopyableFootprint> layouts;
        uint64_t totalSize = 0;
        std::wstring outputFile;
        std::function<void()> callback;
        uint64_t fenceValue = 0;
    };

    struct State {
        std::mutex mutex;
        std::vector<ReadbackInfo> queuedReadbacks;
        std::vector<ReadbackRequest> readbackRequests;
        std::atomic<uint64_t> nextFenceValue{ 0 };
        bool accepting = true;
    };

    std::shared_ptr<State> m_state;
    rhi::Timeline m_readbackFence;
};

} // namespace br
