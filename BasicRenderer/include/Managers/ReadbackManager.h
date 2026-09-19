#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <vector>

#include <rhi.h>

#include "OpenRenderGraph/OpenRenderGraph.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"

namespace br {

class ReadbackManager {
public:
    ReadbackManager();

    void Initialize(rhi::Timeline readbackFence);

    void RequestReadback(std::shared_ptr<org::PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback, bool cubemap);

    std::shared_ptr<org::RenderPass> GetReadbackPass() const { return m_readbackPass; }

    void ProcessReadbackRequests();

    void Cleanup();

private:
    struct ReadbackFrameData {
        struct Copy {
            org::PreparedResourceReference source{};
            rhi::ResourceHandle destination{};
            rhi::CopyableFootprint footprint{};
            uint32_t mip = 0, slice = 0;
        };
        std::vector<Copy> copies;
    };
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

    class ReadbackPass
        : public org::TypedRenderGraphPass<ReadbackPass, ReadbackFrameData>,
          public org::IDynamicDeclaredResources {
    public:
        explicit ReadbackPass(std::shared_ptr<State> state)
            : m_state(std::move(state)) {
        }

        void Declare(org::PassBuilder& builder);
        ReadbackFrameData Prepare(const org::PassPrepareContext& preparation);
        static void Record(const ReadbackFrameData& data, org::PassRecordContext& recording);
        bool DeclaredResourcesChanged() const override;

        void SetReadbackFence(rhi::Timeline fence) {
            m_readbackFence = fence;
        }

    private:
        std::shared_ptr<State> m_state;
        rhi::Timeline m_readbackFence;
    };

    void ClearReadbacks();

    void SaveCubemapToDDS(
        rhi::Device& device,
        org::imm::ImmediateCommandList& commandList,
        std::shared_ptr<org::PixelBuffer> cubemap,
        const std::wstring& outputFile,
        uint64_t fenceValue);

    void SaveTextureToDDS(
        rhi::Device& device,
        org::imm::ImmediateCommandList& commandList,
        org::PixelBuffer* texture,
        const std::wstring& outputFile,
        uint64_t fenceValue);

    std::shared_ptr<ReadbackPass> m_readbackPass;
    std::shared_ptr<State> m_state;
    rhi::Timeline m_readbackFence;
};

} // namespace br
