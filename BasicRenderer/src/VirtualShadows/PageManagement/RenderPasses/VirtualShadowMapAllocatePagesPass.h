#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapAllocatePagesBindings {
    org::PipelineState pipeline;
    std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
    org::DeclaredViewToken requests, requestCount;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken clipmapInfo, pageTable;
    org::DeclaredViewToken pageMetadata, dirtyFlags, freePages, reusablePages, header, stats;
    uint32_t pageRenderBudget = 0;
};
struct VirtualShadowMapAllocatePagesFrame {
    uint32_t pageTableResolution = 0;
    uint32_t maxPhysicalPages = 0;
};

class VirtualShadowMapAllocatePagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapAllocatePagesPass,
    VirtualShadowMapAllocatePagesFrame, VirtualShadowMapAllocatePagesBindings> {
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
    VirtualShadowMapAllocatePagesFrame Prepare(const VirtualShadowMapAllocatePagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapAllocatePagesBindings&,
        const VirtualShadowMapAllocatePagesFrame&, org::PassRecordContext&);

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
