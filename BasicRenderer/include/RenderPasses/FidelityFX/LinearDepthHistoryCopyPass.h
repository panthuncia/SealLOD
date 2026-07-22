#pragma once

#include <algorithm>
#include <unordered_set>

#include "RenderPasses/Base/RenderPass.h"
#include "Render/RenderContext.h"
#include "Managers/ViewManager.h"

class LinearDepthHistoryCopyPass : public RenderPass {
public:
    explicit LinearDepthHistoryCopyPass(ViewManager* viewManager)
        : m_viewManager(viewManager) {
    }

    void DeclareResourceUsages(RenderPassBuilder* builder) override {
        m_copies.clear();
        builder->WithCopySource(
            Builtin::LinearDepthMaps)
            .WithCopyDest(Builtin::LastFrameLinearDepthMaps);

        if (!m_viewManager) {
            return;
        }

        std::unordered_set<uint64_t> declaredPairs;
        m_viewManager->ForEachView([&](uint64_t viewID) {
            auto* view = m_viewManager->Get(viewID);
            if (!view || !view->gpu.linearDepthMap || !view->gpu.lastFrameLinearDepthMap) {
                return;
            }

            const uint64_t sourceID = view->gpu.linearDepthMap->GetGlobalResourceID();
            const uint64_t historyID = view->gpu.lastFrameLinearDepthMap->GetGlobalResourceID();
            const uint64_t pairKey = sourceID ^ (historyID + 0x9e3779b97f4a7c15ull
                + (sourceID << 6u) + (sourceID >> 2u));
            if (!declaredPairs.insert(pairKey).second) {
                return;
            }

            builder->WithCopySource(view->gpu.linearDepthMap)
                .WithCopyDest(view->gpu.lastFrameLinearDepthMap);
            m_copies.push_back({
                viewID,
                view->gpu.linearDepthMap,
                view->gpu.lastFrameLinearDepthMap
            });
        });
    }

    void Setup() override {
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& commandList = executionContext.commandList;

        for (const auto& copy : m_copies) {
            const auto& source = copy.source;
            const auto& history = copy.history;

            const auto& desc = source->GetDescription();
            const uint32_t sliceCount = desc.isCubemap
                ? 6u * (std::max)(1u, desc.arraySize)
                : (desc.isArray ? (std::max)(1u, desc.arraySize) : 1u);
            const uint32_t mipCount = source->GetNumUAVMipLevels();

            for (uint32_t slice = 0; slice < sliceCount; ++slice) {
                for (uint32_t mip = 0; mip < mipCount; ++mip) {
                    rhi::TextureCopyRegion srcRegion = {};
                    srcRegion.texture = source->GetAPIResource().GetHandle();
                    srcRegion.mip = mip;
                    srcRegion.arraySlice = slice;
                    srcRegion.x = 0;
                    srcRegion.y = 0;
                    srcRegion.z = 0;
                    srcRegion.width = (std::max)(1u, source->GetInternalWidth() >> mip);
                    srcRegion.height = (std::max)(1u, source->GetInternalHeight() >> mip);
                    srcRegion.depth = 1;

                    rhi::TextureCopyRegion dstRegion = {};
                    dstRegion.texture = history->GetAPIResource().GetHandle();
                    dstRegion.mip = mip;
                    dstRegion.arraySlice = slice;
                    dstRegion.x = 0;
                    dstRegion.y = 0;
                    dstRegion.z = 0;
                    dstRegion.width = srcRegion.width;
                    dstRegion.height = srcRegion.height;
                    dstRegion.depth = srcRegion.depth;

                    commandList.CopyTextureRegion(dstRegion, srcRegion);
                }
            }

            // Once the declared copy has executed, phase-1 occlusion can safely consume it.
            context.viewManager->MarkDepthHistoryValid(copy.viewID);
        }

        return {};
    }

    void Cleanup() override {
        m_copies.clear();
    }

private:
    struct CopyPair {
        uint64_t viewID = 0;
        std::shared_ptr<PixelBuffer> source;
        std::shared_ptr<PixelBuffer> history;
    };

    ViewManager* m_viewManager = nullptr;
    std::vector<CopyPair> m_copies;
};
