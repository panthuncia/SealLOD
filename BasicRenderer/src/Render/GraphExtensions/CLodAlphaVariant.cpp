#include "Render/GraphExtensions/CLodAlphaVariant.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>

#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/CLodExtension.h"
#include "Render/GraphExtensions/CLodExtensionShared.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITAdaptiveFitPass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITAdaptiveFitUpdatePass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITDepthWarpPass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITEarlyDepthBuildPass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITEarlyDepthPass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITIntegratePass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITOccupancyHistogramPass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITOccupancyRemapPass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITResolvePass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITSetupPass.h"
#include "Render/GraphExtensions/ClusterLOD/AVBOITSparseClearPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClearDeepVisibilityPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterRasterizationPass.h"
#include "Render/GraphExtensions/ClusterLOD/DeepVisibilityResolvePass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesDeepVisibilityRasterizationPass.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderPhase.h"
#include "Resources/PixelBuffer.h"
#include "Resources/components.h"

namespace {

constexpr std::string_view kTransparentExtinctionSetupPassName = "TransparentExtinctionSetupPass";
constexpr std::string_view kTransparentExtinctionOccupancyPassName = "TransparentExtinctionOccupancyPass";
constexpr std::string_view kTransparentExtinctionOccupancyHistogramPassName = "TransparentExtinctionOccupancyHistogramPass";
constexpr std::string_view kTransparentExtinctionAdaptiveFitPassName = "TransparentExtinctionAdaptiveFitPass";
constexpr std::string_view kTransparentExtinctionAdaptiveFitUpdatePassName = "TransparentExtinctionAdaptiveFitUpdatePass";
constexpr std::string_view kTransparentExtinctionDepthWarpPassName = "TransparentExtinctionDepthWarpPass";
constexpr std::string_view kTransparentExtinctionOccupancyRemapPassName = "TransparentExtinctionOccupancyRemapPass";
constexpr std::string_view kTransparentExtinctionSparseClearPassName = "TransparentExtinctionSparseClearPass";
constexpr std::string_view kTransparentExtinctionCapturePassName = "TransparentExtinctionCapturePass";
constexpr std::string_view kTransparentVBOITEarlyDepthBuildPassName = "TransparentVBOITEarlyDepthBuildPass";
constexpr std::string_view kTransparentVBOITEarlyDepthPassName = "TransparentVBOITEarlyDepthPass";
constexpr std::string_view kTransparentTransmittanceIntegratePassName = "TransparentTransmittanceIntegratePass";
constexpr std::string_view kTransparentVBOITShadePassName = "TransparentVBOITShadePass";
constexpr std::string_view kTransparentVBOITResolvePassName = "TransparentVBOITResolvePass";

DirectX::XMUINT2 GetAVBOITLowResolution(const DirectX::XMUINT2& renderResolution)
{
    const uint32_t downsampleFactor = (std::max)(1u, CLodAVBOITDefaultDownsampleFactor);
    return {
        (std::max)(1u, (renderResolution.x + downsampleFactor - 1u) / downsampleFactor),
        (std::max)(1u, (renderResolution.y + downsampleFactor - 1u) / downsampleFactor)
    };
}

TextureDescription CreateAVBOITOccupancyDescription()
{
    TextureDescription desc;
    const auto renderResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    const auto lowResolution = GetAVBOITLowResolution(renderResolution);

    desc.channels = 1;
    desc.format = rhi::Format::R32_Float;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R32_Float;
    desc.hasUAV = true;
    desc.uavFormat = rhi::Format::R32_Float;
    desc.hasNonShaderVisibleUAV = true;
    desc.allowAlias = true;
    desc.imageDimensions.push_back(ImageDimensions{ lowResolution.x, lowResolution.y, 0, 0 });
    return desc;
}

TextureDescription CreateAVBOITSliceVolumeDescription()
{
    TextureDescription desc = CreateAVBOITOccupancyDescription();
    desc.isArray = true;
    desc.arraySize = CLodAVBOITDefaultSliceCount;
    return desc;
}

TextureDescription CreateAVBOITExtinctionDescription()
{
    TextureDescription desc = CreateAVBOITSliceVolumeDescription();
    desc.format = rhi::Format::R32_UInt;
    desc.srvFormat = rhi::Format::R32_UInt;
    desc.uavFormat = rhi::Format::R32_UInt;
    return desc;
}

TextureDescription CreateAVBOITChromaticExtinctionDescription()
{
    TextureDescription desc = CreateAVBOITSliceVolumeDescription();
    desc.channels = 1;
    desc.arraySize = CLodAVBOITDefaultSliceCount * 3u;
    desc.format = rhi::Format::R32_UInt;
    desc.srvFormat = rhi::Format::R32_UInt;
    desc.uavFormat = rhi::Format::R32_UInt;
    return desc;
}

TextureDescription CreateAVBOITIntegratedTransmittanceDescription()
{
    TextureDescription desc = CreateAVBOITSliceVolumeDescription();
    desc.channels = 4;
    desc.format = rhi::Format::R16G16B16A16_Float;
    desc.srvFormat = rhi::Format::R16G16B16A16_Float;
    desc.uavFormat = rhi::Format::R16G16B16A16_Float;
    return desc;
}

TextureDescription CreateAVBOITZeroTransmittanceSliceDescription()
{
    TextureDescription desc = CreateAVBOITOccupancyDescription();
    desc.format = rhi::Format::R32_UInt;
    desc.srvFormat = rhi::Format::R32_UInt;
    desc.uavFormat = rhi::Format::R32_UInt;
    return desc;
}

TextureDescription CreateAVBOITOccupancySliceMaskDescription()
{
    return CreateAVBOITZeroTransmittanceSliceDescription();
}

TextureDescription CreateAVBOITAccumulationDescription()
{
    TextureDescription desc;
    const auto renderResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();

    desc.channels = 4;
    desc.format = rhi::Format::R16G16B16A16_Float;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R16G16B16A16_Float;
    desc.hasRTV = true;
    desc.rtvFormat = rhi::Format::R16G16B16A16_Float;
    desc.clearColor[0] = 0.0f;
    desc.clearColor[1] = 0.0f;
    desc.clearColor[2] = 0.0f;
    desc.clearColor[3] = 0.0f;
    desc.allowAlias = true;
    desc.imageDimensions.push_back(ImageDimensions{ renderResolution.x, renderResolution.y, 0, 0 });
    return desc;
}

TextureDescription CreateAVBOITNormalizationDescription()
{
    return CreateAVBOITAccumulationDescription();
}

TextureDescription CreateAVBOITShadingExtinctionDescription()
{
    return CreateAVBOITAccumulationDescription();
}

TextureDescription CreateAVBOITEarlyDepthDescription()
{
    TextureDescription desc;
    const auto renderResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();

    desc.channels = 1;
    desc.format = rhi::Format::R32_Typeless;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R32_Float;
    desc.hasDSV = true;
    desc.dsvFormat = rhi::Format::D32_Float;
    desc.clearColor[0] = 1.0f;
    desc.clearColor[1] = 0.0f;
    desc.clearColor[2] = 0.0f;
    desc.clearColor[3] = 0.0f;
    desc.allowAlias = true;
    desc.imageDimensions.push_back(ImageDimensions{ renderResolution.x, renderResolution.y, 0, 0 });
    return desc;
}

RenderGraph::ExternalInsertPoint MakeTransparentTailInsertPoint()
{
    auto insertPoint = RenderGraph::ExternalInsertPoint::After("LightCullingPass");
    insertPoint.after.push_back("CLodShadow::VirtualShadowClearDirtyBitsPass");
    insertPoint.before.push_back("Screen-Space Reflections Pass");
    insertPoint.before.push_back("UpscalingPass");
    insertPoint.before.push_back("luminanceHistogramPass");
    return insertPoint;
}

RenderGraph::ExternalInsertPoint MakeTransparentCompositeInsertPoint()
{
    auto insertPoint = RenderGraph::ExternalInsertPoint::After("Specular IBL & SSR Composite Pass");
    insertPoint.after.push_back("SkyboxPass");
    insertPoint.after.push_back("Forward render pass");
    insertPoint.after.push_back("Screen-Space Reflections Pass");
    insertPoint.before.push_back("DebugGridPass");
    insertPoint.before.push_back("UpscalingPass");
    insertPoint.before.push_back("luminanceHistogramPass");
    insertPoint.before.push_back("TonemappingPass");
    return insertPoint;
}

} // namespace

void CLodAlphaVariant::InitializeDeepVisibilityResources(CLodExtension& extension)
{
    const auto& traits = GetVariantTraits(extension.m_type);
    if (traits.rasterOutputKind != CLodRasterOutputKind::DeepVisibility) {
        return;
    }

    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    const flecs::entity typeEntity = traits.ensureTypeEntity(ecsWorld);

    extension.m_deepVisibilityNodesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodDeepVisibilityNode),
        true,
        false,
        false,
        true);
    extension.m_deepVisibilityNodesBuffer->SetName(MakeVariantResourceName(traits, "Deep Visibility Nodes Buffer"));

    extension.m_deepVisibilityCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    extension.m_deepVisibilityCounterBuffer->SetName(MakeVariantResourceName(traits, "Deep Visibility Counter Buffer"));
    extension.m_deepVisibilityCounterBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_deepVisibilityCounterBuffer })
        .add<CLodDeepVisibilityCounterTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_deepVisibilityOverflowCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    extension.m_deepVisibilityOverflowCounterBuffer->SetName(MakeVariantResourceName(traits, "Deep Visibility Overflow Counter Buffer"));
    extension.m_deepVisibilityOverflowCounterBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_deepVisibilityOverflowCounterBuffer })
        .add<CLodDeepVisibilityOverflowCounterTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_deepVisibilityStatsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodDeepVisibilityStats),
        true,
        false,
        true,
        true);
    extension.m_deepVisibilityStatsBuffer->SetName(MakeVariantResourceName(traits, "Deep Visibility Stats Buffer"));
    extension.m_deepVisibilityStatsBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_deepVisibilityStatsBuffer })
        .add<CLodDeepVisibilityStatsTag>()
        .add<CLodExtensionTypeTag>(typeEntity);
}

void CLodAlphaVariant::InitializeAVBOITResources(CLodExtension& extension)
{
    if (extension.m_type != CLodExtensionType::AlphaBlend) {
        return;
    }

    const auto renderResolution =
        SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    extension.m_transparencyConfiguredRenderWidth = renderResolution.x;
    extension.m_transparencyConfiguredRenderHeight = renderResolution.y;

    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    const auto& traits = GetVariantTraits(extension.m_type);
    const flecs::entity typeEntity = traits.ensureTypeEntity(ecsWorld);

    extension.m_AVBOITConfigBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodAVBOITConfig),
        true,
        false,
        false,
        false);
    extension.m_AVBOITConfigBuffer->SetName(MakeVariantResourceName(traits, "AVBOIT Config Buffer"));
    extension.m_AVBOITConfigBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITConfigBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITOccupancyHistogramBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodAVBOITDefaultVirtualSliceCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_AVBOITOccupancyHistogramBuffer->SetName(MakeVariantResourceName(traits, "AVBOIT Occupancy Histogram"));
    extension.m_AVBOITOccupancyHistogramBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITOccupancyHistogramBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITDepthWarpLUTBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodAVBOITDepthWarpLUTResolution,
        sizeof(CLodAVBOITDepthWarpLUTEntry),
        true,
        false,
        false,
        false);
    extension.m_AVBOITDepthWarpLUTBuffer->SetName(MakeVariantResourceName(traits, "AVBOIT Depth Warp LUT"));
    extension.m_AVBOITDepthWarpLUTBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITDepthWarpLUTBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITFitStateBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodAVBOITFitState),
        true,
        false,
        false,
        false);
    extension.m_AVBOITFitStateBuffer->SetName(MakeVariantResourceName(traits, "AVBOIT Adaptive Fit State"));
    extension.m_AVBOITFitStateBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITFitStateBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITEarlyDepthTileCommandsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodAVBOITEarlyDepthTileIndirectCommand),
        true,
        false,
        false,
        true);
    extension.m_AVBOITEarlyDepthTileCommandsBuffer->SetName(MakeVariantResourceName(traits, "AVBOIT Early Depth Tile Commands"));
    extension.m_AVBOITEarlyDepthTileCommandsBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITEarlyDepthTileCommandsBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITEarlyDepthTileCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(uint32_t),
        true,
        false,
        false,
        true);
    extension.m_AVBOITEarlyDepthTileCountBuffer->SetName(MakeVariantResourceName(traits, "AVBOIT Early Depth Tile Count"));
    extension.m_AVBOITEarlyDepthTileCountBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITEarlyDepthTileCountBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITOccupancyTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITOccupancyDescription());
    extension.m_AVBOITOccupancyTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Occupancy"));
    extension.m_AVBOITOccupancyTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITOccupancyTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITCoverageTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITOccupancyDescription());
    extension.m_AVBOITCoverageTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Coverage"));
    extension.m_AVBOITCoverageTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITCoverageTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITOccupancySliceMaskTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITOccupancySliceMaskDescription());
    extension.m_AVBOITOccupancySliceMaskTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Occupancy Slice Mask"));
    extension.m_AVBOITOccupancySliceMaskTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITOccupancySliceMaskTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITExtinctionTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITExtinctionDescription());
    extension.m_AVBOITExtinctionTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Extinction"));
    extension.m_AVBOITExtinctionTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITExtinctionTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITChromaticExtinctionTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITChromaticExtinctionDescription());
    extension.m_AVBOITChromaticExtinctionTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Chromatic Extinction"));
    extension.m_AVBOITChromaticExtinctionTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITChromaticExtinctionTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITIntegratedTransmittanceTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITIntegratedTransmittanceDescription());
    extension.m_AVBOITIntegratedTransmittanceTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Integrated Transmittance"));
    extension.m_AVBOITIntegratedTransmittanceTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITIntegratedTransmittanceTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITZeroTransmittanceSliceTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITZeroTransmittanceSliceDescription());
    extension.m_AVBOITZeroTransmittanceSliceTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Zero Transmittance Slice"));
    extension.m_AVBOITZeroTransmittanceSliceTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITZeroTransmittanceSliceTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITAccumulationTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITAccumulationDescription());
    extension.m_AVBOITAccumulationTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Accumulation"));
    extension.m_AVBOITAccumulationTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITAccumulationTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITNormalizationTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITNormalizationDescription());
    extension.m_AVBOITNormalizationTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Normalization"));
    extension.m_AVBOITNormalizationTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITNormalizationTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITShadingExtinctionTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITShadingExtinctionDescription());
    extension.m_AVBOITShadingExtinctionTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Shading Extinction"));
    extension.m_AVBOITShadingExtinctionTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITShadingExtinctionTexture })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_AVBOITEarlyDepthTexture = PixelBuffer::CreateSharedUnmaterialized(CreateAVBOITEarlyDepthDescription());
    extension.m_AVBOITEarlyDepthTexture->SetName(MakeVariantResourceName(traits, "AVBOIT Early Depth"));
    extension.m_AVBOITEarlyDepthTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_AVBOITEarlyDepthTexture })
        .add<CLodExtensionTypeTag>(typeEntity);
}

void CLodAlphaVariant::TagResourceUsages(CLodExtension& extension)
{
    auto tagBufferUsage = [](const std::shared_ptr<Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            org::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };
    auto tagTextureUsage = [](const std::shared_ptr<PixelBuffer>& texture, std::string_view usage) {
        if (texture) {
            org::memory::SetResourceUsageHint(*texture, std::string(usage));
        }
    };

    tagBufferUsage(extension.m_deepVisibilityNodesBuffer, "Cluster LOD deep visibility");
    tagBufferUsage(extension.m_deepVisibilityCounterBuffer, "Cluster LOD deep visibility");
    tagBufferUsage(extension.m_deepVisibilityOverflowCounterBuffer, "Cluster LOD deep visibility");
    tagBufferUsage(extension.m_deepVisibilityStatsBuffer, "Cluster LOD deep visibility");
    tagBufferUsage(extension.m_AVBOITConfigBuffer, "Cluster LOD AVBOIT");
    tagBufferUsage(extension.m_AVBOITOccupancyHistogramBuffer, "Cluster LOD AVBOIT adaptive histogram");
    tagBufferUsage(extension.m_AVBOITDepthWarpLUTBuffer, "Cluster LOD AVBOIT depth warp LUT");
    tagBufferUsage(extension.m_AVBOITFitStateBuffer, "Cluster LOD AVBOIT adaptive fit state");
    tagBufferUsage(extension.m_AVBOITEarlyDepthTileCommandsBuffer, "Cluster LOD AVBOIT early depth");
    tagBufferUsage(extension.m_AVBOITEarlyDepthTileCountBuffer, "Cluster LOD AVBOIT early depth");
    tagTextureUsage(extension.m_AVBOITOccupancyTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITCoverageTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITOccupancySliceMaskTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITExtinctionTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITChromaticExtinctionTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITIntegratedTransmittanceTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITZeroTransmittanceSliceTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITAccumulationTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITNormalizationTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITShadingExtinctionTexture, "Cluster LOD AVBOIT");
    tagTextureUsage(extension.m_AVBOITEarlyDepthTexture, "Cluster LOD AVBOIT early depth");
}

void CLodAlphaVariant::ReleaseResourceBackings(CLodExtension& extension)
{
    std::unordered_set<Buffer*> releasedBuffers;
    auto releaseBufferBacking = [&releasedBuffers](const std::shared_ptr<Buffer>& buffer) {
        if (buffer && releasedBuffers.insert(buffer.get()).second) {
            buffer->Dematerialize();
        }
    };

    releaseBufferBacking(extension.m_deepVisibilityNodesBuffer);
    releaseBufferBacking(extension.m_deepVisibilityCounterBuffer);
    releaseBufferBacking(extension.m_deepVisibilityOverflowCounterBuffer);
    releaseBufferBacking(extension.m_deepVisibilityStatsBuffer);
    releaseBufferBacking(extension.m_AVBOITConfigBuffer);
    releaseBufferBacking(extension.m_AVBOITOccupancyHistogramBuffer);
    releaseBufferBacking(extension.m_AVBOITDepthWarpLUTBuffer);
    releaseBufferBacking(extension.m_AVBOITFitStateBuffer);
    releaseBufferBacking(extension.m_AVBOITEarlyDepthTileCommandsBuffer);
    releaseBufferBacking(extension.m_AVBOITEarlyDepthTileCountBuffer);

    std::unordered_set<PixelBuffer*> releasedTextures;
    auto releaseTextureBacking = [&releasedTextures](const std::shared_ptr<PixelBuffer>& texture) {
        if (texture && releasedTextures.insert(texture.get()).second) {
            texture->Dematerialize();
        }
    };

    releaseTextureBacking(extension.m_AVBOITOccupancyTexture);
    releaseTextureBacking(extension.m_AVBOITCoverageTexture);
    releaseTextureBacking(extension.m_AVBOITOccupancySliceMaskTexture);
    releaseTextureBacking(extension.m_AVBOITExtinctionTexture);
    releaseTextureBacking(extension.m_AVBOITChromaticExtinctionTexture);
    releaseTextureBacking(extension.m_AVBOITIntegratedTransmittanceTexture);
    releaseTextureBacking(extension.m_AVBOITZeroTransmittanceSliceTexture);
    releaseTextureBacking(extension.m_AVBOITAccumulationTexture);
    releaseTextureBacking(extension.m_AVBOITNormalizationTexture);
    releaseTextureBacking(extension.m_AVBOITShadingExtinctionTexture);
    releaseTextureBacking(extension.m_AVBOITEarlyDepthTexture);
}

void CLodAlphaVariant::RefreshResourcesForCurrentSettings(CLodExtension& extension)
{
    if (extension.m_type != CLodExtensionType::AlphaBlend) {
        return;
    }

    const auto renderResolution =
        SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    const bool renderResolutionChanged =
        extension.m_transparencyConfiguredRenderWidth != renderResolution.x ||
        extension.m_transparencyConfiguredRenderHeight != renderResolution.y;

    if (!renderResolutionChanged || !extension.m_AVBOITOccupancyTexture) {
        return;
    }

    ReleaseResourceBackings(extension);
    InitializeAVBOITResources(extension);
    TagResourceUsages(extension);
}

void CLodAlphaVariant::AppendSinglePassStructuralPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    const RenderPhase& renderPhase,
    bool useAVBOIT,
    bool useReyesForThisVariant,
    bool disableReyesTessellation,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    if (traits.type != CLodExtensionType::AlphaBlend) {
        return;
    }

    if (useAVBOIT) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Render(
                MakeVariantPassName(traits, kTransparentExtinctionSetupPassName),
                std::make_shared<AVBOITSetupPass>(
                    extension.m_AVBOITConfigBuffer,
                    extension.m_AVBOITFitStateBuffer,
                    extension.m_AVBOITDepthWarpLUTBuffer,
                    extension.m_AVBOITOccupancyTexture,
                    extension.m_AVBOITCoverageTexture,
                    extension.m_AVBOITOccupancySliceMaskTexture,
                    extension.m_AVBOITExtinctionTexture,
                    extension.m_AVBOITChromaticExtinctionTexture,
                    extension.m_AVBOITIntegratedTransmittanceTexture,
                    extension.m_AVBOITZeroTransmittanceSliceTexture,
                    extension.m_AVBOITAccumulationTexture,
                    extension.m_AVBOITNormalizationTexture,
                    extension.m_AVBOITShadingExtinctionTexture)));

        auto adaptiveFitPassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentExtinctionAdaptiveFitPassName),
            std::make_shared<AVBOITAdaptiveFitPass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITFitStateBuffer));
        adaptiveFitPassDesc.At(RenderGraph::ExternalInsertPoint::After(
            MakeVariantPassName(traits, kTransparentExtinctionSetupPassName)));
        outPasses.push_back(std::move(adaptiveFitPassDesc));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Render(
                MakeVariantPassName(traits, "ClearDeepVisibilityPass"),
                std::make_shared<ClearDeepVisibilityPass>(
                    extension.m_deepVisibilityCounterBuffer,
                    extension.m_deepVisibilityOverflowCounterBuffer,
                    extension.m_deepVisibilityStatsBuffer)));

        if (useReyesForThisVariant) {
            outPasses.push_back(
                RenderGraph::ExternalPassDesc::Compute(
                    MakeVariantPassName(traits, "ReyesPatchRasterPass1"),
                    std::make_shared<ReyesDeepVisibilityRasterizationPass>(
                        extension.m_visibleClustersBuffer,
                        extension.m_visibleClusterTransformIndicesBuffer,
                        extension.m_reyesDiceQueueBuffer,
                        extension.m_reyesDiceQueueCounterBuffer,
                        extension.m_reyesRasterWorkBuffer,
                        extension.m_reyesRasterWorkCounterBuffer,
                        extension.m_reyesTessTableConfigsBuffer,
                        extension.m_reyesTessTableVerticesBuffer,
                        extension.m_reyesTessTableTrianglesBuffer,
                        extension.m_reyesRasterWorkIndirectArgsBuffer,
                        extension.m_reyesTelemetryBufferPhase1,
                        extension.m_deepVisibilityNodesBuffer,
                        extension.m_deepVisibilityCounterBuffer,
                        extension.m_deepVisibilityOverflowCounterBuffer,
                        slabGroup,
                        MakeVariantResourceName(traits, "Reyes Deep Visibility View Raster Info Buffer"),
                        CLodReyesPatchVisibilityIndexBase(extension.m_visibleClusterCapacity))));
        }
    }

    if (useAVBOIT) {
        ClusterRasterizationPassInputs occupancyPassInputs;
        occupancyPassInputs.clearGbuffer = false;
        occupancyPassInputs.wireframe = false;
        occupancyPassInputs.renderPhase = renderPhase;
        occupancyPassInputs.outputKind = CLodRasterOutputKind::AVBOITOccupancy;
        auto occupancyPassDesc = RenderGraph::ExternalPassDesc::Render(
            MakeVariantPassName(traits, kTransparentExtinctionOccupancyPassName),
            std::make_shared<ClusterRasterizationPass>(
                occupancyPassInputs,
                extension.m_compactedVisibleClustersBuffer,
                extension.m_compactedVisibleClusterTransformIndicesBuffer,
                extension.m_rasterBucketsHistogramBuffer,
                extension.m_rasterBucketsIndirectArgsBuffer,
                extension.m_sortedToUnsortedMappingBuffer,
                nullptr,
                nullptr,
                nullptr,
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITOccupancyTexture,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                extension.m_visibleClustersBuffer,
                slabGroup,
                nullptr,
                nullptr,
                nullptr,
                extension.m_AVBOITOccupancySliceMaskTexture));
        occupancyPassDesc.GeometryPass();
        occupancyPassDesc.At(RenderGraph::ExternalInsertPoint::After(
            MakeVariantPassName(traits, kTransparentExtinctionAdaptiveFitPassName)));
        outPasses.push_back(std::move(occupancyPassDesc));

        auto occupancyHistogramPassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentExtinctionOccupancyHistogramPassName),
            std::make_shared<AVBOITOccupancyHistogramPass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITOccupancyTexture,
                extension.m_AVBOITOccupancySliceMaskTexture,
                extension.m_AVBOITOccupancyHistogramBuffer));
        occupancyHistogramPassDesc.At(RenderGraph::ExternalInsertPoint::After(
            MakeVariantPassName(traits, kTransparentExtinctionOccupancyPassName)));
        outPasses.push_back(std::move(occupancyHistogramPassDesc));

        auto adaptiveFitUpdatePassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentExtinctionAdaptiveFitUpdatePassName),
            std::make_shared<AVBOITAdaptiveFitUpdatePass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITOccupancyHistogramBuffer,
                extension.m_AVBOITFitStateBuffer));
        adaptiveFitUpdatePassDesc.At(RenderGraph::ExternalInsertPoint::After(
            MakeVariantPassName(traits, kTransparentExtinctionOccupancyHistogramPassName)));
        outPasses.push_back(std::move(adaptiveFitUpdatePassDesc));

        auto depthWarpPassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentExtinctionDepthWarpPassName),
            std::make_shared<AVBOITDepthWarpPass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITOccupancyHistogramBuffer,
                extension.m_AVBOITDepthWarpLUTBuffer));
        depthWarpPassDesc.At(RenderGraph::ExternalInsertPoint::After(
            MakeVariantPassName(traits, kTransparentExtinctionAdaptiveFitUpdatePassName)));
        outPasses.push_back(std::move(depthWarpPassDesc));

        auto occupancyRemapPassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentExtinctionOccupancyRemapPassName),
            std::make_shared<AVBOITOccupancyRemapPass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITOccupancyTexture,
                extension.m_AVBOITOccupancySliceMaskTexture,
                extension.m_AVBOITDepthWarpLUTBuffer));
        occupancyRemapPassDesc.At(RenderGraph::ExternalInsertPoint::After(
            MakeVariantPassName(traits, kTransparentExtinctionDepthWarpPassName)));
        outPasses.push_back(std::move(occupancyRemapPassDesc));

        auto sparseClearPassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentExtinctionSparseClearPassName),
            std::make_shared<AVBOITSparseClearPass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITOccupancyTexture,
                extension.m_AVBOITOccupancySliceMaskTexture,
                extension.m_AVBOITExtinctionTexture,
                extension.m_AVBOITChromaticExtinctionTexture,
                extension.m_AVBOITZeroTransmittanceSliceTexture));
        sparseClearPassDesc.At(RenderGraph::ExternalInsertPoint::After(
            MakeVariantPassName(traits, kTransparentExtinctionOccupancyRemapPassName)));
        outPasses.push_back(std::move(sparseClearPassDesc));
    }

    ClusterRasterizationPassInputs rasterizePassInputs;
    rasterizePassInputs.clearGbuffer = false;
    rasterizePassInputs.wireframe = false;
    rasterizePassInputs.renderPhase = renderPhase;
    rasterizePassInputs.outputKind = useAVBOIT
        ? CLodRasterOutputKind::AVBOIT
        : traits.rasterOutputKind;
    auto rasterizeDeepVisibilityPassDesc = RenderGraph::ExternalPassDesc::Render(
        MakeVariantPassName(
            traits,
            useAVBOIT
                ? kTransparentExtinctionCapturePassName
                : std::string_view("RasterizeClustersPass1")),
        std::make_shared<ClusterRasterizationPass>(
            rasterizePassInputs,
            extension.m_compactedVisibleClustersBuffer,
            extension.m_compactedVisibleClusterTransformIndicesBuffer,
            extension.m_rasterBucketsHistogramBuffer,
            extension.m_rasterBucketsIndirectArgsBuffer,
            extension.m_sortedToUnsortedMappingBuffer,
            useAVBOIT ? nullptr : extension.m_deepVisibilityNodesBuffer,
            useAVBOIT ? nullptr : extension.m_deepVisibilityCounterBuffer,
            useAVBOIT ? nullptr : extension.m_deepVisibilityOverflowCounterBuffer,
            useAVBOIT ? extension.m_AVBOITConfigBuffer : nullptr,
            useAVBOIT ? extension.m_AVBOITOccupancyTexture : nullptr,
            useAVBOIT ? extension.m_AVBOITExtinctionTexture : nullptr,
            useAVBOIT ? extension.m_AVBOITChromaticExtinctionTexture : nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            useAVBOIT ? extension.m_visibleClustersBuffer : nullptr,
            slabGroup));
    rasterizeDeepVisibilityPassDesc.GeometryPass();
    outPasses.push_back(std::move(rasterizeDeepVisibilityPassDesc));

    AppendSinglePassResolveTail(
        extension,
        traits,
        slabGroup,
        renderPhase,
        useAVBOIT,
        disableReyesTessellation,
        outPasses);
}

void CLodAlphaVariant::AppendSinglePassResolveTail(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    const RenderPhase& renderPhase,
    bool useAVBOIT,
    bool disableReyesTessellation,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    if (traits.type != CLodExtensionType::AlphaBlend) {
        return;
    }

    if (useAVBOIT) {
        auto integratePassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentTransmittanceIntegratePassName),
            std::make_shared<AVBOITIntegratePass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITFitStateBuffer,
                extension.m_AVBOITOccupancyTexture,
                extension.m_AVBOITCoverageTexture,
                extension.m_AVBOITOccupancySliceMaskTexture,
                extension.m_AVBOITExtinctionTexture,
                extension.m_AVBOITChromaticExtinctionTexture,
                extension.m_AVBOITIntegratedTransmittanceTexture,
                extension.m_AVBOITZeroTransmittanceSliceTexture));
        integratePassDesc.At(MakeTransparentTailInsertPoint());
        outPasses.push_back(std::move(integratePassDesc));

        auto earlyDepthBuildPassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentVBOITEarlyDepthBuildPassName),
            std::make_shared<AVBOITEarlyDepthBuildPass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITZeroTransmittanceSliceTexture,
                extension.m_AVBOITEarlyDepthTileCommandsBuffer,
                extension.m_AVBOITEarlyDepthTileCountBuffer));
        {
            auto insertPoint = MakeTransparentTailInsertPoint();
            insertPoint.after.push_back(MakeVariantPassName(traits, kTransparentTransmittanceIntegratePassName));
            earlyDepthBuildPassDesc.At(std::move(insertPoint));
        }
        outPasses.push_back(std::move(earlyDepthBuildPassDesc));

        auto earlyDepthPassDesc = RenderGraph::ExternalPassDesc::Render(
            MakeVariantPassName(traits, kTransparentVBOITEarlyDepthPassName),
            std::make_shared<AVBOITEarlyDepthPass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITEarlyDepthTileCommandsBuffer,
                extension.m_AVBOITEarlyDepthTileCountBuffer,
                extension.m_AVBOITEarlyDepthTexture));
        {
            auto insertPoint = MakeTransparentTailInsertPoint();
            insertPoint.after.push_back(MakeVariantPassName(traits, kTransparentVBOITEarlyDepthBuildPassName));
            earlyDepthPassDesc.At(std::move(insertPoint));
        }
        outPasses.push_back(std::move(earlyDepthPassDesc));

        ClusterRasterizationPassInputs shadePassInputs;
        shadePassInputs.clearGbuffer = false;
        shadePassInputs.wireframe = false;
        shadePassInputs.renderPhase = renderPhase;
        shadePassInputs.outputKind = CLodRasterOutputKind::AVBOITShading;
        auto shadePassDesc = RenderGraph::ExternalPassDesc::Render(
            MakeVariantPassName(traits, kTransparentVBOITShadePassName),
            std::make_shared<ClusterRasterizationPass>(
                shadePassInputs,
                extension.m_compactedVisibleClustersBuffer,
                extension.m_compactedVisibleClusterTransformIndicesBuffer,
                extension.m_rasterBucketsHistogramBuffer,
                extension.m_rasterBucketsIndirectArgsBuffer,
                extension.m_sortedToUnsortedMappingBuffer,
                nullptr,
                nullptr,
                nullptr,
                extension.m_AVBOITConfigBuffer,
                nullptr,
                nullptr,
                nullptr,
                extension.m_AVBOITIntegratedTransmittanceTexture,
                extension.m_AVBOITZeroTransmittanceSliceTexture,
                extension.m_AVBOITAccumulationTexture,
                extension.m_AVBOITNormalizationTexture,
                extension.m_AVBOITShadingExtinctionTexture,
                extension.m_visibleClustersBuffer,
                slabGroup,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                extension.m_AVBOITEarlyDepthTexture));
        shadePassDesc.GeometryPass();
        {
            auto insertPoint = MakeTransparentTailInsertPoint();
            insertPoint.after.push_back(MakeVariantPassName(traits, kTransparentVBOITEarlyDepthPassName));
            shadePassDesc.At(std::move(insertPoint));
        }
        outPasses.push_back(std::move(shadePassDesc));

        auto resolvePassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, kTransparentVBOITResolvePassName),
            std::make_shared<AVBOITResolvePass>(
                extension.m_AVBOITConfigBuffer,
                extension.m_AVBOITAccumulationTexture,
                extension.m_AVBOITNormalizationTexture,
                extension.m_AVBOITShadingExtinctionTexture));
        {
            auto insertPoint = MakeTransparentCompositeInsertPoint();
            insertPoint.after.push_back(MakeVariantPassName(traits, kTransparentVBOITShadePassName));
            resolvePassDesc.At(std::move(insertPoint));
        }
        outPasses.push_back(std::move(resolvePassDesc));
        return;
    }

    auto resolveDeepVisibilityPassDesc = RenderGraph::ExternalPassDesc::Compute(
        MakeVariantPassName(traits, "DeepVisibilityResolvePass"),
        std::make_shared<DeepVisibilityResolvePass>(
            extension.m_visibleClustersBuffer,
            disableReyesTessellation ? nullptr : extension.m_reyesDiceQueueBuffer,
            disableReyesTessellation ? nullptr : extension.m_reyesTessTableConfigsBuffer,
            disableReyesTessellation ? nullptr : extension.m_reyesTessTableVerticesBuffer,
            disableReyesTessellation ? nullptr : extension.m_reyesTessTableTrianglesBuffer,
            extension.m_deepVisibilityNodesBuffer,
            extension.m_deepVisibilityCounterBuffer,
            extension.m_deepVisibilityOverflowCounterBuffer,
            extension.m_deepVisibilityStatsBuffer,
            CLodReyesPatchVisibilityIndexBase(extension.m_visibleClusterCapacity)));
    resolveDeepVisibilityPassDesc.At(MakeTransparentCompositeInsertPoint());
    outPasses.push_back(std::move(resolveDeepVisibilityPassDesc));
}
