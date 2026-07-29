#pragma once

#include <cstdint>

enum class CLodExtensionType {
    VisiblityBuffer,
    AlphaBlend,
    Shadow,
};

struct CLodExtensionVisibilityBufferTag {};
struct CLodExtensionAlphaBlendTag {};
struct CLodExtensionShadowTag {};

struct CLodExtensionTypeTag {
};

struct CLodVisibleClusterCapacity {
    uint32_t maxVisibleClusters = 0u;
};

struct VisibleClustersBufferTag {};
struct VisibleClusterTransformIndicesBufferTag {};
struct VisibleClustersCounterTag {};
struct CLodWorkGraphTelemetryBufferTag {};
struct CLodOcclusionReplayStateBufferTag {};
struct CLodReyesDiceQueueTag {};
struct CLodReyesTessTableConfigsTag {};
struct CLodReyesTessTableVerticesTag {};
struct CLodReyesTessTableTrianglesTag {};
struct CLodReyesFullClustersCounterTag {};
struct CLodReyesSplitQueueCounterATag {};
struct CLodReyesSplitQueueCounterBTag {};
struct CLodReyesSplitQueueOverflowATag {};
struct CLodReyesSplitQueueOverflowBTag {};
struct CLodReyesDiceQueueCounterTag {};
struct CLodReyesDiceQueueOverflowTag {};
struct CLodReyesTelemetryBufferPhase1Tag {};
struct CLodReyesTelemetryBufferPhase2Tag {};
struct CLodDeepVisibilityCounterTag {};
struct CLodDeepVisibilityOverflowCounterTag {};
struct CLodDeepVisibilityStatsTag {};
struct CLodVirtualShadowPageTableTag {};
struct CLodVirtualShadowPhysicalPagesTag {};
struct CLodVirtualShadowPageMetadataTag {};
struct CLodVirtualShadowInvalidationInputsTag {};
struct CLodVirtualShadowInvalidationCountTag {};
struct CLodVirtualShadowAllocationRequestsTag {};
struct CLodVirtualShadowAllocationCountTag {};
struct CLodVirtualShadowFreePhysicalPagesTag {};
struct CLodVirtualShadowReusablePhysicalPagesTag {};
struct CLodVirtualShadowPageListHeaderTag {};
struct CLodVirtualShadowDirtyPageFlagsTag {};
struct CLodVirtualShadowDirtyHierarchyTag {};
struct CLodVirtualShadowNonRasterableHierarchyTag {};
struct CLodVirtualShadowClipmapInfoTag {};
struct CLodVirtualShadowDirectionalPageViewInfoTag {};
struct CLodVirtualShadowRuntimeStateTag {};
struct CLodVirtualShadowStatsTag {};
