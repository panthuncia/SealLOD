#include "VirtualGeometry/Rasterization/RenderPasses/ClusterRasterizationPass.h"

#include <algorithm>
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

ClusterRasterBindings ClusterRasterizationPass::Declare(org::PassBuilder& declaration) {
    auto* builder = &declaration;
    builder->WithShaderResource(
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
            Builtin::CLod::AssemblyBoneRemapIndices,
            m_compactedVisibleClustersBuffer,
            m_compactedVisibleClusterTransformIndicesBuffer,
            m_rasterBucketsHistogramBuffer,
            m_sortedToUnsortedMappingBuffer)
        .WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer)
        .IsGeometryPass();
    ClusterRasterBindings bindings{
        builder->BindShaderResource(m_rasterBucketsHistogramBuffer),
        builder->BindShaderResource(m_compactedVisibleClustersBuffer),
        builder->BindShaderResource(m_compactedVisibleClusterTransformIndicesBuffer),
        builder->BindShaderResource(m_sortedToUnsortedMappingBuffer),
        builder->BindIndirectArguments(m_rasterBucketsIndirectArgsBuffer)};

    if (m_telemetryBuffer) {
        builder->WithUnorderedAccess(m_telemetryBuffer);
        bindings.telemetry = builder->BindUnorderedAccess(m_telemetryBuffer);
        bindings.hasTelemetry = true;
    }
    if (m_sourceGroupMismatchCounterBuffer) {
        builder->WithUnorderedAccess(m_sourceGroupMismatchCounterBuffer);
    }
    if (m_sourceGroupMismatchDetailsBuffer) {
        builder->WithUnorderedAccess(m_sourceGroupMismatchDetailsBuffer);
    }
    if (m_sourceGroupMismatchCounterBuffer && m_sourceGroupMismatchDetailsBuffer) {
        bindings.mismatchCounter = builder->BindUnorderedAccess(m_sourceGroupMismatchCounterBuffer);
        bindings.mismatchDetails = builder->BindUnorderedAccess(m_sourceGroupMismatchDetailsBuffer);
        bindings.hasMismatch = true;
    }

    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer) {
        for (auto& vb : m_visibilityBuffers) {
            builder->WithUnorderedAccess(vb);
            bindings.visibilityBuffers.push_back(builder->BindUnorderedAccess(vb));
        }
    }
    else if (m_outputKind == CLodRasterOutputKind::DeepVisibility) {
        bindings.deepVisibility = true;
        for (auto& vb : m_visibilityBuffers) {
            builder->WithShaderResource(vb);
            bindings.visibilityBuffers.push_back(builder->BindShaderResource(vb));
        }
        for (auto& headPointers : m_deepVisibilityHeadPointerBuffers) {
            builder->WithUnorderedAccess(headPointers);
        }
        builder->WithUnorderedAccess(
            m_deepVisibilityNodesBuffer,
            m_deepVisibilityCounterBuffer,
            m_deepVisibilityOverflowCounterBuffer);
        bindings.deepNodes = builder->BindUnorderedAccess(m_deepVisibilityNodesBuffer);
        bindings.deepCounter = builder->BindUnorderedAccess(m_deepVisibilityCounterBuffer);
        bindings.deepOverflow = builder->BindUnorderedAccess(m_deepVisibilityOverflowCounterBuffer);
    }
    else if (m_outputKind == CLodRasterOutputKind::AVBOITOccupancy) {
        bindings.avboit = true;
        for (auto& vb : m_visibilityBuffers) {
            builder->WithShaderResource(vb);
            bindings.visibilityBuffers.push_back(builder->BindShaderResource(vb));
        }
        builder->WithShaderResource(m_AVBOITConfigBuffer, m_visibleClustersResolveBuffer)
            .WithUnorderedAccess(
                m_AVBOITOccupancyTexture,
                m_AVBOITOccupancySliceMaskTexture);
        bindings.avboitConfig = builder->BindShaderResource(m_AVBOITConfigBuffer);
        if (m_visibleClustersResolveBuffer) {
            bindings.visibleResolve = builder->BindShaderResource(m_visibleClustersResolveBuffer);
            bindings.hasVisibleResolve = true;
        }
    }
    else if (m_outputKind == CLodRasterOutputKind::AVBOIT) {
        bindings.avboit = true;
        for (auto& vb : m_visibilityBuffers) {
            builder->WithShaderResource(vb);
            bindings.visibilityBuffers.push_back(builder->BindShaderResource(vb));
        }
        builder->WithShaderResource(m_AVBOITConfigBuffer, m_visibleClustersResolveBuffer)
            .WithUnorderedAccess(
                m_AVBOITOccupancyTexture,
                m_AVBOITScalarExtinctionTexture,
                m_AVBOITChromaticExtinctionTexture);
        bindings.avboitConfig = builder->BindShaderResource(m_AVBOITConfigBuffer);
        if (m_visibleClustersResolveBuffer) {
            bindings.visibleResolve = builder->BindShaderResource(m_visibleClustersResolveBuffer);
            bindings.hasVisibleResolve = true;
        }
    }
    else if (m_outputKind == CLodRasterOutputKind::AVBOITShading) {
        bindings.avboit = true;
        const bool shadowsEnabled = m_getShadowsEnabled ? m_getShadowsEnabled() : false;
        for (auto& vb : m_visibilityBuffers) {
            builder->WithShaderResource(vb);
            bindings.visibilityBuffers.push_back(builder->BindShaderResource(vb));
        }
        builder->WithShaderResource(
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
                Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
                Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
                Builtin::Noise::BlueNoise2D,
                m_AVBOITConfigBuffer,
                m_AVBOITIntegratedTransmittanceTexture,
                m_AVBOITZeroTransmittanceSliceTexture,
                m_visibleClustersResolveBuffer)
            .WithRenderTarget(
                m_AVBOITAccumulationTexture,
                m_AVBOITNormalizationTexture,
                m_AVBOITShadingExtinctionTexture);
        bindings.avboitConfig = builder->BindShaderResource(m_AVBOITConfigBuffer);
        if (m_visibleClustersResolveBuffer) {
            bindings.visibleResolve = builder->BindShaderResource(m_visibleClustersResolveBuffer);
            bindings.hasVisibleResolve = true;
        }
        bindings.colors[0] = builder->BindRenderTarget(m_AVBOITAccumulationTexture);
        bindings.colors[1] = builder->BindRenderTarget(m_AVBOITNormalizationTexture);
        bindings.colors[2] = builder->BindRenderTarget(m_AVBOITShadingExtinctionTexture);
        if (shadowsEnabled) {
            builder->WithShaderResource(
                Builtin::Shadows::CLodClipmapInfo,
                Builtin::Shadows::CLodDirectionalPageViewInfo,
                Builtin::Shadows::CLodPageMetadata,
                Builtin::Shadows::CLodPageTable,
                Builtin::Shadows::CLodPhysicalPages,
                Builtin::Shadows::CLodCompactMainCamera,
                Builtin::Shadows::CLodCompactShadowCameras);
        }
        if (m_AVBOITEarlyDepthTexture) {
            builder->WithDepthRead(m_AVBOITEarlyDepthTexture);
            bindings.depth = builder->BindDepthRead(m_AVBOITEarlyDepthTexture);
            bindings.hasDepth = true;
        }
    }
    else if (m_outputKind == CLodRasterOutputKind::VirtualShadow) {
        bindings.virtualShadow = true;
        builder->WithShaderResource(
                m_virtualShadowClipmapInfoBuffer,
                Builtin::Shadows::CLodDirectionalPageViewInfo)
            .WithUnorderedAccess(
                m_virtualShadowPageTableTexture,
                m_virtualShadowPhysicalPagesTexture,
                m_virtualShadowDynamicPagesTexture,
                Builtin::Shadows::CLodStats);
        bindings.pageTable = builder->BindUnorderedAccess(m_virtualShadowPageTableTexture);
        bindings.clipmapInfo = builder->BindShaderResource(m_virtualShadowClipmapInfoBuffer);
        bindings.physicalPages = builder->BindUnorderedAccess(m_virtualShadowPhysicalPagesTexture);
        bindings.dynamicPages = builder->BindUnorderedAccess(m_virtualShadowDynamicPagesTexture);
    }

    // Declare page pool slabs for bindless access (auto-invalidates when new slabs are added).
    if (m_slabResourceGroup) {
        builder->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

void ClusterRasterizationPass::Initialize() {
    if (m_outputKind == CLodRasterOutputKind::AVBOITShading) {
        RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::OpenPBR::OpaqueDielectricEnergyComplement);
    }
    if (m_outputKind == CLodRasterOutputKind::AVBOITShading && m_getShadowsEnabled && m_getShadowsEnabled()) {
        RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
    }
}

void ClusterRasterizationPass::Update(const org::UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    auto numViews = context.ViewCameraBufferSize();
    std::vector<std::shared_ptr<org::PixelBuffer>> visibilityBuffers;
    std::vector<std::shared_ptr<org::PixelBuffer>> deepVisibilityHeadPointerBuffers;

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

    CLodViewRasterInfoTable table(numViews);
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
            table.BindView(cameraIndex, &CLodViewRasterInfo::visibilityUAVDescriptorIndex, viewInfo.visibilityBuffer,
                { org::BindlessViewKind::UnorderedAccess });
            info.scissorMaxX = viewInfo.visibilityBuffer->GetWidth();
            info.scissorMaxY = viewInfo.visibilityBuffer->GetHeight();
            visibilityBuffers.push_back(viewInfo.visibilityBuffer);
        }
        else if (m_outputKind == CLodRasterOutputKind::DeepVisibility) {
            auto headPointers = viewInfo.deepVisibilityHeadPointers;
            if (!headPointers) {
                table[cameraIndex] = info;
                continue;
            }

            table.BindView(cameraIndex, &CLodViewRasterInfo::opaqueVisibilitySRVDescriptorIndex, viewInfo.visibilityBuffer,
                { org::BindlessViewKind::ShaderResource });
            table.BindView(cameraIndex, &CLodViewRasterInfo::deepVisibilityHeadPointerUAVDescriptorIndex, headPointers,
                { org::BindlessViewKind::UnorderedAccess });
            info.scissorMaxX = headPointers->GetWidth();
            info.scissorMaxY = headPointers->GetHeight();
            visibilityBuffers.push_back(viewInfo.visibilityBuffer);
            deepVisibilityHeadPointerBuffers.push_back(std::move(headPointers));
        }
        else {
            table.BindView(cameraIndex, &CLodViewRasterInfo::opaqueVisibilitySRVDescriptorIndex, viewInfo.visibilityBuffer,
                { org::BindlessViewKind::ShaderResource });
            info.scissorMaxX = rasterDimension(viewInfo.visibilityBuffer->GetWidth());
            info.scissorMaxY = rasterDimension(viewInfo.visibilityBuffer->GetHeight());
            visibilityBuffers.push_back(viewInfo.visibilityBuffer);
        }

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

    const bool resourcesChanged =
        (m_visibilityBuffers != visibilityBuffers) ||
        (m_deepVisibilityHeadPointerBuffers != deepVisibilityHeadPointerBuffers);

    m_visibilityBuffers = std::move(visibilityBuffers);
    m_deepVisibilityHeadPointerBuffers = std::move(deepVisibilityHeadPointerBuffers);

    // Rows carry no descriptors: those are resolved when the recipe publishes
    // the table, so a resource moving to a new backing needs no redeclaration.
    const std::vector<CLodViewRasterInfo> rows(table.Rows().begin(), table.Rows().end());
    m_declaredResourcesChanged = m_viewRasterInfos != rows || resourcesChanged;
    m_viewRasterInfos = rows;
    m_viewRasterInfoTable = std::move(table);
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
    const auto srv = [&](org::ResourceBindingToken token) {
        return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index;
    };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) {
        return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index;
    };
    data.width = m_passWidth; data.height = m_passHeight; data.debugName = "CLod raster pass";
    if (m_outputKind == CLodRasterOutputKind::AVBOITShading && m_AVBOITAccumulationTexture
        && m_AVBOITNormalizationTexture && m_AVBOITShadingExtinctionTexture) {
        data.colorCount = 3;
        for (uint32_t i = 0; i < 3; ++i) {
            data.colors[i].rtv = preparation.ResolveView(bindings.colors[i], {org::BindlessViewKind::RenderTarget});
            data.colors[i].loadOp = rhi::LoadOp::Load;
            data.colors[i].storeOp = rhi::StoreOp::Store;
            data.colors[i].clear = preparation.ClearValue(bindings.colors[i]);
        }
        if (bindings.hasDepth) {
            data.hasDepth = true;
            data.depth.dsv = preparation.ResolveView(bindings.depth, {org::BindlessViewKind::DepthStencil});
            data.depth.depthLoad = rhi::LoadOp::Load; data.depth.depthStore = rhi::StoreOp::Store;
            data.depth.stencilLoad = rhi::LoadOp::DontCare; data.depth.stencilStore = rhi::StoreOp::DontCare;
            data.depth.clear = preparation.ClearValue(bindings.depth); data.depth.readOnly = true;
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
        m_rasterBucketsHistogramBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] =
        m_compactedVisibleClustersBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] =
        m_compactedVisibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index;
    misc[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] =
        m_viewRasterInfoTable.Publish(preparation, m_viewRasterInfoPublisher);
    misc[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] =
        m_sortedToUnsortedMappingBuffer->GetSRVInfo(0).slot.index;
    if (data.phase1VisibilityDiagnostics) {
        const auto compareView = [](std::string_view label, uint32_t frozen, uint32_t live) {
            basic_telemetry::SetGauge(std::string("BasicRenderer.CLod.Phase1.View.") +
                std::string(label) + ".Frozen", static_cast<int64_t>(frozen));
            basic_telemetry::SetGauge(std::string("BasicRenderer.CLod.Phase1.View.") +
                std::string(label) + ".Live", static_cast<int64_t>(live));
        };
        compareView("Histogram", misc[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX],
            m_rasterBucketsHistogramBuffer->GetSRVInfo(0).slot.index);
        compareView("Visible", misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX],
            m_compactedVisibleClustersBuffer->GetSRVInfo(0).slot.index);
        compareView("Transforms", misc[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX],
            m_compactedVisibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index);
        compareView("Mapping", misc[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX],
            m_sortedToUnsortedMappingBuffer->GetSRVInfo(0).slot.index);
        const auto validView = std::ranges::find_if(m_viewRasterInfos,
            [](const CLodViewRasterInfo& info) { return info.scissorMaxX != 0u; });
        if (validView != m_viewRasterInfos.end()) {
            if (!m_visibilityBuffers.empty())
                basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.PrimaryVisibilityDescriptorIndex",
                    static_cast<int64_t>(ResolveCLodViewDescriptor(preparation, *m_visibilityBuffers.front(),
                        { org::BindlessViewKind::UnorderedAccess })));
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
    if (m_outputKind == CLodRasterOutputKind::VisibilityBuffer && m_visibilityBuffers.size() == 1u)
        misc[CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX] = ResolveCLodViewDescriptor(
            preparation, *m_visibilityBuffers.front(), { org::BindlessViewKind::UnorderedAccess });
    if (data.phase1VisibilityDiagnostics && bindings.visibilityBuffers.size() == 1u) {
        const auto frozenVisibility = misc[CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX];
        const auto liveVisibility =
            m_visibilityBuffers.front()->GetUAVShaderVisibleInfo(0).slot.index;
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.View.Visibility.Frozen",
            static_cast<int64_t>(frozenVisibility));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1.View.Visibility.Live",
            static_cast<int64_t>(liveVisibility));
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
            uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
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
            ? PSOManager::GetInstance().TryGetClusterLODRasterPSO(flags, m_wireframe, m_visibilityBuffers.size() == 1u)
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
        m_wireframe, m_visibilityBuffers.size(), context->preparedRasterBucketCount,
        context->lighting.shadowsEnabled, context->lighting.punctualLightingEnabled, context->lighting.gtaoEnabled,
        reinterpret_cast<uintptr_t>(m_rasterizationCommandSignature.get()),
        // BuildRecipe embeds these live descriptor indices directly (not via
        // the binding table), so they must be part of the revision.
        m_rasterBucketsHistogramBuffer->GetSRVInfo(0).slot.index,
        m_compactedVisibleClustersBuffer->GetSRVInfo(0).slot.index,
        m_compactedVisibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index,
        m_sortedToUnsortedMappingBuffer->GetSRVInfo(0).slot.index};
    m_viewRasterInfoTable.AppendRevision(preparation, revision);
    for (uint32_t i = 0; i < context->preparedRasterBucketCount; ++i) {
        const auto flags = context->preparedRasterBucketFlags.at(i);
        const org::PipelineState* pso = m_outputKind == CLodRasterOutputKind::VisibilityBuffer
            ? PSOManager::GetInstance().TryGetClusterLODRasterPSO(flags, m_wireframe, m_visibilityBuffers.size() == 1u)
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
