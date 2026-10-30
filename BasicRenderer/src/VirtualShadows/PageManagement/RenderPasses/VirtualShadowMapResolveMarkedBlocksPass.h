#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapResolveMarkedBlocksBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken mask, list, count, requests, requestCount, clipmapData;
    org::DeclaredViewToken pageTable, dirtyFlags, pageViewInfo, stats;
};
struct VirtualShadowMapResolveMarkedBlocksFrame {
    uint32_t activeClipmapCount = 0;
    uint32_t maxAllocationRequests = 0;
};

class VirtualShadowMapResolveMarkedBlocksPass final : public org::TypedRenderGraphPass<VirtualShadowMapResolveMarkedBlocksPass,
    VirtualShadowMapResolveMarkedBlocksFrame, VirtualShadowMapResolveMarkedBlocksBindings> {
public:
    VirtualShadowMapResolveMarkedBlocksPass(
        std::shared_ptr<org::Buffer> markedBlocksMaskBuffer,
        std::shared_ptr<org::Buffer> markedBlocksListBuffer,
        std::shared_ptr<org::Buffer> markedBlocksCountBuffer,
        std::shared_ptr<org::Buffer> allocationRequestsBuffer,
        std::shared_ptr<org::Buffer> allocationCountBuffer,
        std::shared_ptr<org::Buffer> markClipmapDataBuffer,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<org::Buffer> directionalPageViewInfoBuffer,
        std::shared_ptr<org::Buffer> statsBuffer);

    VirtualShadowMapResolveMarkedBlocksBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext& executionContext) override;
    VirtualShadowMapResolveMarkedBlocksFrame Prepare(const VirtualShadowMapResolveMarkedBlocksBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapResolveMarkedBlocksBindings&,
        const VirtualShadowMapResolveMarkedBlocksFrame&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::Buffer> m_markedBlocksMaskBuffer;
    std::shared_ptr<org::Buffer> m_markedBlocksListBuffer;
    std::shared_ptr<org::Buffer> m_markedBlocksCountBuffer;
    std::shared_ptr<org::Buffer> m_allocationRequestsBuffer;
    std::shared_ptr<org::Buffer> m_allocationCountBuffer;
    std::shared_ptr<org::Buffer> m_markClipmapDataBuffer;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<org::Buffer> m_directionalPageViewInfoBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
    uint32_t m_activeClipmapCount = 0u;
};
