#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include <rhi.h>

#include "Managers/Singletons/SettingsManager.h"
#include "Mesh/ClusterLODTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "ShaderBuffers.h"

inline constexpr const char* CLodStreamingMeshManagerGetterSettingName = "getMeshManager";
inline constexpr const char* CLodStreamingCpuUploadBudgetSettingName = "clodStreamingCpuUploadBudgetRequests";
inline constexpr const char* CLodStreamingEnableDirectStorageSettingName = "clodStreamingEnableDirectStorage";
inline constexpr const char* CLodDisableReyesRasterizationSettingName = "clodDisableReyesRasterization";
inline constexpr const char* CLodReyesUseNormalMapsSettingName = "clodReyesUseNormalMaps";
inline constexpr const char* CLodReyesUseAabbOcclusionSettingName = "clodReyesUseAabbOcclusion";
inline constexpr const char* CLodReyesResourceBudgetBytesSettingName = "clodReyesResourceBudgetBytes";
inline constexpr const char* CLodDisableVirtualShadowPageCachingSettingName = "clodDisableVirtualShadowPageCaching";
inline constexpr const char* CLodEnablePageJobVSMSettingName = "clodEnablePageJobVSM";
inline constexpr const char* CLodVSMRasterModeSettingName = "clodVsmRasterMode";
inline constexpr const char* CLodReyesShadowCoarseTargetPagesPerTriangleSettingName = "clodReyesShadowCoarseTargetPagesPerTriangle";
inline constexpr const char* CLodPageJobDiameterThresholdSettingName = "clodPageJobDiameterThreshold";
inline constexpr const char* CLodPageJobSparseRatioSettingName = "clodPageJobSparseRatio";
inline constexpr const char* CLodPageJobMaxPagesPerClusterSettingName = "clodPageJobMaxPagesPerCluster";
inline constexpr const char* CLodPageJobRecordCapacitySettingName = "clodPageJobRecordCapacity";
inline constexpr const char* CLodPageJobForceAllSettingName = "clodPageJobForceAll";
inline constexpr const char* CLodForceTraversalDepthRootSettingName = "clodForceTraversalDepthRoot";
inline constexpr const char* CLodPureComputePhase2ExpansionFactorSettingName = "clodPureComputePhase2ExpansionFactor";
inline constexpr const char* CLodFrustumCullingSettingName = "clodFrustumCulling";
inline constexpr uint32_t CLodPureComputePhase2ExpansionFactorDefault = 2u;
inline constexpr uint32_t CLodPureComputePhase2ExpansionFactorMin = 1u;
inline constexpr uint32_t CLodPureComputePhase2ExpansionFactorMax = 64u;
inline constexpr uint32_t CLodForceTraversalDepthRootDisabled = 0xFFFFFFFFu;
inline constexpr const char* CLodWorkGraphComputePageJobDescriptorBufferId = "CLod::WorkGraphComputePageJobDescriptors";
inline constexpr const char* CLodLevelInfosBufferId = "Builtin::CLod::LevelInfos";
inline constexpr const char* CLodDirectionalVirtualShadowMaxBackingResolutionSettingName = "clodDirectionalVirtualShadowMaxBackingResolution";
inline constexpr const char* CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName = "clodDirectionalVirtualShadowMaxPhysicalPages";
inline constexpr const char* CLodDirectionalVirtualShadowLodBiasSettingName = "clodDirectionalVirtualShadowLodBias";
inline constexpr const char* CLodDirectionalVirtualShadowAutoLodBiasSettingName = "clodDirectionalVirtualShadowAutoLodBias";
inline constexpr const char* CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName = "clodDirectionalVirtualShadowAutoLodBiasScale";
inline constexpr const char* CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName = "clodDirectionalVirtualShadowPredictiveLodInvalidation";
inline constexpr const char* CLodDirectionalVirtualShadowSourceAngleDegreesSettingName = "clodDirectionalVirtualShadowSourceAngleDegrees";
inline constexpr const char* CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName = "clodDirectionalVirtualShadowSmrtRayCountDirectional";
inline constexpr const char* CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName = "clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional";
inline constexpr const char* CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName = "clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees";
inline constexpr const char* CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName = "clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional";
inline constexpr const char* CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName = "clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld";
inline constexpr const char* CLodTransparencyModeSettingName = "clodTransparencyMode";
enum class CLodPriorityMode : uint8_t {
    Max, // Duplicate group requests keep the maximum reported priority
    Sum, // Duplicate group requests accumulate (sum) their priorities
};

enum class CLodSoftwareRasterMode : uint8_t {
    Disabled,
    Compute,
    WorkGraph,
};

enum class CLodCullingBackend : uint8_t {
    WorkGraph,
    PureCompute,
};

enum class CLodVSMRasterMode : uint8_t {
    HardwareOnly,
    Standard,
    PageJob,
    Reyes,
};

enum class CLodRasterOutputKind : uint8_t {
    VisibilityBuffer,
    VirtualShadow,
    DeepVisibility,
    AVBOITOccupancy,
    AVBOIT,
    AVBOITShading,
};

enum class CLodTransparencyMode : uint8_t {
    LinkedListDeepVisibility,
    AVBOIT,
    Disabled,
};

inline constexpr const char* CLodSoftwareRasterModeSettingName = "clodSoftwareRasterMode";
inline constexpr const char* CLodCullingBackendSettingName = "clodCullingBackend";
inline constexpr const char* CLodSoftwareRasterModeNames[] = {
    "Disabled",
    "Compute",
    "Work Graph",
};
inline constexpr int CLodSoftwareRasterModeCount = static_cast<int>(sizeof(CLodSoftwareRasterModeNames) / sizeof(CLodSoftwareRasterModeNames[0]));
inline constexpr const char* CLodCullingBackendNames[] = {
    "Work Graph",
    "Pure Compute",
};
inline constexpr int CLodCullingBackendCount = static_cast<int>(sizeof(CLodCullingBackendNames) / sizeof(CLodCullingBackendNames[0]));
inline constexpr const char* CLodVSMRasterModeNames[] = {
    "Hardware Only",
    "Standard",
    "Page-Job",
    "Reyes",
};
inline constexpr int CLodVSMRasterModeCount = static_cast<int>(sizeof(CLodVSMRasterModeNames) / sizeof(CLodVSMRasterModeNames[0]));
inline constexpr const char* CLodTransparencyModeNames[] = {
    "Linked-List Deep Visibility",
    "AVBOIT",
    "Disabled",
};
inline constexpr int CLodTransparencyModeCount = static_cast<int>(sizeof(CLodTransparencyModeNames) / sizeof(CLodTransparencyModeNames[0]));
inline constexpr float CLodReyesShadowCoarseTargetPagesPerTriangleDefault = 10.0f;
inline constexpr float CLodReyesShadowCoarseTargetPagesPerTriangleMin = 0.25f;
inline constexpr float CLodReyesShadowCoarseTargetPagesPerTriangleMax = 64.0f;
inline constexpr uint32_t CLodAVBOITDefaultSliceCount = 16u;
inline constexpr uint32_t CLodAVBOITDefaultVirtualSliceCount = 32u;
inline constexpr uint32_t CLodAVBOITDepthWarpLUTResolution = 8192u;
inline constexpr uint32_t CLodAVBOITDefaultDownsampleFactor = 4u;
inline constexpr float CLodAVBOITExtinctionQuantizationScale = 4096.0f;
inline constexpr float CLodAVBOITMinDepthDistributionExponent = 0.5f;
inline constexpr float CLodAVBOITDefaultDepthDistributionExponent = 1.0f;
inline constexpr float CLodAVBOITMaxDepthDistributionExponent = 2.0f;
inline constexpr float CLodAVBOITDefaultLookupDepthBiasInSlices = 2.0f;
inline constexpr float CLodAVBOITDefaultZeroTransmittanceThreshold = 1.0e-3f;
inline constexpr float CLodAVBOITDefaultResolutionScale = 1.0f / static_cast<float>(CLodAVBOITDefaultDownsampleFactor);
static_assert(CLodAVBOITDefaultVirtualSliceCount <= 32u, "AVBOIT occupancy slice mask supports up to 32 virtual slices");

constexpr bool CLodSoftwareRasterEnabled(CLodSoftwareRasterMode mode)
{
    return mode != CLodSoftwareRasterMode::Disabled;
}

constexpr bool CLodSoftwareRasterUsesCompute(CLodSoftwareRasterMode mode)
{
    return mode == CLodSoftwareRasterMode::Compute;
}

constexpr bool CLodSoftwareRasterUsesWorkGraph(CLodSoftwareRasterMode mode)
{
    return mode == CLodSoftwareRasterMode::WorkGraph;
}

constexpr bool CLodVSMRasterModeUsesLegacyRasterOnly(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::HardwareOnly;
}

constexpr bool CLodVSMRasterModeUsesLargeClusterPageJob(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::PageJob;
}

constexpr bool CLodVSMRasterModeUsesLargeClusterShadowRouting(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::PageJob || mode == CLodVSMRasterMode::Reyes;
}

constexpr bool CLodVSMRasterModeUsesReyes(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::Reyes;
}

constexpr uint32_t CLodNormalizePureComputePhase2ExpansionFactor(uint32_t value)
{
    value = std::clamp(
        value,
        CLodPureComputePhase2ExpansionFactorMin,
        CLodPureComputePhase2ExpansionFactorMax);
    uint32_t normalized = CLodPureComputePhase2ExpansionFactorMin;
    while ((normalized << 1u) <= value && (normalized << 1u) <= CLodPureComputePhase2ExpansionFactorMax) {
        normalized <<= 1u;
    }
    return normalized;
}

struct RasterBucketsHistogramIndirectCommand
{
    unsigned int clusterCount;
    unsigned int dispatchXDimension;
    unsigned int dispatchX, dispatchY, dispatchZ;
};

struct RasterizeClustersCommand
{
    unsigned int baseClusterOffset;
    unsigned int xDim;
    unsigned int rasterBucketID;
    unsigned int dispatchX, dispatchY, dispatchZ;
};

struct CLodSoftwareRasterPageJobRecord
{
    uint32_t sortedClusterIndex = 0u;
    uint32_t physicalPageIndex = 0u;
    uint32_t packedPagePixelOrigin = 0u;
    uint32_t packedAtlasOrigin = 0u;
    uint32_t clipmapLayer = 0u;
    uint32_t wrappedPageX = 0u;
    uint32_t wrappedPageY = 0u;
};

static_assert(sizeof(CLodSoftwareRasterPageJobRecord) == 28u, "CLodSoftwareRasterPageJobRecord size must match HLSL");

struct CLodWorkGraphComputePageJobDescriptors
{
    uint32_t visibleClustersUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t visibleClustersCounterUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
};

static_assert(sizeof(CLodWorkGraphComputePageJobDescriptors) == 16u, "CLodWorkGraphComputePageJobDescriptors size must match HLSL");

inline constexpr const char* CLodVoxelRasterQueueDescriptorBufferId = "CLod::VoxelRasterQueueDescriptors";

struct CLodVoxelRasterQueueDescriptors
{
    uint32_t workRecordsUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t workRecordCounterUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t workRecordCapacity = 0u;
    uint32_t pad0 = 0u;
};

static_assert(sizeof(CLodVoxelRasterQueueDescriptors) == 16u, "CLodVoxelRasterQueueDescriptors size must match HLSL");

struct CLodVoxelRasterWorkRecord
{
    uint32_t visibleClusterIndex = 0u;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
    uint32_t pad2 = 0u;
};

static_assert(sizeof(CLodVoxelRasterWorkRecord) == 16u, "CLodVoxelRasterWorkRecord size must match HLSL");

struct CLodVoxelRasterDispatchCommand
{
    uint32_t dispatchX = 0u;
    uint32_t dispatchY = 0u;
    uint32_t dispatchZ = 0u;
};

static_assert(sizeof(CLodVoxelRasterDispatchCommand) == 12u, "CLodVoxelRasterDispatchCommand size must match HLSL");

struct CLodViewRasterInfo
{
    uint32_t visibilityUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t opaqueVisibilitySRVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t deepVisibilityHeadPointerUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t scissorMinX;
    uint32_t scissorMinY;
    uint32_t scissorMaxX;
    uint32_t scissorMaxY;
    float viewportScaleX;
    float viewportScaleY;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;

    friend bool operator==(const CLodViewRasterInfo&, const CLodViewRasterInfo&) = default;
};

static_assert(sizeof(CLodViewRasterInfo) == 48u, "CLodViewRasterInfo size must match HLSL");

struct CLodDeepVisibilityNode
{
    uint64_t visKey;
    uint32_t next;
    uint32_t flags;
};

static_assert(sizeof(CLodDeepVisibilityNode) == 16u, "CLodDeepVisibilityNode size must match HLSL");

struct CLodDeepVisibilityStats
{
    uint32_t truncatedPixelCount = 0u;
    uint32_t truncatedNodeCount = 0u;
    uint32_t totalResolvedSamples = 0u;
    uint32_t maxRawNodeCount = 0u;
    uint32_t maxResolvedSamples = 0u;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
    uint32_t pad2 = 0u;
};

static_assert(sizeof(CLodDeepVisibilityStats) == 32u, "CLodDeepVisibilityStats size must match HLSL");

struct CLodAVBOITConfig
{
    uint32_t occupancyUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t coverageUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t occupancySliceMaskUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t depthWarpLUTSRVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t scalarExtinctionUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t chromaticExtinctionUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t integratedTransmittanceUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t shadingTransmittanceSRVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t zeroTransmittanceSliceUAVDescriptorIndex = 0xFFFFFFFFu;
    uint32_t sliceCount = 0u;
    uint32_t virtualSliceCount = 0u;
    uint32_t lowResolutionWidth = 0u;
    uint32_t lowResolutionHeight = 0u;
    float viewNearDepth = 0.0f;
    float viewFarDepth = 0.0f;
    float depthDistributionExponent = CLodAVBOITDefaultDepthDistributionExponent;
    float lookupDepthBiasInSlices = CLodAVBOITDefaultLookupDepthBiasInSlices;
    float zeroTransmittanceThreshold = CLodAVBOITDefaultZeroTransmittanceThreshold;
    float pad0 = 0.0f;
};

static_assert(sizeof(CLodAVBOITConfig) == 76u, "CLodAVBOITConfig size must match HLSL");

inline constexpr uint32_t CLodAVBOITDepthWarpFlagFilterToNext = 1u;

struct CLodAVBOITDepthWarpLUTEntry
{
    float warpedSliceCoordinate = 0.0f;
    uint32_t flags = 0u;
};

static_assert(sizeof(CLodAVBOITDepthWarpLUTEntry) == 8u, "CLodAVBOITDepthWarpLUTEntry size must match HLSL");

struct CLodAVBOITFitState
{
    uint32_t fittedVirtualSliceCount = CLodAVBOITDefaultVirtualSliceCount;
    uint32_t occupiedVirtualSliceCount = 0u;
    float fittedDepthDistributionExponent = CLodAVBOITDefaultDepthDistributionExponent;
    uint32_t pad1 = 0u;
};

static_assert(sizeof(CLodAVBOITFitState) == 16u, "CLodAVBOITFitState size must match HLSL");

struct CLodAVBOITEarlyDepthTileIndirectCommand
{
    uint32_t lowResolutionPixelX = 0u;
    uint32_t lowResolutionPixelY = 0u;
    uint32_t zeroTransmittanceSlice = 0u;
    uint32_t vertexCountPerInstance = 4u;
    uint32_t instanceCount = 1u;
    uint32_t startVertexLocation = 0u;
    uint32_t startInstanceLocation = 0u;
};

static_assert(sizeof(CLodAVBOITEarlyDepthTileIndirectCommand) == 28u, "CLodAVBOITEarlyDepthTileIndirectCommand size must match HLSL");

inline constexpr uint32_t CLodVirtualShadowMaxSupportedClipmapCount = 22u;
inline constexpr uint32_t CLodVirtualShadowDefaultClipmapCount = 22u;
inline constexpr uint32_t CLodVirtualShadowPhysicalPageSize = 128u;
inline constexpr uint32_t CLodVirtualShadowFixedVirtualPageCountPerAxis = 128u;
inline constexpr uint32_t CLodVirtualShadowFixedVirtualResolution =
    CLodVirtualShadowFixedVirtualPageCountPerAxis * CLodVirtualShadowPhysicalPageSize;
inline constexpr uint32_t CLodVirtualShadowDefaultVirtualResolution = CLodVirtualShadowFixedVirtualResolution;
inline constexpr uint32_t CLodVirtualShadowMaxVirtualResolution = CLodVirtualShadowFixedVirtualResolution;
inline constexpr uint32_t CLodVirtualShadowDefaultPageTableResolution =
    CLodVirtualShadowDefaultVirtualResolution / CLodVirtualShadowPhysicalPageSize;
inline constexpr uint32_t CLodVirtualShadowMaxPageTableResolution =
    CLodVirtualShadowMaxVirtualResolution / CLodVirtualShadowPhysicalPageSize;
inline constexpr uint32_t CLodVirtualShadowMinBackingResolution = 4096u;
inline constexpr uint32_t CLodVirtualShadowMediumBackingResolution = 8192u;
inline constexpr uint32_t CLodVirtualShadowMaxBackingResolution = 16384u;
inline constexpr uint32_t CLodVirtualShadowDefaultBackingResolution = CLodVirtualShadowMaxBackingResolution;
inline constexpr uint32_t CLodVirtualShadowDefaultPhysicalAtlasPagesWide =
    CLodVirtualShadowDefaultBackingResolution / CLodVirtualShadowPhysicalPageSize;
inline constexpr uint32_t CLodVirtualShadowDefaultPhysicalAtlasPagesHigh =
    CLodVirtualShadowDefaultBackingResolution / CLodVirtualShadowPhysicalPageSize;
inline constexpr uint32_t CLodVirtualShadowMaxPhysicalAtlasPagesWide =
    CLodVirtualShadowMaxBackingResolution / CLodVirtualShadowPhysicalPageSize;
inline constexpr uint32_t CLodVirtualShadowMaxPhysicalAtlasPagesHigh =
    CLodVirtualShadowMaxBackingResolution / CLodVirtualShadowPhysicalPageSize;
inline constexpr uint32_t CLodVirtualShadowDefaultPhysicalPageCount =
    CLodVirtualShadowDefaultPhysicalAtlasPagesWide * CLodVirtualShadowDefaultPhysicalAtlasPagesHigh;
inline constexpr uint32_t CLodVirtualShadowMaxPhysicalPageCount =
    CLodVirtualShadowMaxPhysicalAtlasPagesWide * CLodVirtualShadowMaxPhysicalAtlasPagesHigh;
inline constexpr float CLodVirtualShadowDefaultDirectionalLodBias = 3.0f;
inline constexpr float CLodVirtualShadowDefaultDirectionalSourceAngleDegrees = 6.0f;
inline constexpr uint32_t CLodVirtualShadowDefaultSmrtRayCountDirectional = 2u;
inline constexpr uint32_t CLodVirtualShadowDefaultSmrtSamplesPerRayDirectional = 2u;
inline constexpr float CLodVirtualShadowDefaultSmrtMaxRayAngleFromLightDegrees = 5.0f;
inline constexpr float CLodVirtualShadowDefaultSmrtRayLengthScaleDirectional = 0.02f;
inline constexpr float CLodVirtualShadowDefaultSmrtMaxTraceDistanceWorld = 150.0f;
inline constexpr uint32_t CLodVirtualShadowMarkTileSize = 16u;
inline constexpr uint32_t CLodVirtualShadowBlockPagesPerAxis = 4u;
inline constexpr uint32_t CLodVirtualShadowBlockPackedPhysicalPageIndexCount =
    (CLodVirtualShadowBlockPagesPerAxis * CLodVirtualShadowBlockPagesPerAxis) / 2u;
inline constexpr uint32_t CLodVirtualShadowBlockMaxTrackedPerCluster = 32u;
inline constexpr uint32_t CLodVirtualShadowMaxBlocksPerAxis =
    (CLodVirtualShadowMaxPageTableResolution + CLodVirtualShadowBlockPagesPerAxis - 1u) / CLodVirtualShadowBlockPagesPerAxis;
inline constexpr uint32_t CLodVirtualShadowMaxBlocksPerClipmap =
    CLodVirtualShadowMaxBlocksPerAxis * CLodVirtualShadowMaxBlocksPerAxis;
inline constexpr uint32_t CLodVirtualShadowMaxMarkedBlockCount =
    CLodVirtualShadowMaxBlocksPerClipmap * CLodVirtualShadowMaxSupportedClipmapCount;
inline constexpr uint32_t CLodVirtualShadowMaxMarkTileGridDimension = 512u;
inline constexpr uint32_t CLodVirtualShadowMaxMarkTileCount =
    CLodVirtualShadowMaxMarkTileGridDimension * CLodVirtualShadowMaxMarkTileGridDimension;
inline constexpr uint32_t CLodVirtualShadowMaxAllocationRequests = 1u << 16;
inline constexpr uint32_t CLodVirtualShadowMaxInvalidationInputs = 1u << 16;
inline constexpr uint32_t CLodPageJobDefaultRecordCapacity = 1u << 20;
inline constexpr uint32_t CLodVirtualShadowMovedInstanceBitCapacity = 1u << 20;
inline constexpr uint32_t CLodVirtualShadowPredictiveCandidateCapacity = 1u << 16;
inline constexpr uint32_t CLodVirtualShadowPredictiveRawPageCapacity = 1u << 20;
inline constexpr uint32_t CLodVirtualShadowClipmapValidFlag = 0x1u;
inline constexpr uint32_t CLodVirtualShadowClipmapInvalidateFlag = 0x2u;
inline constexpr uint32_t CLodVirtualShadowPageAllocatedMask = 0x80000000u;
inline constexpr uint32_t CLodVirtualShadowPageDirtyMask = 0x40000000u;
inline constexpr uint32_t CLodVirtualShadowPageContentValidMask = 0x20000000u;
inline constexpr uint32_t CLodVirtualShadowPageVisitedMask = 0x10000000u;
inline constexpr uint32_t CLodVirtualShadowPageRerenderedThisFrameMask = 0x08000000u;
inline constexpr uint32_t CLodVirtualShadowPhysicalPageIndexMask = 0x07FFFFFFu;
inline constexpr uint32_t CLodVirtualShadowPhysicalPageResidentFlag = 0x1u;
inline constexpr uint32_t CLodVirtualShadowInvalidationFlagUsePreviousBounds = 0x1u;
inline constexpr uint32_t CLodVirtualShadowInvalidationFlagUseCurrentBounds = 0x2u;
inline constexpr uint32_t CLodVirtualShadowInvalidationFlagSkinned = 0x4u;

constexpr uint32_t CLodVirtualShadowDirtyWordCount(uint32_t physicalPageCount)
{
    return (physicalPageCount + 31u) / 32u;
}

constexpr uint32_t CLodVirtualShadowBlockCountPerAxis(uint32_t pageTableResolution)
{
    return (pageTableResolution + CLodVirtualShadowBlockPagesPerAxis - 1u) / CLodVirtualShadowBlockPagesPerAxis;
}

constexpr uint32_t CLodVirtualShadowSanitizeBackingResolution(uint32_t backingResolution)
{
    if (backingResolution <= CLodVirtualShadowMinBackingResolution) {
        return CLodVirtualShadowMinBackingResolution;
    }

    if (backingResolution <= CLodVirtualShadowMediumBackingResolution) {
        return CLodVirtualShadowMediumBackingResolution;
    }

    return CLodVirtualShadowMaxBackingResolution;
}

constexpr uint32_t CLodVirtualShadowPhysicalAtlasPageAxisLimitFromBackingResolution(uint32_t backingResolution)
{
    return CLodVirtualShadowSanitizeBackingResolution(backingResolution) / CLodVirtualShadowPhysicalPageSize;
}

constexpr uint32_t CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(uint32_t backingResolution)
{
    const uint32_t axisLimit = CLodVirtualShadowPhysicalAtlasPageAxisLimitFromBackingResolution(backingResolution);
    return axisLimit * axisLimit;
}

constexpr uint32_t CLodVirtualShadowMovedInstanceBitWordCount()
{
    return (CLodVirtualShadowMovedInstanceBitCapacity + 31u) / 32u;
}

constexpr uint32_t CLodVirtualShadowPredictedPageListCapacity()
{
    return CLodVirtualShadowPredictiveRawPageCapacity;
}

constexpr uint32_t CLodVirtualShadowPredictedPageBitsetWordCount()
{
    return (CLodVirtualShadowPredictedPageListCapacity() + 31u) / 32u;
}

constexpr uint32_t CLodVirtualShadowSanitizeVirtualResolution(uint32_t virtualResolution)
{
    (void)virtualResolution;
    return CLodVirtualShadowFixedVirtualResolution;
}

constexpr uint32_t CLodVirtualShadowPageTableResolutionFromVirtualResolution(uint32_t virtualResolution)
{
    return CLodVirtualShadowSanitizeVirtualResolution(virtualResolution) / CLodVirtualShadowPhysicalPageSize;
}

constexpr uint32_t CLodVirtualShadowPhysicalAtlasPagesWideFromPageCount(
    uint32_t maxPhysicalPages,
    uint32_t maxAtlasPagesWide = CLodVirtualShadowMaxPhysicalAtlasPagesWide)
{
    if (maxPhysicalPages == 0u) {
        return 0u;
    }

    const uint32_t sanitizedAtlasPagesWide = maxAtlasPagesWide > CLodVirtualShadowMaxPhysicalAtlasPagesWide
        ? CLodVirtualShadowMaxPhysicalAtlasPagesWide
        : maxAtlasPagesWide;

    return maxPhysicalPages < sanitizedAtlasPagesWide
        ? maxPhysicalPages
        : sanitizedAtlasPagesWide;
}

constexpr uint32_t CLodVirtualShadowPhysicalAtlasPagesHighFromPageCount(uint32_t maxPhysicalPages, uint32_t atlasPagesWide)
{
    return atlasPagesWide == 0u ? 0u : ((maxPhysicalPages + atlasPagesWide - 1u) / atlasPagesWide);
}

constexpr uint32_t CLodVirtualShadowSanitizePhysicalPageCount(uint32_t maxPhysicalPages)
{
    return maxPhysicalPages > CLodVirtualShadowMaxPhysicalPageCount
        ? CLodVirtualShadowMaxPhysicalPageCount
        : maxPhysicalPages;
}

constexpr uint32_t CLodVirtualShadowSanitizePhysicalPageCount(uint32_t maxPhysicalPages, uint32_t maxSupportedPhysicalPages)
{
    const uint32_t sanitizedMaxSupportedPhysicalPages =
        maxSupportedPhysicalPages > CLodVirtualShadowMaxPhysicalPageCount
        ? CLodVirtualShadowMaxPhysicalPageCount
        : maxSupportedPhysicalPages;
    return maxPhysicalPages > sanitizedMaxSupportedPhysicalPages
        ? sanitizedMaxSupportedPhysicalPages
        : maxPhysicalPages;
}

constexpr uint32_t CLodVirtualShadowPhysicalAtlasPagesWideFromPhysicalPageCount(uint32_t maxPhysicalPages)
{
    return CLodVirtualShadowPhysicalAtlasPagesWideFromPageCount(
        CLodVirtualShadowSanitizePhysicalPageCount(maxPhysicalPages));
}

constexpr uint32_t CLodVirtualShadowPhysicalAtlasPagesWideFromPhysicalPageCount(
    uint32_t maxPhysicalPages,
    uint32_t backingResolution)
{
    const uint32_t sanitizedMaxPhysicalPages = CLodVirtualShadowSanitizePhysicalPageCount(
        maxPhysicalPages,
        CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(backingResolution));
    return CLodVirtualShadowPhysicalAtlasPagesWideFromPageCount(
        sanitizedMaxPhysicalPages,
        CLodVirtualShadowPhysicalAtlasPageAxisLimitFromBackingResolution(backingResolution));
}

constexpr uint32_t CLodVirtualShadowPhysicalAtlasPagesHighFromPhysicalPageCount(uint32_t maxPhysicalPages)
{
    const uint32_t physicalPageCount = CLodVirtualShadowSanitizePhysicalPageCount(maxPhysicalPages);
    const uint32_t atlasPagesWide = CLodVirtualShadowPhysicalAtlasPagesWideFromPageCount(physicalPageCount);
    return CLodVirtualShadowPhysicalAtlasPagesHighFromPageCount(physicalPageCount, atlasPagesWide);
}

constexpr uint32_t CLodVirtualShadowPhysicalAtlasPagesHighFromPhysicalPageCount(
    uint32_t maxPhysicalPages,
    uint32_t backingResolution)
{
    const uint32_t physicalPageCount = CLodVirtualShadowSanitizePhysicalPageCount(
        maxPhysicalPages,
        CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(backingResolution));
    const uint32_t atlasPagesWide = CLodVirtualShadowPhysicalAtlasPagesWideFromPhysicalPageCount(
        physicalPageCount,
        backingResolution);
    return CLodVirtualShadowPhysicalAtlasPagesHighFromPageCount(physicalPageCount, atlasPagesWide);
}

constexpr uint32_t CLodVirtualShadowMaxDirectionalPageViewInfoEntryCount()
{
    return CLodVirtualShadowMaxSupportedClipmapCount *
        CLodVirtualShadowMaxPageTableResolution *
        CLodVirtualShadowMaxPageTableResolution;
}

struct CLodVirtualShadowResolutionConfig
{
    uint32_t virtualResolution = CLodVirtualShadowDefaultVirtualResolution;
    uint32_t pageTableResolution = CLodVirtualShadowDefaultPageTableResolution;
    uint32_t physicalAtlasPagesWide = CLodVirtualShadowDefaultPhysicalAtlasPagesWide;
    uint32_t physicalAtlasPagesHigh = CLodVirtualShadowDefaultPhysicalAtlasPagesHigh;
    uint32_t maxPhysicalPages = CLodVirtualShadowDefaultPhysicalPageCount;
    uint32_t maxAllocationRequests = (std::min)(CLodVirtualShadowDefaultPhysicalPageCount, CLodVirtualShadowMaxAllocationRequests);
    float directionalLodBias = CLodVirtualShadowDefaultDirectionalLodBias;
};

constexpr CLodVirtualShadowResolutionConfig CLodVirtualShadowBuildResolutionConfig(
    uint32_t virtualResolution,
    uint32_t maxPhysicalPages,
    float directionalLodBias = CLodVirtualShadowDefaultDirectionalLodBias,
    uint32_t backingResolution = CLodVirtualShadowDefaultBackingResolution)
{
    const uint32_t sanitizedVirtualResolution = CLodVirtualShadowSanitizeVirtualResolution(virtualResolution);
    const uint32_t maxSupportedPhysicalPages = CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(backingResolution);
    const uint32_t sanitizedMaxPhysicalPages = CLodVirtualShadowSanitizePhysicalPageCount(
        maxPhysicalPages,
        maxSupportedPhysicalPages);
    const uint32_t physicalAtlasPagesWide = CLodVirtualShadowPhysicalAtlasPagesWideFromPhysicalPageCount(
        sanitizedMaxPhysicalPages,
        backingResolution);

    return {
        .virtualResolution = sanitizedVirtualResolution,
        .pageTableResolution = CLodVirtualShadowPageTableResolutionFromVirtualResolution(sanitizedVirtualResolution),
        .physicalAtlasPagesWide = physicalAtlasPagesWide,
        .physicalAtlasPagesHigh = CLodVirtualShadowPhysicalAtlasPagesHighFromPageCount(sanitizedMaxPhysicalPages, physicalAtlasPagesWide),
        .maxPhysicalPages = sanitizedMaxPhysicalPages,
        .maxAllocationRequests = (std::min)(sanitizedMaxPhysicalPages, CLodVirtualShadowMaxAllocationRequests),
        .directionalLodBias = directionalLodBias,
    };
}

constexpr CLodVirtualShadowResolutionConfig CLodVirtualShadowBuildResolutionConfig(uint32_t virtualResolution)
{
    return CLodVirtualShadowBuildResolutionConfig(
        virtualResolution,
        CLodVirtualShadowDefaultPhysicalPageCount,
        CLodVirtualShadowDefaultDirectionalLodBias,
        CLodVirtualShadowDefaultBackingResolution);
}

constexpr CLodVirtualShadowResolutionConfig CLodVirtualShadowBuildResolutionConfig()
{
    return CLodVirtualShadowBuildResolutionConfig(
        CLodVirtualShadowFixedVirtualResolution,
        CLodVirtualShadowDefaultPhysicalPageCount,
        CLodVirtualShadowDefaultDirectionalLodBias,
        CLodVirtualShadowDefaultBackingResolution);
}

inline uint32_t CLodVirtualShadowGetConfiguredMaxBackingResolution()
{
    auto& settingsManager = SettingsManager::GetInstance();
    return CLodVirtualShadowSanitizeBackingResolution(
        settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName)());
}

inline uint32_t CLodVirtualShadowGetConfiguredMaxPhysicalPageCapacity()
{
    auto& settingsManager = SettingsManager::GetInstance();
    const uint32_t configuredBackingResolution = CLodVirtualShadowSanitizeBackingResolution(
        settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName)());
    const uint32_t configuredMaxPhysicalPages =
        settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName)();
    return CLodVirtualShadowSanitizePhysicalPageCount(
        configuredMaxPhysicalPages,
        CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(configuredBackingResolution));
}

inline uint32_t CLodVirtualShadowGetConfiguredComputeClusterCapacity(uint32_t maxVisibleClusters)
{
    return (std::max)(1u, maxVisibleClusters);
}

inline float CLodVirtualShadowAutomaticDirectionalLodBiasFromBudget(uint32_t maxPhysicalPages, float autoLodBiasScale)
{
    const uint32_t sanitizedMaxPhysicalPages = CLodVirtualShadowSanitizePhysicalPageCount(maxPhysicalPages);
    if (sanitizedMaxPhysicalPages == 0u) {
        return 0.0f;
    }

    const float pressureRatio =
        static_cast<float>(CLodVirtualShadowDefaultPhysicalPageCount) /
        static_cast<float>(sanitizedMaxPhysicalPages);
    const float positiveScale = (std::max)(autoLodBiasScale, 0.0f);
    return pressureRatio > 1.0f ? (std::log2(pressureRatio) * positiveScale) : 0.0f;
}

inline CLodVirtualShadowResolutionConfig CLodVirtualShadowBuildRuntimeResolutionConfig()
{
    auto& settingsManager = SettingsManager::GetInstance();
    const uint32_t configuredBackingResolution =
        CLodVirtualShadowSanitizeBackingResolution(
            settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName)());
    const uint32_t configuredMaxPhysicalPages =
        settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName)();
    const float manualDirectionalLodBias =
        settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowLodBiasSettingName)();
    const bool autoDirectionalLodBias =
        settingsManager.getSettingGetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName)();
    const float autoDirectionalLodBiasScale =
        settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName)();

    float effectiveDirectionalLodBias = manualDirectionalLodBias;
    if (autoDirectionalLodBias) {
        effectiveDirectionalLodBias +=
            CLodVirtualShadowAutomaticDirectionalLodBiasFromBudget(configuredMaxPhysicalPages, autoDirectionalLodBiasScale);
    }

    return CLodVirtualShadowBuildResolutionConfig(
        CLodVirtualShadowFixedVirtualResolution,
        configuredMaxPhysicalPages,
        effectiveDirectionalLodBias,
        configuredBackingResolution);
}

static_assert(
    (CLodVirtualShadowDefaultVirtualResolution % CLodVirtualShadowPhysicalPageSize) == 0u,
    "CLod virtual shadow resolution must be divisible by the page size");

static_assert(
    (CLodVirtualShadowMaxVirtualResolution % CLodVirtualShadowPhysicalPageSize) == 0u,
    "CLod max virtual shadow resolution must be divisible by the page size");

static_assert(
    CLodVirtualShadowDefaultClipmapCount <= CLodVirtualShadowMaxSupportedClipmapCount,
    "Default VSM clipmap count must not exceed the supported maximum");

static_assert(
    CLodVirtualShadowMaxPhysicalPageCount <= CLodVirtualShadowPhysicalPageIndexMask,
    "VSM physical page count must fit in the page table physical page index bits");

struct CLodVirtualShadowClipmapInfo
{
    float worldOriginX = 0.0f;
    float worldOriginY = 0.0f;
    float worldOriginZ = 0.0f;
    float texelWorldSize = 0.0f;
    uint32_t pageOffsetX = 0u;
    uint32_t pageOffsetY = 0u;
    uint32_t pageTableLayer = 0u;
    uint32_t shadowCameraBufferIndex = 0xFFFFFFFFu;
    uint32_t clipLevel = 0u;
    uint32_t flags = 0u;
    int32_t clearOffsetX = 0;
    int32_t clearOffsetY = 0;
    float directionalLodBias = CLodVirtualShadowDefaultDirectionalLodBias;
    uint32_t virtualResolution = CLodVirtualShadowDefaultVirtualResolution;
    uint32_t pageTableResolution = CLodVirtualShadowDefaultPageTableResolution;
    uint32_t physicalAtlasPagesWide = CLodVirtualShadowDefaultPhysicalAtlasPagesWide;
    uint32_t physicalAtlasPagesHigh = CLodVirtualShadowDefaultPhysicalAtlasPagesHigh;
};

static_assert(sizeof(CLodVirtualShadowClipmapInfo) == 68u, "CLodVirtualShadowClipmapInfo size must match HLSL");

struct CLodVirtualShadowMainCameraInfo
{
    DirectX::XMFLOAT4 positionWorldSpace{};
    DirectX::XMMATRIX viewInverse{};
    DirectX::XMMATRIX projectionInverse{};
};

static_assert(sizeof(CLodVirtualShadowMainCameraInfo) == 144u, "CLodVirtualShadowMainCameraInfo size must match HLSL");

struct CLodVirtualShadowCompactShadowCameraInfo
{
    DirectX::XMMATRIX view{};
    DirectX::XMMATRIX projection{};
    DirectX::XMMATRIX viewProjection{};
    uint32_t isOrtho = 0u;
    uint32_t pad[3] = {};
};

static_assert(sizeof(CLodVirtualShadowCompactShadowCameraInfo) == 208u, "CLodVirtualShadowCompactShadowCameraInfo size must match HLSL");

struct CLodVirtualShadowMarkClipmapData
{
    float texelWorldSize = 0.0f;
    uint32_t pageOffsetX = 0u;
    uint32_t pageOffsetY = 0u;
    uint32_t pageTableLayer = 0u;
    uint32_t flags = 0u;
    uint32_t virtualResolution = CLodVirtualShadowDefaultVirtualResolution;
    uint32_t pageTableResolution = CLodVirtualShadowDefaultPageTableResolution;
    uint32_t physicalAtlasPagesWide = CLodVirtualShadowDefaultPhysicalAtlasPagesWide;
    uint32_t physicalAtlasPagesHigh = CLodVirtualShadowDefaultPhysicalAtlasPagesHigh;
    float directionalLodBias = CLodVirtualShadowDefaultDirectionalLodBias;
    uint32_t pad0[2] = {};
    DirectX::XMFLOAT4 directionalPageViewRow{};
    DirectX::XMMATRIX shadowViewProjection{};
};

static_assert(sizeof(CLodVirtualShadowMarkClipmapData) == 128u, "CLodVirtualShadowMarkClipmapData size must match HLSL");

struct CLodVirtualShadowMarkTileWorkItem
{
    uint32_t tileCoordX = 0u;
    uint32_t tileCoordY = 0u;
    uint32_t minDepthBits = 0x7F7FFFFFu;
    uint32_t maxDepthBits = 0u;
};

static_assert(sizeof(CLodVirtualShadowMarkTileWorkItem) == 16u, "CLodVirtualShadowMarkTileWorkItem size must match HLSL");

struct CLodVirtualShadowBlockMeta
{
    uint32_t packedVirtualBlockOrigin = 0u;
    uint32_t packedWrappedBlockOrigin = 0u;
    uint32_t activePageMask = 0u;
    uint32_t packedActiveRectAndFlags = 0u;
    uint32_t packedPhysicalPageIndices[CLodVirtualShadowBlockPackedPhysicalPageIndexCount] = {};
};

static_assert(sizeof(CLodVirtualShadowBlockMeta) == 48u, "CLodVirtualShadowBlockMeta size must match HLSL");

struct CLodVirtualShadowPageAllocationRequest
{
    uint32_t virtualAddress = 0u;
    uint32_t clipmapIndex = 0u;
    uint32_t priority = 0u;
    uint32_t flags = 0u;
};

static_assert(sizeof(CLodVirtualShadowPageAllocationRequest) == 16u, "CLodVirtualShadowPageAllocationRequest size must match HLSL");

struct CLodVirtualShadowInvalidationInput
{
    uint32_t perMeshInstanceBufferIndex = 0u;
    uint32_t flags = 0u;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
};

static_assert(sizeof(CLodVirtualShadowInvalidationInput) == 16u, "CLodVirtualShadowInvalidationInput size must match HLSL");

struct CLodVirtualShadowPredictiveInvalidationCandidate
{
    DirectX::XMFLOAT4 worldCenterAndRadius{};
    uint32_t shadowViewId = 0xFFFFFFFFu;
    uint32_t sourceGroupGlobalIndex = 0xFFFFFFFFu;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
};

static_assert(sizeof(CLodVirtualShadowPredictiveInvalidationCandidate) == 32u, "CLodVirtualShadowPredictiveInvalidationCandidate size must match HLSL");

struct CLodVirtualShadowPredictedRawPage
{
    uint32_t virtualAddress = 0u;
    uint32_t clipmapIndex = 0u;
    uint32_t sourceGroupGlobalIndex = 0xFFFFFFFFu;
    uint32_t pad0 = 0u;
};

static_assert(sizeof(CLodVirtualShadowPredictedRawPage) == 16u, "CLodVirtualShadowPredictedRawPage size must match HLSL");

struct CLodVirtualShadowPredictedPage
{
    uint32_t virtualAddress = 0u;
    uint32_t clipmapIndex = 0u;
    uint32_t sourceGroupGlobalIndex = 0xFFFFFFFFu;
    uint32_t pad0 = 0u;
};

static_assert(sizeof(CLodVirtualShadowPredictedPage) == 16u, "CLodVirtualShadowPredictedPage size must match HLSL");

struct CLodVirtualShadowPhysicalPageMeta
{
    uint32_t ownerVirtualAddress = 0u;
    uint32_t lastTouchedFrame = 0u;
    uint32_t flags = 0u;
    uint32_t ownerClipmapIndex = 0u;
};

static_assert(sizeof(CLodVirtualShadowPhysicalPageMeta) == 16u, "CLodVirtualShadowPhysicalPageMeta size must match HLSL");

struct CLodVirtualShadowPageListHeader
{
    uint32_t freePageCount = 0u;
    uint32_t reusablePageCount = 0u;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
};

static_assert(sizeof(CLodVirtualShadowPageListHeader) == 16u, "CLodVirtualShadowPageListHeader size must match HLSL");

struct CLodVirtualShadowRuntimeState
{
    uint32_t clipmapCount = 0u;
    uint32_t supportedClipmapCount = 0u;
    uint32_t virtualResolution = 0u;
    uint32_t pageTableResolution = 0u;
    uint32_t physicalAtlasPagesWide = 0u;
    uint32_t physicalAtlasPagesHigh = 0u;
    uint32_t maxPhysicalPages = 0u;
    uint32_t maxAllocationRequests = 0u;
    float directionalLodBias = CLodVirtualShadowDefaultDirectionalLodBias;
};

static_assert(sizeof(CLodVirtualShadowRuntimeState) == 36u, "CLodVirtualShadowRuntimeState size must match expected layout");

struct CLodVirtualShadowStats
{
    uint32_t activeClipmapCount = 0u;
    uint32_t validClipmapCount = 0u;
    uint32_t allocationRequestCount = 0u;
    uint32_t allocationDispatchGroupCount = 0u;
    uint32_t freePhysicalPageCount = 0u;
    uint32_t reusablePhysicalPageCount = 0u;
    uint32_t setupResetApplied = 0u;
    uint32_t markRequestOverflowCount = 0u;
    uint32_t setupResetForced = 0u;
    uint32_t setupResetNoPreviousState = 0u;
    uint32_t setupResetStructureMismatch = 0u;
    uint32_t setupResetLightDirectionChanged = 0u;
    float currentAllocationPercentage = 0.0f;
    float targetPressureLodBias = 0.0f;
    float smoothedPressureLodBias = 0.0f;
    uint32_t framesSinceOverBudget = 0u;
    uint32_t setupWrappedClearedPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t setupStaleDirtyClearedPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t markResidentCleanHits[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t markResidentDirtyHits[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t preAllocateNonZeroPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t preAllocateDirtyPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t selectedPixels[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t projectionRejectedPixels[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t requestedPages[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t nonZeroPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t allocatedPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t dirtyPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t clearedUnwrittenDirtyPages[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t visitedPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t visitedDirtyPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t predictiveInvalidatedPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t invalidatedCurrentBoundsPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t invalidatedPreviousBoundsPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
    uint32_t invalidatedSkinnedPageTableEntries[CLodVirtualShadowMaxSupportedClipmapCount] = {};
};

static_assert(
    sizeof(CLodVirtualShadowStats) == (16u * sizeof(uint32_t)) + (19u * CLodVirtualShadowMaxSupportedClipmapCount * sizeof(uint32_t)),
    "CLodVirtualShadowStats size must match HLSL");


inline constexpr uint32_t CLodReyesMaxSplitPassCount = 4u;
inline constexpr uint32_t CLodReyesMaxVisibilityMicroTrianglesPerPatch = 128u;
inline constexpr uint32_t CLodReyesRasterBatchMicroTriangleCount = 16u;
inline constexpr uint32_t CLodReyesHardwareRasterPackedEntryCount = 5u;
inline constexpr uint32_t CLodReyesHardwareRasterMaxPackedMicroTriangles =
    CLodReyesHardwareRasterPackedEntryCount * CLodReyesRasterBatchMicroTriangleCount;
inline constexpr uint32_t CLodReyesMaxSourceTrianglesPerVisibleCluster = 128u;
inline constexpr uint32_t CLodReyesMaxRasterWorkItemsPerPatch =
    (CLodReyesMaxVisibilityMicroTrianglesPerPatch + CLodReyesRasterBatchMicroTriangleCount - 1u) / CLodReyesRasterBatchMicroTriangleCount;
static_assert(CLodReyesHardwareRasterMaxPackedMicroTriangles * 3u <= 256u, "Reyes packed hardware raster experiment exceeds mesh-shader vertex output budget");
inline constexpr float CLodReyesShadowFineTargetTexelsPerMicroTriangle = 1.0f;
inline constexpr float CLodReyesShadowCoarseTargetPageFraction = 0.1f;
inline constexpr float CLodReyesShadowCoarseTargetTexelsPerTriangle =
    CLodReyesShadowCoarseTargetPageFraction * static_cast<float>(CLodVirtualShadowPhysicalPageSize);

constexpr uint32_t CLodReyesPatchVisibilityIndexBase(uint32_t maxVisibleClusters)
{
    return maxVisibleClusters;
}

enum class CLodReyesRouteKind : uint32_t
{
    Visibility = 0u,
    FineMicropolyVSM = 1u,
    CoarseHardwareVSM = 2u,
};

inline constexpr uint32_t CLodReyesFlagSkinned = 1u << 0;
inline constexpr uint32_t CLodReyesFlagDisplacementEnabled = 1u << 1;
inline constexpr uint32_t CLodReyesFlagCoarseDirtyOnlyLeaf = 1u << 2;
inline constexpr uint32_t CLodReyesFlagRouteShift = 8u;
inline constexpr uint32_t CLodReyesFlagRouteMask = 0x3u << CLodReyesFlagRouteShift;

constexpr uint32_t CLodReyesEncodeFlags(bool skinned, bool displacementEnabled, CLodReyesRouteKind routeKind)
{
    return
        (skinned ? CLodReyesFlagSkinned : 0u) |
        (displacementEnabled ? CLodReyesFlagDisplacementEnabled : 0u) |
        ((static_cast<uint32_t>(routeKind) << CLodReyesFlagRouteShift) & CLodReyesFlagRouteMask);
}

constexpr CLodReyesRouteKind CLodReyesDecodeRouteKind(uint32_t flags)
{
    return static_cast<CLodReyesRouteKind>((flags & CLodReyesFlagRouteMask) >> CLodReyesFlagRouteShift);
}

struct CLodReyesFullClusterOutput
{
    uint32_t visibleClusterIndex = 0u;
    uint32_t instanceID = 0u;
    uint32_t materialIndex = 0u;
    uint32_t flags = 0u;
};

static_assert(sizeof(CLodReyesFullClusterOutput) == 16u, "CLodReyesFullClusterOutput size must remain compact");

struct CLodReyesOwnedClusterEntry
{
    uint32_t visibleClusterIndex = 0u;
    uint32_t instanceID = 0u;
    uint32_t materialIndex = 0u;
    uint32_t flags = 0u;
};

static_assert(sizeof(CLodReyesOwnedClusterEntry) == 16u, "CLodReyesOwnedClusterEntry size must remain compact");

struct CLodReyesSplitQueueEntry
{
    struct DomainVertexUV
    {
        float u = 0.0f;
        float v = 0.0f;
    };

    uint32_t visibleClusterIndex = 0u;
    uint32_t instanceID = 0u;
    uint32_t localMeshletIndex = 0u;
    uint32_t materialIndex = 0u;
    uint32_t viewID = 0u;
    uint32_t splitLevel = 0u;
    uint32_t quantizedTessFactor = 0u;
    uint32_t flags = 0u;
    uint32_t sourcePrimitiveAndSplitConfig = 0u;
    DomainVertexUV domainVertex0UV;
    DomainVertexUV domainVertex1UV;
    DomainVertexUV domainVertex2UV;
};

static_assert(sizeof(CLodReyesSplitQueueEntry::DomainVertexUV) == 8u, "CLodReyesSplitQueueEntry::DomainVertexUV must match HLSL float2 layout");
static_assert(sizeof(CLodReyesSplitQueueEntry) == 60u, "CLodReyesSplitQueueEntry size must remain compact");

struct CLodReyesDiceQueueEntry
{
    uint32_t visibleClusterIndex = 0u;
    uint32_t instanceID = 0u;
    uint32_t localMeshletIndex = 0u;
    uint32_t materialIndex = 0u;
    uint32_t viewID = 0u;
    uint32_t splitLevel = 0u;
    uint32_t quantizedTessFactor = 0u;
    uint32_t flags = 0u;
    uint32_t sourcePrimitiveAndSplitConfig = 0u;
    CLodReyesSplitQueueEntry::DomainVertexUV domainVertex0UV;
    CLodReyesSplitQueueEntry::DomainVertexUV domainVertex1UV;
    CLodReyesSplitQueueEntry::DomainVertexUV domainVertex2UV;
    uint32_t tessTableConfigIndex = 0u;
    uint32_t reserved = 0u;
};

static_assert(sizeof(CLodReyesDiceQueueEntry) == 68u, "CLodReyesDiceQueueEntry size must remain compact");

struct CLodReyesTessTableConfigEntry
{
    uint32_t firstTriangle = 0u;
    uint32_t firstVertex = 0u;
    uint32_t numTriangles = 0u;
    uint32_t numVertices = 0u;
};

static_assert(sizeof(CLodReyesTessTableConfigEntry) == 16u, "CLodReyesTessTableConfigEntry size must match HLSL");

struct CLodReyesRasterWorkEntry
{
    uint32_t diceQueueIndex = 0u;
    uint32_t microTriangleOffset = 0u;
    uint32_t microTriangleCount = 0u;
    uint32_t rasterBucketIndex = 0u;
};

static_assert(sizeof(CLodReyesRasterWorkEntry) == 16u, "CLodReyesRasterWorkEntry size must match HLSL");

struct CLodReyesPackedRasterWorkGroupEntry
{
    uint32_t firstCompactedRasterWorkIndex = 0u;
    uint32_t rasterWorkEntryCount = 0u;
    uint32_t requestedMicroTriangleCount = 0u;
    uint32_t reserved = 0u;
};

static_assert(sizeof(CLodReyesPackedRasterWorkGroupEntry) == 16u, "CLodReyesPackedRasterWorkGroupEntry size must match HLSL");

inline constexpr uint64_t CLodReyesSplitQueueBufferTestCapBytes = 256ull * 1024ull * 1024ull;
inline constexpr uint32_t CLodReyesMaxSplitQueueEntriesForTesting =
    static_cast<uint32_t>(CLodReyesSplitQueueBufferTestCapBytes / sizeof(CLodReyesSplitQueueEntry));

constexpr uint32_t CLodReyesSplitQueueCapacity(uint32_t maxVisibleClusters)
{
    const uint64_t requestedCapacity =
        static_cast<uint64_t>(maxVisibleClusters) * static_cast<uint64_t>(CLodReyesMaxSourceTrianglesPerVisibleCluster);
    return requestedCapacity > static_cast<uint64_t>(CLodReyesMaxSplitQueueEntriesForTesting)
        ? CLodReyesMaxSplitQueueEntriesForTesting
        : static_cast<uint32_t>(requestedCapacity);
}

inline constexpr uint64_t CLodReyesDiceQueueBufferTestCapBytes = 256ull * 1024ull * 1024ull;
inline constexpr uint32_t CLodReyesMaxDiceQueueEntriesForTesting =
    static_cast<uint32_t>(CLodReyesDiceQueueBufferTestCapBytes / sizeof(CLodReyesDiceQueueEntry));

constexpr uint32_t CLodReyesDiceQueueCapacity(uint32_t maxVisibleClusters)
{
    const uint64_t requestedCapacity =
        static_cast<uint64_t>(maxVisibleClusters) * static_cast<uint64_t>(CLodReyesMaxSourceTrianglesPerVisibleCluster);
    return requestedCapacity > static_cast<uint64_t>(CLodReyesMaxDiceQueueEntriesForTesting)
        ? CLodReyesMaxDiceQueueEntriesForTesting
        : static_cast<uint32_t>(requestedCapacity);
}

inline constexpr uint64_t CLodReyesRasterWorkBufferTestCapBytes = 1024ull * 1024ull * 512ull; // 512 MB for testing
inline constexpr uint32_t CLodReyesMaxRasterWorkEntriesForTesting =
    static_cast<uint32_t>(CLodReyesRasterWorkBufferTestCapBytes / sizeof(CLodReyesRasterWorkEntry));

constexpr uint32_t CLodReyesRasterWorkCapacity(uint32_t maxVisibleClusters)
{
    const uint64_t requestedCapacity =
        static_cast<uint64_t>(maxVisibleClusters) * static_cast<uint64_t>(CLodReyesMaxRasterWorkItemsPerPatch);
    return requestedCapacity > static_cast<uint64_t>(CLodReyesMaxRasterWorkEntriesForTesting)
        ? CLodReyesMaxRasterWorkEntriesForTesting
        : static_cast<uint32_t>(requestedCapacity);
}

struct CLodReyesDispatchIndirectCommand
{
    uint32_t dispatchX = 0u;
    uint32_t dispatchY = 1u;
    uint32_t dispatchZ = 1u;
};

static_assert(sizeof(CLodReyesDispatchIndirectCommand) == 12u, "CLodReyesDispatchIndirectCommand size must match HLSL");

struct CLodReyesTelemetry
{
    uint32_t visibleClusterInputCount = 0u;
    uint32_t fullClusterOutputCount = 0u;
    uint32_t ownedClusterOutputCount = 0u;
    uint32_t immediateDiceQueueEntryCount = 0u;
    uint32_t finalDiceQueueEntryCount = 0u;
    uint32_t phaseIndex = 0u;
    uint32_t deepestSplitLevelReached = 0u;
    uint32_t configuredMaxSplitPassCount = 0u;
    uint32_t patchRasterizedPatchCount = 0u;
    uint32_t dicedPatchCount = 0u;
    uint32_t dicedTriangleEstimateCount = 0u;
    uint32_t dicedVertexEstimateCount = 0u;
    uint32_t patchRasterizedMicroTriangleCount = 0u;
    uint32_t rasterWorkEntryCount = 0u;
    uint32_t hardwareRasterMeshGroupCount = 0u;
    uint32_t hardwareRasterMicroTriangleCount = 0u;
    uint32_t hardwareRasterRequestedMicroTriangleCount = 0u;
    uint32_t hardwareRasterPackedWorkEntryCount = 0u;
    uint32_t splitInputCounts[CLodReyesMaxSplitPassCount] = {};
    uint32_t splitChildOutputCounts[CLodReyesMaxSplitPassCount] = {};
    uint32_t splitDiceOutputCounts[CLodReyesMaxSplitPassCount] = {};
    uint32_t splitQueueOverflowCounts[CLodReyesMaxSplitPassCount] = {};
    uint32_t diceQueueOverflowCounts[CLodReyesMaxSplitPassCount] = {};
    uint32_t invalidSplitPatchDomainCount = 0u;
    uint32_t invalidDicePatchDomainCount = 0u;
    uint32_t splitCollapseFallbackDiceCount = 0u;
    uint32_t rasterWorkOverflowPatchCount = 0u;
    uint32_t rasterWorkOverflowBatchCount = 0u;
    uint32_t canonicalFactorTieCount = 0u;
    uint32_t flippedTessTableConfigCount = 0u;
    uint32_t splitConfigTieCount = 0u;
    uint32_t splitFrustumCullCount = 0u;
    uint32_t splitShadowDirtyCullCount = 0u;
    uint32_t splitChildCullCount = 0u;
    uint32_t splitCoarseOnlyDirtyEligibleCount = 0u;
    uint32_t splitCoarseOnlyDirtyRejectedCount = 0u;
    uint32_t splitCoarseOnlyDirtyLeafOutputCount = 0u;
    uint32_t splitConfigSelectionCounts[4] = {};
    uint32_t canonicalRotationCounts[3] = {};
    uint32_t siblingSharedEdgeCheckCount = 0u;
    uint32_t siblingSharedEdgeMismatchCount = 0u;
    uint32_t rasterClipCullCount = 0u;
    uint32_t rasterPreAreaCullCount = 0u;
    uint32_t rasterWindingSwapCount = 0u;
    uint32_t rasterPostSwapNonNegativeAreaCount = 0u;
    uint32_t rasterEmptyBoundsCullCount = 0u;
    uint32_t rasterZeroMicroTriangleCount = 0u;
    uint32_t rasterMicroTriangleOverflowCount = 0u;
    uint32_t rasterNearPlaneClippedQuadCount = 0u;
    uint32_t rasterTinyTriangleFallbackCount = 0u;
    uint32_t splitOcclusionTestCount = 0u;
    uint32_t splitOcclusionDeferCount = 0u;
    uint32_t splitOcclusionDropCount = 0u;
    uint32_t diceOcclusionTestCount = 0u;
    uint32_t diceOcclusionDeferCount = 0u;
    uint32_t diceOcclusionDropCount = 0u;
    uint32_t replaySplitQueueOverflowCount = 0u;
    uint32_t replayDiceQueueOverflowCount = 0u;
    uint32_t replaySplitMergeCount = 0u;
    uint32_t replayDiceMergeCount = 0u;
};

inline constexpr uint32_t CLodReplayBufferSizeBytes = 200u * 1024u * 1024u; // 200 MB physical, GPU uses first 100 MB
inline constexpr uint32_t CLodReplayNodeRegionSizeBytes = 50u * 1024u * 1024u;    // must match HLSL CLOD_REPLAY_NODE_REGION_SIZE_BYTES
inline constexpr uint32_t CLodReplayMeshletRegionOffset = CLodReplayNodeRegionSizeBytes;
inline constexpr uint32_t CLodNodeReplayStrideBytes = 12u;   // sizeof(TraverseNodeRecord): 3 uints
inline constexpr uint32_t CLodMeshletReplayStrideBytes = 24u; // sizeof(MeshletBucketRecord): 6 uints
inline constexpr uint32_t CLodVoxelRasterThreadsPerGroup = 64u;
inline constexpr uint32_t CLodReplayBufferNumUints = CLodReplayBufferSizeBytes / sizeof(uint32_t);
inline constexpr uint32_t CLodMaxViewDepthIndices = 512u;
inline constexpr uint32_t CLodStreamingInitialGroupCapacity = 1024u;
inline constexpr uint32_t CLodStreamingRequestCapacity = (1u << 16);
inline constexpr uint32_t CLodUsedGroupsCapacity = (1u << 17); // 128K entries for GPU used-groups append buffer

constexpr uint32_t CLodBitsetWordCount(uint32_t bitCount)
{
    return (bitCount + 31u) / 32u;
}

inline uint32_t CLodRoundUpCapacity(uint32_t required)
{
    uint32_t capacity = CLodStreamingInitialGroupCapacity;
    while (capacity < required) {
        capacity *= 2u;
    }
    return capacity;
}

inline uint32_t CLodVoxelRasterWorkCapacity(uint32_t maxVisibleClusters)
{
    return std::max(1u, maxVisibleClusters /** 16u*/);
}

inline std::shared_ptr<Buffer> CreateAliasedUnmaterializedStructuredBuffer(
    uint32_t numElements,
    uint32_t elementSize,
    bool unorderedAccess = true,
    bool unorderedAccessCounter = false,
    bool createNonShaderVisibleUAV = false,
    bool allowAlias = true)
{
    auto buffer = Buffer::CreateUnmaterializedStructuredBuffer(
        numElements,
        elementSize,
        unorderedAccess,
        unorderedAccessCounter,
        createNonShaderVisibleUAV,
        rhi::HeapType::DeviceLocal);
    // Read-only structured buffers in the CLod pipeline are typically populated via
    // BUFFER_UPLOAD before their first graph pass reads them. The aliasing planner
    // cannot synthesize an initialization write for that pattern, so disallow aliasing
    // unless the buffer participates in unordered-access writes.
    buffer->SetAllowAlias(allowAlias && unorderedAccess);
    return buffer;
}

inline std::shared_ptr<Buffer> CreateAliasedUnmaterializedRawBuffer(
    uint64_t bufferSizeBytes,
    bool unorderedAccess = true,
    bool createNonShaderVisibleUAV = false,
    bool allowAlias = true)
{
    if (bufferSizeBytes == 0 || (bufferSizeBytes % 4u) != 0u) {
        throw std::runtime_error("Raw buffer requires a non-zero byte size that is divisible by 4");
    }

    auto buffer = Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, bufferSizeBytes, unorderedAccess);

    BufferBase::DescriptorRequirements requirements{};
    requirements.createSRV = true;
    requirements.createUAV = unorderedAccess;
    requirements.createNonShaderVisibleUAV = unorderedAccess && createNonShaderVisibleUAV;
    requirements.uavCounterOffset = 0;
    requirements.srvDesc = rhi::SrvDesc{
        .dimension = rhi::SrvDim::Buffer,
        .formatOverride = rhi::Format::R32_Typeless,
        .buffer = {
            .kind = rhi::BufferViewKind::Raw,
            .firstElement = 0,
            .numElements = static_cast<uint32_t>(bufferSizeBytes / 4u),
            .structureByteStride = 0,
        },
    };
    requirements.uavDesc = rhi::UavDesc{
        .dimension = rhi::UavDim::Buffer,
        .formatOverride = rhi::Format::R32_Typeless,
        .buffer = {
            .kind = rhi::BufferViewKind::Raw,
            .firstElement = 0,
            .numElements = static_cast<uint32_t>(bufferSizeBytes / 4u),
            .structureByteStride = 0,
            .counterOffsetInBytes = 0,
        },
    };

    buffer->SetDescriptorRequirements(requirements);
    buffer->SetAllowAlias(allowAlias);
    return buffer;
}

// Returns the number of pages needed for a group.
// Uses the physical page count set during the build pipeline.
inline uint32_t CLodEstimatePagesNeeded(
    const ClusterLODRuntimeSummary::GroupChunkHint& hint,
    uint32_t /*vertexByteSize*/,
    uint64_t /*pageSize*/)
{
    return hint.pageCount;
}
