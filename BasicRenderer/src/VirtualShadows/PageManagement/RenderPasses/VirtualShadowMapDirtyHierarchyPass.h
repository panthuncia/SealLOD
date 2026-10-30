#pragma once

#include <memory>
#include <vector>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapDirtyHierarchyBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken pageTable;
    std::vector<org::DeclaredViewToken> hierarchy;
    org::DeclaredViewToken clipmapInfo;
};

struct VirtualShadowMapDirtyHierarchyFrame {
    uint32_t pageTableResolution = 0;
};

class VirtualShadowMapDirtyHierarchyPass final : public org::TypedRenderGraphPass<VirtualShadowMapDirtyHierarchyPass,
    VirtualShadowMapDirtyHierarchyFrame, VirtualShadowMapDirtyHierarchyBindings> {
public:
    VirtualShadowMapDirtyHierarchyPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::PixelBuffer> dirtyHierarchyTexture,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer);

    VirtualShadowMapDirtyHierarchyBindings Declare(org::PassBuilder& builder);
    VirtualShadowMapDirtyHierarchyFrame Prepare(const VirtualShadowMapDirtyHierarchyBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapDirtyHierarchyBindings&,
        const VirtualShadowMapDirtyHierarchyFrame&, org::PassRecordContext&);

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_dirtyHierarchyTexture;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
};
