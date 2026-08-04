#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "Render/RendererComponents.h"
#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class PixelBuffer;
class DynamicBuffer;
class VirtualShadowInvalidationQueue;

class VirtualShadowMapInvalidatePagesPass final : public ComputePass {
public:
    VirtualShadowMapInvalidatePagesPass(
        std::shared_ptr<Buffer> invalidationInputsBuffer,
        std::shared_ptr<Buffer> invalidationCountBuffer,
        std::shared_ptr<Buffer> invalidatedInstancesBitsetBuffer,
        std::shared_ptr<Buffer> clipmapInfoBuffer,
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> directionalPageViewInfoBuffer,
        std::shared_ptr<Buffer> statsBuffer,
        std::shared_ptr<VirtualShadowInvalidationQueue> extensionInvalidations = nullptr);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    PipelineState m_boundsPso;
    std::shared_ptr<Buffer> m_invalidationInputsBuffer;
    std::shared_ptr<Buffer> m_invalidationCountBuffer;
    std::shared_ptr<Buffer> m_invalidatedInstancesBitsetBuffer;
    std::shared_ptr<Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_directionalPageViewInfoBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
    std::shared_ptr<DynamicBuffer> m_boundsInvalidationBuffer;
    std::shared_ptr<VirtualShadowInvalidationQueue> m_extensionInvalidations;
    uint32_t m_pendingInputCount = 0u;
    uint32_t m_pendingBoundsCount = 0u;
    bool m_invalidateAllActiveClipmaps = false;
    flecs::query<const Components::ObjectDrawInfo> m_transformChangedQuery;
};
