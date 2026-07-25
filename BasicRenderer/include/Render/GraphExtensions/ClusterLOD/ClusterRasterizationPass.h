#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <rhi.h>

#include "BuiltinRenderPasses.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/RenderPhase.h"
#include "RenderPasses/Base/RenderPass.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Resources/PixelBuffer.h"

class Buffer;
class ResourceGroup;

struct ClusterRasterizationPassInputs {
    bool wireframe;
    bool clearGbuffer;
    RenderPhase renderPhase;
    CLodRasterOutputKind outputKind = CLodRasterOutputKind::VisibilityBuffer;

    RG_DEFINE_PASS_INPUTS(ClusterRasterizationPassInputs, &ClusterRasterizationPassInputs::wireframe, &ClusterRasterizationPassInputs::clearGbuffer, &ClusterRasterizationPassInputs::renderPhase, &ClusterRasterizationPassInputs::outputKind);
};

class ClusterRasterizationPass : public RenderPass, public IDynamicDeclaredResources {
public:
    ClusterRasterizationPass(
        ClusterRasterizationPassInputs inputs,
        std::shared_ptr<Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<Buffer> sortedToUnsortedMappingBuffer,
        std::shared_ptr<Buffer> deepVisibilityNodesBuffer = nullptr,
        std::shared_ptr<Buffer> deepVisibilityCounterBuffer = nullptr,
        std::shared_ptr<Buffer> deepVisibilityOverflowCounterBuffer = nullptr,
        std::shared_ptr<Buffer> AVBOITConfigBuffer = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITOccupancyTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITScalarExtinctionTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITChromaticExtinctionTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITIntegratedTransmittanceTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITZeroTransmittanceSliceTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITAccumulationTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITNormalizationTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITShadingExtinctionTexture = nullptr,
        std::shared_ptr<Buffer> visibleClustersResolveBuffer = nullptr,
        std::shared_ptr<ResourceGroup> slabResourceGroup = nullptr,
        std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture = nullptr,
        std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture = nullptr,
        std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITOccupancySliceMaskTexture = nullptr,
        std::shared_ptr<PixelBuffer> AVBOITEarlyDepthTexture = nullptr,
        std::shared_ptr<Buffer> telemetryBuffer = nullptr,
        std::shared_ptr<Buffer> sourceGroupMismatchCounterBuffer = nullptr,
        std::shared_ptr<Buffer> sourceGroupMismatchDetailsBuffer = nullptr,
        std::shared_ptr<Buffer> virtualShadowPageMetadataBuffer = nullptr,
        std::shared_ptr<Buffer> virtualShadowDirectionalPageViewInfoBuffer = nullptr);
    ~ClusterRasterizationPass();

    void DeclareResourceUsages(RenderPassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    bool m_wireframe = false;
    bool m_meshShaders = false;
    bool m_clearGbuffer = true;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;

    std::vector<CLodViewRasterInfo> m_viewRasterInfos;
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    std::vector<std::shared_ptr<PixelBuffer>> m_deepVisibilityHeadPointerBuffers;

    std::shared_ptr<Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_sortedToUnsortedMappingBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityNodesBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<Buffer> m_AVBOITConfigBuffer;
    std::shared_ptr<PixelBuffer> m_AVBOITOccupancyTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITScalarExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITChromaticExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITIntegratedTransmittanceTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITZeroTransmittanceSliceTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITAccumulationTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITNormalizationTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITShadingExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITEarlyDepthTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITOccupancySliceMaskTexture;
    std::shared_ptr<Buffer> m_visibleClustersResolveBuffer;
    std::shared_ptr<PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<Buffer> m_virtualShadowPageMetadataBuffer;
    std::shared_ptr<Buffer> m_virtualShadowDirectionalPageViewInfoBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<Buffer> m_sourceGroupMismatchCounterBuffer;
    std::shared_ptr<Buffer> m_sourceGroupMismatchDetailsBuffer;

    std::shared_ptr<ResourceGroup> m_slabResourceGroup;

    rhi::CommandSignaturePtr m_rasterizationCommandSignature;

    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    uint32_t m_passWidth = 1;
    uint32_t m_passHeight = 1;
    uint32_t m_deepVisibilityNodeCapacity = 1;
    bool m_declaredResourcesChanged = true;
    std::function<bool()> m_getPunctualLightingEnabled;
    std::function<bool()> m_getShadowsEnabled;
    bool m_gtaoEnabled = false;

    RenderPhase m_renderPhase;
};
