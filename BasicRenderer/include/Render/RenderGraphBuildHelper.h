#pragma once
#include "Scene/Components.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "../../generated/BuiltinResources.h"
#include "RenderPasses/PostProcessing/BloomSamplePass.h"
#include "RenderPasses/PostProcessing/BloomBlendPass.h"
#include "RenderPasses/PrimaryDepthCopyPass.h"
#include "RenderPasses/VisUtil/BuildPixelListPass.h"
#include "RenderPasses/VisUtil/EvaluateMaterialGroupsPass.h"
#include "RenderPasses/VisUtil/MaterialHistogramPass.h"
#include "RenderPasses/VisUtil/MaterialPixelCounterResetPass.h"
#include "RenderPasses/VisUtil/MaterialBlockScanPass.h"
#include "RenderPasses/VisUtil/MaterialBlockOffsetsPass.h"
#include "RenderPasses/VisUtil/BuildMaterialIndirectCommandBufferPass.h"
#include "RenderPasses/VisUtil/TerrainRegionMaterialEvaluationPasses.h"
#include "RenderPasses/TerrainRvtPasses.h"
#include "Render/IndirectCommand.h"
#include "RenderPasses/brdfIntegrationPass.h"
#include "RenderPasses/GTAO/XeGTAODenoisePass.h"
#include "RenderPasses/GTAO/XeGTAOFilterPass.h"
#include "RenderPasses/GTAO/XeGTAOMainPass.h"
#include "RenderPasses/LightCullingPass.h"
#include "RenderPasses/ClusterGenerationPass.h"
#include "RenderPasses/EnvironmentConversionPass.h"
#include "RenderPasses/EnvironmentSHPass.h"
#include "RenderPasses/DeferredShadingPass.h"
#include "RenderPasses/SkyboxRenderPass.h"
#include "RenderPasses/PostProcessing/ScreenSpaceReflectionsPass.h"
#include "RenderPasses/PostProcessing/SpecularIBLPass.h"
#include "RenderPasses/RayTracing/RayTracedReflectionsPass.h"
#include "RenderPasses/FidelityFX/Downsample.h"
#include "RenderPasses/FidelityFX/LinearDepthHistoryCopyPass.h"
#include "Resources/Buffers/Buffer.h"
#include "Render/MemoryIntrospectionAPI.h"

inline void TagPassTechnique(RenderGraph* graph, std::string_view passName, std::string_view techniquePath) {
    graph->SetPassTechnique(std::string(passName), std::string(techniquePath));
}

void CreateGBufferResources(RenderGraph* graph) {
    // GBuffer resources
	auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();

    TextureDescription normalsWorldSpaceDesc;
    normalsWorldSpaceDesc.channels = 3;
    normalsWorldSpaceDesc.format = rhi::Format::R32G32B32A32_Float;
    normalsWorldSpaceDesc.hasRTV = true;
	normalsWorldSpaceDesc.rtvFormat = rhi::Format::R32G32B32A32_Float;
    normalsWorldSpaceDesc.hasSRV = true;
    normalsWorldSpaceDesc.srvFormat = rhi::Format::R32G32B32A32_Float;
    normalsWorldSpaceDesc.hasUAV = true;
	normalsWorldSpaceDesc.hasNonShaderVisibleUAV = true;
	normalsWorldSpaceDesc.uavFormat = rhi::Format::R32G32B32A32_Float;
    normalsWorldSpaceDesc.allowAlias = true;
    ImageDimensions dims = { resolution.x, resolution.y, 0, 0 };
    normalsWorldSpaceDesc.imageDimensions.push_back(dims);
    auto normalsWorldSpace = PixelBuffer::CreateSharedUnmaterialized(normalsWorldSpaceDesc);
    normalsWorldSpace->SetName("Normals World Space");
    rg::memory::SetResourceUsageHint(*normalsWorldSpace, "Visibility Buffer Resources");

    graph->RegisterResource(Builtin::GBuffer::Normals, normalsWorldSpace);

    std::shared_ptr<PixelBuffer> albedo;
    std::shared_ptr<PixelBuffer> coat;
    std::shared_ptr<PixelBuffer> fuzz;
    std::shared_ptr<PixelBuffer> metallicRoughness;
    std::shared_ptr<PixelBuffer> emissive;

    TextureDescription albedoDesc;
    albedoDesc.channels = 4;
    albedoDesc.hasRTV = true;
    albedoDesc.format = rhi::Format::R8G8B8A8_UNorm;
    albedoDesc.hasSRV = true;
	albedoDesc.hasUAV = true;
    albedoDesc.hasNonShaderVisibleUAV = true;
    ImageDimensions albedoDims = { resolution.x, resolution.y, 0, 0 };
    albedoDesc.imageDimensions.push_back(albedoDims);
    albedoDesc.allowAlias = true;
    albedo = PixelBuffer::CreateSharedUnmaterialized(albedoDesc);
    albedo->SetName("Albedo");
    rg::memory::SetResourceUsageHint(*albedo, "GBuffer");
    graph->RegisterResource(Builtin::GBuffer::Albedo, albedo);

    TextureDescription coatDesc;
    coatDesc.channels = 4;
    coatDesc.hasRTV = true;
    coatDesc.format = rhi::Format::R16G16B16A16_Float;
    coatDesc.hasSRV = true;
	coatDesc.hasUAV = true;
    coatDesc.hasNonShaderVisibleUAV = true;
    ImageDimensions coatDims = { resolution.x, resolution.y, 0, 0 };
    coatDesc.imageDimensions.push_back(coatDims);
    coatDesc.allowAlias = true;
    coat = PixelBuffer::CreateSharedUnmaterialized(coatDesc);
    coat->SetName("OpenPBR Coat");
    rg::memory::SetResourceUsageHint(*coat, "GBuffer");
    graph->RegisterResource(Builtin::GBuffer::Coat, coat);

    TextureDescription fuzzDesc;
    fuzzDesc.channels = 4;
    fuzzDesc.hasRTV = true;
    fuzzDesc.format = rhi::Format::R16G16B16A16_Float;
    fuzzDesc.hasSRV = true;
	fuzzDesc.hasUAV = true;
    fuzzDesc.hasNonShaderVisibleUAV = true;
    ImageDimensions fuzzDims = { resolution.x, resolution.y, 0, 0 };
    fuzzDesc.imageDimensions.push_back(fuzzDims);
    fuzzDesc.allowAlias = true;
    fuzz = PixelBuffer::CreateSharedUnmaterialized(fuzzDesc);
    fuzz->SetName("OpenPBR Fuzz");
    rg::memory::SetResourceUsageHint(*fuzz, "GBuffer");
    graph->RegisterResource(Builtin::GBuffer::Fuzz, fuzz);

    TextureDescription metallicRoughnessDesc;
    metallicRoughnessDesc.channels = 4;
    metallicRoughnessDesc.hasRTV = true;
    metallicRoughnessDesc.format = rhi::Format::R8G8B8A8_UNorm;
    metallicRoughnessDesc.hasSRV = true;
	metallicRoughnessDesc.hasUAV = true;
	metallicRoughnessDesc.hasNonShaderVisibleUAV = true;
	metallicRoughnessDesc.allowAlias = true;
    ImageDimensions metallicRoughnessDims = { resolution.x, resolution.y, 0, 0 };
    metallicRoughnessDesc.imageDimensions.push_back(metallicRoughnessDims);
    metallicRoughness = PixelBuffer::CreateSharedUnmaterialized(metallicRoughnessDesc);
    metallicRoughness->SetName("Metallic Roughness");
    rg::memory::SetResourceUsageHint(*metallicRoughness, "GBuffer");
    graph->RegisterResource(Builtin::GBuffer::MetallicRoughness, metallicRoughness);

    TextureDescription emissiveDesc;
    emissiveDesc.channels = 4;
    emissiveDesc.hasRTV = true;
    emissiveDesc.format = rhi::Format::R16G16B16A16_Float;
    emissiveDesc.hasSRV = true;
	emissiveDesc.hasUAV = true;
	emissiveDesc.hasNonShaderVisibleUAV = true;
	emissiveDesc.allowAlias = true;
    ImageDimensions emissiveDims = { resolution.x, resolution.y, 0, 0 };
    emissiveDesc.imageDimensions.push_back(emissiveDims);
    emissive = PixelBuffer::CreateSharedUnmaterialized(emissiveDesc);
    emissive->SetName("Emissive");
    rg::memory::SetResourceUsageHint(*emissive, "GBuffer");
    graph->RegisterResource(Builtin::GBuffer::Emissive, emissive);
}

void CreateDebugVisualizationResources(RenderGraph* graph) {
    auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();

    TextureDescription debugVisDesc;
    debugVisDesc.channels = 2;
    debugVisDesc.format = rhi::Format::R32G32_UInt;
    debugVisDesc.hasUAV = true;
    debugVisDesc.uavFormat = rhi::Format::R32G32_UInt;
    debugVisDesc.hasSRV = true;
    debugVisDesc.srvFormat = rhi::Format::R32G32_UInt;
    debugVisDesc.hasNonShaderVisibleUAV = true;
    debugVisDesc.allowAlias = true;
    ImageDimensions debugVisDims = { resolution.x, resolution.y, 0, 0 };
    debugVisDesc.imageDimensions.push_back(debugVisDims);
    auto debugVisTex = PixelBuffer::CreateSharedUnmaterialized(debugVisDesc);
    debugVisTex->SetName("Debug Visualization");
    rg::memory::SetResourceUsageHint(*debugVisTex, "Debug");
    graph->RegisterResource(Builtin::DebugVisualization, debugVisTex);
}

void BuildBRDFIntegrationPass(RenderGraph* graph) {
	TextureDescription brdfDesc;
    brdfDesc.arraySize = 1;
    brdfDesc.channels = 2;
    brdfDesc.isCubemap = false;
    brdfDesc.hasRTV = true;
    brdfDesc.format = rhi::Format::R16G16_Float;
    brdfDesc.generateMipMaps = false;
    brdfDesc.hasSRV = true;
    brdfDesc.srvFormat = rhi::Format::R16G16_Float;
	brdfDesc.hasUAV = true;
	brdfDesc.uavFormat = rhi::Format::R16G16_Float;
    ImageDimensions dims = { 512, 512, 0, 0 };
    brdfDesc.imageDimensions.push_back(dims);
    auto brdfIntegrationTexture = PixelBuffer::CreateSharedUnmaterialized(brdfDesc);
    brdfIntegrationTexture->SetName("BRDF Integration Texture");
    rg::memory::SetResourceUsageHint(*brdfIntegrationTexture, "Environment lighting");
    brdfIntegrationTexture->EnableIdleDematerialization(120);
	graph->RegisterResource(Builtin::BRDFLUT, brdfIntegrationTexture);
	graph->BuildRenderPass<BRDFIntegrationPass>("BRDF Integration Pass");
    TagPassTechnique(graph, "BRDF Integration Pass", "Environment Lighting::BRDF Integration");
}

inline void RegisterVisUtilResources(RenderGraph* graph)
{
    auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    const uint32_t maxPixels = resolution.x * resolution.y;

    auto& rm = ResourceManager::GetInstance();
    (void)rm;

    // Total pixel count buffer (uint[1])
    auto totalPixelCountBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    totalPixelCountBuffer->SetAllowAlias(true);
    totalPixelCountBuffer->SetName("VisUtil::TotalPixelCountBuffer");
    rg::memory::SetResourceUsageHint(*totalPixelCountBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TotalPixelCountBuffer", totalPixelCountBuffer);

	// PixelRef: uint pixelXY; (packed)
    struct PixelRefPOD { uint32_t pixelXY; };
    auto pixelListBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        maxPixels,
        sizeof(PixelRefPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
	pixelListBuffer->SetAllowAlias(true);
    pixelListBuffer->SetName("VisUtil::PixelListBuffer");
    rg::memory::SetResourceUsageHint(*pixelListBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::PixelListBuffer", pixelListBuffer);

    constexpr uint32_t maxTerrainRegions = 65536u;
    auto terrainRegionPixelCountBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        maxTerrainRegions,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionPixelCountBuffer->SetAllowAlias(true);
    terrainRegionPixelCountBuffer->SetName("VisUtil::TerrainRegionPixelCountBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionPixelCountBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionPixelCountBuffer", terrainRegionPixelCountBuffer);

    auto terrainRegionOffsetBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        maxTerrainRegions,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionOffsetBuffer->SetAllowAlias(true);
    terrainRegionOffsetBuffer->SetName("VisUtil::TerrainRegionOffsetBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionOffsetBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionOffsetBuffer", terrainRegionOffsetBuffer);

    auto terrainRegionWriteCursorBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        maxTerrainRegions,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionWriteCursorBuffer->SetAllowAlias(true);
    terrainRegionWriteCursorBuffer->SetName("VisUtil::TerrainRegionWriteCursorBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionWriteCursorBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionWriteCursorBuffer", terrainRegionWriteCursorBuffer);

    auto terrainRegionBlockSumsBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        (maxTerrainRegions + 1023u) / 1024u,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionBlockSumsBuffer->SetAllowAlias(true);
    terrainRegionBlockSumsBuffer->SetName("VisUtil::TerrainRegionBlockSumsBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionBlockSumsBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionBlockSumsBuffer", terrainRegionBlockSumsBuffer);

    auto terrainRegionScannedBlockSumsBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        (maxTerrainRegions + 1023u) / 1024u,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionScannedBlockSumsBuffer->SetAllowAlias(true);
    terrainRegionScannedBlockSumsBuffer->SetName("VisUtil::TerrainRegionScannedBlockSumsBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionScannedBlockSumsBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionScannedBlockSumsBuffer", terrainRegionScannedBlockSumsBuffer);

    auto terrainRegionTotalPixelCountBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionTotalPixelCountBuffer->SetAllowAlias(true);
    terrainRegionTotalPixelCountBuffer->SetName("VisUtil::TerrainRegionTotalPixelCountBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionTotalPixelCountBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionTotalPixelCountBuffer", terrainRegionTotalPixelCountBuffer);

    auto terrainRegionActiveListBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        maxTerrainRegions,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionActiveListBuffer->SetAllowAlias(true);
    terrainRegionActiveListBuffer->SetName("VisUtil::TerrainRegionActiveListBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionActiveListBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionActiveListBuffer", terrainRegionActiveListBuffer);

    auto terrainRegionActiveCountBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionActiveCountBuffer->SetAllowAlias(true);
    terrainRegionActiveCountBuffer->SetName("VisUtil::TerrainRegionActiveCountBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionActiveCountBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionActiveCountBuffer", terrainRegionActiveCountBuffer);

    auto terrainRegionPixelListBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        maxPixels,
        sizeof(PixelRefPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionPixelListBuffer->SetAllowAlias(true);
    terrainRegionPixelListBuffer->SetName("VisUtil::TerrainRegionPixelListBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionPixelListBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::VisUtil::TerrainRegionPixelListBuffer", terrainRegionPixelListBuffer);

    auto terrainRegionMaterialEvalCommandBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        maxTerrainRegions,
        sizeof(TerrainRegionMaterialEvaluationIndirectCommand),
        true,
        true,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionMaterialEvalCommandBuffer->SetAllowAlias(true);
    terrainRegionMaterialEvalCommandBuffer->SetName("IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionMaterialEvalCommandBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer", terrainRegionMaterialEvalCommandBuffer);

    auto terrainRegionMaterialEvalCommandBuildDispatchArgsBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(D3D12_DISPATCH_ARGUMENTS),
        true,
        true,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRegionMaterialEvalCommandBuildDispatchArgsBuffer->SetAllowAlias(true);
    terrainRegionMaterialEvalCommandBuildDispatchArgsBuffer->SetName("IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer");
    rg::memory::SetResourceUsageHint(*terrainRegionMaterialEvalCommandBuildDispatchArgsBuffer, "Visibility Buffer Resources");
    graph->RegisterResource("Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer", terrainRegionMaterialEvalCommandBuildDispatchArgsBuffer);

    struct TerrainRvtInfoPOD {
        uint32_t pageSize;
        uint32_t borderTexels;
        uint32_t physicalTileTexelSide;
        uint32_t physicalAtlasPagesWide;
        uint32_t physicalAtlasPagesHigh;
        uint32_t maxPhysicalPages;
        uint32_t maxVirtualPageTableEntries;
        uint32_t maxRequests;
        uint32_t maxGenerationEntries;
        uint32_t mipCount;
        uint32_t pageTableResolution;
        uint32_t flags;
        float basePageWorldSize;
        uint32_t physicalAtlasPoolCount;
        uint32_t maxTerrainSets;
        uint32_t maxClipLevels;
        uint32_t maxGeneratedPagesPerFrame;
        float mipOffset;
    };
    struct TerrainRvtClipInfoPOD {
        uint32_t terrainSetIndex;
        uint32_t clipLevel;
        uint32_t tableBaseSlot;
        uint32_t tableResolution;
        uint32_t originPage[2];
        uint32_t terrainPageCount[2];
        float pageWorldSize;
        float invPageWorldSize;
        uint32_t valid;
        uint32_t terrainClipCount;
        int32_t clearDelta[2];
    };
    struct TerrainRvtPageTagPOD {
        uint32_t terrainSetIndex;
        uint32_t clipLevel;
        uint32_t pageX;
        uint32_t pageY;
    };
    struct TerrainRvtPageRequestPOD {
        uint32_t pageTableIndex;
        uint32_t terrainSetIndex;
        uint32_t clipLevel;
        uint32_t contentMask;
        uint32_t pageX;
        uint32_t pageY;
        uint32_t pad0;
        uint32_t pad1;
    };
    struct TerrainRvtGenerationRequestPOD {
        uint32_t pageTableIndex;
        uint32_t physicalPageIndex;
        uint32_t contentMask;
        uint32_t terrainSetIndex;
        uint32_t clipLevel;
        uint32_t pageX;
        uint32_t pageY;
        uint32_t pad0;
    };
    struct TerrainRvtPhysicalPageAtlasInfoPOD {
        float atlasBaseUv[2];
        float pageUvScale[2];
        float poolIndex;
        float pad0[3];
    };
    struct TerrainRvtHeightResidentCacheEntryPOD {
        uint32_t status;
        uint32_t requestedTerrainSetIndex;
        uint32_t requestedClipLevel;
        uint32_t requestedPageX;
        uint32_t requestedPageY;
        uint32_t residentClipLevel;
        uint32_t residentPageTableIndex;
        uint32_t physicalPageIndex;
        uint32_t residentPageX;
        uint32_t residentPageY;
        uint32_t pad0;
        uint32_t pad1;
    };
    struct TerrainRvtStatsPOD {
        uint32_t heightRequests;
        uint32_t materialRequests;
        uint32_t requestOverflows;
        uint32_t generatedPages;
        uint32_t allocationFailures;
        uint32_t heightFallbacks;
        uint32_t materialFallbacks;
        uint32_t residentHits;
        uint32_t heightSampleAttempts;
        uint32_t materialSampleAttempts;
        uint32_t heightSampleHits;
        uint32_t materialSampleHits;
        uint32_t heightPageTableMisses;
        uint32_t materialPageTableMisses;
        uint32_t heightComputePageFailures;
        uint32_t materialComputePageFailures;
        uint32_t heightDisabledFallbacks;
        uint32_t materialDisabledFallbacks;
        uint32_t heightForcedFallbacks;
        uint32_t materialForcedFallbacks;
        uint32_t markComputePageFailures;
        uint32_t markWorldRectCalls;
        uint32_t markWorldRectPages;
        uint32_t resolveResidentPages;
        uint32_t generationHeightPages;
        uint32_t generationMaterialPages;
        uint32_t generationCombinedPages;
        uint32_t generationTexels;
        uint32_t materialSampleRequestedPageXor;
        uint32_t materialSampleResidentPageXor;
        uint32_t materialSamplePhysicalPageXor;
        uint32_t materialSampleRequestedPageMin;
        uint32_t materialSampleRequestedPageMax;
        uint32_t materialSampleResidentPageMin;
        uint32_t materialSampleResidentPageMax;
        uint32_t materialSamplePhysicalPageMin;
        uint32_t materialSamplePhysicalPageMax;
        uint32_t materialSampleCoarserResidentHits;
        uint32_t materialSampleAtlasPoolMask;
        uint32_t heightOwnerMismatches;
        uint32_t materialOwnerMismatches;
        uint32_t requestPageTableXor;
        uint32_t requestPageTableMin;
        uint32_t requestPageTableMax;
        uint32_t generationPageTableMin;
        uint32_t generationPageTableMax;
        uint32_t materialSampleAttemptedPageXor;
        uint32_t materialSampleAttemptedPageMin;
        uint32_t materialSampleAttemptedPageMax;
        uint32_t materialSamplePageMissRequestedPageXor;
        uint32_t materialSamplePageMissRequestedPageMin;
        uint32_t materialSamplePageMissRequestedPageMax;
        uint32_t heightSampleAttemptedPageXor;
        uint32_t heightSampleAttemptedPageMin;
        uint32_t heightSampleAttemptedPageMax;
        uint32_t heightSamplePageMissRequestedPageXor;
        uint32_t heightSamplePageMissRequestedPageMin;
        uint32_t heightSamplePageMissRequestedPageMax;
        uint32_t heightFastSampleAttempts;
        uint32_t heightFastSampleHits;
        uint32_t heightFastPageMissRequests;
        uint32_t heightFullSampleAttempts;
        uint32_t heightFullSampleHits;
        uint32_t generationPageTableXor;
        uint32_t generationPhysicalPageXor;
        uint32_t physicalPageOwnerCollisions;
        uint32_t heightRequestMipHistogram[16];
        uint32_t materialRequestMipHistogram[16];
        uint32_t heightSampleMipHistogram[16];
        uint32_t materialSampleMipHistogram[16];
        uint32_t generationMipHistogram[16];
    };

    const uint32_t terrainRvtPageTableEntries = TerrainRvt::MaxPageTableEntries();
    auto terrainRvtInfoBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(TerrainRvtInfoPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtInfoBuffer->SetAllowAlias(false);
    terrainRvtInfoBuffer->SetName("TerrainRvt::Info");
    rg::memory::SetResourceUsageHint(*terrainRvtInfoBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtInfo, terrainRvtInfoBuffer);

    auto terrainRvtClipInfosBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        TerrainRvt::MaxClipInfoCount(),
        sizeof(TerrainRvtClipInfoPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtClipInfosBuffer->SetAllowAlias(false);
    terrainRvtClipInfosBuffer->SetName("TerrainRvt::ClipInfos");
    rg::memory::SetResourceUsageHint(*terrainRvtClipInfosBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtClipInfos, terrainRvtClipInfosBuffer);

    auto terrainRvtPageTableBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        terrainRvtPageTableEntries,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtPageTableBuffer->SetAllowAlias(false);
    terrainRvtPageTableBuffer->SetName("TerrainRvt::PageTable");
    rg::memory::SetResourceUsageHint(*terrainRvtPageTableBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtPageTable, terrainRvtPageTableBuffer);

    auto terrainRvtPageKeysBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        terrainRvtPageTableEntries,
        sizeof(TerrainRvtPageTagPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtPageKeysBuffer->SetAllowAlias(false);
    terrainRvtPageKeysBuffer->SetName("TerrainRvt::PageKeys");
    rg::memory::SetResourceUsageHint(*terrainRvtPageKeysBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtPageKeys, terrainRvtPageKeysBuffer);

    auto terrainRvtPhysicalPageOwnerBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        TerrainRvt::MaxPhysicalPages(),
        sizeof(uint32_t) * 4u,
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtPhysicalPageOwnerBuffer->SetAllowAlias(false);
    terrainRvtPhysicalPageOwnerBuffer->SetName("TerrainRvt::PhysicalPageOwner");
    rg::memory::SetResourceUsageHint(*terrainRvtPhysicalPageOwnerBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtPhysicalPageOwner, terrainRvtPhysicalPageOwnerBuffer);

    auto terrainRvtPhysicalPageAtlasBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        TerrainRvt::MaxPhysicalPages(),
        sizeof(TerrainRvtPhysicalPageAtlasInfoPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtPhysicalPageAtlasBuffer->SetAllowAlias(false);
    terrainRvtPhysicalPageAtlasBuffer->SetName("TerrainRvt::PhysicalPageAtlas");
    rg::memory::SetResourceUsageHint(*terrainRvtPhysicalPageAtlasBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtPhysicalPageAtlas, terrainRvtPhysicalPageAtlasBuffer);

    auto terrainRvtHeightResidentCacheBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        terrainRvtPageTableEntries,
        sizeof(TerrainRvtHeightResidentCacheEntryPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtHeightResidentCacheBuffer->SetAllowAlias(false);
    terrainRvtHeightResidentCacheBuffer->SetName("TerrainRvt::HeightResidentCache");
    rg::memory::SetResourceUsageHint(*terrainRvtHeightResidentCacheBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtHeightResidentCache, terrainRvtHeightResidentCacheBuffer);

    auto terrainRvtRequestMasksBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        terrainRvtPageTableEntries,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtRequestMasksBuffer->SetAllowAlias(false);
    terrainRvtRequestMasksBuffer->SetName("TerrainRvt::RequestMasks");
    rg::memory::SetResourceUsageHint(*terrainRvtRequestMasksBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtRequestMasks, terrainRvtRequestMasksBuffer);

    auto terrainRvtRequestListBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        terrainRvtPageTableEntries,
        sizeof(TerrainRvtPageRequestPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtRequestListBuffer->SetAllowAlias(false);
    terrainRvtRequestListBuffer->SetName("TerrainRvt::RequestList");
    rg::memory::SetResourceUsageHint(*terrainRvtRequestListBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtRequestList, terrainRvtRequestListBuffer);

    auto terrainRvtCountersBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        TerrainRvt::CounterCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtCountersBuffer->SetAllowAlias(false);
    terrainRvtCountersBuffer->SetName("TerrainRvt::Counters");
    rg::memory::SetResourceUsageHint(*terrainRvtCountersBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtCounters, terrainRvtCountersBuffer);

    auto terrainRvtGenerationListBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        TerrainRvt::MaxPhysicalPages(),
        sizeof(TerrainRvtGenerationRequestPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtGenerationListBuffer->SetAllowAlias(false);
    terrainRvtGenerationListBuffer->SetName("TerrainRvt::GenerationList");
    rg::memory::SetResourceUsageHint(*terrainRvtGenerationListBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtGenerationList, terrainRvtGenerationListBuffer);

    auto terrainRvtStatsBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(TerrainRvtStatsPOD),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtStatsBuffer->SetAllowAlias(false);
    terrainRvtStatsBuffer->SetName("TerrainRvt::Stats");
    rg::memory::SetResourceUsageHint(*terrainRvtStatsBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtStats, terrainRvtStatsBuffer);

    auto terrainRvtGenerateDispatchArgsBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(D3D12_DISPATCH_ARGUMENTS),
        true,
        true,
        false,
        rhi::HeapType::DeviceLocal);
    terrainRvtGenerateDispatchArgsBuffer->SetAllowAlias(false);
    terrainRvtGenerateDispatchArgsBuffer->SetName("TerrainRvt::GenerateDispatchArgs");
    rg::memory::SetResourceUsageHint(*terrainRvtGenerateDispatchArgsBuffer, "Terrain RVT");
    graph->RegisterResource(Builtin::Terrain::RvtGenerateDispatchArgs, terrainRvtGenerateDispatchArgsBuffer);

    const uint32_t terrainRvtPhysicalTileSide = TerrainRvt::PageSize() + TerrainRvt::BorderTexels() * 2u;
    const uint32_t terrainRvtAtlasSide = TerrainRvt::AtlasPagesWide() * terrainRvtPhysicalTileSide;
    const uint32_t terrainRvtAtlasHeight = TerrainRvt::AtlasPagesHigh() * terrainRvtPhysicalTileSide;
    const uint64_t terrainRvtAtlasTexels =
        static_cast<uint64_t>(terrainRvtAtlasSide) *
        static_cast<uint64_t>(terrainRvtAtlasHeight) *
        static_cast<uint64_t>(TerrainRvt::AtlasPoolCount());
    const uint64_t terrainRvtAtlasBytes = terrainRvtAtlasTexels * (2u + 4u + 8u + 4u);
    spdlog::info(
        "Terrain RVT atlas allocation: pageSize={} border={} tileSide={} pages={}x{} pools={} texture={}x{} totalAtlasBytes={} ({:.2f} MiB)",
        TerrainRvt::PageSize(),
        TerrainRvt::BorderTexels(),
        terrainRvtPhysicalTileSide,
        TerrainRvt::AtlasPagesWide(),
        TerrainRvt::AtlasPagesHigh(),
        TerrainRvt::AtlasPoolCount(),
        terrainRvtAtlasSide,
        terrainRvtAtlasHeight,
        terrainRvtAtlasBytes,
        static_cast<double>(terrainRvtAtlasBytes) / (1024.0 * 1024.0));
    auto createTerrainRvtAtlas = [&](std::string_view name, const char* debugName, rhi::Format format, uint32_t channels) {
        TextureDescription desc;
        desc.arraySize = TerrainRvt::AtlasPoolCount();
        desc.channels = channels;
        desc.isCubemap = false;
        desc.isArray = true;
        desc.hasSRV = true;
        desc.hasUAV = true;
        desc.hasNonShaderVisibleUAV = true;
        desc.format = format;
        desc.srvFormat = format;
        desc.uavFormat = format;
        desc.allowAlias = false;
        desc.imageDimensions.push_back({ terrainRvtAtlasSide, terrainRvtAtlasHeight, 0, 0 });
        auto texture = PixelBuffer::CreateSharedUnmaterialized(desc);
        texture->SetName(debugName);
        rg::memory::SetResourceUsageHint(*texture, "Terrain RVT");
        graph->RegisterResource(name, texture);
    };

    createTerrainRvtAtlas(Builtin::Terrain::RvtHeightAtlas, "TerrainRvt::HeightAtlas", rhi::Format::R16_Float, 1);
    createTerrainRvtAtlas(Builtin::Terrain::RvtAlbedoAtlas, "TerrainRvt::AlbedoAtlas", rhi::Format::R8G8B8A8_UNorm, 4);
    createTerrainRvtAtlas(Builtin::Terrain::RvtNormalAtlas, "TerrainRvt::NormalAtlas", rhi::Format::R16G16B16A16_Float, 4);
    createTerrainRvtAtlas(Builtin::Terrain::RvtMaterialAtlas, "TerrainRvt::MaterialAtlas", rhi::Format::R8G8B8A8_UNorm, 4);
}

void BuildGBufferPipeline(RenderGraph* graph) {
    RegisterVisUtilResources(graph);
    bool occlusionCulling = SettingsManager::GetInstance().getSettingGetter<bool>("enableOcclusionCulling")();
	bool enableWireframe = SettingsManager::GetInstance().getSettingGetter<bool>("enableWireframe")();
	bool useMeshShaders = SettingsManager::GetInstance().getSettingGetter<bool>("enableMeshShader")();
	bool indirect = SettingsManager::GetInstance().getSettingGetter<bool>("enableIndirectDraws")();
    bool visibilityRendering = SettingsManager::GetInstance().getSettingGetter<bool>("enableVisibilityRendering")();
    bool terrainRegionMaterialEvaluation = SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainRegionMaterialEvaluation")();
    bool terrainRvt = SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainRvt")();

    if (!useMeshShaders) {
        indirect = false; // Mesh shader pipelines are required for indirect draws
	}

    // Z prepass goes before light clustering for when active cluster determination is implemented
    bool clearRTVs = false;
    const bool needsVisibilityMaterialEvaluation = visibilityRendering;
    if (!needsVisibilityMaterialEvaluation && (!occlusionCulling || !indirect)) {
        clearRTVs = true; // We will not run an earlier pass
    }
    if (needsVisibilityMaterialEvaluation) {
        // Reset material counters
        graph->BuildComputePass<MaterialUAVResetPass>("MaterialPixelCounterResetPass");
        TagPassTechnique(graph, "MaterialPixelCounterResetPass", "Primary Visibility::GBuffer Construction::Material Groups");

        // Build material histogram
        graph->BuildComputePass<MaterialHistogramPass>("MaterialHistogramPass");
        TagPassTechnique(graph, "MaterialHistogramPass", "Primary Visibility::GBuffer Construction::Material Groups");

        // Prefix sum material histogram
        graph->BuildComputePass<MaterialBlockScanPass>("MaterialBlockScanPass");
        TagPassTechnique(graph, "MaterialBlockScanPass", "Primary Visibility::GBuffer Construction::Material Groups");

        graph->BuildComputePass<MaterialBlockOffsetsPass>("MaterialBlockOffsetsPass");
        TagPassTechnique(graph, "MaterialBlockOffsetsPass", "Primary Visibility::GBuffer Construction::Material Groups");

        // Build pixel list
        graph->BuildComputePass<BuildPixelListPass>("BuildPixelListPass");
        TagPassTechnique(graph, "BuildPixelListPass", "Primary Visibility::GBuffer Construction::VisUtil");

        // Build indirect command buffer for material passes
        graph->BuildComputePass<BuildMaterialIndirectCommandBufferPass>("BuildMaterialIndirectCommandBufferPass");
        TagPassTechnique(graph, "BuildMaterialIndirectCommandBufferPass", "Primary Visibility::GBuffer Construction::Material Groups");

        if (terrainRvt) {
            graph->BuildComputePass<TerrainRvtFrameResetPass>("TerrainRvtFrameResetPass");
            TagPassTechnique(graph, "TerrainRvtFrameResetPass", "Primary Visibility::Terrain RVT");

            graph->BuildComputePass<TerrainRvtResolveRequestsPass>("TerrainRvtResolveMaterialRequestsPass");
            TagPassTechnique(graph, "TerrainRvtResolveMaterialRequestsPass", "Primary Visibility::Terrain RVT");

            graph->BuildComputePass<TerrainRvtBuildGenerateDispatchArgsPass>("TerrainRvtBuildMaterialGenerateDispatchArgsPass");
            TagPassTechnique(graph, "TerrainRvtBuildMaterialGenerateDispatchArgsPass", "Primary Visibility::Terrain RVT");

            graph->BuildComputePass<TerrainRvtGeneratePagesPass>("TerrainRvtGenerateMaterialPagesPass");
            TagPassTechnique(graph, "TerrainRvtGenerateMaterialPagesPass", "Primary Visibility::Terrain RVT");

            graph->BuildComputePass<TerrainRvtFinalizeGeneratedPagesPass>("TerrainRvtFinalizeGeneratedMaterialPagesPass");
            TagPassTechnique(graph, "TerrainRvtFinalizeGeneratedMaterialPagesPass", "Primary Visibility::Terrain RVT");

            graph->BuildComputePass<TerrainRvtBuildHeightResidentCachePass>("TerrainRvtBuildHeightResidentCachePass");
            TagPassTechnique(graph, "TerrainRvtBuildHeightResidentCachePass", "Primary Visibility::Terrain RVT");

            graph->BuildComputePass<TerrainRvtClearFeedbackRequestsPass>("TerrainRvtClearFeedbackRequestsPass");
            TagPassTechnique(graph, "TerrainRvtClearFeedbackRequestsPass", "Primary Visibility::Terrain RVT");
        }

        if (terrainRegionMaterialEvaluation) {
            graph->BuildComputePass<TerrainRegionCounterResetPass>("TerrainRegionCounterResetPass");
            TagPassTechnique(graph, "TerrainRegionCounterResetPass", "Primary Visibility::GBuffer Construction::Terrain Regions");

            graph->BuildComputePass<TerrainRegionHistogramPass>("TerrainRegionHistogramPass");
            TagPassTechnique(graph, "TerrainRegionHistogramPass", "Primary Visibility::GBuffer Construction::Terrain Regions");

            graph->BuildComputePass<TerrainRegionBlockScanPass>("TerrainRegionBlockScanPass");
            TagPassTechnique(graph, "TerrainRegionBlockScanPass", "Primary Visibility::GBuffer Construction::Terrain Regions");

            graph->BuildComputePass<TerrainRegionBlockOffsetsPass>("TerrainRegionBlockOffsetsPass");
            TagPassTechnique(graph, "TerrainRegionBlockOffsetsPass", "Primary Visibility::GBuffer Construction::Terrain Regions");

            graph->BuildComputePass<TerrainRegionPixelListPass>("TerrainRegionPixelListPass");
            TagPassTechnique(graph, "TerrainRegionPixelListPass", "Primary Visibility::GBuffer Construction::Terrain Regions");

            graph->BuildComputePass<BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass>("BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass");
            TagPassTechnique(graph, "BuildTerrainRegionMaterialIndirectCommandBuildDispatchArgsPass", "Primary Visibility::GBuffer Construction::Terrain Regions");

            graph->BuildComputePass<BuildTerrainRegionMaterialIndirectCommandBufferPass>("BuildTerrainRegionMaterialIndirectCommandBufferPass");
            TagPassTechnique(graph, "BuildTerrainRegionMaterialIndirectCommandBufferPass", "Primary Visibility::GBuffer Construction::Terrain Regions");

            graph->BuildComputePass<EvaluateTerrainRegionMaterialGroupsPass>("EvaluateTerrainRegionMaterialGroupsPass");
            TagPassTechnique(graph, "EvaluateTerrainRegionMaterialGroupsPass", "Primary Visibility::GBuffer Construction::Terrain Regions");
        }

        // Evaluate material groups
        graph->BuildComputePass<EvaluateMaterialGroupsPass>("EvaluateMaterialGroupsPass");
        TagPassTechnique(graph, "EvaluateMaterialGroupsPass", "Primary Visibility::GBuffer Construction::Material Groups");

        // PrimaryDepthCopyPass is disabled for CLod two-phase path.
    }
}

void RegisterGTAOResources(RenderGraph* graph) {
    auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    constexpr uint64_t gtaoAliasPoolID = 1;

    TextureDescription workingDepthsDesc;
    workingDepthsDesc.arraySize = 1;
    workingDepthsDesc.channels = 1;
    workingDepthsDesc.isCubemap = false;
    workingDepthsDesc.hasUAV = true;
	workingDepthsDesc.hasSRV = true;
    workingDepthsDesc.format = rhi::Format::R32_Float;
    workingDepthsDesc.generateMipMaps = true;
    workingDepthsDesc.allowAlias = true;
    ImageDimensions dims1 = { resolution.x, resolution.y, 0, 0 };
    workingDepthsDesc.imageDimensions.push_back(dims1);
    auto workingDepths = PixelBuffer::CreateSharedUnmaterialized(workingDepthsDesc);
    //workingDepths->SetAliasingPool(gtaoAliasPoolID);
    rg::memory::SetResourceUsageHint(*workingDepths, "GTAO resources");
    workingDepths->SetName("GTAO Working Depths");

    TextureDescription workingEdgesDesc;
    workingEdgesDesc.arraySize = 1;
    workingEdgesDesc.channels = 1;
    workingEdgesDesc.isCubemap = false;
    workingEdgesDesc.hasUAV = true;
	workingEdgesDesc.hasSRV = true;
    workingEdgesDesc.format = rhi::Format::R8_UNorm;
    workingEdgesDesc.generateMipMaps = false;
    workingEdgesDesc.imageDimensions.push_back(dims1);
	workingEdgesDesc.allowAlias = true;
    auto workingEdges = PixelBuffer::CreateSharedUnmaterialized(workingEdgesDesc);
    rg::memory::SetResourceUsageHint(*workingDepths, "GTAO resources");
    workingEdges->SetName("GTAO Working Edges");

    TextureDescription workingAOTermDesc;
    workingAOTermDesc.arraySize = 1;
    workingAOTermDesc.channels = 1;
    workingAOTermDesc.isCubemap = false;
    workingAOTermDesc.hasUAV = true;
	workingAOTermDesc.hasSRV = true;
    workingAOTermDesc.format = rhi::Format::R8_UInt;
    workingAOTermDesc.generateMipMaps = false;
    workingAOTermDesc.imageDimensions.push_back(dims1);
    workingAOTermDesc.allowAlias = true;
    auto workingAOTerm1 = PixelBuffer::CreateSharedUnmaterialized(workingAOTermDesc);
    workingAOTerm1->SetName("GTAO Working AO Term 1");
    rg::memory::SetResourceUsageHint(*workingAOTerm1, "GTAO resources");
    auto workingAOTerm2 = PixelBuffer::CreateSharedUnmaterialized(workingAOTermDesc);
    workingAOTerm2->SetName("GTAO Working AO Term 2");
    rg::memory::SetResourceUsageHint(*workingAOTerm2, "GTAO resources");
    std::shared_ptr<PixelBuffer> outputAO = PixelBuffer::CreateSharedUnmaterialized(workingAOTermDesc);
    //outputAO->SetAliasingPool(gtaoAliasPoolID);
    outputAO->SetName("GTAO Output AO Term");
    rg::memory::SetResourceUsageHint(*outputAO, "GTAO resources");

    graph->RegisterResource(Builtin::GTAO::WorkingAOTerm1, workingAOTerm1);
    graph->RegisterResource(Builtin::GTAO::WorkingAOTerm2, workingAOTerm2);
    graph->RegisterResource(Builtin::GTAO::OutputAOTerm, outputAO);
    graph->RegisterResource(Builtin::GTAO::WorkingDepths, workingDepths);
    graph->RegisterResource(Builtin::GTAO::WorkingEdges, workingEdges);
}

void BuildGTAOPipeline(RenderGraph* graph, const Components::Camera* currentCamera) {
    auto GTAOConstantBuffer = CreateIndexedConstantBuffer(sizeof(GTAOInfo),"GTAO constants");

    graph->RegisterResource("Builtin::GTAO::ConstantsBuffer", GTAOConstantBuffer);

    graph->BuildComputePass<GTAOFilterPass>("GTAOFilterPass"); // Depth filter pass
    TagPassTechnique(graph, "GTAOFilterPass", "Post Process::GTAO");

    graph->BuildComputePass<GTAOMainPass>("GTAOMainPass"); // Main pass
    TagPassTechnique(graph, "GTAOMainPass", "Post Process::GTAO");

    graph->BuildComputePass<GTAODenoisePass>("GTAODenoisePass"); // Denoise pass
    TagPassTechnique(graph, "GTAODenoisePass", "Post Process::GTAO");
}

void BuildLightClusteringPipeline(RenderGraph* graph) {
    // light pages counter
    auto lightPagesCounter = Buffer::CreateUnmaterializedStructuredBuffer(
        1,
        sizeof(unsigned int),
        true,
        false,
        false,
        rhi::HeapType::DeviceLocal);
    lightPagesCounter->SetName("Light Pages Counter");
    graph->RegisterResource(Builtin::Light::PagesCounter, lightPagesCounter);

    graph->BuildComputePass<ClusterGenerationPass>("ClusterGenerationPass");
    TagPassTechnique(graph, "ClusterGenerationPass", "Lighting::Clustered Lighting");

    graph->BuildComputePass<LightCullingPass>("LightCullingPass");
    TagPassTechnique(graph, "LightCullingPass", "Lighting::Clustered Lighting");
}

void BuildEnvironmentPipeline(RenderGraph* graph) {
    graph->BuildComputePass<EnvironmentConversionPass>("Environment Conversion Pass");
    TagPassTechnique(graph, "Environment Conversion Pass", "Environment Lighting::Capture & Filtering");

    graph->BuildComputePass<EnvironmentSHPass>("Environment Spherical Harmonics Pass");
    TagPassTechnique(graph, "Environment Spherical Harmonics Pass", "Environment Lighting::Capture & Filtering");

    graph->BuildRenderPass<EnvironmentFilterPass>("Environment Prefilter Pass");
    TagPassTechnique(graph, "Environment Prefilter Pass", "Environment Lighting::Capture & Filtering");
}

void BuildLinearDepthDownsamplePass(RenderGraph* graph) {
    graph->BuildComputePass<DownsamplePass>("LinearDepthDownsamplePass");
    TagPassTechnique(graph, "LinearDepthDownsamplePass", "Depth::Linear Depth");
}

void BuildLinearDepthHistoryCopyPass(RenderGraph* graph) {
    graph->BuildRenderPass<LinearDepthHistoryCopyPass>("LinearDepthHistoryCopyPass");
    TagPassTechnique(graph, "LinearDepthHistoryCopyPass", "Post Process::Depth History");
}

void BuildPrimaryPass(RenderGraph* graph, Environment* currentEnvironment) {

	bool gtaoEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("enableGTAO")();
	bool meshShaders = SettingsManager::GetInstance().getSettingGetter<bool>("enableMeshShader")();
	bool indirect = SettingsManager::GetInstance().getSettingGetter<bool>("enableIndirectDraws")();
	bool wireframe = SettingsManager::GetInstance().getSettingGetter<bool>("enableWireframe")();

	// Uses existing GBuffer resources
    graph->BuildComputePass<DeferredShadingPass>("DeferredShadingPass");
    TagPassTechnique(graph, "DeferredShadingPass", "Lighting::Primary Shading");

    // Skybox needs the final opaque depth classification. In the CLod two-phase path,
    // a second linear-depth copy is inserted immediately before DeferredShadingPass.
    // Building the skybox here keeps it after that final depth write while still
    // letting forward/transparent passes blend over the background.
    if (currentEnvironment != nullptr) {
        graph->BuildComputePass<SkyboxRenderPass>("SkyboxPass");
        TagPassTechnique(graph, "SkyboxPass", "Lighting::Primary Shading");
    }

	// Forward pass for materials incompatible with deferred rendering
    graph->BuildRenderPass<ForwardRenderPass>("Forward render pass", ForwardRenderPassInputs{
        wireframe,
        meshShaders,
        indirect});
    TagPassTechnique(graph, "Forward render pass", "Lighting::Primary Shading");
}

void BuildPPLLPipeline(RenderGraph* graph) {
	auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
	bool useMeshShaders = SettingsManager::GetInstance().getSettingGetter<bool>("enableMeshShader")();
	bool indirect = SettingsManager::GetInstance().getSettingGetter<bool>("enableIndirectDraws")();
	bool wireframe = SettingsManager::GetInstance().getSettingGetter<bool>("enableWireframe")();
    if (!useMeshShaders) {
        indirect = false; // Mesh shader pipelines are required for indirect draws
	}

    static const size_t aveFragsPerPixel = 5;
    auto numPPLLNodes = resolution.x * resolution.y * aveFragsPerPixel;
    static const size_t PPLLNodeSize = 24; // two uints, four floats
    TextureDescription desc;
    ImageDimensions dimensions;
    dimensions.width = resolution.x;
    dimensions.height = resolution.y;
    dimensions.rowPitch = resolution.x * sizeof(unsigned int);
    dimensions.slicePitch = dimensions.rowPitch * resolution.y;
    desc.imageDimensions.push_back(dimensions);
    desc.channels = 1;
    desc.format = rhi::Format::R32_UInt;
    desc.hasRTV = false;
    desc.hasUAV = true;
    desc.hasNonShaderVisibleUAV = true;
    desc.allowAlias = true;
    //auto PPLLHeadPointerTexture = PixelBuffer::CreateSharedUnmaterialized(desc);
    //PPLLHeadPointerTexture->SetName("PPLLHeadPointerTexture");
    //rg::memory::SetResourceUsageHint(*PPLLHeadPointerTexture, "OIT resources");
    //auto PPLLBuffer = Buffer::CreateUnmaterializedStructuredBuffer(
    //    static_cast<uint32_t>(numPPLLNodes),
    //    static_cast<uint32_t>(PPLLNodeSize),
    //    true,
    //    false,
    //    false,
    //    rhi::HeapType::DeviceLocal);
    //PPLLBuffer->SetAllowAlias(true);
    //PPLLBuffer->SetName("PPLLBuffer");
    //rg::memory::SetResourceUsageHint(*PPLLBuffer, "OIT resources");
    //auto PPLLCounter = Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, sizeof(uint32_t), true);
    //{
    //    BufferBase::DescriptorRequirements descReq{};
    //    descReq.createCBV = false;
    //    descReq.createSRV = true;
    //    descReq.createUAV = true;
    //    descReq.createNonShaderVisibleUAV = true;
    //    descReq.uavCounterOffset = 0;

    //    descReq.srvDesc = rhi::SrvDesc{
    //        .dimension = rhi::SrvDim::Buffer,
    //        .formatOverride = rhi::Format::R32_UInt,
    //        .buffer = {
    //            .kind = rhi::BufferViewKind::Typed,
    //            .firstElement = 0,
    //            .numElements = 1,
    //            .structureByteStride = 0,
    //        },
    //    };

    //    descReq.uavDesc = rhi::UavDesc{
    //        .dimension = rhi::UavDim::Buffer,
    //        .formatOverride = rhi::Format::R32_UInt,
    //        .buffer = {
    //            .kind = rhi::BufferViewKind::Typed,
    //            .firstElement = 0,
    //            .numElements = 1,
    //            .structureByteStride = 0,
    //            .counterOffsetInBytes = 0,
    //        },
    //    };

    //    PPLLCounter->SetDescriptorRequirements(descReq);
    //}
    //PPLLCounter->SetName("PPLLCounter");
    //rg::memory::SetResourceUsageHint(*PPLLCounter, "OIT resources");

    //graph->RegisterResource(Builtin::PPLL::HeadPointerTexture, PPLLHeadPointerTexture);
    //graph->RegisterResource(Builtin::PPLL::DataBuffer, PPLLBuffer);
    //graph->RegisterResource(Builtin::PPLL::Counter, PPLLCounter);

    //graph->BuildRenderPass<PPLLFillPass>("PPFillPass", PPLLFillPassInputs{
    //    wireframe,
    //    numPPLLNodes,
    //    useMeshShaders,
    //    indirect });

    //graph->BuildRenderPass<PPLLResolvePass>("PPLLResolvePass");
}

void BuildBloomPipeline(RenderGraph* graph) {
	auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    // Calculate max mips
	unsigned int maxBloomMips = static_cast<unsigned int>(std::log2(std::max(resolution.x, resolution.y))) + 1;
    unsigned int numBloomMips = 5;
	if (maxBloomMips < numBloomMips) {
		numBloomMips = maxBloomMips; // Limit to max mips
	}

	// Downsample numBloomMips mips of the HDR color target
    for (unsigned int i = 0; i < numBloomMips; i++) {
        const std::string passName = "BloomDownsamplePass" + std::to_string(i);
        graph->BuildRenderPass<BloomSamplePass>(passName, BloomSamplePassInputs{ i, false });
        graph->SetPassTechnique(passName, "Post Process::Bloom");
    }

	// Upsample numBloomMips - 1 mips of the HDR color target, starting from the last mip
    for (unsigned int i = numBloomMips-1; i > 0; i--) {
        const std::string passName = "BloomUpsamplePass" + std::to_string(i);
        graph->BuildRenderPass<BloomSamplePass>(passName, BloomSamplePassInputs{ i, true });
        graph->SetPassTechnique(passName, "Post Process::Bloom");
    }
    
    // Upsample and blend the first mip with the HDR color target
    graph->BuildRenderPass<BloomBlendPass>("BloomUpsampleAndBlendPass");
    TagPassTechnique(graph, "BloomUpsampleAndBlendPass", "Post Process::Bloom");
}

void BuildSSRPasses(RenderGraph* graph) {
	auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();

    TextureDescription ssrDesc;
    ssrDesc.arraySize = 1;
    ssrDesc.channels = 4;
    ssrDesc.isCubemap = false;
    ssrDesc.hasRTV = true;
    ssrDesc.format = rhi::Format::R16G16B16A16_Float;
    ssrDesc.generateMipMaps = false;
    ssrDesc.hasSRV = true;
    ssrDesc.srvFormat = rhi::Format::R16G16B16A16_Float;
	ssrDesc.hasUAV = true;
	ssrDesc.uavFormat = rhi::Format::R16G16B16A16_Float;
	ssrDesc.hasNonShaderVisibleUAV = true; // For ClearUnorderedAccessView
    ImageDimensions dims = { resolution.x, resolution.y, 0, 0 };
    ssrDesc.imageDimensions.push_back(dims);
    ssrDesc.allowAlias = true;
    auto ssrTexture = PixelBuffer::CreateSharedUnmaterialized(ssrDesc);
    ssrTexture->SetName("SSR Texture");
    rg::memory::SetResourceUsageHint(*ssrTexture, "Post-Processing resources");
	graph->RegisterResource(Builtin::PostProcessing::ScreenSpaceReflections, ssrTexture);

    graph->BuildComputePass<ScreenSpaceReflectionsPass>("Screen-Space Reflections Pass");
    TagPassTechnique(graph, "Screen-Space Reflections Pass", "Post Process::Screen-Space Reflections");

    graph->BuildRenderPass<SpecularIBLPass>("Specular IBL & SSR Composite Pass");
    TagPassTechnique(graph, "Specular IBL & SSR Composite Pass", "Post Process::Screen-Space Reflections");
}

void BuildRayTracedReflectionPasses(RenderGraph* graph) {
    auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();

    TextureDescription rtReflectionDesc;
    rtReflectionDesc.arraySize = 1;
    rtReflectionDesc.channels = 4;
    rtReflectionDesc.isCubemap = false;
    rtReflectionDesc.hasRTV = true;
    rtReflectionDesc.format = rhi::Format::R16G16B16A16_Float;
    rtReflectionDesc.generateMipMaps = false;
    rtReflectionDesc.hasSRV = true;
    rtReflectionDesc.srvFormat = rhi::Format::R16G16B16A16_Float;
    rtReflectionDesc.hasUAV = true;
    rtReflectionDesc.uavFormat = rhi::Format::R16G16B16A16_Float;
    rtReflectionDesc.hasNonShaderVisibleUAV = true;
    rtReflectionDesc.imageDimensions.push_back({ resolution.x, resolution.y, 0, 0 });
    rtReflectionDesc.allowAlias = true;

    auto rtReflectionTexture = PixelBuffer::CreateSharedUnmaterialized(rtReflectionDesc);
    rtReflectionTexture->SetName("Ray Traced Reflections Texture");
    rg::memory::SetResourceUsageHint(*rtReflectionTexture, "Post-Processing resources");
    graph->RegisterResource(Builtin::PostProcessing::ScreenSpaceReflections, rtReflectionTexture);

    graph->BuildComputePass<RayTracedReflectionsPass>("Ray Traced Reflections Pass");
    TagPassTechnique(graph, "Ray Traced Reflections Pass", "Ray Tracing::Reflections");

    graph->BuildRenderPass<SpecularIBLPass>("Specular IBL & RT Reflections Composite Pass");
    TagPassTechnique(graph, "Specular IBL & RT Reflections Composite Pass", "Ray Tracing::Reflections");
}
