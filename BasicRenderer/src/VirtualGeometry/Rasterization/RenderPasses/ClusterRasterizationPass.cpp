#include "VirtualGeometry/Rasterization/RenderPasses/ClusterRasterizationPass.h"

#include <algorithm>
#include <bit>
#include <limits>

#include <BasicTelemetry/Telemetry.h>

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
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedRenderIndirect.h"
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

    // Rows carry no descriptors: those are resolved when the recipe publishes
    // the table, so a resource moving to a new backing needs no redeclaration.
    m_declaredResourcesChanged = resourcesChanged || m_viewInputs != viewInputs || m_viewRasterInfos.size() != table.size();
    m_viewInputs = std::move(viewInputs);
    m_viewRasterInfos = std::move(table);
}

bool ClusterRasterizationPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

br::render::PreparedRenderIndirectSequence ClusterRasterizationPass::BuildRecipe(
    const ClusterRasterBindings& bindings, const org::PassPrepareContext& preparation) const {
    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer &&
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDisableNonVoxelVisibilitySettingName)())
        return {};
    const auto* context = preparation.preparationData
        ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    if (!context) throw std::logic_error("CLod raster preparation requires the owned render snapshot");
    br::render::PreparedRenderIndirectSequence data{};
    data.phase1VisibilityDiagnostics =
        m_rasterBucketsIndirectArgsBuffer->GetName().find("HW phase1") != std::string::npos;
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_rasterizationCommandSignature);
    data.arguments = preparation.CaptureResource(bindings.indirectArgs);
    const auto argumentHandle = preparation.ResolveCapturedResource(data.arguments).GetHandle();
    BT_PLOT("CLod.RasterArgs.ConsumerResourceIndex", static_cast<int64_t>(argumentHandle.index));
    BT_PLOT("CLod.RasterArgs.ConsumerResourceGeneration", static_cast<int64_t>(argumentHandle.generation));
    if (m_rasterBucketsIndirectArgsBuffer->GetName().find("HW phase1") != std::string::npos) {
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.ConsumerResourceIndex",
            static_cast<int64_t>(argumentHandle.index));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.ConsumerResourceGeneration",
            static_cast<int64_t>(argumentHandle.generation));
        BT_PLOT("CLod.RasterArgs.PrimaryConsumerResourceIndex", static_cast<int64_t>(argumentHandle.index));
        BT_PLOT("CLod.RasterArgs.PrimaryConsumerResourceGeneration", static_cast<int64_t>(argumentHandle.generation));
    }
    const auto srv = [&](org::DeclaredViewToken token) {
        return preparation.Resolve(token).index;
    };
    const auto uav = [&](org::DeclaredViewToken token) {
        return preparation.Resolve(token).index;
    };
    data.width = m_passWidth; data.height = m_passHeight; data.debugName = "CLod raster pass";
    if (m_outputKind == CLodRasterOutputKind::AVBOITShading && m_AVBOITAccumulationTexture
        && m_AVBOITNormalizationTexture && m_AVBOITShadingExtinctionTexture) {
        data.colorCount = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            data.colors[i].rtv = preparation.Resolve(bindings.colors[i]);
            data.colors[i].loadOp = rhi::LoadOp::Load;
            data.colors[i].storeOp = rhi::StoreOp::Store;
            data.colors[i].clear = preparation.ClearValue(bindings.colors[i].Resource());
        }
        if (bindings.hasDepth) {
            data.hasDepth = true;
            data.depth.dsv = preparation.Resolve(bindings.depth);
            data.depth.depthLoad = rhi::LoadOp::Load; data.depth.depthStore = rhi::StoreOp::Store;
            data.depth.stencilLoad = rhi::LoadOp::DontCare; data.depth.stencilStore = rhi::StoreOp::DontCare;
            data.depth.clear = preparation.ClearValue(bindings.depth.Resource()); data.depth.readOnly = true;
        }
    }
    auto& misc = data.constants;
    if (m_outputKind == CLodRasterOutputKind::AVBOITShading) {
        misc[MiscEnableShadows] = context->lighting.shadowsEnabled;
        misc[MiscEnablePunctualLights] = context->lighting.punctualLightingEnabled;
        misc[MiscEnableGTAO] = context->lighting.gtaoEnabled;
    }
    // The view table and the single-view visibility UAV below are resolved
    // against this frame's bindings (see CLodViewTables.h).
    misc[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] =
        srv(bindings.histogram);
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] =
        srv(bindings.visible);
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
        srv(bindings.transforms);
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] =
        bindings.viewTable.Publish(preparation, m_viewRasterInfoPublisher, m_viewRasterInfos);
    misc[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] =
        srv(bindings.mapping);
    if (data.phase1VisibilityDiagnostics) {
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
    }
    misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer && m_viewInputs.size() == 1u)
        misc[CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.visibilityBuffers.front()).index;
    if (data.phase1VisibilityDiagnostics && bindings.visibilityBuffers.size() == 1u) {
        const auto frozenVisibility = misc[CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX];
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.View.Visibility.Frozen",
            static_cast<int64_t>(frozenVisibility));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PreparedResourceHeapIndex",
            static_cast<int64_t>(data.resourceHeap.index));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PreparedResourceHeapGeneration",
            static_cast<int64_t>(data.resourceHeap.generation));
    }
    if (bindings.hasTelemetry && IsCLodWorkGraphTelemetryEnabled())
        misc[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    if (bindings.hasMismatch) {
        misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.mismatchCounter);
        misc[CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX] = uav(bindings.mismatchDetails);
    }
    if (bindings.virtualShadow) {
        const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] =
            uav(bindings.pageTable);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = uav(bindings.physicalPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = uav(bindings.dynamicPages);
        misc[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        misc[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = config.virtualResolution;
    }
    if (bindings.deepVisibility) {
        misc[CLOD_RASTER_DEEP_VISIBILITY_NODE_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.deepNodes);
        misc[CLOD_RASTER_DEEP_VISIBILITY_NODE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.deepCounter);
        misc[CLOD_RASTER_DEEP_VISIBILITY_OVERFLOW_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.deepOverflow);
        misc[CLOD_RASTER_DEEP_VISIBILITY_NODE_CAPACITY] = m_deepVisibilityNodeCapacity;
    }
    if (bindings.avboit) {
        misc[CLOD_RASTER_AVBOIT_VBOIT_CONFIG_DESCRIPTOR_INDEX] = srv(bindings.avboitConfig);
        misc[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = bindings.hasVisibleResolve
            ? srv(bindings.visibleResolve) : 0xFFFFFFFFu;
        misc[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    }
    const auto numBuckets = context->preparedRasterBucketCount;
    BT_PLOT("CLod.RasterArgs.PreparedBucketCount", static_cast<int64_t>(numBuckets));
    BT_PLOT("CLod.RasterArgs.PreparedBackingBytes", static_cast<int64_t>(m_rasterBucketsIndirectArgsBuffer->GetSize()));
    data.steps.reserve(numBuckets);
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
            if (m_rasterBucketsIndirectArgsBuffer->GetName().find("HW phase1") != std::string::npos) {
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
        br::render::PreparedRenderIndirectSequence::Step step{};
        step.program = preparation.CaptureProgramBinding(*pso);
        step.argumentsOffset = static_cast<uint64_t>(i) * sizeof(RasterizeClustersCommand);
        data.steps.push_back(std::move(step));
    }
    BT_PLOT("CLod.RasterArgs.PreparedHardwareSteps",
        static_cast<int64_t>(data.steps.size()));
    BT_PLOT("CLod.RasterArgs.MissingHardwarePrograms",
        static_cast<int64_t>(missingPrograms));
    if (m_rasterBucketsIndirectArgsBuffer->GetName().find("HW phase1") != std::string::npos) {
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.CommandLayoutMismatches",
            static_cast<int64_t>(commandLayoutMismatches));
    }
    return data;
}

std::vector<uint64_t> ClusterRasterizationPass::RecipeRevision(const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    std::vector<uint64_t> revision{SettingsManager::GetInstance().Revision(), m_passWidth, m_passHeight,
        m_wireframe, m_viewInputs.size(), context->preparedRasterBucketCount,
        context->lighting.shadowsEnabled, context->lighting.punctualLightingEnabled, context->lighting.gtaoEnabled,
        reinterpret_cast<uintptr_t>(m_rasterizationCommandSignature.get())};
    for (const auto& row : m_viewRasterInfos) {
        revision.insert(revision.end(), {row.scissorMinX, row.scissorMinY, row.scissorMaxX, row.scissorMaxY,
            std::bit_cast<uint32_t>(row.viewportScaleX), std::bit_cast<uint32_t>(row.viewportScaleY)});
    }
    for (uint32_t i = 0; i < context->preparedRasterBucketCount; ++i) {
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
        revision.push_back(static_cast<uint64_t>(flags));
        revision.push_back(reinterpret_cast<uintptr_t>(pso ? pso->PeekPayload() : nullptr));
    }
    return revision;
}
