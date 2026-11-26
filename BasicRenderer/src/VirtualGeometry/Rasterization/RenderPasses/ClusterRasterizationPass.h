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
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"
#include "BasicRenderer/Extensions/RenderContext.h"
namespace org { class RenderGraph; }
struct UpdateContext;
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/DeclaredTableLayout.h"
#include "Resources/PixelBuffer.h"
#include <BasicRenderer/Pipeline/PipelineDesc.h>

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
    br::CommandSignatureState commandSignature;
    org::DeclaredViewToken histogram, visible, transforms, mapping;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken telemetry, mismatchCounter, mismatchDetails;
    org::DeclaredViewToken pageTable, clipmapInfo, physicalPages, dynamicPages;
    org::DeclaredViewToken deepNodes, deepCounter, deepOverflow;
    org::DeclaredViewToken avboitConfig, visibleResolve;
    std::array<org::DeclaredViewToken, 3> colors{};
    org::DeclaredViewToken depth;
    std::vector<org::DeclaredViewToken> visibilityBuffers;
    CLodViewRasterDeclaration viewTable;
    bool hasTelemetry = false, hasMismatch = false, virtualShadow = false;
    bool deepVisibility = false, avboit = false, hasVisibleResolve = false, hasDepth = false;
    bool hasColors = false, avboitShading = false, singleViewVisibility = false;
};

// One indirect mesh dispatch per raster bucket with a resolved pipeline.
struct ClusterRasterStep {
    br::PipelineState pipeline;
    uint64_t argumentsOffset = 0;
    bool operator==(const ClusterRasterStep& other) const {
        return pipeline.PeekPayload() == other.pipeline.PeekPayload() && argumentsOffset == other.argumentsOffset;
    }
};
struct ClusterRasterFrame {
    std::vector<ClusterRasterStep> steps;
    std::array<rhi::ClearValue, 3> colorClears{};
    rhi::ClearValue depthClear{};
    uint32_t width = 1, height = 1;
    uint32_t viewRasterInfoTable = 0;
    uint32_t deepVisibilityNodeCapacity = 1;
    uint32_t virtualShadowPageTableResolution = 0, virtualShadowVirtualResolution = 0;
    bool shadows = false, punctualLights = false;
    bool telemetryEnabled = false;
    bool phase1VisibilityDiagnostics = false;
    bool operator==(const ClusterRasterFrame& other) const;
};

class ClusterRasterizationPass
    : public org::TypedRenderGraphPass<ClusterRasterizationPass,
          ClusterRasterFrame, ClusterRasterBindings>,
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
    ClusterRasterFrame Prepare(const ClusterRasterBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const ClusterRasterBindings& bindings, const ClusterRasterFrame& frame,
        org::PassRecordContext& context);
    // Kept recordings (M6 step 8): the frame's buckets and their pipelines, settings and lighting values, taken from the
    // host data before Update (RenderGraphPass::TakeFrameValues) and read by Prepare. Revision names them with what Update
    // took (the views, the pass size).
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override;
    void Revision(std::vector<uint64_t>& out) const;

private:
    bool m_wireframe = false;
    bool m_meshShaders = false;
    bool m_clearGbuffer = true;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;

    std::vector<CLodViewRasterInfo> m_viewRasterInfos; // Rows without descriptors (change detection).
    std::vector<uint32_t> ViewInputCameras() const;
    // Takes this frame's views; returns whether what the pass declares changed.
    bool UpdateViews(const UpdateContext& context);
    // Built by Update from the view snapshot; its descriptors are resolved and
    // the table published when the frame is prepared.
    struct ViewInput {
        uint32_t camera;
        std::shared_ptr<org::PixelBuffer> visibility, headPointers;
        bool operator==(const ViewInput&) const = default;
    };
    std::vector<ViewInput> m_viewInputs;
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Raster View Raster Info"};

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


    uint32_t m_passWidth = 1;
    uint32_t m_passHeight = 1;
    uint32_t m_deepVisibilityNodeCapacity = 1;
    bool m_declaredResourcesChanged = true;
    bool m_declaredShadowsEnabled = false;
    std::function<bool()> m_getPunctualLightingEnabled;
    std::function<bool()> m_getShadowsEnabled;

    RenderPhase m_renderPhase;

    // What TakeFrameValues took for this run.
    struct Taken {
        std::vector<ClusterRasterStep> steps;
        uint32_t bucketCount = 0;
        uint32_t missingPrograms = 0;
        uint32_t virtualShadowPageTableResolution = 0, virtualShadowVirtualResolution = 0;
        uint32_t globalPSOFlags = 0;
        ShadowingMode shadowingMode = ShadowingMode::None;
        bool punctualLights = false;
        bool telemetryEnabled = false;
        bool disabled = false;
    };
    Taken m_taken;
    bool m_declaredSingleViewVisibility = false;
};
