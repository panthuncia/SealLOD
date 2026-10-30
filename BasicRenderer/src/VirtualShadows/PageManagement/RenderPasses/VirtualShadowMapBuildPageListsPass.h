#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapBuildPageListsBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken pageTable, pageMetadata, allocationCount;
    org::DeclaredViewToken freePages, reusablePages, header;
};

struct VirtualShadowMapBuildPageListsFrame {
    uint32_t maxPhysicalPages = 0;
    uint32_t pageTableResolution = 0;
};

class VirtualShadowMapBuildPageListsPass final : public org::TypedRenderGraphPass<VirtualShadowMapBuildPageListsPass,
    VirtualShadowMapBuildPageListsFrame, VirtualShadowMapBuildPageListsBindings> {
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
    VirtualShadowMapBuildPageListsFrame Prepare(const VirtualShadowMapBuildPageListsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapBuildPageListsBindings&,
        const VirtualShadowMapBuildPageListsFrame&, org::PassRecordContext&);
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
