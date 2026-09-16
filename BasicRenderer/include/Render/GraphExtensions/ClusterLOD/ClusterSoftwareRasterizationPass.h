#pragma once

#include <memory>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

struct ClusterSoftwareRasterFrameData {
    br::render::PreparedComputeIndirectSequence raster;
    bool enabled = false;
    bool hasSkinCache = false;
    uint32_t bucketCount = 0;
    std::array<uint32_t, NumMiscUintRootConstants> cacheConstants{};
    std::array<uint32_t, NumMiscUintRootConstants> clearConstants{};
    org::PreparedProgramBinding clearProgram{}, buildProgram{}, finalizeProgram{}, skinProgram{}, resolveProgram{};
    rhi::CommandSignatureHandle cacheDispatchSignature{};
    org::PreparedResourceReference cacheIndirectArgs{};
    org::PreparedResourceReference cacheAllocator{}, cacheHash{}, cacheWorkRecords{}, cachePositions{}, cacheMapping{};
};

struct ClusterSoftwareRasterBindings {
    org::ResourceBindingToken histogram, visible, transforms, mapping, viewInfo, indirectArgs;
    org::ResourceBindingToken pageTable, clipmapInfo, physicalPages, dynamicPages, telemetry;
    org::ResourceBindingToken skinMapping, skinHash, skinPositions, skinAllocator, skinWork, skinArgs, skinMembership;
    bool virtualShadow = false, hasTelemetry = false, hasSkinCache = false;
};

class ClusterSoftwareRasterizationPass
    : public org::TypedRenderGraphPass<ClusterSoftwareRasterizationPass,
          uint32_t, ClusterSoftwareRasterBindings, ClusterSoftwareRasterFrameData>,
      public IDynamicDeclaredResources {
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

    ClusterSoftwareRasterBindings Declare(org::PassBuilder& builder);
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext&) const;
    uint32_t PrepareInvocation(const ClusterSoftwareRasterFrameData&, const ClusterSoftwareRasterBindings&,
        const org::PassPrepareContext&) const;
    ClusterSoftwareRasterFrameData BuildRecipe(const ClusterSoftwareRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ClusterSoftwareRasterFrameData&, const uint32_t&,
        org::PassRecordContext&);

private:
    std::shared_ptr<rhi::CommandSignaturePtr> m_rasterizationCommandSignature;
    std::shared_ptr<rhi::CommandSignaturePtr> m_dynamicWindSkinCacheDispatchCommandSignature;
    PipelineState m_dynamicWindSkinCacheBuildPipeline;
    PipelineState m_dynamicWindSkinCacheSkinPipeline;
    PipelineState m_dynamicWindSkinCacheFinalizePipeline;
    PipelineState m_dynamicWindSkinCacheResolvePipeline;
    PipelineState m_dynamicWindSkinCacheClearPipeline;
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
    std::shared_ptr<Buffer> m_dynamicWindSkinCacheMappingBuffer;
    std::shared_ptr<Buffer> m_dynamicWindSkinCacheHashBuffer;
    std::shared_ptr<Buffer> m_dynamicWindSkinCachePositionsBuffer;
    std::shared_ptr<Buffer> m_dynamicWindSkinCacheAllocatorBuffer;
    std::shared_ptr<Buffer> m_dynamicWindSkinCacheWorkRecordsBuffer;
    std::shared_ptr<Buffer> m_dynamicWindSkinCacheIndirectArgsBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    bool m_declaredResourcesChanged = true;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    uint32_t m_dynamicWindSkinCacheHashEntryCount = 0u;
    uint32_t m_dynamicWindSkinCachePositionCapacity = 0u;
    mutable uint32_t m_dynamicWindSkinCacheGeneration = 1u;
};
