#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <vector>

#include <rhi.h>

#include "OpenRenderGraph/OpenRenderGraph.h"

namespace br {

class ReadbackManager {
public:
    ReadbackManager();

    void Initialize(rhi::Timeline readbackFence);

    void RequestReadback(std::shared_ptr<PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback, bool cubemap);

    std::shared_ptr<RenderPass> GetReadbackPass() const { return m_readbackPass; }

    void ProcessReadbackRequests();

    void Cleanup();

private:
    struct ReadbackInfo {
        bool cubemap = false;
        std::shared_ptr<PixelBuffer> texture;
        std::wstring outputFile;
        std::function<void()> callback;
    };

    struct ReadbackRequest {
        std::shared_ptr<Resource> readbackBuffer;
        std::vector<rhi::CopyableFootprint> layouts;
        uint64_t totalSize = 0;
        std::wstring outputFile;
        std::function<void()> callback;
        uint64_t fenceValue = 0;
    };

    class ReadbackPass : public RenderPass, public IHasImmediateModeCommands {
    public:
        explicit ReadbackPass(ReadbackManager& owner)
            : m_owner(owner) {
        }

        void Setup() override {
        }

        void RecordImmediateCommands(ImmediateExecutionContext& context) override;

        PassReturn Execute(PassExecutionContext& context) override;

        void Cleanup() override {
        }

        void SetReadbackFence(rhi::Timeline fence) {
            m_readbackFence = fence;
        }

    private:
        ReadbackManager& m_owner;
        rhi::Timeline m_readbackFence;
        uint64_t m_pendingFenceValue = 0;
        bool m_hasWork = false;
    };

    uint64_t AcquireNextFenceValue() noexcept {
        return m_nextFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    void ClearReadbacks();

    void SaveCubemapToDDS(
        rhi::Device& device,
        org::imm::ImmediateCommandList& commandList,
        std::shared_ptr<PixelBuffer> cubemap,
        const std::wstring& outputFile,
        uint64_t fenceValue);

    void SaveTextureToDDS(
        rhi::Device& device,
        org::imm::ImmediateCommandList& commandList,
        PixelBuffer* texture,
        const std::wstring& outputFile,
        uint64_t fenceValue);

    std::shared_ptr<ReadbackPass> m_readbackPass;
    rhi::Timeline m_readbackFence;
    std::atomic<uint64_t> m_nextFenceValue{ 0 };
    std::mutex m_mutex;
    std::vector<ReadbackInfo> m_queuedReadbacks;
    std::vector<ReadbackRequest> m_readbackRequests;
};

} // namespace br
