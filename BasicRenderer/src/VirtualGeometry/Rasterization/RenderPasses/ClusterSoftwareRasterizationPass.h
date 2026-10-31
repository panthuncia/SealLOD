#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ClusterSoftwareRasterBindings {
    std::shared_ptr<const rhi::CommandSignaturePtr> rasterSignature, cacheDispatchSignature;
    org::PipelineState cacheClear, cacheBuild, cacheFinalize, cacheSkin, cacheResolve;
    org::DeclaredViewToken histogram, visible, transforms, mapping;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken pageTable, clipmapInfo, physicalPages, dynamicPages, telemetry;
    org::DeclaredViewToken skinMapping, skinHash, skinPositions, skinAllocator, skinWork, skinArgs, skinMembership;
    CLodViewRasterDeclaration viewRasterInfoLayout;
    uint32_t skinCacheHashEntries = 0, skinCachePositionCapacity = 0;
    bool virtualShadow = false, hasTelemetry = false, hasSkinCache = false;
};

// One indirect dispatch per raster bucket with a resolved pipeline.
struct ClusterSoftwareRasterStep {
    org::PipelineState pipeline;
    uint64_t argumentsOffset = 0;
};
struct ClusterSoftwareRasterFrame {
    std::vector<ClusterSoftwareRasterStep> steps;
    uint32_t bucketCount = 0;
    // Skin-cache generation for this invocation; 0 when the cache does not run.
    uint32_t skinCacheGeneration = 0;
    uint32_t viewRasterInfoTable = 0;
    uint32_t pageTableResolution = 0, virtualResolution = 0;
    bool enabled = false;
    bool hasSkinCache = false;
};

class ClusterSoftwareRasterizationPass
    : public org::TypedRenderGraphPass<ClusterSoftwareRasterizationPass,
          ClusterSoftwareRasterFrame, ClusterSoftwareRasterBindings>,
      public org::IDynamicDeclaredResources {
public:
    ClusterSoftwareRasterizationPass(
        std::shared_ptr<org::Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> sortedToUnsortedMappingBuffer,
        CLodRasterOutputKind outputKind,
        std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr,
        bool runWhenComputeSWRasterEnabledOnly = false);
    ~ClusterSoftwareRasterizationPass();

    ClusterSoftwareRasterBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    ClusterSoftwareRasterFrame Prepare(const ClusterSoftwareRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ClusterSoftwareRasterBindings& bindings, const ClusterSoftwareRasterFrame& frame,
        org::PassRecordContext& context);

private:
    std::shared_ptr<rhi::CommandSignaturePtr> m_rasterizationCommandSignature;
    std::shared_ptr<rhi::CommandSignaturePtr> m_dynamicWindSkinCacheDispatchCommandSignature;
    org::PipelineState m_dynamicWindSkinCacheBuildPipeline;
    org::PipelineState m_dynamicWindSkinCacheSkinPipeline;
    org::PipelineState m_dynamicWindSkinCacheFinalizePipeline;
    org::PipelineState m_dynamicWindSkinCacheResolvePipeline;
    org::PipelineState m_dynamicWindSkinCacheClearPipeline;
    std::shared_ptr<org::Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_sortedToUnsortedMappingBuffer;
    // The per-view table the shader reads; it embeds the visibility UAVs, so
    // it is published during preparation from the frame's bindings.
    CLodDeclaredViewRasterTable m_viewRasterInfoTable;
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Software Raster View Raster Info"};
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheMappingBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheHashBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCachePositionsBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheAllocatorBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheWorkRecordsBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheIndirectArgsBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    bool m_declaredResourcesChanged = true;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    uint32_t m_dynamicWindSkinCacheHashEntryCount = 0u;
    uint32_t m_dynamicWindSkinCachePositionCapacity = 0u;
    // Ordered preparation owns the counter; inactive work consumes no generation.
    mutable uint32_t m_dynamicWindSkinCacheGeneration = 1u;
};
