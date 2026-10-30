#include "VirtualGeometry/Rasterization/RenderPasses/ClusterRasterizationPass.h"
#include "Render/PassBuilders.h"

#include <algorithm>
#include <bit>
#include <limits>

#include <BasicTelemetry/Telemetry.h>
#include <BasicTelemetry/Tracy.h>

#include "Materials/MaterialManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Scene/Views/ViewManager.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"

namespace {
constexpr uint32_t kDeepVisibilityAverageFragmentsPerPixel = 5u;
}

ClusterRasterizationPass::ClusterRasterizationPass(
    ClusterRasterizationPassInputs inputs,
    std::shared_ptr<org::Buffer> compactedVisibleClustersBuffer,
    std::shared_ptr<org::Buffer> compactedVisibleClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
    std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
    std::shared_ptr<org::Buffer> sortedToUnsortedMappingBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityNodesBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityCounterBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityOverflowCounterBuffer,
    std::shared_ptr<org::Buffer> AVBOITConfigBuffer,
    std::shared_ptr<org::PixelBuffer> AVBOITOccupancyTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITScalarExtinctionTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITChromaticExtinctionTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITIntegratedTransmittanceTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITZeroTransmittanceSliceTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITAccumulationTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITNormalizationTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITShadingExtinctionTexture,
    std::shared_ptr<org::Buffer> visibleClustersResolveBuffer,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup,
    std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
    std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
    std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
    std::shared_ptr<org::PixelBuffer> AVBOITOccupancySliceMaskTexture,
    std::shared_ptr<org::PixelBuffer> AVBOITEarlyDepthTexture,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    std::shared_ptr<org::Buffer> sourceGroupMismatchCounterBuffer,
    std::shared_ptr<org::Buffer> sourceGroupMismatchDetailsBuffer,
    std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture)
    : m_compactedVisibleClustersBuffer(std::move(compactedVisibleClustersBuffer))
    , m_compactedVisibleClusterTransformIndicesBuffer(std::move(compactedVisibleClusterTransformIndicesBuffer))
    , m_rasterBucketsHistogramBuffer(std::move(rasterBucketsHistogramBuffer))
    , m_rasterBucketsIndirectArgsBuffer(std::move(rasterBucketsIndirectArgsBuffer))
    , m_sortedToUnsortedMappingBuffer(std::move(sortedToUnsortedMappingBuffer))
    , m_deepVisibilityNodesBuffer(std::move(deepVisibilityNodesBuffer))
    , m_deepVisibilityCounterBuffer(std::move(deepVisibilityCounterBuffer))
    , m_deepVisibilityOverflowCounterBuffer(std::move(deepVisibilityOverflowCounterBuffer))
    , m_AVBOITConfigBuffer(std::move(AVBOITConfigBuffer))
    , m_AVBOITOccupancyTexture(std::move(AVBOITOccupancyTexture))
    , m_AVBOITScalarExtinctionTexture(std::move(AVBOITScalarExtinctionTexture))
    , m_AVBOITChromaticExtinctionTexture(std::move(AVBOITChromaticExtinctionTexture))
    , m_AVBOITIntegratedTransmittanceTexture(std::move(AVBOITIntegratedTransmittanceTexture))
    , m_AVBOITZeroTransmittanceSliceTexture(std::move(AVBOITZeroTransmittanceSliceTexture))
    , m_AVBOITAccumulationTexture(std::move(AVBOITAccumulationTexture))
    , m_AVBOITNormalizationTexture(std::move(AVBOITNormalizationTexture))
    , m_AVBOITShadingExtinctionTexture(std::move(AVBOITShadingExtinctionTexture))
    , m_AVBOITEarlyDepthTexture(std::move(AVBOITEarlyDepthTexture))
    , m_AVBOITOccupancySliceMaskTexture(std::move(AVBOITOccupancySliceMaskTexture))
    , m_visibleClustersResolveBuffer(std::move(visibleClustersResolveBuffer))
    , m_virtualShadowPageTableTexture(std::move(virtualShadowPageTableTexture))
    , m_virtualShadowPhysicalPagesTexture(std::move(virtualShadowPhysicalPagesTexture))
    , m_virtualShadowDynamicPagesTexture(std::move(virtualShadowDynamicPagesTexture))
    , m_virtualShadowClipmapInfoBuffer(std::move(virtualShadowClipmapInfoBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_sourceGroupMismatchCounterBuffer(std::move(sourceGroupMismatchCounterBuffer))
    , m_sourceGroupMismatchDetailsBuffer(std::move(sourceGroupMismatchDetailsBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup)) {
    m_wireframe = inputs.wireframe;
    m_clearGbuffer = inputs.clearGbuffer;
    m_renderPhase = std::move(inputs.renderPhase);
    m_outputKind = inputs.outputKind;

    auto& settingsManager = SettingsManager::GetInstance();
    m_getPunctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting");
    m_getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
    m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();


    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 3 } } },
        {.kind = rhi::IndirectArgKind::DispatchMesh }
    };
    auto device = DeviceManager::GetInstance().GetDevice();
    m_rasterizationCommandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterizeClustersCommand) },
        PSOManager::GetInstance().GetRootSignature().GetHandle(),
        *m_rasterizationCommandSignature);
}

ClusterRasterizationPass::~ClusterRasterizationPass() = default;

ClusterRasterBindings ClusterRasterizationPass::Declare(org::PassBuilder& builder) {
    builder.ShaderResource(
            Builtin::PerObjectBuffer,
            Builtin::NormalMatrixBuffer,
            Builtin::PerMeshBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::CameraBuffer,
            Builtin::CLod::Offsets,
            Builtin::CLod::GroupChunks,
            Builtin::CLod::Groups,
			Builtin::CLod::GroupPageMap,
            Builtin::CLod::Segments,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices);
    builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    builder.IsGeometryPass();
    ClusterRasterBindings bindings;
    bindings.commandSignature = m_rasterizationCommandSignature;
    bindings.histogram = builder.ShaderResource(m_rasterBucketsHistogramBuffer);
    bindings.visible = builder.ShaderResource(m_compactedVisibleClustersBuffer);
    bindings.transforms = builder.ShaderResource(m_compactedVisibleClusterTransformIndicesBuffer);
    bindings.mapping = builder.ShaderResource(m_sortedToUnsortedMappingBuffer);
    bindings.indirectArgs = builder.IndirectArguments(m_rasterBucketsIndirectArgsBuffer);
    bindings.viewTable = org::DeclaredTableLayout<CLodViewRasterInfo>(m_viewRasterInfos.size());
    for (const auto& view : m_viewInputs) {
        if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer) {
            bindings.visibilityBuffers.push_back(builder.UnorderedAccess(view.visibility, {},
                bindings.viewTable.Field(view.camera, &CLodViewRasterInfo::visibilityUAVDescriptorIndex)));
        } else {
            bindings.visibilityBuffers.push_back(builder.ShaderResource(view.visibility, {},
                bindings.viewTable.Field(view.camera, &CLodViewRasterInfo::opaqueVisibilitySRVDescriptorIndex)));
            if (m_outputKind == CLodRasterOutputKind::DeepVisibility)
                builder.UnorderedAccess(view.headPointers, {},
                    bindings.viewTable.Field(view.camera, &CLodViewRasterInfo::deepVisibilityHeadPointerUAVDescriptorIndex));
        }
    }
    if (m_telemetryBuffer) {
        bindings.telemetry = builder.UnorderedAccess(m_telemetryBuffer);
        bindings.hasTelemetry = true;
    }
    if (m_sourceGroupMismatchCounterBuffer) bindings.mismatchCounter = builder.UnorderedAccess(m_sourceGroupMismatchCounterBuffer);
    if (m_sourceGroupMismatchDetailsBuffer) bindings.mismatchDetails = builder.UnorderedAccess(m_sourceGroupMismatchDetailsBuffer);
    bindings.hasMismatch = m_sourceGroupMismatchCounterBuffer && m_sourceGroupMismatchDetailsBuffer;
    if (m_outputKind == CLodRasterOutputKind::DeepVisibility) {
        bindings.deepVisibility = true;
        bindings.deepNodes = builder.UnorderedAccess(m_deepVisibilityNodesBuffer);
        bindings.deepCounter = builder.UnorderedAccess(m_deepVisibilityCounterBuffer);
        bindings.deepOverflow = builder.UnorderedAccess(m_deepVisibilityOverflowCounterBuffer);
    }
    if (m_outputKind == CLodRasterOutputKind::AVBOITOccupancy || m_outputKind == CLodRasterOutputKind::AVBOIT
        || m_outputKind == CLodRasterOutputKind::AVBOITShading) {
        bindings.avboit = true;
        bindings.avboitConfig = builder.ShaderResource(m_AVBOITConfigBuffer);
        if (m_visibleClustersResolveBuffer) {
            bindings.visibleResolve = builder.ShaderResource(m_visibleClustersResolveBuffer);
            bindings.hasVisibleResolve = true;
        }
    }
    if (m_outputKind == CLodRasterOutputKind::AVBOITOccupancy) {
        builder.UnorderedAccess(m_AVBOITOccupancyTexture, m_AVBOITOccupancySliceMaskTexture);
    } else if (m_outputKind == CLodRasterOutputKind::AVBOIT) {
        builder.UnorderedAccess(m_AVBOITOccupancyTexture, m_AVBOITScalarExtinctionTexture, m_AVBOITChromaticExtinctionTexture);
    } else if (m_outputKind == CLodRasterOutputKind::AVBOITShading) {
        builder.ShaderResource(
                Builtin::Light::BufferGroup,
                Builtin::Environment::PrefilteredCubemapsGroup,
                Builtin::Environment::InfoBuffer,
                Builtin::Light::ActiveLightIndices,
                Builtin::Light::InfoBuffer,
                Builtin::Light::PointLightCubemapBuffer,
                Builtin::Light::SpotLightMatrixBuffer,
                Builtin::Light::DirectionalLightCascadeBuffer,
                Builtin::Light::ClusterBuffer,
                Builtin::Light::PagesBuffer,
                Builtin::OpenPBR::FuzzLTC,
                Builtin::OpenPBR::IdealMetalEnergyComplement,
                Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
                Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
                Builtin::Noise::BlueNoise2D,
            m_AVBOITIntegratedTransmittanceTexture, m_AVBOITZeroTransmittanceSliceTexture);
        builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
        const auto target = [&](const auto& resource) { return builder.RenderTarget(resource).View(); };
        bindings.colors = {target(m_AVBOITAccumulationTexture), target(m_AVBOITNormalizationTexture), target(m_AVBOITShadingExtinctionTexture)};
        bindings.hasColors = true;
        if (m_declaredShadowsEnabled) {
            builder.ShaderResource(Builtin::Shadows::CLodClipmapInfo, Builtin::Shadows::CLodDirectionalPageViewInfo,
                Builtin::Shadows::CLodPageMetadata, Builtin::Shadows::CLodPhysicalPages,
                Builtin::Shadows::CLodCompactMainCamera, Builtin::Shadows::CLodCompactShadowCameras);
            builder.ShaderResource(Builtin::Shadows::CLodPageTable, {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
        }
        if (m_AVBOITEarlyDepthTexture) {
            bindings.depth = builder.DepthRead(m_AVBOITEarlyDepthTexture).View();
            bindings.hasDepth = true;
        }
    } else if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        bindings.virtualShadow = true;
        bindings.pageTable = builder.UnorderedAccess(m_virtualShadowPageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)});
        bindings.clipmapInfo = builder.ShaderResource(m_virtualShadowClipmapInfoBuffer);
        bindings.physicalPages = builder.UnorderedAccess(m_virtualShadowPhysicalPagesTexture);
        bindings.dynamicPages = builder.UnorderedAccess(m_virtualShadowDynamicPagesTexture);
        builder.ShaderResource(Builtin::Shadows::CLodDirectionalPageViewInfo);
        builder.UnorderedAccess(Builtin::Shadows::CLodStats);
    }
    if (m_slabResourceGroup) builder.ShaderResource(static_cast<const org::IResourceResolver&>(ResourceGroupResolver(m_slabResourceGroup)));
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    bindings.avboitShading = m_outputKind == CLodRasterOutputKind::AVBOITShading;
    bindings.singleViewVisibility = m_outputKind == CLodRasterOutputKind::VisibilityBuffer && m_viewInputs.size() == 1u;
    return bindings;
}

void ClusterRasterizationPass::Initialize() {}

void ClusterRasterizationPass::Update(const org::UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    auto numViews = context.ViewCameraBufferSize();

    uint32_t maxViewWidth = 1;
    uint32_t maxViewHeight = 1;
    uint64_t totalViewPixels = 0;
    const bool lowResolutionAVBOITRaster =
        m_outputKind == CLodRasterOutputKind::AVBOITOccupancy ||
        m_outputKind == CLodRasterOutputKind::AVBOIT;
    const auto rasterDimension = [&](uint32_t dimension) {
        return lowResolutionAVBOITRaster
            ? (dimension + CLodAVBOITDefaultDownsampleFactor - 1u) / CLodAVBOITDefaultDownsampleFactor
            : dimension;
    };

    if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        maxViewWidth = virtualShadowConfig.virtualResolution;
        maxViewHeight = maxViewWidth;
    }

    for (const auto& viewInfo : context.Views()) {

        if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
            if (viewInfo.shadow && viewInfo.lightType == Components::LightType::Directional) {
                totalViewPixels += static_cast<uint64_t>(maxViewWidth) * static_cast<uint64_t>(maxViewHeight);
            }
            continue;
        }

        if (!viewInfo.visibilityBuffer) continue;

        if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer) {
            maxViewWidth = std::max(maxViewWidth, viewInfo.visibilityBuffer->GetWidth());
            maxViewHeight = std::max(maxViewHeight, viewInfo.visibilityBuffer->GetHeight());
        }
        else if (m_outputKind == CLodRasterOutputKind::DeepVisibility) {
            auto headPointers = viewInfo.deepVisibilityHeadPointers;
            if (!headPointers) {
                continue;
            }

            maxViewWidth = std::max(maxViewWidth, headPointers->GetWidth());
            maxViewHeight = std::max(maxViewHeight, headPointers->GetHeight());
            totalViewPixels += static_cast<uint64_t>(headPointers->GetWidth()) *
                static_cast<uint64_t>(headPointers->GetHeight());
        }
        else {
            maxViewWidth = std::max(maxViewWidth, rasterDimension(viewInfo.visibilityBuffer->GetWidth()));
            maxViewHeight = std::max(maxViewHeight, rasterDimension(viewInfo.visibilityBuffer->GetHeight()));
        }
    }

    std::vector<CLodViewRasterInfo> table(numViews);
    std::vector<ViewInput> viewInputs;
    for (const auto& viewInfo : context.Views()) {
        auto cameraIndex = viewInfo.cameraBufferIndex;
        if (cameraIndex >= table.size()) continue;
        CLodViewRasterInfo info{};
        info.scissorMinX = 0;
        info.scissorMinY = 0;

        if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
            if (viewInfo.shadow && viewInfo.lightType == Components::LightType::Directional) {
                info.scissorMaxX = maxViewWidth;
                info.scissorMaxY = maxViewHeight;
                info.viewportScaleX = 1.0f;
                info.viewportScaleY = 1.0f;
            }
            table[cameraIndex] = info;
            continue;
        }

        if (!viewInfo.visibilityBuffer) continue;

        if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer) {
            info.scissorMaxX = viewInfo.visibilityBuffer->GetWidth();
            info.scissorMaxY = viewInfo.visibilityBuffer->GetHeight();
        }
        else if (m_outputKind == CLodRasterOutputKind::DeepVisibility) {
            auto headPointers = viewInfo.deepVisibilityHeadPointers;
            if (!headPointers) {
                table[cameraIndex] = info;
                continue;
            }

            info.scissorMaxX = headPointers->GetWidth();
            info.scissorMaxY = headPointers->GetHeight();
        }
        else {
            info.scissorMaxX = rasterDimension(viewInfo.visibilityBuffer->GetWidth());
            info.scissorMaxY = rasterDimension(viewInfo.visibilityBuffer->GetHeight());
        }

        viewInputs.push_back({cameraIndex, viewInfo.visibilityBuffer,
            m_outputKind == CLodRasterOutputKind::DeepVisibility ? viewInfo.deepVisibilityHeadPointers : nullptr});
        info.viewportScaleX = static_cast<float>(info.scissorMaxX) / static_cast<float>(maxViewWidth);
        info.viewportScaleY = static_cast<float>(info.scissorMaxY) / static_cast<float>(maxViewHeight);
        table[cameraIndex] = info;
    }

    m_passWidth = maxViewWidth;
    m_passHeight = maxViewHeight;
    if (m_outputKind == CLodRasterOutputKind::DeepVisibility) {
        const uint64_t maxNodes = totalViewPixels * kDeepVisibilityAverageFragmentsPerPixel;
        m_deepVisibilityNodeCapacity = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(std::min<uint64_t>(maxNodes, std::numeric_limits<uint32_t>::max())));
        if (m_deepVisibilityNodesBuffer) {
            m_deepVisibilityNodesBuffer->ResizeStructured(m_deepVisibilityNodeCapacity);
        }
    }
    else {
        m_deepVisibilityNodeCapacity = 1u;
    }

    const bool shadowsEnabled = m_outputKind == CLodRasterOutputKind::AVBOITShading
        && m_getShadowsEnabled && m_getShadowsEnabled();
    const bool resourcesChanged = m_declaredShadowsEnabled != shadowsEnabled;
    m_declaredShadowsEnabled = shadowsEnabled;

    // Rows carry no descriptors: those are resolved when Prepare publishes
    // the table, so a resource moving to a new backing needs no redeclaration.
    m_declaredResourcesChanged = resourcesChanged || m_viewInputs != viewInputs || m_viewRasterInfos.size() != table.size();
    m_viewInputs = std::move(viewInputs);
    m_viewRasterInfos = std::move(table);
}

bool ClusterRasterizationPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

ClusterRasterFrame ClusterRasterizationPass::Prepare(
    const ClusterRasterBindings& bindings, const org::PassPrepareContext& preparation) const {
    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer &&
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDisableNonVoxelVisibilitySettingName)())
        return {};
    const auto* context = preparation.preparationData
        ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    if (!context) throw std::logic_error("CLod raster preparation requires the owned render snapshot");
    ClusterRasterFrame frame{};
    const bool phase1 = m_rasterBucketsIndirectArgsBuffer->GetName().find("HW phase1") != std::string::npos;
    frame.phase1VisibilityDiagnostics = phase1;
    const auto argumentHandle =
        preparation.ResolveCapturedResource(preparation.CaptureResource(bindings.indirectArgs)).GetHandle();
    BT_PLOT("CLod.RasterArgs.ConsumerResourceIndex", static_cast<int64_t>(argumentHandle.index));
    BT_PLOT("CLod.RasterArgs.ConsumerResourceGeneration", static_cast<int64_t>(argumentHandle.generation));
    if (phase1) {
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.ConsumerResourceIndex",
            static_cast<int64_t>(argumentHandle.index));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.ConsumerResourceGeneration",
            static_cast<int64_t>(argumentHandle.generation));
        BT_PLOT("CLod.RasterArgs.PrimaryConsumerResourceIndex", static_cast<int64_t>(argumentHandle.index));
        BT_PLOT("CLod.RasterArgs.PrimaryConsumerResourceGeneration", static_cast<int64_t>(argumentHandle.generation));
    }
    frame.width = m_passWidth;
    frame.height = m_passHeight;
    if (bindings.hasColors && m_AVBOITAccumulationTexture && m_AVBOITNormalizationTexture && m_AVBOITShadingExtinctionTexture) {
        for (uint32_t i = 0; i < 3; ++i) frame.colorClears[i] = preparation.ClearValue(bindings.colors[i].Resource());
        if (bindings.hasDepth) frame.depthClear = preparation.ClearValue(bindings.depth.Resource());
    }
    if (bindings.avboitShading) {
        frame.shadows = context->lighting.shadowsEnabled;
        frame.punctualLights = context->lighting.punctualLightingEnabled;
        frame.gtao = context->lighting.gtaoEnabled;
    }
    // The view table and the single-view visibility UAV are resolved against
    // this frame's bindings (see CLodViewTables.h).
    frame.viewRasterInfoTable = bindings.viewTable.Publish(preparation, m_viewRasterInfoPublisher, m_viewRasterInfos);
    if (phase1) {
        const auto validView = std::ranges::find_if(m_viewRasterInfos,
            [](const CLodViewRasterInfo& info) { return info.scissorMaxX != 0u; });
        if (validView != m_viewRasterInfos.end()) {
            if (!m_viewInputs.empty())
                basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PrimaryVisibilityDescriptorIndex",
                    static_cast<int64_t>(preparation.Resolve(bindings.visibilityBuffers.front()).index));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PrimaryVisibilityWidth",
                static_cast<int64_t>(validView->scissorMaxX));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PrimaryVisibilityHeight",
                static_cast<int64_t>(validView->scissorMaxY));
        }
        if (bindings.visibilityBuffers.size() == 1u) {
            const auto frozenVisibility = bindings.singleViewVisibility
                ? preparation.Resolve(bindings.visibilityBuffers.front()).index : 0xFFFFFFFFu;
            const auto resourceHeap = context->textureDescriptorHeap.GetHandle();
            basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.View.Visibility.Frozen",
                static_cast<int64_t>(frozenVisibility));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PreparedResourceHeapIndex",
                static_cast<int64_t>(resourceHeap.index));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PreparedResourceHeapGeneration",
                static_cast<int64_t>(resourceHeap.generation));
        }
    }
    frame.telemetryEnabled = IsCLodWorkGraphTelemetryEnabled();
    if (bindings.virtualShadow) {
        const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
        frame.virtualShadowPageTableResolution = config.pageTableResolution;
        frame.virtualShadowVirtualResolution = config.virtualResolution;
    }
    frame.deepVisibilityNodeCapacity = m_deepVisibilityNodeCapacity;
    const auto numBuckets = context->preparedRasterBucketCount;
    BT_PLOT("CLod.RasterArgs.PreparedBucketCount", static_cast<int64_t>(numBuckets));
    BT_PLOT("CLod.RasterArgs.PreparedBackingBytes", static_cast<int64_t>(m_rasterBucketsIndirectArgsBuffer->GetSize()));
    frame.steps.reserve(numBuckets);
    uint32_t missingPrograms = 0u;
    uint32_t commandLayoutMismatches = 0u;
    const auto commandLayout = PSOManager::GetInstance().GetRootSignature().GetHandle();
    for (uint32_t i = 0; i < numBuckets; ++i) {
        const auto flags = context->preparedRasterBucketFlags.at(i);
        const org::PipelineState* pso = m_outputKind == CLodRasterOutputKind::VisibilityBuffer
            ? PSOManager::GetInstance().TryGetClusterLODRasterPSO(flags, m_wireframe, m_viewInputs.size() == 1u)
            : m_outputKind == CLodRasterOutputKind::VirtualShadow
                ? PSOManager::GetInstance().TryGetClusterLODVirtualShadowRasterPSO(flags, m_wireframe)
            : m_outputKind == CLodRasterOutputKind::AVBOITOccupancy
                ? PSOManager::GetInstance().TryGetClusterLODAVBOITOccupancyPSO(flags, m_wireframe)
            : m_outputKind == CLodRasterOutputKind::AVBOIT
                ? PSOManager::GetInstance().TryGetClusterLODAVBOITRasterPSO(flags, m_wireframe)
            : m_outputKind == CLodRasterOutputKind::AVBOITShading
                ? PSOManager::GetInstance().TryGetClusterLODAVBOITShadePSO(flags, m_wireframe, context->globalPSOFlags)
            : PSOManager::GetInstance().TryGetClusterLODDeepVisibilityRasterPSO(flags, m_wireframe);
        if (!pso) {
            ++missingPrograms;
            continue;
        }
        if (const auto payload = pso->GetPayload()) {
            if (payload->layout.index != commandLayout.index ||
                payload->layout.generation != commandLayout.generation) {
                ++commandLayoutMismatches;
            }
            if (phase1) {
                basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.CommandLayoutIndex",
                    static_cast<int64_t>(commandLayout.index));
                basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.CommandLayoutGeneration",
                    static_cast<int64_t>(commandLayout.generation));
                basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.PipelineLayoutIndex",
                    static_cast<int64_t>(payload->layout.index));
                basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.PipelineLayoutGeneration",
                    static_cast<int64_t>(payload->layout.generation));
            }
        }
        frame.steps.push_back({*pso, static_cast<uint64_t>(i) * sizeof(RasterizeClustersCommand)});
    }
    BT_PLOT("CLod.RasterArgs.PreparedHardwareSteps",
        static_cast<int64_t>(frame.steps.size()));
    BT_PLOT("CLod.RasterArgs.MissingHardwarePrograms",
        static_cast<int64_t>(missingPrograms));
    if (phase1) {
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.CommandLayoutMismatches",
            static_cast<int64_t>(commandLayoutMismatches));
    }
    return frame;
}

void ClusterRasterizationPass::Record(const ClusterRasterBindings& bindings, const ClusterRasterFrame& frame,
    org::PassRecordContext& context) {
    // Preparation may legitimately produce no draws (for example an empty
    // culling result). Graph-owned barriers still execute around this pass,
    // but it emits no render-pass or root-signature commands.
    if (frame.steps.empty()) return;
    if (frame.phase1VisibilityDiagnostics) {
        const auto arguments = context.Resolve(bindings.indirectArgs).GetHandle();
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.RecordedSteps",
            static_cast<int64_t>(frame.steps.size()));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.RecordedArgumentResourceIndex",
            static_cast<int64_t>(arguments.index));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.RecordedArgumentResourceGeneration",
            static_cast<int64_t>(arguments.generation));
        if (const auto* payload = frame.steps.front().pipeline.PeekPayload()) {
            const auto firstPipeline = payload->pso->GetHandle();
            basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.RecordedFirstPipelineIndex",
                static_cast<int64_t>(firstPipeline.index));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.RecordedFirstPipelineGeneration",
                static_cast<int64_t>(firstPipeline.generation));
        }
    }

    br::render::MiscConstants misc{};
    if (bindings.avboitShading) {
        misc[MiscEnableShadows] = frame.shadows;
        misc[MiscEnablePunctualLights] = frame.punctualLights;
        misc[MiscEnableGTAO] = frame.gtao;
    }
    misc[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = context.Index(bindings.visible);
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = context.Index(bindings.transforms);
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = frame.viewRasterInfoTable;
    misc[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] = context.Index(bindings.mapping);
    misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    if (bindings.singleViewVisibility)
        misc[CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX] = context.Index(bindings.visibilityBuffers.front());
    if (bindings.hasTelemetry && frame.telemetryEnabled)
        misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = context.Index(bindings.telemetry);
    if (bindings.hasMismatch) {
        misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.mismatchCounter);
        misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX] = context.Index(bindings.mismatchDetails);
    }
    if (bindings.virtualShadow) {
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] = context.Index(bindings.pageTable);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = context.Index(bindings.clipmapInfo);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = context.Index(bindings.physicalPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = context.Index(bindings.dynamicPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = frame.virtualShadowPageTableResolution;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = frame.virtualShadowVirtualResolution;
    }
    if (bindings.deepVisibility) {
        misc[CLOD_RASTER_DEEP_VISIBILITY_NODE_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.deepNodes);
        misc[CLOD_RASTER_DEEP_VISIBILITY_NODE_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.deepCounter);
        misc[CLOD_RASTER_DEEP_VISIBILITY_OVERFLOW_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.deepOverflow);
        misc[CLOD_RASTER_DEEP_VISIBILITY_NODE_CAPACITY] = frame.deepVisibilityNodeCapacity;
    }
    if (bindings.avboit) {
        misc[CLOD_RASTER_AVBOIT_VBOIT_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.avboitConfig);
        misc[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = bindings.hasVisibleResolve
            ? context.Index(bindings.visibleResolve) : 0xFFFFFFFFu;
        misc[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    }

    std::array<rhi::ColorAttachment, 3> colors{};
    rhi::DepthAttachment depth{};
    rhi::PassBeginInfo pass{};
    if (bindings.hasColors) {
        for (uint32_t i = 0; i < 3; ++i) {
            colors[i].rtv = context.View(bindings.colors[i]);
            colors[i].loadOp = rhi::LoadOp::Load;
            colors[i].storeOp = rhi::StoreOp::Store;
            colors[i].clear = frame.colorClears[i];
        }
        pass.colors = {colors.data(), static_cast<uint32_t>(colors.size())};
        if (bindings.hasDepth) {
            depth.dsv = context.View(bindings.depth);
            depth.depthLoad = rhi::LoadOp::Load; depth.depthStore = rhi::StoreOp::Store;
            depth.stencilLoad = rhi::LoadOp::DontCare; depth.stencilStore = rhi::StoreOp::DontCare;
            depth.clear = frame.depthClear; depth.readOnly = true;
            pass.depth = &depth;
        }
    }
    pass.width = frame.width; pass.height = frame.height; pass.debugName = "CLod raster pass";
    auto& commands = context.Commands();
    commands.BeginPass(pass);
    commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
    for (const auto& step : frame.steps) {
        // Root arguments belong to the bound layout: push them after every bind.
        context.BindGraphicsPipeline(step.pipeline);
        br::render::PushMiscConstants(context, misc);
        context.ExecuteIndirect(bindings.commandSignature, bindings.indirectArgs, step.argumentsOffset);
    }
    commands.EndPass();
}
