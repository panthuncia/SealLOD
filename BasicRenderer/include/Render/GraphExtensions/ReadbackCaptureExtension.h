#pragma once

#include <unordered_map>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Render/RenderGraph/RenderGraph.h"
#include "RenderPasses/ReadbackCapturePass.h"
#include "RenderPasses/ReadbackCopyCapturePass.h"
#include "Render/Runtime/IReadbackService.h"

class ReadbackCaptureExtension final : public org::RenderGraph::IRenderGraphExtension {
public:
    explicit ReadbackCaptureExtension(std::shared_ptr<org::runtime::IReadbackService> readbackService)
        : m_readbackService(readbackService) {
    }

    void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>&) override {
        // Readback capture is per-frame and ephemeral; we emit it via GatherFramePasses().
    }

    void GatherFramePasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& out) override {
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
            org::QueueKind preferredQueueKind = capture.preferredQueueKind;
            if (preferredQueueKind != org::QueueKind::Graphics && preferredQueueKind != org::QueueKind::Copy) {
                spdlog::warn(
                    "ReadbackCaptureExtension: capture for pass '{}' requested unsupported queue kind {}; falling back to graphics.",
                    capture.passName,
                    static_cast<int>(preferredQueueKind));
                preferredQueueKind = org::QueueKind::Graphics;
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

            // Published-state resolvers expose their backing resource directly to
            // readback clients, but that backing is not necessarily registered as
            // a named graph resource. Register and frame-pin it here so the
            // explicitly anchored capture pass can transition and copy the exact
            // resource that the client requested.
            auto handle = rg.RequestResourceHandle(resource.get(), /*allowFailure=*/false);
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
            if (preferredQueueKind == org::QueueKind::Copy) {
                ++copyQueueCaptures;
            }

            // Captures anchored after a pass interrupt the frame there; captures
            // of end-of-frame contents run after the graph.
            const bool afterGraph = capture.passName == org::runtime::kReadbackAfterGraph;
            const auto where = afterGraph
                ? org::RenderGraph::ExternalInsertPoint::End()
                : org::RenderGraph::ExternalInsertPoint::After(capture.passName);
            auto& localIndex = localIndexByAnchorPass[capture.passName];
            const std::string passInstanceName =
                "ReadbackCapture::" +
                capture.passName +
                "::" +
                (preferredQueueKind == org::QueueKind::Copy ? "Copy" : "Graphics") +
                "::Slot" +
                std::to_string(localIndex++);

            if (preferredQueueKind == org::QueueKind::Copy) {
                // Route through copy-queue CopyPass for lower latency
                org::ReadbackCopyCaptureInputs inputs{};
                inputs.target = org::ResourceHandleAndRange(handle, capture.range);

                auto pass = std::make_shared<org::ReadbackCopyCapturePass>(inputs, resource, std::move(capture.callback), m_readbackService, passInstanceName);
                out.push_back(
                    org::RenderGraph::ExternalPassDesc::Copy(
                        passInstanceName,
                        std::move(pass))
                        .At(where)
                        .InterruptsFrame(!afterGraph)
                        .PreferQueue(org::QueueKind::Copy)
                        .PinToQueue(static_cast<org::QueueSlotIndex>(2))
                        .CollectStatistics(false)
                        .RegisterByName(false));
            }
            else {
                // Default: graphics-queue RenderPass (existing path)
                org::ReadbackCaptureInputs inputs{};
                inputs.target = org::ResourceHandleAndRange(handle, capture.range);

                auto pass = std::make_shared<org::ReadbackCapturePass>(inputs, resource, std::move(capture.callback), m_readbackService, passInstanceName);
                out.push_back(
                    org::RenderGraph::ExternalPassDesc::Render(
                        passInstanceName,
                        std::move(pass))
                        .At(where)
                        .InterruptsFrame(!afterGraph)
                        .PinToQueue(static_cast<org::QueueSlotIndex>(0))
                        .CollectStatistics(false)
                        .RegisterByName(false));
            }
        }

        TracyPlot("ReadbackCaptureExtension.AcceptedCaptures", static_cast<int64_t>(acceptedCaptures));
        TracyPlot("ReadbackCaptureExtension.MenuRenderPassCaptures", static_cast<int64_t>(menuAnchorCaptures));
        TracyPlot("ReadbackCaptureExtension.CopyQueueCaptures", static_cast<int64_t>(copyQueueCaptures));
    }

private:
    std::shared_ptr<org::runtime::IReadbackService> m_readbackService;
};
