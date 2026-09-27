#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapAllocatePagesBindings {
    org::ResourceBindingToken requests, requestCount, indirectArgs, clipmapInfo, pageTable;
    org::ResourceBindingToken pageMetadata, dirtyFlags, freePages, reusablePages, header, stats;
    uint32_t pageRenderBudget = 0;
};

class VirtualShadowMapAllocatePagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapAllocatePagesPass,
    br::render::PreparedComputeIndirect, VirtualShadowMapAllocatePagesBindings> {
public:
    VirtualShadowMapAllocatePagesPass(
        std::shared_ptr<org::Buffer> allocationRequestsBuffer,
        std::shared_ptr<org::Buffer> allocationCountBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<org::Buffer> freePhysicalPagesBuffer,
        std::shared_ptr<org::Buffer> reusablePhysicalPagesBuffer,
        std::shared_ptr<org::Buffer> pageListHeaderBuffer,
        std::shared_ptr<org::Buffer> statsBuffer);

    VirtualShadowMapAllocatePagesBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeIndirect Prepare(const VirtualShadowMapAllocatePagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapAllocatePagesBindings&,
        const br::render::PreparedComputeIndirect&, org::PassRecordContext&);

private:
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
    std::shared_ptr<org::Buffer> m_allocationRequestsBuffer;
    std::shared_ptr<org::Buffer> m_allocationCountBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<org::Buffer> m_freePhysicalPagesBuffer;
    std::shared_ptr<org::Buffer> m_reusablePhysicalPagesBuffer;
    std::shared_ptr<org::Buffer> m_pageListHeaderBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
};
