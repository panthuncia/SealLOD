#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ReyesVirtualShadowRasterBindings {
    org::ResourceBindingToken visible, transforms, diceQueue, diceCounter, work, workCounter;
    org::ResourceBindingToken tessConfigs, tessVertices, tessTriangles, indirectArgs, telemetry, viewInfo;
    org::ResourceBindingToken pageTable, physicalPages, dynamicPages, clipmapInfo;
    uint32_t phase = 0, pageTableResolution = 0, virtualResolution = 0;
};

class ReyesVirtualShadowRasterizationPass final : public org::TypedRenderGraphPass<ReyesVirtualShadowRasterizationPass,
    br::render::PreparedComputeIndirect, ReyesVirtualShadowRasterBindings>, public org::IDynamicDeclaredResources {
public:
    ReyesVirtualShadowRasterizationPass(
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> diceQueueBuffer,
        std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
        std::shared_ptr<org::Buffer> rasterWorkBuffer,
        std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
        std::shared_ptr<org::Buffer> tessTableTrianglesBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup,
        std::string_view resourceName,
        uint32_t phaseIndex);

    ReyesVirtualShadowRasterBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    br::render::PreparedComputeIndirect Prepare(const ReyesVirtualShadowRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesVirtualShadowRasterBindings&, const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording);

private:
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkBuffer;
    std::shared_ptr<org::Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<org::Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<org::Buffer> m_tessTableTrianglesBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<org::Buffer> m_viewRasterInfoBuffer;

    uint32_t m_phaseIndex = 0u;
    std::vector<CLodViewRasterInfo> m_viewRasterInfos;
    bool m_declaredResourcesChanged = true;
    CLodVirtualShadowResolutionConfig m_shadowConfig{};
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
