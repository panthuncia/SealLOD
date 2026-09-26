#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "BasicRenderer/Scene/RendererComponents.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }
namespace org { class DynamicBuffer; }
class VirtualShadowInvalidationQueue;

struct VirtualShadowMapInvalidatePagesBindings {
    org::ResourceBindingToken inputs, inputCount, clipmapInfo, bounds;
    org::ResourceBindingToken pageTable, dirtyFlags, pageMetadata, pageViewInfo, stats;
    uint32_t pendingInputCount = 0, pendingBoundsCount = 0;
    bool invalidateAllActiveClipmaps = false;
};

class VirtualShadowMapInvalidatePagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapInvalidatePagesPass,
    br::render::PreparedComputePipelineSequence, VirtualShadowMapInvalidatePagesBindings> {
public:
    VirtualShadowMapInvalidatePagesPass(
        std::shared_ptr<org::Buffer> invalidationInputsBuffer,
        std::shared_ptr<org::Buffer> invalidationCountBuffer,
        std::shared_ptr<org::Buffer> invalidatedInstancesBitsetBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> directionalPageViewInfoBuffer,
        std::shared_ptr<org::Buffer> statsBuffer,
        std::shared_ptr<VirtualShadowInvalidationQueue> extensionInvalidations = nullptr);

    VirtualShadowMapInvalidatePagesBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    br::render::PreparedComputePipelineSequence Prepare(const VirtualShadowMapInvalidatePagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapInvalidatePagesBindings&,
        const br::render::PreparedComputePipelineSequence&, org::PassRecordContext&);

private:
    org::PipelineState m_pso;
    org::PipelineState m_boundsPso;
    std::shared_ptr<org::Buffer> m_invalidationInputsBuffer;
    std::shared_ptr<org::Buffer> m_invalidationCountBuffer;
    std::shared_ptr<org::Buffer> m_invalidatedInstancesBitsetBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_directionalPageViewInfoBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
    std::shared_ptr<org::DynamicBuffer> m_boundsInvalidationBuffer;
    std::shared_ptr<VirtualShadowInvalidationQueue> m_extensionInvalidations;
    uint32_t m_pendingInputCount = 0u;
    uint32_t m_pendingBoundsCount = 0u;
    bool m_invalidateAllActiveClipmaps = false;
    flecs::query<const Components::ObjectDrawInfo> m_transformChangedQuery;
};
