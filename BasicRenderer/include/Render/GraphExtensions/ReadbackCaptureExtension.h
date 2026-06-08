#pragma once

#include <unordered_map>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Render/RenderGraph/RenderGraph.h"
#include "RenderPasses/ReadbackCapturePass.h"
#include "RenderPasses/ReadbackCopyCapturePass.h"
#include "Render/Runtime/IReadbackService.h"

class ReadbackCaptureExtension final : public RenderGraph::IRenderGraphExtension {
public:
    explicit ReadbackCaptureExtension(rg::runtime::IReadbackService* readbackService)
        : m_readbackService(readbackService) {
    }

    void GatherStructuralPasses(RenderGraph&, std::vector<RenderGraph::ExternalPassDesc>&) override {
        // Readback capture is per-frame and ephemeral; we emit it via GatherFramePasses().
    }

    void GatherFramePasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& out) override {
        if (!m_readbackService) {
            return;
        }

        auto captures = m_readbackService->ConsumeCaptureRequests();
        TracyPlot("ReadbackCaptureExtension.ConsumedCaptures", static_cast<int64_t>(captures.size()));

        std::unordered_map<std::string, uint32_t> localIndexByAnchorPass;
        uint64_t acceptedCaptures = 0;
        uint64_t menuAnchorCaptures = 0;
        uint64_t copyQueueCaptures = 0;

        for (auto& capture : captures) {
            QueueKind preferredQueueKind = capture.preferredQueueKind;
            if (preferredQueueKind != QueueKind::Graphics && preferredQueueKind != QueueKind::Copy) {
                spdlog::warn(
                    "ReadbackCaptureExtension: capture for pass '{}' requested unsupported queue kind {}; falling back to graphics.",
                    capture.passName,
                    static_cast<int>(preferredQueueKind));
                preferredQueueKind = QueueKind::Graphics;
            }

            auto resource = capture.resource.lock();
            if (!resource && capture.resourceId != 0) {
                resource = rg.GetResourceByID(capture.resourceId);
            }

            if (!resource) {
                spdlog::warn(
                    "ReadbackCaptureExtension: dropping capture for pass '{}' because resource id {} is no longer available.",
                    capture.passName,
                    capture.resourceId);
                continue;
            }

            auto handle = rg.RequestResourceHandle(resource.get(), /*allowFailure=*/true);
            if (handle.GetGeneration() == 0) {
                spdlog::warn(
                    "ReadbackCaptureExtension: failed to resolve handle for capture resource id {} after pass '{}'.",
                    capture.resourceId,
                    capture.passName);
                continue;
            }

            ++acceptedCaptures;
            if (capture.passName == "MenuRenderPass") {
                ++menuAnchorCaptures;
            }
            if (preferredQueueKind == QueueKind::Copy) {
                ++copyQueueCaptures;
            }

            auto& localIndex = localIndexByAnchorPass[capture.passName];
            const std::string passInstanceName =
                "ReadbackCapture::" +
                capture.passName +
                "::" +
                (preferredQueueKind == QueueKind::Copy ? "Copy" : "Graphics") +
                "::Slot" +
                std::to_string(localIndex++);

            if (preferredQueueKind == QueueKind::Copy) {
                // Route through copy-queue CopyPass for lower latency
                ReadbackCopyCaptureInputs inputs{};
                inputs.target = ResourceHandleAndRange(handle, capture.range);

                auto pass = std::make_shared<ReadbackCopyCapturePass>(inputs, std::move(capture.callback), m_readbackService, passInstanceName);
                out.push_back(
                    RenderGraph::ExternalPassDesc::Copy(
                        passInstanceName,
                        std::move(pass))
                        .At(RenderGraph::ExternalInsertPoint::After(capture.passName))
                        .PreferQueue(QueueKind::Copy)
                        .PinToQueue(static_cast<QueueSlotIndex>(2))
                        .CollectStatistics(false)
                        .RegisterByName(false));
            }
            else {
                // Default: graphics-queue RenderPass (existing path)
                ReadbackCaptureInputs inputs{};
                inputs.target = ResourceHandleAndRange(handle, capture.range);

                auto pass = std::make_shared<ReadbackCapturePass>(inputs, std::move(capture.callback), m_readbackService, passInstanceName);
                out.push_back(
                    RenderGraph::ExternalPassDesc::Render(
                        passInstanceName,
                        std::move(pass))
                        .At(RenderGraph::ExternalInsertPoint::After(capture.passName))
                        .PinToQueue(static_cast<QueueSlotIndex>(0))
                        .CollectStatistics(false)
                        .RegisterByName(false));
            }
        }

        TracyPlot("ReadbackCaptureExtension.AcceptedCaptures", static_cast<int64_t>(acceptedCaptures));
        TracyPlot("ReadbackCaptureExtension.MenuRenderPassCaptures", static_cast<int64_t>(menuAnchorCaptures));
        TracyPlot("ReadbackCaptureExtension.CopyQueueCaptures", static_cast<int64_t>(copyQueueCaptures));
    }

private:
    rg::runtime::IReadbackService* m_readbackService = nullptr; // non-owning
};
