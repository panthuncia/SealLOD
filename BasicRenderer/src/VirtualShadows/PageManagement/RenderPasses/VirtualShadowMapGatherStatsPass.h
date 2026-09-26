#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapGatherStatsBindings {
    org::ResourceBindingToken pageTable, allocationCount, allocationArgs, header, pageMetadata, clipmapInfo, stats;
    bool capturePreAllocateState = false;
};

class VirtualShadowMapGatherStatsPass final : public org::TypedRenderGraphPass<VirtualShadowMapGatherStatsPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapGatherStatsBindings> {
public:
    VirtualShadowMapGatherStatsPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> allocationCountBuffer,
        std::shared_ptr<org::Buffer> allocationIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> pageListHeaderBuffer,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> statsBuffer,
        bool capturePreAllocateState);

    VirtualShadowMapGatherStatsBindings Declare(org::PassBuilder& builder);
    void Initialize();
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapGatherStatsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapGatherStatsBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_allocationCountBuffer;
    std::shared_ptr<org::Buffer> m_allocationIndirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_pageListHeaderBuffer;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
    bool m_capturePreAllocateState = false;
};
