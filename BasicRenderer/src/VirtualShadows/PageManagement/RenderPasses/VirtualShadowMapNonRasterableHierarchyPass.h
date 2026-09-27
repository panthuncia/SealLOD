#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapNonRasterableHierarchyBindings {
    org::ResourceBindingToken pageTable;
    org::ResourceBindingToken hierarchy;
    org::ResourceBindingToken clipmapInfo;
};

class VirtualShadowMapNonRasterableHierarchyPass final : public org::TypedRenderGraphPass<VirtualShadowMapNonRasterableHierarchyPass,
    br::render::PreparedComputeDispatchSequence, VirtualShadowMapNonRasterableHierarchyBindings> {
public:
    VirtualShadowMapNonRasterableHierarchyPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::PixelBuffer> nonRasterableHierarchyTexture,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer);

    VirtualShadowMapNonRasterableHierarchyBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatchSequence Prepare(const VirtualShadowMapNonRasterableHierarchyBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapNonRasterableHierarchyBindings&,
        const br::render::PreparedComputeDispatchSequence&, org::PassRecordContext&);

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_nonRasterableHierarchyTexture;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
};
