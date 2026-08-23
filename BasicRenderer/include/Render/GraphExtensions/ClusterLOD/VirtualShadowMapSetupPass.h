#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;
class VirtualShadowCasterRegistry;

class VirtualShadowMapSetupPass final : public ComputePass {
public:
    VirtualShadowMapSetupPass(
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> allocationCountBuffer,
        std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<Buffer> clipmapInfoBuffer,
        std::shared_ptr<Buffer> markClipmapDataBuffer,
        std::shared_ptr<Buffer> compactMainCameraBuffer,
        std::shared_ptr<Buffer> compactShadowCameraBuffer,
        std::shared_ptr<Buffer> statsBuffer,
        std::shared_ptr<Buffer> runtimeStateBuffer,
        std::shared_ptr<Buffer> fallbackCandidateCountBuffer,
        std::shared_ptr<VirtualShadowCasterRegistry> virtualShadowCasters,
        bool forceResetResources);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_allocationCountBuffer;
    std::shared_ptr<Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<Buffer> m_markClipmapDataBuffer;
    std::shared_ptr<Buffer> m_compactMainCameraBuffer;
    std::shared_ptr<Buffer> m_compactShadowCameraBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
    std::shared_ptr<Buffer> m_runtimeStateBuffer;
    std::shared_ptr<Buffer> m_fallbackCandidateCountBuffer;
    std::shared_ptr<VirtualShadowCasterRegistry> m_virtualShadowCasters;
    bool m_forceResetResources = false;
    bool m_resetResources = false;
    bool m_resetReasonForced = false;
    bool m_resetReasonNoPreviousState = false;
    bool m_resetReasonStructureMismatch = false;
    bool m_resetReasonLightDirectionChanged = false;
    bool m_feedbackRecoveryRefresh = false;
};
