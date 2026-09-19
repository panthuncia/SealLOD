#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapClearDirtyBitsBindings {
    org::ResourceBindingToken pageTable;
    org::ResourceBindingToken dirtyFlags;
    org::ResourceBindingToken stats;
    bool completeEmptyAdmittedPages = false;
};

class VirtualShadowMapClearDirtyBitsPass final : public org::TypedRenderGraphPass<VirtualShadowMapClearDirtyBitsPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapClearDirtyBitsBindings> {
public:
    VirtualShadowMapClearDirtyBitsPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> allocationRequestsBuffer,
        std::shared_ptr<org::Buffer> allocationCountBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> dirtyFlagsBuffer,
        std::shared_ptr<org::Buffer> statsBuffer);

    VirtualShadowMapClearDirtyBitsBindings Declare(org::PassBuilder& builder);
    void Initialize();
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapClearDirtyBitsBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapClearDirtyBitsBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_dirtyFlagsBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
};
