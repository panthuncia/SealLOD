#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "RenderPasses/Base/ComputePass.h"
#include "Resources/PixelBuffer.h"

class Buffer;
class ResourceGroup;

class ClusterSoftwareRasterizationPass : public ComputePass, public IDynamicDeclaredResources {
public:
    ClusterSoftwareRasterizationPass(
        std::shared_ptr<Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<Buffer> sortedToUnsortedMappingBuffer,
        std::shared_ptr<Buffer> viewRasterInfoBuffer,
        CLodRasterOutputKind outputKind,
        std::shared_ptr<PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        std::shared_ptr<ResourceGroup> slabResourceGroup = nullptr,
        bool runWhenComputeSWRasterEnabledOnly = false);
    ~ClusterSoftwareRasterizationPass();

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    rhi::CommandSignaturePtr m_rasterizationCommandSignature;
    std::shared_ptr<Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_sortedToUnsortedMappingBuffer;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    bool m_declaredResourcesChanged = true;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
