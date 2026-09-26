#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <DirectXMath.h>

#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }
class VirtualShadowCasterRegistry;

struct VirtualShadowMapSetupBindings {
    org::ResourceBindingToken pageTable, pageMetadata, allocationCount, dirtyFlags;
    org::ResourceBindingToken clipmapInfo, markClipmapData, stats, runtimeState, fallbackCandidateCount;
    uint32_t packedFlags = 0;
    float autoBiasScale = 0.0f;
};

class VirtualShadowMapSetupPass final : public org::TypedRenderGraphPass<VirtualShadowMapSetupPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapSetupBindings> {
public:
    VirtualShadowMapSetupPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> allocationCountBuffer,
        std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> markClipmapDataBuffer,
        std::shared_ptr<org::Buffer> compactMainCameraBuffer,
        std::shared_ptr<org::Buffer> compactShadowCameraBuffer,
        std::shared_ptr<org::Buffer> statsBuffer,
        std::shared_ptr<org::Buffer> runtimeStateBuffer,
        std::shared_ptr<org::Buffer> fallbackCandidateCountBuffer,
        std::shared_ptr<VirtualShadowCasterRegistry> virtualShadowCasters,
        bool forceResetResources);

    VirtualShadowMapSetupBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext& executionContext) override;
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapSetupBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapSetupBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_allocationCountBuffer;
    std::shared_ptr<org::Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_markClipmapDataBuffer;
    std::shared_ptr<org::Buffer> m_compactMainCameraBuffer;
    std::shared_ptr<org::Buffer> m_compactShadowCameraBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
    std::shared_ptr<org::Buffer> m_runtimeStateBuffer;
    std::shared_ptr<org::Buffer> m_fallbackCandidateCountBuffer;
    std::shared_ptr<VirtualShadowCasterRegistry> m_virtualShadowCasters;
    bool m_forceResetResources = false;
    bool m_resetResources = false;
    bool m_resetReasonForced = false;
    bool m_resetReasonNoPreviousState = false;
    bool m_resetReasonStructureMismatch = false;
    bool m_resetReasonLightDirectionChanged = false;
    bool m_feedbackRecoveryRefresh = false;

    // Temporal setup state belongs to this graph generation. Keeping it on the
    // pass prevents a newly built graph from mutating history still selected by
    // an older retained generation.
    std::array<CLodVirtualShadowClipmapInfo, CLodVirtualShadowMaxSupportedClipmapCount>
        m_previousClipmapInfos{};
    std::array<int64_t, CLodVirtualShadowMaxSupportedClipmapCount>
        m_previousClipmapPageOffsetX{};
    std::array<int64_t, CLodVirtualShadowMaxSupportedClipmapCount>
        m_previousClipmapPageOffsetY{};
    DirectX::XMFLOAT3 m_previousDirectionalLightDirection{};
    DirectX::XMUINT2 m_previousRenderResolution{};
    uint32_t m_pendingRenderResolutionResetFrames = 0u;
    bool m_previousClipmapInfosValid = false;
    bool m_previousDirectionalLightDirectionValid = false;
    bool m_previousRenderResolutionValid = false;
};
