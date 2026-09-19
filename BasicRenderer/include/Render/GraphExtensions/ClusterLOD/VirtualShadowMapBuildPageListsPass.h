#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapBuildPageListsBindings {
    org::ResourceBindingToken pageTable, pageMetadata, allocationCount;
    org::ResourceBindingToken freePages, reusablePages, header;
};

class VirtualShadowMapBuildPageListsPass final : public org::TypedRenderGraphPass<VirtualShadowMapBuildPageListsPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapBuildPageListsBindings> {
public:
    VirtualShadowMapBuildPageListsPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> allocationCountBuffer,
        std::shared_ptr<org::Buffer> freePhysicalPagesBuffer,
        std::shared_ptr<org::Buffer> reusablePhysicalPagesBuffer,
        std::shared_ptr<org::Buffer> pageListHeaderBuffer);

    VirtualShadowMapBuildPageListsBindings Declare(org::PassBuilder& builder);
    void Initialize();
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapBuildPageListsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapBuildPageListsBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_allocationCountBuffer;
    std::shared_ptr<org::Buffer> m_freePhysicalPagesBuffer;
    std::shared_ptr<org::Buffer> m_reusablePhysicalPagesBuffer;
    std::shared_ptr<org::Buffer> m_pageListHeaderBuffer;
};
