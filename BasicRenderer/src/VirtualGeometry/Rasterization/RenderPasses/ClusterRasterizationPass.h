#pragma once

#include <functional>
#include <array>
#include <memory>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <rhi.h>

#include "BuiltinRenderPasses.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "BasicRenderer/Pipeline/RenderPhase.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedRenderIndirect.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "Resources/PixelBuffer.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ClusterRasterizationPassInputs {
    bool wireframe;
    bool clearGbuffer;
    RenderPhase renderPhase;
    CLodRasterOutputKind outputKind = CLodRasterOutputKind::VisibilityBuffer;

    RG_DEFINE_PASS_INPUTS(ClusterRasterizationPassInputs, &ClusterRasterizationPassInputs::wireframe, &ClusterRasterizationPassInputs::clearGbuffer, &ClusterRasterizationPassInputs::renderPhase, &ClusterRasterizationPassInputs::outputKind);
};

struct ClusterRasterBindings {
    org::ResourceBindingToken histogram, visible, transforms, mapping, indirectArgs;
    org::ResourceBindingToken telemetry, mismatchCounter, mismatchDetails;
    org::ResourceBindingToken pageTable, clipmapInfo, physicalPages, dynamicPages;
    org::ResourceBindingToken deepNodes, deepCounter, deepOverflow;
    org::ResourceBindingToken avboitConfig, visibleResolve;
    std::array<org::ResourceBindingToken, 3> colors{};
    org::ResourceBindingToken depth;
    std::vector<org::ResourceBindingToken> visibilityBuffers;
    bool hasTelemetry = false, hasMismatch = false, virtualShadow = false;
    bool deepVisibility = false, avboit = false, hasVisibleResolve = false, hasDepth = false;
};

class ClusterRasterizationPass
    : public org::TypedRenderGraphPass<ClusterRasterizationPass,
          org::EmptyPassFrameData, ClusterRasterBindings, br::render::PreparedRenderIndirectSequence>,
      public org::IDynamicDeclaredResources {
public:
    ClusterRasterizationPass(
        ClusterRasterizationPassInputs inputs,
        std::shared_ptr<org::Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> sortedToUnsortedMappingBuffer,
        std::shared_ptr<org::Buffer> deepVisibilityNodesBuffer = nullptr,
        std::shared_ptr<org::Buffer> deepVisibilityCounterBuffer = nullptr,
        std::shared_ptr<org::Buffer> deepVisibilityOverflowCounterBuffer = nullptr,
        std::shared_ptr<org::Buffer> AVBOITConfigBuffer = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITOccupancyTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITScalarExtinctionTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITChromaticExtinctionTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITIntegratedTransmittanceTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITZeroTransmittanceSliceTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITAccumulationTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITNormalizationTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITShadingExtinctionTexture = nullptr,
        std::shared_ptr<org::Buffer> visibleClustersResolveBuffer = nullptr,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr,
          std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture = nullptr,
          std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture = nullptr,
          std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITOccupancySliceMaskTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> AVBOITEarlyDepthTexture = nullptr,
          std::shared_ptr<org::Buffer> telemetryBuffer = nullptr,
          std::shared_ptr<org::Buffer> sourceGroupMismatchCounterBuffer = nullptr,
          std::shared_ptr<org::Buffer> sourceGroupMismatchDetailsBuffer = nullptr,
          std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture = nullptr);
    ~ClusterRasterizationPass();

    ClusterRasterBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext&) const;
    org::EmptyPassFrameData PrepareInvocation(const br::render::PreparedRenderIndirectSequence&,
        const ClusterRasterBindings&, const org::PassPrepareContext&) const { return {}; }
    br::render::PreparedRenderIndirectSequence BuildRecipe(const ClusterRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const br::render::PreparedRenderIndirectSequence& data, const org::EmptyPassFrameData&,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedRenderIndirectSequence(data, recording);
    }

private:
    bool m_wireframe = false;
    bool m_meshShaders = false;
    bool m_clearGbuffer = true;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;

    std::vector<CLodViewRasterInfo> m_viewRasterInfos; // Rows without descriptors (change detection).
    // Built by Update from the view snapshot; its descriptors are resolved and
    // the table published when the recipe is built.
    CLodViewRasterInfoTable m_viewRasterInfoTable;
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Raster View Raster Info"};
    std::vector<std::shared_ptr<org::PixelBuffer>> m_visibilityBuffers;
    std::vector<std::shared_ptr<org::PixelBuffer>> m_deepVisibilityHeadPointerBuffers;

    std::shared_ptr<org::Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_sortedToUnsortedMappingBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityNodesBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<org::Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<org::Buffer> m_AVBOITConfigBuffer;
    std::shared_ptr<org::PixelBuffer> m_AVBOITOccupancyTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITScalarExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITChromaticExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITIntegratedTransmittanceTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITZeroTransmittanceSliceTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITAccumulationTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITNormalizationTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITShadingExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITEarlyDepthTexture;
    std::shared_ptr<org::PixelBuffer> m_AVBOITOccupancySliceMaskTexture;
    std::shared_ptr<org::Buffer> m_visibleClustersResolveBuffer;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::Buffer> m_sourceGroupMismatchCounterBuffer;
    std::shared_ptr<org::Buffer> m_sourceGroupMismatchDetailsBuffer;

    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;

    std::shared_ptr<rhi::CommandSignaturePtr> m_rasterizationCommandSignature;

    uint32_t m_passWidth = 1;
    uint32_t m_passHeight = 1;
    uint32_t m_deepVisibilityNodeCapacity = 1;
    bool m_declaredResourcesChanged = true;
    std::function<bool()> m_getPunctualLightingEnabled;
    std::function<bool()> m_getShadowsEnabled;
    bool m_gtaoEnabled = false;

    RenderPhase m_renderPhase;
};
