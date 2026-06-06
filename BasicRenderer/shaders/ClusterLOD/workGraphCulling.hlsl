// Compile with DXC target: lib_6_8 (Shader Model 6.8)
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/indirectCommands.hlsli"
#include "include/waveIntrinsicsHelpers.hlsli"
#include "include/occlusionCulling.hlsli"
#include "include/materialFlags.hlsli"
#include "PerPassRootConstants/clodWorkGraphRootConstants.h"
#ifdef CLOD_COMPUTE_INCLUDE_ONLY
#include "PerPassRootConstants/clodPureComputeCullingRootConstants.h"
#endif
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/vertex.hlsli"
#include "include/skinningCommon.hlsli"

#ifndef CLOD_WG_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER_ID
#define CLOD_WG_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER_ID "CLod::WorkGraphComputePageJobDescriptors"
#endif

#ifndef CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID
#define CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID "CLod::VoxelRasterQueueDescriptors"
#endif

#ifndef CLOD_HIERARCHY_LEVEL_INFOS_BUFFER_ID
#define CLOD_HIERARCHY_LEVEL_INFOS_BUFFER_ID "Builtin::CLod::LevelInfos"
#endif

#ifndef CLOD_WG_ENABLE_SW_CLASSIFICATION
#define CLOD_WG_ENABLE_SW_CLASSIFICATION 1
#endif

#ifndef CLOD_WG_ENABLE_SW_NODE_OUTPUT
#define CLOD_WG_ENABLE_SW_NODE_OUTPUT CLOD_WG_ENABLE_SW_CLASSIFICATION
#endif

#ifndef CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER
#define CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER 0
#endif

// Set to 1 to enable occlusion culling for VSM / shadow cameras (ortho).
// Defaults to 0 (off): ortho cameras skip occlusion culling entirely.
#ifndef CLOD_VSM_OCCLUSION_CULLING
#define CLOD_VSM_OCCLUSION_CULLING 0
#endif

// Set to 1 to reuse the current primary camera for VSM LOD decisions.
// Defaults to 0 so shadow views use their own camera for both culling and LOD.
#ifndef CLOD_VSM_USE_PRIMARY_CAMERA_FOR_LOD
#define CLOD_VSM_USE_PRIMARY_CAMERA_FOR_LOD 0
#endif

// meshopt_Meshlet layout on GPU
struct Meshlet
{
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
};

struct ClusterLODNodeRange
{
    uint isLeaf; // 0=internal node, 1=voxel group leaf, 2=segment leaf
    uint indexOrOffset; // voxel leaf: group-local section index; segment-leaf: mesh-local segment index
                         // internal: childOffset (relative to lodNodesBase)
    uint countMinusOne; // internal: childCountMinusOne; voxel leaf: refinedGroup+1, or 0 for terminal
    uint ownerGroupId;  // segment-leaf: mesh-local group index (for page resolution + streaming)
};

struct ClusterLODTraversalMetric
{
    float4 cullCenterAndRadius; // xyz center (mesh space), w radius (mesh space)
    float4 lodCenterAndRadius; // xyz center (mesh space), w radius (mesh space)
    float maxQuadricError; // mesh-space conservative error bound for this subtree/leaf
    float pad0[3];
};

struct ClusterLODNode
{
    ClusterLODNodeRange range;
    ClusterLODTraversalMetric metric;
};

static const uint WG_COUNTER_OBJECT_CULL_THREADS = 0;
static const uint WG_COUNTER_OBJECT_CULL_IN_RANGE_THREADS = 1;
static const uint WG_COUNTER_OBJECT_CULL_VISIBLE_THREADS = 2;
static const uint WG_COUNTER_OBJECT_CULL_TRAVERSE_RECORDS = 3;

static const uint WG_COUNTER_TRAVERSE_THREADS = 4;
static const uint WG_COUNTER_TRAVERSE_INTERNAL_NODE_RECORDS = 5;
static const uint WG_COUNTER_TRAVERSE_LEAF_NODE_RECORDS = 6;
static const uint WG_COUNTER_TRAVERSE_CULLED_NODE_RECORDS = 7;
static const uint WG_COUNTER_TRAVERSE_REJECTED_BY_ERROR_RECORDS = 8;
static const uint WG_COUNTER_TRAVERSE_ACTIVE_CHILD_THREADS = 9;
static const uint WG_COUNTER_TRAVERSE_TRAVERSE_RECORDS = 10;

static const uint WG_COUNTER_CLUSTER_CULL_THREADS = 11;
static const uint WG_COUNTER_CLUSTER_CULL_IN_RANGE_THREADS = 12;
static const uint WG_COUNTER_CLUSTER_CULL_WAVES = 13;
static const uint WG_COUNTER_CLUSTER_CULL_ACTIVE_LANES = 14;
static const uint WG_COUNTER_CLUSTER_CULL_SURVIVING_LANES = 15;
static const uint WG_COUNTER_CLUSTER_CULL_ZERO_SURVIVOR_WAVES = 16;
static const uint WG_COUNTER_CLUSTER_CULL_VISIBLE_CLUSTER_WRITES = 17;
static const uint WG_COUNTER_CLUSTER_CULL_BUCKET_RECORDS_DISPATCHED = 100;
static const uint WG_COUNTER_CLUSTER_CULL_DENSE_EXPANSION_BUCKETS = 101;
static const uint WG_COUNTER_CLUSTER_CULL_DENSE_CLUSTERS_DISPATCHED = 102;
static const uint WG_COUNTER_TRAVERSE_VOXEL_LEAF_RECORDS = 103;
static const uint WG_COUNTER_TRAVERSE_VOXEL_REJECTED_BY_ERROR_RECORDS = 104;
static const uint WG_COUNTER_TRAVERSE_VOXEL_DESCRIPTOR_HITS = 105;
static const uint WG_COUNTER_TRAVERSE_VOXEL_DESCRIPTOR_MISSES = 106;
static const uint WG_COUNTER_TRAVERSE_VOXEL_RASTER_WORK_RECORDS = 107;
static const uint WG_COUNTER_TRAVERSE_VOXEL_RASTER_WORK_DROPPED = 108;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB = 122u;

static const uint WG_COUNTER_TRAVERSE_COALESCED_LAUNCHES = 18;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_RECORDS = 19;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_1 = 20;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_2 = 21;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_3 = 22;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_4 = 23;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_5 = 24;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_6 = 25;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_7 = 26;
static const uint WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_8 = 27;

static const uint WG_COUNTER_PHASE1_OCCLUSION_NODE_REPLAY_ENQUEUE_ATTEMPTS = 28;
static const uint WG_COUNTER_PHASE1_OCCLUSION_CLUSTER_REPLAY_ENQUEUE_ATTEMPTS = 29;

static const uint WG_COUNTER_PHASE2_REPLAY_NODE_LAUNCHES = 30;
static const uint WG_COUNTER_PHASE2_REPLAY_NODE_INPUT_RECORDS = 31;
static const uint WG_COUNTER_PHASE2_REPLAY_NODE_RECORDS_EMITTED = 32;

static const uint WG_COUNTER_PHASE2_REPLAY_MESHLET_LAUNCHES = 33;
static const uint WG_COUNTER_PHASE2_REPLAY_MESHLET_INPUT_RECORDS = 34;
static const uint WG_COUNTER_PHASE2_REPLAY_MESHLET_BUCKET_RECORDS_EMITTED = 35;

static const uint WG_COUNTER_PHASE2_REPLAY_TRAVERSE_RECORDS_CONSUMED = 36;
static const uint WG_COUNTER_PHASE2_REPLAY_CLUSTER_BUCKET_RECORDS_CONSUMED = 37;

static const uint WG_COUNTER_SEGMENT_EVALUATE_THREADS = 38;
static const uint WG_COUNTER_SEGMENT_EVALUATE_SEGMENT_RECORDS = 39;
static const uint WG_COUNTER_SEGMENT_EVALUATE_EMIT_BUCKET_THREADS = 40;
static const uint WG_COUNTER_SEGMENT_EVALUATE_REFINED_TRAVERSAL_THREADS = 41;
static const uint WG_COUNTER_SEGMENT_EVALUATE_NON_RESIDENT_REFINED_CHILD_THREADS = 42;
static const uint WG_COUNTER_SEGMENT_EVALUATE_COALESCED_LAUNCHES = 43;
static const uint WG_COUNTER_SEGMENT_EVALUATE_COALESCED_INPUT_RECORDS = 44;

static const uint WG_COUNTER_CLUSTER_CULL_MESHLET_ITERATIONS = 45;
static const uint WG_COUNTER_CLUSTER_CULL_REJECTED_FRUSTUM = 46;
static const uint WG_COUNTER_CLUSTER_CULL_REJECTED_CONDITION2 = 47;
static const uint WG_COUNTER_CLUSTER_CULL_REJECTED_OCCLUSION = 48;
static const uint WG_COUNTER_CLUSTER_CULL_REJECTED_OUT_OF_RANGE = 49;
static const uint WG_COUNTER_CLUSTER_CULL_REJECTED_PAGE_BOUNDS = 50;
static const uint WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES = 51;

static const uint WG_COUNTER_CHILD_PREFILTER_FRUSTUM_CULLED = 52;
static const uint WG_COUNTER_CHILD_PREFILTER_LOD_REJECTED = 53;
static const uint WG_COUNTER_CLUSTER_CULL_SHADOW_CLIPMAP_MISSES = 54;
static const uint WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_REGION_HITS = 55;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_FRUSTUM = 56;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_LEFT = 57;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_RIGHT = 58;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_BOTTOM = 59;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_TOP = 60;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_NEAR = 61;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_FAR = 62;
static const uint WG_COUNTER_OBJECT_CULL_INVALID_BOUNDS = 63;
static const uint WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_QUERIES = 64;
static const uint WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_QUERIES_CLIPPED = 65;
static const uint WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_REGION_COARSE_MIP_CHECKS = 66;

static const uint WG_COUNTER_PAGEJOB_BUILD_CLUSTERS_PROCESSED = 67;
static const uint WG_COUNTER_PAGEJOB_BUILD_PAGES_EMITTED = 68;
static const uint WG_COUNTER_PAGEJOB_BUILD_FALLBACK_TO_HW = 69;
static const uint WG_COUNTER_PAGEJOB_RASTER_TRIANGLES_CLIPPED = 70;
static const uint WG_COUNTER_PAGEJOB_RASTER_PIXELS_WRITTEN = 71;
static const uint WG_COUNTER_PAGEJOB_RASTER_FLAG_WRITES = 72;

static const uint WG_COUNTER_CLASSIFY_CONTRIBUTING = 73;
static const uint WG_COUNTER_CLASSIFY_ROUTED_HW = 74;
static const uint WG_COUNTER_CLASSIFY_ROUTED_SW = 75;
static const uint WG_COUNTER_CLASSIFY_ROUTED_PAGEJOB = 76;
static const uint WG_COUNTER_CLASSIFY_PJ_REJECT_REYES_DISPLACEMENT = 77;
static const uint WG_COUNTER_CLASSIFY_PJ_REJECT_ALPHA_TESTED = 78;
static const uint WG_COUNTER_CLASSIFY_PJ_REJECT_NO_CLIPMAP_INDEX = 79;
static const uint WG_COUNTER_CLASSIFY_PJ_REJECT_BELOW_THRESHOLD = 80;
static const uint WG_COUNTER_CLASSIFY_PJ_REJECT_DISABLED = 81;
static const uint WG_COUNTER_CLASSIFY_PJ_REJECT_ALREADY_SW = 82;
static const uint WG_COUNTER_CLASSIFY_SW_DISABLED = 83;

static const uint WG_COUNTER_PAGEJOB_BUILD_GROUPS_LAUNCHED = 84;
static const uint WG_COUNTER_PAGEJOB_BUILD_NO_CLIPMAP = 85;
static const uint WG_COUNTER_PAGEJOB_BUILD_PAGES_SCANNED = 86;
static const uint WG_COUNTER_PAGEJOB_BUILD_ZERO_DIRTY_PAGES = 87;
static const uint WG_COUNTER_PAGEJOB_RASTER_JOBS_LAUNCHED = 88;
static const uint WG_COUNTER_PAGEJOB_RASTER_TOTAL_TRIS = 89;
static const uint WG_COUNTER_PAGEJOB_RASTER_TRIS_DEPTH_REJECT = 90;
static const uint WG_COUNTER_PAGEJOB_RASTER_TRIS_BACKFACE_CULL = 91;
static const uint WG_COUNTER_PAGEJOB_RASTER_TRIS_AABB_EMPTY = 92;
static const uint WG_COUNTER_PAGEJOB_RASTER_TRIS_RASTERIZED = 93;
static const uint WG_COUNTER_PAGEJOB_RASTER_PIXELS_TESTED = 94;
static const uint WG_COUNTER_PAGEJOB_RASTER_JOBS_WITH_PIXELS = 95;

static const uint WG_COUNTER_PAGEJOB_DBG_PHYS_DESCRIPTOR = 96;
static const uint WG_COUNTER_PAGEJOB_DBG_ATLAS_WIDTH = 97;
static const uint WG_COUNTER_PAGEJOB_DBG_ATLAS_HEIGHT = 98;
static const uint WG_COUNTER_PAGEJOB_DBG_OOB_PIXELS = 99;

static const uint CLOD_STREAM_REQUEST_CAPACITY = (1u << 16);
static const uint CLOD_USED_GROUPS_CAPACITY = (1u << 17);
static const uint CLOD_STREAM_VIEWID_MASK = 0xFFFFu;
static const uint CLOD_STREAM_PRIORITY_SHIFT = 16u;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
static const uint CLOD_VIRTUAL_SHADOW_PREDICTIVE_CANDIDATE_CAPACITY = (1u << 16);
#endif

static const uint CLOD_RECORD_SOURCE_PASS1 = 0;
static const uint CLOD_RECORD_SOURCE_REPLAY = 1;

bool CLodForcedTraversalDepthRootEnabled(CLodMeshMetadata clodMeshMetadata)
{
    const uint forcedDepth = CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT;
    return
        forcedDepth != CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT_DISABLED &&
        forcedDepth < clodMeshMetadata.lodLevelCount;
}

uint CLodResolveTraversalRootNode(CLodMeshMetadata clodMeshMetadata)
{
    if (!CLodForcedTraversalDepthRootEnabled(clodMeshMetadata))
    {
        return clodMeshMetadata.rootNode;
    }

    StructuredBuffer<CLodHierarchyLevelInfo> levelInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(CLOD_HIERARCHY_LEVEL_INFOS_BUFFER_ID)];
    return levelInfos[clodMeshMetadata.lodLevelInfoBase + CLOD_WG_FORCED_TRAVERSAL_DEPTH_ROOT].rootNode;
}

bool CLodWorkGraphTelemetryEnabled()
{
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_TELEMETRY_ENABLED) != 0u;
}

bool CLodWorkGraphOcclusionEnabled()
{
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_OCCLUSION_ENABLED) != 0u;
}

bool CLodWorkGraphFrustumCullingEnabled()
{
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_DISABLE_FRUSTUM_CULLING) == 0u;
}

bool CLodWorkGraphSWRasterEnabled()
{
#if CLOD_WG_ENABLE_SW_CLASSIFICATION
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_SW_RASTER_ENABLED) != 0u;
#else
    return false;
#endif
}

bool CLodWorkGraphIsPhase2()
{
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_PHASE2) != 0u;
}

uint CLodWorkGraphSharedVisibleClusterWriteCapacity(
    uint visibleClusterCapacity,
    uint phase1HWBase,
    uint phase1SWBase)
{
    if (!CLodWorkGraphIsPhase2())
    {
        return visibleClusterCapacity;
    }

    if (phase1HWBase >= visibleClusterCapacity)
    {
        return 0u;
    }

    const uint capacityAfterHW = visibleClusterCapacity - phase1HWBase;
    return (phase1SWBase < capacityAfterHW)
        ? (capacityAfterHW - phase1SWBase)
        : 0u;
}

bool CLodWorkGraphUseComputeSWRaster()
{
#if CLOD_WG_ENABLE_SW_CLASSIFICATION
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_COMPUTE_SW_RASTER) != 0u;
#else
    return false;
#endif
}

bool CLodWorkGraphUseDedicatedComputePageJobBuffer()
{
#if CLOD_WG_ENABLE_SW_CLASSIFICATION && CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER
    StructuredBuffer<uint4> pageJobDescriptorBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(CLOD_WG_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER_ID)];
    const uint2 descriptorPair = pageJobDescriptorBuffer[0].xy;
    // Shadow PageJob/Reyes large-cluster routing consumes the dedicated side-channel
    // buffer after culling regardless of whether visibility SW raster runs in compute
    // or work-graph mode. If descriptors are present, keep emitting into that buffer.
    return
        descriptorPair.x != 0xFFFFFFFFu &&
        descriptorPair.y != 0xFFFFFFFFu;
#else
    return false;
#endif
}

bool CLodWorkGraphShadowDirtyPageCullingEnabled()
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_DISABLE_SHADOW_DIRTY_PAGE_CULLING) == 0u;
#else
    return false;
#endif
}

bool CLodWorkGraphVirtualShadowPredictiveLodInvalidationEnabled()
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    return (CLOD_WG_FLAGS & CLOD_WG_FLAG_VSM_PREDICTIVE_LOD_INVALIDATION) != 0u;
#else
    return false;
#endif
}

float CLodSWRasterDiameterThreshold()
{
#if CLOD_WG_ENABLE_SW_CLASSIFICATION
    return float(CLOD_WG_FLAGS >> CLOD_WG_SW_RASTER_THRESHOLD_SHIFT);
#else
    return 0.0f;
#endif
}

// Page-job VSM flags helpers: decode from CLOD_WG_PAGE_JOB_FLAGS root constant.
bool CLodPageJobEnabled()
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    return (CLOD_WG_PAGE_JOB_FLAGS & CLOD_WG_PAGE_JOB_FLAG_ENABLED) != 0u;
#else
    return false;
#endif
}

bool CLodPageJobForceAll()
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    return (CLOD_WG_PAGE_JOB_FLAGS & CLOD_WG_PAGE_JOB_FLAG_FORCE_ALL) != 0u;
#else
    return false;
#endif
}

float CLodPageJobDiameterThreshold()
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    return float((CLOD_WG_PAGE_JOB_FLAGS & CLOD_WG_PAGE_JOB_DIAMETER_THRESHOLD_MASK) >> CLOD_WG_PAGE_JOB_DIAMETER_THRESHOLD_SHIFT);
#else
    return 0.0f;
#endif
}

float CLodPageJobSparseRatio()
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    return float((CLOD_WG_PAGE_JOB_FLAGS & CLOD_WG_PAGE_JOB_SPARSE_RATIO_MASK) >> CLOD_WG_PAGE_JOB_SPARSE_RATIO_SHIFT) / 255.0f;
#else
    return 0.0f;
#endif
}

uint CLodPageJobMaxPagesPerCluster()
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    uint v = (CLOD_WG_PAGE_JOB_FLAGS & CLOD_WG_PAGE_JOB_MAX_PAGES_MASK) >> CLOD_WG_PAGE_JOB_MAX_PAGES_SHIFT;
    return v == 0u ? PAGEJOB_MAX_TILE_JOBS_PER_CLUSTER : v;
#else
    return PAGEJOB_MAX_TILE_JOBS_PER_CLUSTER;
#endif
}

float CLodProjectedDiameterPixels(float radiusWorld, float projY, float viewportHeightPixels, float viewSpaceZ, float zNear, bool isOrtho)
{
    const float diameterScale = 2.0f * abs(projY) * max(viewportHeightPixels, 1.0f);
    const float projectedDiameter = radiusWorld * diameterScale;

    if (isOrtho) {
        return projectedDiameter;
    }

    return projectedDiameter / max(-viewSpaceZ, zNear);
}

void WGTelemetryAdd(uint counterIndex, uint value);

uint CLodBitMask(uint key)
{
    return 1u << (key & 31u);
}

uint CLodBitWordAddress(uint key)
{
    return (key >> 5u) * 4u;
}

bool CLodReadBit(ByteAddressBuffer bits, uint key)
{
    const uint packed = bits.Load(CLodBitWordAddress(key));
    return (packed & CLodBitMask(key)) != 0u;
}

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
bool CLodVirtualShadowFindClipmapForView(uint viewId, out uint outClipmapIndex, out CLodVirtualShadowClipmapInfo outClipmapInfo)
{
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodClipmapInfo)];

    [unroll]
    for (uint clipmapIndex = 0u; clipmapIndex < kCLodVirtualShadowClipmapCount; ++clipmapIndex)
    {
        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
        if (CLodVirtualShadowClipmapIsValid(clipmapInfo) && clipmapInfo.shadowCameraBufferIndex == viewId)
        {
            outClipmapIndex = clipmapIndex;
            outClipmapInfo = clipmapInfo;
            return true;
        }
    }

    outClipmapIndex = 0u;
    outClipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    return false;
}

uint CLodResolveLodViewId(uint cullViewId)
{
#if !CLOD_VSM_USE_PRIMARY_CAMERA_FOR_LOD
    return cullViewId;
#else
    uint clipmapIndex = 0u;
    CLodVirtualShadowClipmapInfo clipmapInfo;
    if (!CLodVirtualShadowFindClipmapForView(cullViewId, clipmapIndex, clipmapInfo))
    {
        return cullViewId;
    }

    ConstantBuffer<PerFrameBuffer> perFrameBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    return perFrameBuffer.mainCameraIndex;
#endif
}

bool CLodVirtualShadowComputeSphereAabbUvBounds(
    float3 worldCenter,
    float radiusWorld,
    CLodVirtualShadowCompactShadowCameraInfo shadowCamera,
    out float2 uvMin,
    out float2 uvMax,
    out bool queryClipped)
{
    float2 ndcMin = float2(1.0e30f, 1.0e30f);
    float2 ndcMax = float2(-1.0e30f, -1.0e30f);
    bool failOpen = false;

    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        const float3 cornerOffset = float3(
            (cornerIndex & 0x1u) != 0u ? radiusWorld : -radiusWorld,
            (cornerIndex & 0x2u) != 0u ? radiusWorld : -radiusWorld,
            (cornerIndex & 0x4u) != 0u ? radiusWorld : -radiusWorld);
        const float4 clipCorner = mul(float4(worldCenter + cornerOffset, 1.0f), shadowCamera.viewProjection);

        float3 ndcCorner = 0.0f.xxx;
        if (CLodVirtualShadowCompactCameraIsOrtho(shadowCamera))
        {
            ndcCorner = clipCorner.xyz;
        }
        else
        {
            const float safeW = abs(clipCorner.w);
            if (safeW <= 1.0e-6f || clipCorner.w <= 0.0f || clipCorner.z < 0.0f || clipCorner.z > clipCorner.w)
            {
                failOpen = true;
                break;
            }

            ndcCorner = clipCorner.xyz / clipCorner.w;
        }

        ndcMin = min(ndcMin, ndcCorner.xy);
        ndcMax = max(ndcMax, ndcCorner.xy);
    }

    if (failOpen)
    {
        uvMin = 0.0f.xx;
        uvMax = 1.0f.xx;
        queryClipped = true;
        return true;
    }

    if (ndcMax.x < -1.0f || ndcMin.x > 1.0f ||
        ndcMax.y < -1.0f || ndcMin.y > 1.0f)
    {
        uvMin = 0.0f.xx;
        uvMax = 0.0f.xx;
        queryClipped = false;
        return false;
    }

    queryClipped =
        ndcMin.x < -1.0f || ndcMax.x > 1.0f ||
        ndcMin.y < -1.0f || ndcMax.y > 1.0f;

    ndcMin = clamp(ndcMin, -1.0f.xx, 1.0f.xx);
    ndcMax = clamp(ndcMax, -1.0f.xx, 1.0f.xx);

    uvMin = float2(ndcMin.x * 0.5f + 0.5f, 1.0f - (ndcMax.y * 0.5f + 0.5f));
    uvMax = float2(ndcMax.x * 0.5f + 0.5f, 1.0f - (ndcMin.y * 0.5f + 0.5f));
    return true;
}

bool CLodVirtualShadowConservativeAnyHitTexture2DArraySphereQuery(
    Texture2DArray<uint> queryTexture,
    uint arrayLayer,
    uint2 baseResolution,
    in const CLodVirtualShadowCompactShadowCameraInfo camera,
    float3 viewSpaceCenter,
    float scaledBoundingRadius,
    out uint sampledMipLevel,
    out bool queryClipped)
{
    viewSpaceCenter.y = -viewSpaceCenter.y;

    float4 vLBRT;
    if (CLodVirtualShadowCompactCameraIsOrtho(camera))
    {
        viewSpaceCenter.y = -viewSpaceCenter.y;
        vLBRT = sphere_screen_extents_ortho(viewSpaceCenter.xyz, scaledBoundingRadius, camera.projection);
    }
    else
    {
        vLBRT = sphere_screen_extents(viewSpaceCenter.xyz, scaledBoundingRadius, camera.projection);
        vLBRT.x = -vLBRT.x;
        vLBRT.z = -vLBRT.z;
    }

    const float4 vToUV = float4(0.5f, -0.5f, 0.5f, -0.5f);
    const float4 vUV = vLBRT.xwzy * vToUV + 0.5f;
    const float2 uvMin = vUV.xy;
    const float2 uvMax = vUV.zw;

    if (uvMax.x < 0.0f || uvMin.x > 1.0f ||
        uvMax.y < 0.0f || uvMin.y > 1.0f)
    {
        sampledMipLevel = 0u;
        queryClipped = false;
        return false;
    }

    queryClipped = any(uvMin < 0.0f.xx) || any(uvMax > 1.0f.xx);

    const float2 clampedUvMin = saturate(uvMin);
    const float2 clampedUvMax = saturate(uvMax);
    const float2 baseResolutionF = float2(baseResolution);
    const float2 minTexel = clamp(baseResolutionF * clampedUvMin, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float2 maxTexel = clamp(baseResolutionF * clampedUvMax, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float pixelWidth = max(maxTexel.x - minTexel.x, maxTexel.y - minTexel.y);
    const uint sampleWidth = 2u;
    const uint maxMipLevel = firstbithigh(max(baseResolution.x, baseResolution.y));

    sampledMipLevel = min(
        (uint)clamp(ceil(log2(max(pixelWidth, 1.0f))) - log2((float)sampleWidth), 0.0f, (float)maxMipLevel),
        maxMipLevel);

    const int2 quadCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 minCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 maxCornerTexel = int2(maxTexel) >> sampledMipLevel;
    const int2 atMipPixelWidth = maxCornerTexel - minCornerTexel + 1;
    const int2 texelBounds = max(int2(0, 0), (int2(baseResolution) >> sampledMipLevel) - 1);

    [loop]
    for (uint x = 0u; x <= sampleWidth; ++x)
    {
        [loop]
        for (uint y = 0u; y <= sampleWidth; ++y)
        {
            if ((int)x >= atMipPixelWidth.x || (int)y >= atMipPixelWidth.y)
            {
                continue;
            }

            const int2 sampleTexel = clamp(quadCornerTexel + int2(x, y), int2(0, 0), texelBounds);
            if (queryTexture.Load(int4(sampleTexel, arrayLayer, sampledMipLevel)) != 0u)
            {
                return true;
            }
        }
    }

    return false;
}

bool CLodVirtualShadowDirtyHierarchyAnyHit(
    Texture2DArray<uint> queryTexture,
    uint arrayLayer,
    uint2 baseResolution,
    float2 uvMin,
    float2 uvMax,
    out uint sampledMipLevel)
{
    const float2 clampedUvMin = saturate(uvMin);
    const float2 clampedUvMax = saturate(uvMax);
    const float2 baseResolutionF = float2(baseResolution);
    const float2 minTexel = clamp(baseResolutionF * clampedUvMin, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float2 maxTexel = clamp(baseResolutionF * clampedUvMax, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float pixelWidth = max(maxTexel.x - minTexel.x, maxTexel.y - minTexel.y);
    const uint sampleWidth = 2u;
    const uint maxMipLevel = firstbithigh(max(baseResolution.x, baseResolution.y));

    sampledMipLevel = min(
        (uint)clamp(ceil(log2(max(pixelWidth, 1.0f))) - log2((float)sampleWidth), 0.0f, (float)maxMipLevel),
        maxMipLevel);

    const int2 quadCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 minCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 maxCornerTexel = int2(maxTexel) >> sampledMipLevel;
    const int2 atMipPixelWidth = maxCornerTexel - minCornerTexel + 1;
    const int2 texelBounds = max(int2(0, 0), (int2(baseResolution) >> sampledMipLevel) - 1);

    [loop]
    for (uint x = 0u; x <= sampleWidth; ++x)
    {
        [loop]
        for (uint y = 0u; y <= sampleWidth; ++y)
        {
            if ((int)x >= atMipPixelWidth.x || (int)y >= atMipPixelWidth.y)
            {
                continue;
            }

            const int2 sampleTexel = clamp(quadCornerTexel + int2(x, y), int2(0, 0), texelBounds);
            if (queryTexture.Load(int4(sampleTexel, arrayLayer, sampledMipLevel)) != 0u)
            {
                return true;
            }
        }
    }

    return false;
}

// Set to 1 to use AABB-from-sphere projection for the dirty page query,
// 0 to use the original projected bounding sphere footprint directly.
#define CLOD_VSM_USE_AABB_DIRTY_QUERY 1
#ifndef CLOD_VSM_USE_AABB_DIRTY_QUERY
#define CLOD_VSM_USE_AABB_DIRTY_QUERY 0
#endif

bool CLodVirtualShadowBoundsTouchDirtyPages(float3 worldCenter, float radiusWorld, uint viewId)
{
    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_QUERIES, 1);

    uint clipmapIndex = 0u;
    CLodVirtualShadowClipmapInfo clipmapInfo;
    if (!CLodVirtualShadowFindClipmapForView(viewId, clipmapIndex, clipmapInfo))
    {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_SHADOW_CLIPMAP_MISSES, 1);
        return true;
    }

    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> shadowCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactShadowCameras)];
    const CLodVirtualShadowCompactShadowCameraInfo shadowCamera = shadowCameras[clipmapIndex];
    Texture2DArray<uint> dirtyHierarchy = ResourceDescriptorHeap[CLOD_WG_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX];

    uint sampledMipLevel = 0u;
    bool queryClipped = false;

#if CLOD_VSM_USE_AABB_DIRTY_QUERY
    float2 uvMin = 0.0f.xx;
    float2 uvMax = 0.0f.xx;
    const bool queryValid = CLodVirtualShadowComputeSphereAabbUvBounds(
        worldCenter,
        radiusWorld,
        shadowCamera,
        uvMin,
        uvMax,
        queryClipped);
    const bool touchesDirtyPages = queryValid
        ? CLodVirtualShadowDirtyHierarchyAnyHit(
            dirtyHierarchy,
            clipmapInfo.pageTableLayer,
            uint2(clipmapInfo.pageTableResolution, clipmapInfo.pageTableResolution),
            uvMin,
            uvMax,
            sampledMipLevel)
        : false;
#else
    const float3 meshletCenterViewSpace = mul(float4(worldCenter, 1.0f), shadowCamera.view).xyz;
    const bool touchesDirtyPages = CLodVirtualShadowConservativeAnyHitTexture2DArraySphereQuery(
        dirtyHierarchy,
        clipmapInfo.pageTableLayer,
        uint2(clipmapInfo.pageTableResolution, clipmapInfo.pageTableResolution),
        shadowCamera,
        meshletCenterViewSpace,
        radiusWorld,
        sampledMipLevel,
        queryClipped);
#endif

    if (queryClipped)
    {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_QUERIES_CLIPPED, 1);
    }
    if (sampledMipLevel > 0u)
    {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_REGION_COARSE_MIP_CHECKS, 1);
    }
    if (touchesDirtyPages)
    {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_SHADOW_DIRTY_REGION_HITS, 1);
    }

    return touchesDirtyPages;
}

bool CLodVirtualShadowMeshletTouchesDirtyPages(float3 worldCenter, float radiusWorld, uint viewId)
{
    return CLodVirtualShadowBoundsTouchDirtyPages(worldCenter, radiusWorld, viewId);
}

bool CLodVirtualShadowComputeMeshletBlockCoverage(
    float3 worldCenter,
    float radiusWorld,
    uint shadowClipmapIndex,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    out uint2 minPageCoord,
    out uint2 maxPageCoord,
    out uint2 minBlockCoord,
    out uint2 blockCount)
{
    minPageCoord = uint2(0u, 0u);
    maxPageCoord = uint2(0u, 0u);
    minBlockCoord = uint2(0u, 0u);
    blockCount = uint2(0u, 0u);

    if (shadowClipmapIndex >= kCLodVirtualShadowClipmapCount || !CLodVirtualShadowClipmapIsValid(clipmapInfo))
    {
        return false;
    }

    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> shadowCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactShadowCameras)];
    const CLodVirtualShadowCompactShadowCameraInfo shadowCamera = shadowCameras[shadowClipmapIndex];

    float2 uvMin = 0.0f.xx;
    float2 uvMax = 0.0f.xx;
    bool queryClipped = false;
    const bool queryValid = CLodVirtualShadowComputeSphereAabbUvBounds(
        worldCenter,
        radiusWorld,
        shadowCamera,
        uvMin,
        uvMax,
        queryClipped);
    if (!queryValid)
    {
        return false;
    }

    minPageCoord = CLodVirtualShadowVirtualPageCoordsFromUv(uvMin, clipmapInfo);
    maxPageCoord = CLodVirtualShadowVirtualPageCoordsFromUv(uvMax, clipmapInfo);
    minBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(minPageCoord);
    const uint2 maxBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(maxPageCoord);
    blockCount = maxBlockCoord - minBlockCoord + 1u;
    return all(blockCount > uint2(0u, 0u));
}

bool CLodVirtualShadowBuildVisibleClusterBlockPayload(
    uint shadowClipmapIndex,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable,
    uint2 meshletMinPageCoord,
    uint2 meshletMaxPageCoord,
    uint2 blockCoord,
    out uint vsmPayload)
{
    vsmPayload = 0u;

    const uint2 blockOriginPageCoord = CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord);
    uint2 minLocalPageCoord = uint2(kCLodVirtualShadowBlockPagesPerAxis - 1u, kCLodVirtualShadowBlockPagesPerAxis - 1u);
    uint2 maxLocalPageCoord = uint2(0u, 0u);
    bool hasActivePage = false;

    [unroll]
    for (uint localPageY = 0u; localPageY < kCLodVirtualShadowBlockPagesPerAxis; ++localPageY)
    {
        [unroll]
        for (uint localPageX = 0u; localPageX < kCLodVirtualShadowBlockPagesPerAxis; ++localPageX)
        {
            const uint2 localPageCoord = uint2(localPageX, localPageY);
            const uint2 pageCoord = blockOriginPageCoord + localPageCoord;
            if (any(pageCoord < meshletMinPageCoord) || any(pageCoord > meshletMaxPageCoord))
            {
                continue;
            }

            if (pageCoord.x >= clipmapInfo.pageTableResolution || pageCoord.y >= clipmapInfo.pageTableResolution)
            {
                continue;
            }

            const uint2 wrappedPageCoord = CLodVirtualShadowWrappedPageCoords(pageCoord, clipmapInfo);
            const uint pageEntry = pageTable[uint3(wrappedPageCoord, clipmapInfo.pageTableLayer)];
            if (!CLodVirtualShadowPageEntryCanRaster(pageEntry))
            {
                continue;
            }

            hasActivePage = true;
            minLocalPageCoord = min(minLocalPageCoord, localPageCoord);
            maxLocalPageCoord = max(maxLocalPageCoord, localPageCoord);
        }
    }

    if (!hasActivePage)
    {
        return false;
    }

    vsmPayload = CLodPackVisibleClusterVsmPayloadForBlock(
        shadowClipmapIndex,
        blockCoord,
        minLocalPageCoord,
        maxLocalPageCoord,
        false);
    return true;
}

uint CLodVirtualShadowCountVisibleClusterBlocksForMeshlet(
    uint shadowClipmapIndex,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable,
    uint2 meshletMinPageCoord,
    uint2 meshletMaxPageCoord,
    uint2 minBlockCoord,
    uint2 blockCount)
{
    uint activeBlockCount = 0u;
    const uint blockSoftCap = min(
        max(1u, CLodPageJobMaxPagesPerCluster()),
        kCLodVirtualShadowBlockMaxTrackedPerCluster);
    const uint totalBlockCount = blockCount.x * blockCount.y;
    [loop]
    for (uint blockLinearIndex = 0u; blockLinearIndex < totalBlockCount; ++blockLinearIndex)
    {
        const uint2 blockCoord = uint2(blockLinearIndex % blockCount.x, blockLinearIndex / blockCount.x) + minBlockCoord;
        uint vsmPayload = 0u;
        if (CLodVirtualShadowBuildVisibleClusterBlockPayload(
                shadowClipmapIndex,
                clipmapInfo,
                pageTable,
                meshletMinPageCoord,
                meshletMaxPageCoord,
                blockCoord,
                vsmPayload))
        {
            activeBlockCount++;
            if (activeBlockCount > blockSoftCap)
            {
                return 1u;
            }
        }
    }

    return activeBlockCount;
}

void CLodVirtualShadowEmitVisibleClusterBlocksForMeshlet(
    globallycoherent RWByteAddressBuffer visibleClusters,
    uint writeBase,
    uint maxWriteCount,
    uint viewId,
    uint instanceIndex,
    uint localMeshletIndex,
    uint visibleGroupId,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint shadowClipmapIndex,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable,
    uint2 meshletMinPageCoord,
    uint2 meshletMaxPageCoord,
    uint2 minBlockCoord,
    uint2 blockCount)
{
    const uint blockSoftCap = min(
        max(1u, CLodPageJobMaxPagesPerCluster()),
        kCLodVirtualShadowBlockMaxTrackedPerCluster);
    uint activeBlockCount = 0u;
    const uint totalBlockCount = blockCount.x * blockCount.y;
    [loop]
    for (uint blockLinearIndex = 0u; blockLinearIndex < totalBlockCount; ++blockLinearIndex)
    {
        const uint2 blockCoord = uint2(blockLinearIndex % blockCount.x, blockLinearIndex / blockCount.x) + minBlockCoord;
        uint vsmPayload = 0u;
        if (CLodVirtualShadowBuildVisibleClusterBlockPayload(
                shadowClipmapIndex,
                clipmapInfo,
                pageTable,
                meshletMinPageCoord,
                meshletMaxPageCoord,
                blockCoord,
                vsmPayload))
        {
            activeBlockCount++;
            if (activeBlockCount > blockSoftCap)
            {
                if (maxWriteCount != 0u)
                {
                    CLodStoreVisibleClusterGloballyCoherent(
                        visibleClusters,
                        writeBase,
                        viewId,
                        instanceIndex,
                        localMeshletIndex,
                        visibleGroupId,
                        pageSlabDescriptorIndex,
                        pageSlabByteOffset,
                        shadowClipmapIndex);
                }
                return;
            }
        }
    }

    uint emittedCount = 0u;
    [loop]
    for (uint blockLinearIndex = 0u; blockLinearIndex < totalBlockCount; ++blockLinearIndex)
    {
        const uint2 blockCoord = uint2(blockLinearIndex % blockCount.x, blockLinearIndex / blockCount.x) + minBlockCoord;
        uint vsmPayload = 0u;
        if (!CLodVirtualShadowBuildVisibleClusterBlockPayload(
                shadowClipmapIndex,
                clipmapInfo,
                pageTable,
                meshletMinPageCoord,
                meshletMaxPageCoord,
                blockCoord,
                vsmPayload))
        {
            continue;
        }

        if (emittedCount < maxWriteCount)
        {
            CLodStoreVisibleClusterWithVsmPayloadGloballyCoherent(
                visibleClusters,
                writeBase + emittedCount,
                viewId,
                instanceIndex,
                localMeshletIndex,
                visibleGroupId,
                pageSlabDescriptorIndex,
                pageSlabByteOffset,
                vsmPayload);
        }
        emittedCount++;
    }
}

bool CLodVirtualShadowInstanceInvalidatedThisFrame(uint instanceIndex)
{
    if (instanceIndex >= kCLodVirtualShadowMovedInstanceBitCapacity)
    {
        return false;
    }

    StructuredBuffer<uint> invalidatedInstancesBitset =
        ResourceDescriptorHeap[CLOD_WG_SHADOW_INVALIDATED_INSTANCES_DESCRIPTOR_INDEX];
    const uint word = invalidatedInstancesBitset[instanceIndex >> 5u];
    return ((word >> (instanceIndex & 31u)) & 1u) != 0u;
}
#else
uint CLodResolveLodViewId(uint cullViewId)
{
    return cullViewId;
}

bool CLodVirtualShadowInstanceInvalidatedThisFrame(uint instanceIndex)
{
    (void)instanceIndex;
    return false;
}
#endif

bool CLodTrySetBit(RWByteAddressBuffer bits, uint key)
{
    uint oldPacked = 0;
    bits.InterlockedOr(CLodBitWordAddress(key), CLodBitMask(key), oldPacked);
    return (oldPacked & CLodBitMask(key)) == 0u;
}

uint CLodPackViewPriority(uint viewId, float fallbackErrorOverDistance)
{
    const float clampedPriority = min(max(fallbackErrorOverDistance * 1024.0f, 0.0f), 65535.0f);
    const uint quantizedPriority = (uint)(clampedPriority + 0.5f);
    return ((quantizedPriority & CLOD_STREAM_VIEWID_MASK) << CLOD_STREAM_PRIORITY_SHIFT)
        | (viewId & CLOD_STREAM_VIEWID_MASK);
}

static const uint TRAVERSE_THREADS_PER_GROUP = 32;
static const uint BVH_MAX_CHILDREN = 8;
static const uint COALESCED_INPUT_COUNT_HISTOGRAM_BUCKETS = 8;

static const uint TRAVERSE_RECORDS_PER_GROUP = TRAVERSE_THREADS_PER_GROUP;
static const uint SEGMENT_EVALUATE_THREADS_PER_GROUP = 32;
static const uint SEGMENT_EVALUATE_RECORDS_PER_GROUP = SEGMENT_EVALUATE_THREADS_PER_GROUP;
static const uint MAX_RECORDS_PER_SEGMENT = 8;

void WGTelemetryAdd(uint counterIndex, uint value)
{
    if (!CLodWorkGraphTelemetryEnabled())
    {
        return;
    }

    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryCounters[counterIndex], value);
}

float ProjectedGeometricError(
    float3 worldCenter,
    float worldRadius,
    float errorMeshSpace,
    float errorScale,
    float3 cameraPos,
    float zNear,
    bool isOrtho);

bool SphereOutsideFrustumViewSpace(float3 viewSpaceCenter, float radius, Camera camera);

void CLodAppendVoxelRasterClusterWork(
    CLodMeshMetadata clodMeshMetadata,
    uint instanceIndex,
    uint viewId,
    uint localGroupId,
    ClusterLODGroup voxelGroup,
    ClusterLODGroupSegment voxelSegment,
    CLodVoxelGroupDescriptor voxelDescriptor,
    float4x4 objectModelMatrix,
    float lodUniformScale,
    Camera cullCamera,
    CullingCameraInfo lodCam,
    bool lodCameraIsOrtho,
    bool clusterDirtyPageCullingEnabled,
    uint meshBufferIndex,
    float ownGroupErrorOverDistance)
{
    (void)lodCam;
    (void)lodCameraIsOrtho;
    (void)meshBufferIndex;
    (void)ownGroupErrorOverDistance;

    if (voxelDescriptor.clusterCount == 0u)
    {
        return;
    }

    GroupPageMapEntry voxelPageEntry = CLodLoadVoxelPageMapEntry(clodMeshMetadata, voxelGroup, voxelSegment.pageIndex);
    CLodVoxelPageHeader voxelPageHeader = CLodLoadVoxelPageHeader(voxelPageEntry.slabDescriptorIndex, voxelPageEntry.slabByteOffset);
    if (voxelPageHeader.magic != CLOD_VOXEL_PAGE_MAGIC ||
        voxelPageHeader.version != CLOD_VOXEL_PAGE_VERSION ||
        voxelSegment.firstMeshletInPage + voxelDescriptor.clusterCount > voxelPageHeader.clusterCount)
    {
        return;
    }

    StructuredBuffer<CLodVoxelRasterQueueDescriptors> queueDescriptorBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID)];
    const CLodVoxelRasterQueueDescriptors queueDescriptors = queueDescriptorBuffer[0];
    if (queueDescriptors.workRecordsUAVDescriptorIndex == 0xFFFFFFFFu ||
        queueDescriptors.workRecordCounterUAVDescriptorIndex == 0xFFFFFFFFu ||
        queueDescriptors.workRecordCapacity == 0u)
    {
        return;
    }

    RWStructuredBuffer<CLodVoxelRasterWorkRecord> workRecords =
        ResourceDescriptorHeap[queueDescriptors.workRecordsUAVDescriptorIndex];
    RWStructuredBuffer<uint> workRecordCounter =
        ResourceDescriptorHeap[queueDescriptors.workRecordCounterUAVDescriptorIndex];
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> visibleClusterCounter =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState =
        ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> phase1HWBaseCounter = ResourceDescriptorHeap[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX];
    const uint phase1HWBase = CLodWorkGraphIsPhase2() ? phase1HWBaseCounter.Load(0) : 0u;
    StructuredBuffer<uint> phase1SWBaseCounter = ResourceDescriptorHeap[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX];
    const uint phase1SWBase = CLodWorkGraphIsPhase2() ? phase1SWBaseCounter.Load(0) : 0u;

    uint appendedCount = 0u;
    uint droppedCount = 0u;
    const uint visibleClusterCapacity = CLOD_WG_VISIBLE_CLUSTERS_CAPACITY;
    const uint visibleClusterWriteCapacity = CLodWorkGraphSharedVisibleClusterWriteCapacity(
        visibleClusterCapacity,
        phase1HWBase,
        phase1SWBase);

    for (uint clusterIndex = 0u; clusterIndex < voxelDescriptor.clusterCount; ++clusterIndex)
    {
        const uint localVoxelClusterIndex = voxelDescriptor.firstCluster + clusterIndex;
        const uint pageLocalClusterIndex = voxelSegment.firstMeshletInPage + clusterIndex;
        const CLodVoxelClusterRecord voxelCluster = CLodLoadVoxelClusterFromPage(
            voxelPageEntry.slabDescriptorIndex,
            voxelPageEntry.slabByteOffset,
            voxelPageHeader.clusterRecordsOffset,
            pageLocalClusterIndex);
        if (voxelCluster.cubeCount == 0u || voxelCluster.cubeCount > CLOD_VOXEL_MAX_CUBES_PER_CLUSTER)
        {
            continue;
        }

        const float3 clusterWorldCenter = mul(float4(voxelCluster.bounds.xyz, 1.0f), objectModelMatrix).xyz;
        const float clusterWorldRadius = voxelCluster.bounds.w * lodUniformScale;
        const float3 clusterViewCenter = mul(float4(clusterWorldCenter, 1.0f), cullCamera.view).xyz;
        if (CLodWorkGraphFrustumCullingEnabled() &&
            SphereOutsideFrustumViewSpace(clusterViewCenter, clusterWorldRadius, cullCamera))
        {
            continue;
        }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (clusterDirtyPageCullingEnabled && !CLodVirtualShadowBoundsTouchDirtyPages(clusterWorldCenter, clusterWorldRadius, viewId))
        {
            continue;
        }
#else
        (void)clusterDirtyPageCullingEnabled;
#endif

        uint combinedSlot = 0u;
        InterlockedAdd(replayState[0].visibleClusterCombinedCount, 1u, combinedSlot);
        if (combinedSlot >= visibleClusterWriteCapacity)
        {
            InterlockedMin(replayState[0].visibleClusterCombinedCount, visibleClusterWriteCapacity);
            ++droppedCount;
            continue;
        }

        uint baseSlot = 0u;
        InterlockedAdd(workRecordCounter[0], 1u, baseSlot);
        if (baseSlot >= queueDescriptors.workRecordCapacity)
        {
            InterlockedMin(workRecordCounter[0], queueDescriptors.workRecordCapacity);
            ++droppedCount;
            continue;
        }

        uint visibleBase = 0u;
        InterlockedAdd(visibleClusterCounter[0], 1u, visibleBase);
        const uint visibleClusterIndex = phase1HWBase + visibleBase;
        CLodStoreVisibleClusterWithVsmPayloadGloballyCoherent(
            visibleClusters,
            visibleClusterIndex,
            viewId,
            instanceIndex,
            localVoxelClusterIndex & 0x3FFFu,
            localGroupId,
            localVoxelClusterIndex >> 14u,
            0u,
            CLodVisibleClusterMarkVoxelPayload(CLodBuildVisibleClusterVsmPayloadFromClipmapIndex(CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX)));

        CLodVoxelRasterWorkRecord record;
        record.visibleClusterIndex = visibleClusterIndex;
        record.pad0 = 0u;
        record.pad1 = 0u;
        record.pad2 = 0u;
        workRecords[baseSlot] = record;
        ++appendedCount;
    }

    if (appendedCount != 0u)
    {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_RASTER_WORK_RECORDS, appendedCount);
    }
    if (droppedCount != 0u)
    {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_RASTER_WORK_DROPPED, droppedCount);
    }
}

void ReplayReserveNodeSlotsWave(
    RWStructuredBuffer<CLodReplayBufferState> replayState,
    uint capacity,
    out uint slot,
    out bool valid)
{
    const uint4 activeMask = WaveActiveBallot(true);
    const uint activeCount = CountBits128(activeMask);
    const uint leaderLane = WaveFirstLaneFromMask(activeMask);
    const uint laneRank = GetLaneRankInGroup(activeMask, WaveGetLaneIndex());

    uint baseSlot = 0;
    if (WaveGetLaneIndex() == leaderLane)
    {
        InterlockedAdd(replayState[0].nodeWriteCount, activeCount, baseSlot);
    }

    baseSlot = WaveReadLaneAt(baseSlot, leaderLane);
    slot = baseSlot + laneRank;
    valid = slot < capacity;

    const uint droppedCount = CountBits128(WaveActiveBallot(!valid));
    if (WaveGetLaneIndex() == leaderLane && droppedCount > 0)
    {
        InterlockedAdd(replayState[0].nodeDropped, droppedCount);
    }
}

void ReplayReserveMeshletSlotsWave(
    RWStructuredBuffer<CLodReplayBufferState> replayState,
    uint capacity,
    out uint slot,
    out bool valid)
{
    const uint4 activeMask = WaveActiveBallot(true);
    const uint activeCount = CountBits128(activeMask);
    const uint leaderLane = WaveFirstLaneFromMask(activeMask);
    const uint laneRank = GetLaneRankInGroup(activeMask, WaveGetLaneIndex());

    uint baseSlot = 0;
    if (WaveGetLaneIndex() == leaderLane)
    {
        InterlockedAdd(replayState[0].meshletWriteCount, activeCount, baseSlot);
    }

    baseSlot = WaveReadLaneAt(baseSlot, leaderLane);
    slot = baseSlot + laneRank;
    valid = slot < capacity;

    const uint droppedCount = CountBits128(WaveActiveBallot(!valid));
    if (WaveGetLaneIndex() == leaderLane && droppedCount > 0)
    {
        InterlockedAdd(replayState[0].meshletDropped, droppedCount);
    }
}

uint PackTraverseNodeId(uint nodeId, uint sourceTag, uint allowRefine)
{
    return (sourceTag << 31u) | (allowRefine << 30u) | (nodeId & 0x3FFFFFFFu);
}

uint UnpackNodeId(uint packed)      { return packed & 0x3FFFFFFFu; }
uint UnpackSourceTag(uint packed)   { return packed >> 31u; }
uint UnpackAllowRefine(uint packed) { return (packed >> 30u) & 1u; }

uint PackGroupId(uint groupId, uint sourceTag)
{
    return (sourceTag << 31u) | (groupId & 0x7FFFFFFFu);
}

uint UnpackGroupId(uint packed)        { return packed & 0x7FFFFFFFu; }
uint UnpackGroupSourceTag(uint packed) { return packed >> 31u; }

uint PackMeshletIndexAndCount(uint firstIndex, uint count)
{
    return (count << 16u) | (firstIndex & 0xFFFFu);
}

uint UnpackMeshletFirstIndex(uint packed) { return packed & 0xFFFFu; }
uint UnpackMeshletCount(uint packed)      { return packed >> 16u; }

// Write a TraverseNodeRecord directly to the node replay region.
bool ReplayTryAppendNode(uint instanceIndex, uint viewId, uint nodeId)
{
    WGTelemetryAdd(WG_COUNTER_PHASE1_OCCLUSION_NODE_REPLAY_ENQUEUE_ATTEMPTS, 1);

    RWByteAddressBuffer replayBuffer = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];

    uint slot = 0;
    bool valid = false;
    ReplayReserveNodeSlotsWave(replayState, CLOD_NODE_REPLAY_CAPACITY, slot, valid);

    if (!valid) {
        return false;
    }

    // TraverseNodeRecord layout: instanceIndex, nodeIdPacked, viewId (3 uints = 12 bytes)
    const uint byteOffset = slot * CLOD_NODE_REPLAY_STRIDE_BYTES;
    const uint packed = PackTraverseNodeId(nodeId, CLOD_RECORD_SOURCE_REPLAY, 1u);
    replayBuffer.Store2(byteOffset, uint2(instanceIndex, packed));
    replayBuffer.Store(byteOffset + 8u, viewId);
    return true;
}

// Write a MeshletBucketRecord directly to the meshlet replay region.
bool ReplayTryAppendMeshlet(uint instanceIndex, uint viewId, uint groupId, uint localMeshletIndex,
                            uint pageSlabDescriptorIndex, uint pageSlabByteOffset)
{
    WGTelemetryAdd(WG_COUNTER_PHASE1_OCCLUSION_CLUSTER_REPLAY_ENQUEUE_ATTEMPTS, 1);

    RWByteAddressBuffer replayBuffer = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];

    uint slot = 0;
    bool valid = false;
    ReplayReserveMeshletSlotsWave(replayState, CLOD_MESHLET_REPLAY_CAPACITY, slot, valid);

    if (!valid) {
        return false;
    }

    // MeshletBucketRecord layout: instanceIndex, viewId, groupIdPacked,
    //                             meshletIndexAndCount, pageSlabDescriptorIndex, pageSlabByteOffset (6 uints = 24 bytes)
    const uint byteOffset = CLOD_REPLAY_MESHLET_REGION_OFFSET + slot * CLOD_MESHLET_REPLAY_STRIDE_BYTES;
    replayBuffer.Store4(byteOffset,       uint4(instanceIndex, viewId,
                                                PackGroupId(groupId, CLOD_RECORD_SOURCE_REPLAY),
                                                PackMeshletIndexAndCount(localMeshletIndex, 1u)));
    replayBuffer.Store2(byteOffset + 16u, uint2(pageSlabDescriptorIndex, pageSlabByteOffset));
    return true;
}

// Records
struct ObjectCullRecord
{
    uint viewDataIndex; // One record per view, times...
    uint activeDrawSetIndicesSRVIndex; // One record per draw set
    uint activeDrawCount;
    uint drawRecordVisibilityGenerationSRVIndex;
    uint3 dispatchGrid : SV_DispatchGrid; // Drives dispatch size
};

struct TraverseNodeRecord
{
    uint instanceIndex;
    uint nodeIdPacked; // [31]=sourceTag, [30]=allowRefine, [29:0]=nodeId
    uint viewId;
};

struct MeshletBucketRecord
{
    uint instanceIndex;
    uint viewId;
    uint groupIdPacked;         // [31]=sourceTag, [30:0]=groupId
    uint meshletIndexAndCount;  // [31:16]=count, [15:0]=firstLocalMeshletIndex
    uint pageSlabDescriptorIndex;
    uint pageSlabByteOffset;
};

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
struct CLodVirtualShadowPredictiveInvalidationCandidate
{
    float4 worldCenterAndRadius;
    uint shadowViewId;
    uint sourceGroupGlobalIndex;
    uint pad0;
    uint pad1;
};

void CLodAppendVirtualShadowPredictiveInvalidationCandidate(
    float3 worldCenter,
    float radiusWorld,
    uint shadowViewId,
    uint sourceGroupGlobalIndex)
{
    RWStructuredBuffer<CLodVirtualShadowPredictiveInvalidationCandidate> candidateBuffer =
        ResourceDescriptorHeap[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> candidateCount =
        ResourceDescriptorHeap[CLOD_WG_SHADOW_PREDICTIVE_INVALIDATION_CANDIDATE_COUNT_DESCRIPTOR_INDEX];

    uint candidateIndex = 0u;
    InterlockedAdd(candidateCount[0], 1u, candidateIndex);
    if (candidateIndex < CLOD_VIRTUAL_SHADOW_PREDICTIVE_CANDIDATE_CAPACITY)
    {
        CLodVirtualShadowPredictiveInvalidationCandidate candidate;
        candidate.worldCenterAndRadius = float4(worldCenter, radiusWorld);
        candidate.shadowViewId = shadowViewId;
        candidate.sourceGroupGlobalIndex = sourceGroupGlobalIndex;
        candidate.pad0 = 0u;
        candidate.pad1 = 0u;
        candidateBuffer[candidateIndex] = candidate;
    }
}
#endif

// Conservative max-axis scale from a row-vector local->world
float MaxAxisScale_RowVector(float4x4 M)
{
    float3 ax = float3(M[0][0], M[0][1], M[0][2]);
    float3 ay = float3(M[1][0], M[1][1], M[1][2]);
    float3 az = float3(M[2][0], M[2][1], M[2][2]);
    return max(length(ax), max(length(ay), length(az)));
}

BoundingSphere ComputeSkinnedMeshletBounds(
    CLodMeshletDescriptor desc,
    CLodPageHeader pageHeader,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint skinningInstanceSlot)
{
    BoundingSphere staticBounds = { desc.bounds };
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot) || CLodDescBoneCount(desc) == 0u)
    {
        return staticBounds;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[pageSlabDescriptorIndex];
    const uint boneListBase = pageSlabByteOffset + pageHeader.boneIndexStreamOffset + desc.boneListOffset * 4u;

    float3 mergedCenter = float3(0.0f, 0.0f, 0.0f);
    float mergedRadius = 0.0f;
    bool mergedInitialized = false;

    [loop]
    for (uint boneIndex = 0; boneIndex < CLodDescBoneCount(desc); ++boneIndex)
    {
        const uint jointIndex = slab.Load(boneListBase + boneIndex * 4u);
        const float4x4 boneSkinMatrix = LoadBoneSkinMatrix(skinningInstanceSlot, jointIndex);
        const float3 transformedCenter = mul(float4(staticBounds.sphere.xyz, 1.0f), boneSkinMatrix).xyz;
        const float transformedRadius = staticBounds.sphere.w * SkinningMaxAxisScale_RowVector(boneSkinMatrix);

        if (!mergedInitialized)
        {
            mergedCenter = transformedCenter;
            mergedRadius = transformedRadius;
            mergedInitialized = true;
            continue;
        }

        const float3 delta = transformedCenter - mergedCenter;
        const float dist = length(delta);
        if (dist + transformedRadius <= mergedRadius)
        {
            continue;
        }
        if (dist + mergedRadius <= transformedRadius)
        {
            mergedCenter = transformedCenter;
            mergedRadius = transformedRadius;
            continue;
        }

        const float newRadius = 0.5f * (dist + mergedRadius + transformedRadius);
        const float t = (newRadius - mergedRadius) / max(dist, 1e-12f);
        mergedCenter += delta * t;
        mergedRadius = newRadius;
    }

    if (!mergedInitialized)
    {
        return staticBounds;
    }

    BoundingSphere result = { float4(mergedCenter, mergedRadius * (1.0f + 1e-5f)) };
    return result;
}

float3 ToViewSpace(float3 objectCenter, row_major matrix objectModelMatrix, row_major matrix viewMatrix)
{
    float4 worldSpaceCenter = mul(float4(objectCenter, 1.0f), objectModelMatrix);
    return mul(worldSpaceCenter, viewMatrix).xyz;
}

bool SphereOutsideFrustumViewSpace(float3 viewSpaceCenter, float radius, Camera camera)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = camera.clippingPlanes[i].plane;
        float distanceToPlane = dot(plane.xyz, viewSpaceCenter) + plane.w;
        if (distanceToPlane < -radius)
        {
            return true;
        }
    }

    return false;
}

bool SphereOutsideFrustumViewSpace(float3 viewSpaceCenter, float radius, float4 planes[6])
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float distanceToPlane = dot(planes[i].xyz, viewSpaceCenter) + planes[i].w;
        if (distanceToPlane < -radius)
        {
            return true;
        }
    }

    return false;
}

void WGTelemetryAddObjectCullPlaneReject(uint planeIndex)
{
    switch (planeIndex)
    {
    case 0u: WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_LEFT, 1); break;
    case 1u: WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_RIGHT, 1); break;
    case 2u: WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_BOTTOM, 1); break;
    case 3u: WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_TOP, 1); break;
    case 4u: WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_NEAR, 1); break;
    case 5u: WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_FAR, 1); break;
    }
}

// Perspective views attenuate projected geometric error by distance.
// Orthographic views keep a constant world-to-screen scale, so the projected
// error reduces to the world-space error directly.
float ProjectedGeometricError(
    float3 worldCenter,
    float worldRadius,
    float errorMeshSpace,
    float errorScale,
    float3 cameraPos,
    float zNear,
    bool isOrtho)
{
    const float worldSpaceError = errorMeshSpace * errorScale;
    if (isOrtho) {
        return worldSpaceError;
    }

    // Conservative "distance to sphere surface"
    float dist = length(worldCenter - cameraPos);
    float denom = max(dist - worldRadius, zNear);

    return worldSpaceError / denom;
}

bool CLodGroupIsResident(uint groupGlobalIndex)
{
    StructuredBuffer<CLodStreamingRuntimeState> runtimeState =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingRuntimeState)];
    const uint activeGroupScanCount = runtimeState[0].activeGroupScanCount;
    if (groupGlobalIndex >= activeGroupScanCount)
    {
        return false;
    }

    ByteAddressBuffer nonResidentBits =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingNonResidentBits)];
    return !CLodReadBit(nonResidentBits, groupGlobalIndex);
}

void CLodMarkGroupTouched(uint groupGlobalIndex)
{
    RWStructuredBuffer<uint> usedGroupsCounter =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingTouchedGroupsCounter)];
    RWStructuredBuffer<uint> usedGroupsBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingTouchedGroups)];
    uint usedSlot = 0u;
    InterlockedAdd(usedGroupsCounter[0], 1u, usedSlot);
    if (usedSlot < CLOD_USED_GROUPS_CAPACITY)
    {
        usedGroupsBuffer[usedSlot] = groupGlobalIndex;
    }
}

void CLodRequestGroupLoad(
    uint groupGlobalIndex,
    uint instanceIndex,
    uint meshBufferIndex,
    uint viewId,
    float requestPriorityErrorOverDistance)
{
    StructuredBuffer<CLodStreamingRuntimeState> runtimeState =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingRuntimeState)];
    const uint activeGroupScanCount = runtimeState[0].activeGroupScanCount;
    if (groupGlobalIndex >= activeGroupScanCount)
    {
        return;
    }

    RWStructuredBuffer<CLodStreamingRequest> loadRequests =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingLoadRequests)];
    RWStructuredBuffer<uint> loadRequestKeys =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingLoadRequestKeys)];
    RWStructuredBuffer<uint> loadRequestCounter =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingLoadCounter)];
    uint requestIndex = 0u;
    InterlockedAdd(loadRequestCounter[0], 1u, requestIndex);
    if (requestIndex < CLOD_STREAM_REQUEST_CAPACITY)
    {
        CLodStreamingRequest req = (CLodStreamingRequest)0;
        req.groupGlobalIndex = groupGlobalIndex;
        req.meshInstanceIndex = instanceIndex;
        req.meshBufferIndex = meshBufferIndex;
        req.viewId = CLodPackViewPriority(viewId, requestPriorityErrorOverDistance);
        loadRequests[requestIndex] = req;

        const uint priority16 = (req.viewId >> 16u) & 0xffffu;
        loadRequestKeys[requestIndex] = 0xffffu - priority16;
    }
}

bool CLodTouchAndRequestGroupResident(
    uint groupGlobalIndex,
    uint instanceIndex,
    uint meshBufferIndex,
    uint viewId,
    float requestPriorityErrorOverDistance)
{
    CLodMarkGroupTouched(groupGlobalIndex);
    if (CLodGroupIsResident(groupGlobalIndex))
    {
        return true;
    }

    CLodRequestGroupLoad(
        groupGlobalIndex,
        instanceIndex,
        meshBufferIndex,
        viewId,
        requestPriorityErrorOverDistance);
    return false;
}

bool CLodRefinedChildSuppressesParent(
    uint groupsBase,
    uint childGroupLocalIndex,
    bool hasRefinedChild,
    float4x4 objectModelMatrix,
    float lodUniformScale,
    CullingCameraInfo lodCam,
    bool lodCameraIsOrtho,
    uint instanceIndex,
    uint meshBufferIndex,
    uint viewId,
    float requestPriorityErrorOverDistance,
    float3 predictiveCenterWorld,
    float predictiveRadiusWorld,
    bool emitNonResidentTelemetry)
{
    if (!hasRefinedChild)
    {
        return false;
    }

    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];

    const uint childGroupGlobalIndex = groupsBase + childGroupLocalIndex;
    const ClusterLODGroup childGroup = groups[childGroupGlobalIndex];
    const float3 childWorldCenter = mul(float4(childGroup.bounds.centerAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
    const float childWorldRadius = childGroup.bounds.centerAndRadius.w * lodUniformScale;
    const float childBoundaryEOD = ProjectedGeometricError(
        childWorldCenter,
        childWorldRadius,
        childGroup.bounds.error,
        lodUniformScale,
        lodCam.positionWorldSpace.xyz,
        lodCam.zNear,
        lodCameraIsOrtho);

    if (childBoundaryEOD <= lodCam.errorOverDistanceThreshold)
    {
        return false;
    }

    if (CLodTouchAndRequestGroupResident(
        childGroupGlobalIndex,
        instanceIndex,
        meshBufferIndex,
        viewId,
        requestPriorityErrorOverDistance))
    {
        return true;
    }

    if (emitNonResidentTelemetry)
    {
        WGTelemetryAdd(WG_COUNTER_SEGMENT_EVALUATE_NON_RESIDENT_REFINED_CHILD_THREADS, 1);
    }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    if (CLodWorkGraphVirtualShadowPredictiveLodInvalidationEnabled())
    {
        const bool useChildPredictiveBounds = predictiveRadiusWorld < 0.0f;
        CLodAppendVirtualShadowPredictiveInvalidationCandidate(
            useChildPredictiveBounds ? childWorldCenter : predictiveCenterWorld,
            useChildPredictiveBounds ? childWorldRadius : predictiveRadiusWorld,
            viewId,
            childGroupGlobalIndex);
    }
#endif

    return false;
}

struct CLodRenderableLeaf
{
    bool valid;
    bool isVoxel;
    bool canRender;
    uint groupGlobalIndex;
    ClusterLODGroup group;
    float errorOverDistance;
};

bool CLodPrepareRenderableLeaf(
    CLodMeshMetadata clodMeshMetadata,
    ClusterLODNode node,
    bool parentAllowsRefine,
    float4x4 objectModelMatrix,
    float lodUniformScale,
    CullingCameraInfo lodCam,
    bool lodCameraIsOrtho,
    bool nodeTouchesDirtyPages,
    bool forceLodDecision,
    uint instanceIndex,
    uint meshBufferIndex,
    uint viewId,
    out CLodRenderableLeaf leaf)
{
    leaf = (CLodRenderableLeaf)0;
    leaf.groupGlobalIndex = clodMeshMetadata.groupsBase + node.range.ownerGroupId;
    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    leaf.group = groups[leaf.groupGlobalIndex];
    leaf.isVoxel = (leaf.group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;

    if (leaf.isVoxel)
    {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_LEAF_RECORDS, 1);
    }

    const float3 groupWorldCenter = mul(float4(leaf.group.bounds.centerAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
    const float groupWorldRadius = leaf.group.bounds.centerAndRadius.w * lodUniformScale;
    leaf.errorOverDistance = ProjectedGeometricError(
        groupWorldCenter,
        groupWorldRadius,
        node.metric.maxQuadricError,
        lodUniformScale,
        lodCam.positionWorldSpace.xyz,
        lodCam.zNear,
        lodCameraIsOrtho);
    const bool wantsRender = forceLodDecision || (parentAllowsRefine && (leaf.errorOverDistance >= lodCam.errorOverDistanceThreshold));
    if (!wantsRender)
    {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_REJECTED_BY_ERROR_RECORDS, 1);
        if (leaf.isVoxel)
        {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_REJECTED_BY_ERROR_RECORDS, 1);
        }
        return false;
    }

    if (!nodeTouchesDirtyPages)
    {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES, 1);
        return false;
    }

    leaf.canRender = CLodTouchAndRequestGroupResident(
        leaf.groupGlobalIndex,
        instanceIndex,
        meshBufferIndex,
        viewId,
        leaf.errorOverDistance);
    if (!leaf.canRender)
    {
        if (leaf.isVoxel)
        {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_DESCRIPTOR_MISSES, 1);
        }
    }

    leaf.valid = true;
    return true;
}

// Node: ObjectCull (entry)
#ifndef CLOD_COMPUTE_INCLUDE_ONLY
[Shader("node")]
[NodeID("ObjectCull")]
[NodeLaunch("broadcasting")]
[NumThreads(64, 1, 1)]
[NodeMaxDispatchGrid(10000, 1, 1)]
[NodeIsProgramEntry]
void WG_ObjectCull(
    DispatchNodeInputRecord< ObjectCullRecord> inRec,
    const uint3 vGroupThreadID : SV_GroupThreadID,
    const uint3 vDispatchThreadID : SV_DispatchThreadID,
    [MaxRecords(64)] NodeOutput<TraverseNodeRecord> TraverseNodes) {
    const ObjectCullRecord hdr = inRec.Get();
    const bool inRange = (vDispatchThreadID.x < hdr.activeDrawCount);
    bool entryVisible = inRange;
    uint drawRecordIndex = 0u;

    WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_THREADS, 1);
    if (inRange) {
        WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_IN_RANGE_THREADS, 1);
    }

    uint outCount = 0;
    TraverseNodeRecord outRecord = (TraverseNodeRecord) 0;

    if (entryVisible) {
        StructuredBuffer<uint2> activeDrawSetIndicesBuffer =
                    ResourceDescriptorHeap[hdr.activeDrawSetIndicesSRVIndex];
        StructuredBuffer<uint> drawRecordVisibilityGenerations =
                    ResourceDescriptorHeap[hdr.drawRecordVisibilityGenerationSRVIndex];

        const uint2 activeEntry = activeDrawSetIndicesBuffer[vDispatchThreadID.x];
        drawRecordIndex = activeEntry.x;
        const uint activeGeneration = activeEntry.y;
        if (activeGeneration == 0u || drawRecordVisibilityGenerations[drawRecordIndex] != activeGeneration) {
            entryVisible = false;
        }
    }

    if (entryVisible) {
        const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(drawRecordIndex);
        const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDrawRecord(drawRecord);
        const PerObjectBuffer instanceTransform = LoadInstanceTransformForDrawRecord(drawRecord);

        StructuredBuffer<PerMeshBuffer> perMeshBuffer =
                    ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshBuffer perMesh = perMeshBuffer[instanceData.perMeshBufferIndex];

        const row_major matrix objectModelMatrix = instanceTransform.model;

        StructuredBuffer<Camera> cameras =
                    ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const Camera camera = cameras[hdr.viewDataIndex];

        const float3 objectSpaceCenter = instanceData.boundingSphere.sphere.xyz;
        const float3 viewSpaceCenter = ToViewSpace(objectSpaceCenter, objectModelMatrix, camera.view);
        const float worldRadius = instanceData.boundingSphere.sphere.w * MaxAxisScale_RowVector(objectModelMatrix);

        bool culled = false;
        if (any(isnan(viewSpaceCenter)) || any(isinf(viewSpaceCenter)) || isnan(worldRadius) || isinf(worldRadius)) {
            WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_INVALID_BOUNDS, 1);
            culled = true;
        }
        else if (CLodWorkGraphFrustumCullingEnabled()) {
            [unroll]
            for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
            {
                const float4 plane = camera.clippingPlanes[planeIndex].plane;
                const float distanceToPlane = dot(plane.xyz, viewSpaceCenter) + plane.w;
                if (distanceToPlane < -worldRadius)
                {
                    WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_FRUSTUM, 1);
                    WGTelemetryAddObjectCullPlaneReject(planeIndex);
                    culled = true;
                    break;
                }
            }
        }
        if (!culled) {
            StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
                            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];

            const MeshInstanceClodOffsets off = LoadCLodOffsetsForDrawRecord(drawRecord);
            const CLodMeshMetadata clodMeshMetadata = clodMeshMetadataBuffer[off.clodMeshMetadataIndex];

            outRecord.viewId = hdr.viewDataIndex;
            outRecord.instanceIndex = drawRecordIndex;
            outRecord.nodeIdPacked = PackTraverseNodeId(CLodResolveTraversalRootNode(clodMeshMetadata), CLOD_RECORD_SOURCE_PASS1, 1u);
            outCount = 1;

            WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_VISIBLE_THREADS, 1);
            WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_TRAVERSE_RECORDS, 1);
        }
    }

    // Uniform call; per-thread count may be 0/1.
    ThreadNodeOutputRecords<TraverseNodeRecord> outRecs =
        TraverseNodes.GetThreadNodeOutputRecords(outCount);

    if (outCount == 1) {
        outRecs.Get() = outRecord;
    }

    // Must be uniform even when some threads requested 0 records.
    outRecs.OutputComplete();
}

// Node: TraverseNodes (recursive, BVH-only)
[Shader("node")]
[NodeID("TraverseNodes")]
[NodeLaunch("coalescing")]
[NodeIsProgramEntry]
[NumThreads(TRAVERSE_THREADS_PER_GROUP, 1, 1)]
[NodeMaxRecursionDepth(25)]
void WG_TraverseNodes(
    [MaxRecords(TRAVERSE_RECORDS_PER_GROUP)] GroupNodeInputRecords<TraverseNodeRecord> inRecs,
    uint GI : SV_GroupIndex,
    [MaxRecords(TRAVERSE_RECORDS_PER_GROUP * BVH_MAX_CHILDREN)] NodeOutput<TraverseNodeRecord> TraverseNodes,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<MeshletBucketRecord> ClusterCull1,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<MeshletBucketRecord> ClusterCull2,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<MeshletBucketRecord> ClusterCull4,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<MeshletBucketRecord> ClusterCull8,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<MeshletBucketRecord> ClusterCull16,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<MeshletBucketRecord> ClusterCull32,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<MeshletBucketRecord> ClusterCull64)
{
    const uint slot = GI;
    const uint inputCount = inRecs.Count();
    const bool slotActive = slot < inputCount;

    WGTelemetryAdd(WG_COUNTER_TRAVERSE_THREADS, 1);
    if (slot == 0) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_COALESCED_LAUNCHES, 1);
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_COALESCED_INPUT_RECORDS, inputCount);
        if (inputCount > 0 && inputCount <= COALESCED_INPUT_COUNT_HISTOGRAM_BUCKETS) {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_1 + (inputCount - 1), 1);
        }
    }

    uint emitTraverseCount = 0;
    TraverseNodeRecord childRecords[BVH_MAX_CHILDREN];
    MeshletBucketRecord bucketRecord = (MeshletBucketRecord)0;
    uint emittedSegmentMeshletCount = 0;
    uint n64 = 0, n32 = 0, n16 = 0, n8 = 0, n4 = 0, n2 = 0, n1 = 0;
    bool emitBucket = false;

    if (slotActive) {
        const TraverseNodeRecord rec = inRecs[slot];
        const bool parentAllowsRefine = (UnpackAllowRefine(rec.nodeIdPacked) != 0u);
        if (UnpackSourceTag(rec.nodeIdPacked) == CLOD_RECORD_SOURCE_REPLAY) {
            WGTelemetryAdd(WG_COUNTER_PHASE2_REPLAY_TRAVERSE_RECORDS_CONSUMED, 1);
        }
        const bool replaySource = (UnpackSourceTag(rec.nodeIdPacked) == CLOD_RECORD_SOURCE_REPLAY);

        StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
        const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(rec.instanceIndex);
        const MeshInstanceClodOffsets off = LoadCLodOffsetsForDrawRecord(drawRecord);
        const CLodMeshMetadata clodMeshMetadata = clodMeshMetadataBuffer[off.clodMeshMetadataIndex];
        const bool forceLodDecision = CLodForcedTraversalDepthRootEnabled(clodMeshMetadata);
        const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDrawRecord(drawRecord);
        const PerObjectBuffer instanceTransform = LoadInstanceTransformForDrawRecord(drawRecord);
        StructuredBuffer<PerMeshBuffer> perMeshBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshBuffer perMesh = perMeshBuffer[instanceData.perMeshBufferIndex];
        const bool isSkinned = (perMesh.vertexFlags & VERTEX_SKINNED) != 0u;
        const row_major matrix objectModelMatrix = instanceTransform.model;
        StructuredBuffer<Camera> cameras =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const uint cullViewId = rec.viewId;
        const uint lodViewId = CLodResolveLodViewId(cullViewId);
        const Camera cullCamera = cameras[cullViewId];
        const Camera lodCamera = cameras[lodViewId];
        StructuredBuffer<CullingCameraInfo> cameraInfos =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
        const CullingCameraInfo lodCam = cameraInfos[lodViewId];
        StructuredBuffer<ClusterLODNode> lodNodes =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Nodes)];

        const ClusterLODNode node = lodNodes[clodMeshMetadata.lodNodesBase + UnpackNodeId(rec.nodeIdPacked)];

        if (node.range.isLeaf == CLOD_NODE_INTERNAL) {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_INTERNAL_NODE_RECORDS, 1);
        }
        else {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_LEAF_NODE_RECORDS, 1);
        }

        const float objectUniformScale = MaxAxisScale_RowVector(objectModelMatrix);
        const float cullUniformScale = objectUniformScale;
        const float lodUniformScale = objectUniformScale;
        const float3 nodeCullCenterObjectSpace = isSkinned ? instanceData.boundingSphere.sphere.xyz : node.metric.cullCenterAndRadius.xyz;
        const float nodeCullRadiusObjectSpace = isSkinned ? instanceData.boundingSphere.sphere.w : node.metric.cullCenterAndRadius.w;
        const float3 nodeLodCenterObjectSpace = node.metric.lodCenterAndRadius.xyz;
        const float nodeLodRadiusObjectSpace = node.metric.lodCenterAndRadius.w;
        const float3 nodeCenterViewSpace = ToViewSpace(nodeCullCenterObjectSpace, objectModelMatrix, cullCamera.view);
        const float nodeRadiusWorld = nodeCullRadiusObjectSpace * cullUniformScale;
        const bool nodeCulled =
            CLodWorkGraphFrustumCullingEnabled() &&
            !replaySource &&
            SphereOutsideFrustumViewSpace(nodeCenterViewSpace, nodeRadiusWorld, cullCamera);
    #if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        const bool objectInvalidatedThisFrame = CLodVirtualShadowInstanceInvalidatedThisFrame(rec.instanceIndex);
        const bool dirtyPageCullingEnabled = CLodWorkGraphShadowDirtyPageCullingEnabled() && !objectInvalidatedThisFrame;
    #endif

        if (nodeCulled) {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_CULLED_NODE_RECORDS, 1);
        }
        else {
            // LOD pre-filter: for segment-leaves, check if the own group's
            // error-over-distance exceeds threshold (condition 1 of the
            // meshoptimizer rendering rule).  Uses the actual group sphere
            // for accuracy; the BVH node sphere is only for frustum culling.
            // For internal nodes, the BVH node sphere and propagated max
            // error provide a conservative bound.

            if (node.range.isLeaf != CLOD_NODE_INTERNAL) {
                bool nodeTouchesDirtyPages = true;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                if (dirtyPageCullingEnabled)
                {
                    const float3 nodeCullCenterWorld = mul(float4(nodeCullCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
                    nodeTouchesDirtyPages = CLodVirtualShadowBoundsTouchDirtyPages(nodeCullCenterWorld, nodeRadiusWorld, rec.viewId);
                }
#endif

                CLodRenderableLeaf leaf;
                if (CLodPrepareRenderableLeaf(
                    clodMeshMetadata,
                    node,
                    parentAllowsRefine,
                    objectModelMatrix,
                    lodUniformScale,
                    lodCam,
                    lodCamera.isOrtho,
                    nodeTouchesDirtyPages,
                    forceLodDecision,
                    rec.instanceIndex,
                    instanceData.perMeshBufferIndex,
                    rec.viewId,
                    leaf))
                {
                    if (!forceLodDecision && CLodRefinedChildSuppressesParent(
                        clodMeshMetadata.groupsBase,
                        node.range.countMinusOne - 1u,
                        node.range.countMinusOne != 0u,
                        objectModelMatrix,
                        lodUniformScale,
                        lodCam,
                        lodCamera.isOrtho,
                        rec.instanceIndex,
                        instanceData.perMeshBufferIndex,
                        rec.viewId,
                        leaf.errorOverDistance,
                        0.0f.xxx,
                        -1.0f,
                        false))
                    {
                        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CONDITION2, 1);
                    }
                    else if (!leaf.canRender)
                    {
                        // The group was requested above; keep traversal/request side effects
                        // but do not emit render work until the page data is resident.
                    }
                    else if (leaf.isVoxel)
                    {
                        const float voxelRepresentationError = leaf.group.representationError > 0.0f ? leaf.group.representationError : leaf.group.bounds.error;
                        const float voxelRepresentationErrorOverDistance = ProjectedGeometricError(
                            mul(float4(leaf.group.bounds.centerAndRadius.xyz, 1.0f), objectModelMatrix).xyz,
                            leaf.group.bounds.centerAndRadius.w * lodUniformScale,
                            voxelRepresentationError,
                            lodUniformScale,
                            lodCam.positionWorldSpace.xyz,
                            lodCam.zNear,
                            lodCamera.isOrtho);
                        const bool voxelRepresentationAcceptable = forceLodDecision || voxelRepresentationErrorOverDistance <= lodCam.errorOverDistanceThreshold;

                        StructuredBuffer<ClusterLODGroupSegment> segments =
                            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
                        const uint segGlobalIndex = clodMeshMetadata.segmentsBase + node.range.indexOrOffset;
                        const ClusterLODGroupSegment seg = segments[segGlobalIndex];
                        CLodVoxelGroupDescriptor voxelDescriptor;
                        if (!voxelRepresentationAcceptable)
                        {
                            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_REJECTED_BY_ERROR_RECORDS, 1);
                        }
                        else if (CLodTryLoadVoxelDescriptorForSegment(clodMeshMetadata, leaf.group, seg, voxelDescriptor))
                        {
                            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_DESCRIPTOR_HITS, 1);
                            CLodAppendVoxelRasterClusterWork(
                                clodMeshMetadata,
                                rec.instanceIndex,
                                rec.viewId,
                                node.range.ownerGroupId,
                                leaf.group,
                                seg,
                                voxelDescriptor,
                                objectModelMatrix,
                                lodUniformScale,
                                cullCamera,
                                lodCam,
                                lodCamera.isOrtho,
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                                dirtyPageCullingEnabled,
#else
                                false,
#endif
                                instanceData.perMeshBufferIndex,
                                leaf.errorOverDistance);
                        }
                        else
                        {
                            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_DESCRIPTOR_MISSES, 1);
                        }
                    }
                    else
                    {
                        // Final emit for triangle meshlet payloads.
                        StructuredBuffer<ClusterLODGroupSegment> segments =
                            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
                        const uint segGlobalIndex = clodMeshMetadata.segmentsBase + node.range.indexOrOffset;
                        const ClusterLODGroupSegment seg = segments[segGlobalIndex];

                        emitBucket = (seg.meshletCount != 0);

                        if (emitBucket) {
                            WGTelemetryAdd(WG_COUNTER_SEGMENT_EVALUATE_EMIT_BUCKET_THREADS, 1);
                            emittedSegmentMeshletCount = seg.meshletCount;

                            const GroupPageMapEntry pageEntry = LoadGroupPageMapEntry(clodMeshMetadata.pageMapBase, seg.pageIndex);
                            if (pageEntry.slabDescriptorIndex == 0u)
                            {
                                WGTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB, 1);
                                return;
                            }

                            bucketRecord.instanceIndex = rec.instanceIndex;
                            bucketRecord.viewId = rec.viewId;
                            bucketRecord.groupIdPacked = PackGroupId(node.range.ownerGroupId, UnpackSourceTag(rec.nodeIdPacked));
                            bucketRecord.meshletIndexAndCount = PackMeshletIndexAndCount(seg.firstMeshletInPage, 0); // count set per-record below
                            bucketRecord.pageSlabDescriptorIndex = pageEntry.slabDescriptorIndex;
                            bucketRecord.pageSlabByteOffset = pageEntry.slabByteOffset;

                            // Decompose meshlet count into bucket-sized records (max 8 records)
                            uint tail = seg.meshletCount;
                            uint budget = MAX_RECORDS_PER_SEGMENT;

                            n64 = min(tail / 64, budget);
                            tail -= n64 * 64;
                            budget -= n64;

                            if (tail >= 32 && budget >= 2) { n32 = 1; tail -= 32; budget--; }
                            if (tail >= 16 && budget >= 2) { n16 = 1; tail -= 16; budget--; }
                            if (tail >= 8  && budget >= 2) { n8  = 1; tail -= 8;  budget--; }
                            if (tail >= 4  && budget >= 2) { n4  = 1; tail -= 4;  budget--; }
                            if (tail >= 2  && budget >= 2) { n2  = 1; tail -= 2;  budget--; }

                            if      (tail > 32) { n64++; }
                            else if (tail > 16) { n32++; }
                            else if (tail > 8)  { n16++; }
                            else if (tail > 4)  { n8++;  }
                            else if (tail > 2)  { n4++;  }
                            else if (tail > 1)  { n2++;  }
                            else if (tail > 0)  { n1 = 1; }
                        }
                    }
                }
            }
            else {
                // Internal node: LOD check + occlusion + child emission.
                const float3 lodCheckWorldCenter = mul(float4(nodeLodCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
                const float lodCheckWorldRadius = nodeLodRadiusObjectSpace * lodUniformScale;
                const float nodeErrorOverDistance = ProjectedGeometricError(
                    lodCheckWorldCenter,
                    lodCheckWorldRadius,
                    node.metric.maxQuadricError,
                    lodUniformScale,
                    lodCam.positionWorldSpace.xyz,
                    lodCam.zNear,
                    lodCamera.isOrtho);
                const bool nodeWantsTraversal =
                    forceLodDecision ||
                    (parentAllowsRefine && (nodeErrorOverDistance >= lodCam.errorOverDistanceThreshold));

                if (!nodeWantsTraversal) {
                    WGTelemetryAdd(WG_COUNTER_TRAVERSE_REJECTED_BY_ERROR_RECORDS, 1);
                }
                else {
                    bool nodeTouchesDirtyPages = true;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                    if (dirtyPageCullingEnabled)
                    {
                        const float3 nodeCullCenterWorld = mul(float4(nodeCullCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
                        nodeTouchesDirtyPages = CLodVirtualShadowBoundsTouchDirtyPages(nodeCullCenterWorld, nodeRadiusWorld, rec.viewId);
                    }
#endif

                    if (!nodeTouchesDirtyPages)
                    {
                        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES, 1);
                    }
                    else {
                        bool occlusionCulled = false;
                        if (CLodWorkGraphOcclusionEnabled() && (!cullCamera.isOrtho || CLOD_VSM_OCCLUSION_CULLING)) {
                            StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
                                ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
                            const uint depthMapDescriptorIndex = viewDepthSRVIndices[cullViewId].linearDepthSRVIndex;
                            if (depthMapDescriptorIndex != 0) {
                                if (replaySource) {
                                    // Phase 2 replay: HZB is from this frame's Phase 1 depth,
                                    // so test current-frame bounding spheres.
                                    OcclusionCullingPerspectiveTexture2D(
                                        occlusionCulled,
                                        cullCamera,
                                        nodeCenterViewSpace,
                                        -nodeCenterViewSpace.z,
                                        nodeRadiusWorld,
                                        depthMapDescriptorIndex);
                                } else {
                                    // Phase 1: HZB is from previous frame's depth,
                                    // so reproject bounding sphere into previous frame's camera space.
                                    const row_major matrix prevModelMatrix = instanceTransform.prevModel;
                                    const float prevNodeCullScale = MaxAxisScale_RowVector(prevModelMatrix);
                                    const float3 prevNodeCenterViewSpace = ToViewSpace(nodeCullCenterObjectSpace, prevModelMatrix, cullCamera.prevView);
                                    const float prevNodeRadiusWorld = nodeCullRadiusObjectSpace * prevNodeCullScale;
                                    OcclusionCullingPerspectiveTexture2D(
                                        occlusionCulled,
                                        cullCamera,
                                        prevNodeCenterViewSpace,
                                        -prevNodeCenterViewSpace.z,
                                        prevNodeRadiusWorld,
                                        depthMapDescriptorIndex,
                                        cullCamera.prevUnjitteredProjection);
                                }
                            }
                        }

                        if (occlusionCulled) {
                            if (!replaySource) {
                                ReplayTryAppendNode(
                                    rec.instanceIndex,
                                    rec.viewId,
                                    UnpackNodeId(rec.nodeIdPacked));
                            }
                        }
                        else {
                            const uint childCount = min(node.range.countMinusOne + 1u, BVH_MAX_CHILDREN);
                            const uint sourceTag = UnpackSourceTag(rec.nodeIdPacked);

                            // Pre-filter children: load each child, frustum cull + LOD check,
                            // and only emit records for survivors.
                            [loop]
                            for (uint childIndex = 0; childIndex < childCount; ++childIndex) {
                                const uint childNodeId = node.range.indexOrOffset + childIndex;
                                const ClusterLODNode child = lodNodes[clodMeshMetadata.lodNodesBase + childNodeId];

                                // Frustum cull child.
                                const float3 childCullCenterOS = isSkinned ? instanceData.boundingSphere.sphere.xyz : child.metric.cullCenterAndRadius.xyz;
                                const float childCullRadiusOS = isSkinned ? instanceData.boundingSphere.sphere.w : child.metric.cullCenterAndRadius.w;
                                const float3 childCenterVS = ToViewSpace(childCullCenterOS, objectModelMatrix, cullCamera.view);
                                const float childRadiusWorld = childCullRadiusOS * cullUniformScale;
                                if (CLodWorkGraphFrustumCullingEnabled() &&
                                    !replaySource &&
                                    SphereOutsideFrustumViewSpace(childCenterVS, childRadiusWorld, cullCamera)) {
                                    WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_FRUSTUM_CULLED, 1);
                                    continue;
                                }

                                // LOD pre-filter for internal children only.
                                // Leaf children use the group sphere for LOD (different from node sphere),
                                // so we skip the LOD check here and let the leaf thread handle it.
                                if (!forceLodDecision && child.range.isLeaf == CLOD_NODE_INTERNAL) {
                                    const float3 childWorldCenter = mul(float4(child.metric.lodCenterAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
                                    const float childLodRadiusWorld = child.metric.lodCenterAndRadius.w * lodUniformScale;
                                    const float childEOD = ProjectedGeometricError(
                                        childWorldCenter, childLodRadiusWorld,
                                        child.metric.maxQuadricError, lodUniformScale,
                                        lodCam.positionWorldSpace.xyz, lodCam.zNear,
                                        lodCamera.isOrtho);
                                    if (childEOD < lodCam.errorOverDistanceThreshold) {
                                        WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_LOD_REJECTED, 1);
                                        continue;
                                    }
                                }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                                if (dirtyPageCullingEnabled) {
                                    const float3 childCullCenterWorld = mul(float4(childCullCenterOS, 1.0f), objectModelMatrix).xyz;
                                    if (!CLodVirtualShadowBoundsTouchDirtyPages(childCullCenterWorld, childRadiusWorld, rec.viewId)) {
                                        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES, 1);
                                        continue;
                                    }
                                }
#endif

                                TraverseNodeRecord childRecord = (TraverseNodeRecord)0;
                                childRecord.instanceIndex = rec.instanceIndex;
                                childRecord.viewId = rec.viewId;
                                childRecord.nodeIdPacked = PackTraverseNodeId(childNodeId, sourceTag, 1u);
                                childRecords[emitTraverseCount] = childRecord;
                                emitTraverseCount++;
                            }
                        }
                    }
                }
            }
        }
    }

    // Allocate output records- all calls must be uniform across threads.
    ThreadNodeOutputRecords<TraverseNodeRecord>  outNodes = TraverseNodes.GetThreadNodeOutputRecords(emitTraverseCount);
    ThreadNodeOutputRecords<MeshletBucketRecord> out64 = ClusterCull64.GetThreadNodeOutputRecords(n64);
    ThreadNodeOutputRecords<MeshletBucketRecord> out32 = ClusterCull32.GetThreadNodeOutputRecords(n32);
    ThreadNodeOutputRecords<MeshletBucketRecord> out16 = ClusterCull16.GetThreadNodeOutputRecords(n16);
    ThreadNodeOutputRecords<MeshletBucketRecord> out8  = ClusterCull8.GetThreadNodeOutputRecords(n8);
    ThreadNodeOutputRecords<MeshletBucketRecord> out4  = ClusterCull4.GetThreadNodeOutputRecords(n4);
    ThreadNodeOutputRecords<MeshletBucketRecord> out2  = ClusterCull2.GetThreadNodeOutputRecords(n2);
    ThreadNodeOutputRecords<MeshletBucketRecord> out1  = ClusterCull1.GetThreadNodeOutputRecords(n1);

    if (emitTraverseCount > 0) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_ACTIVE_CHILD_THREADS, emitTraverseCount);
        [unroll]
        for (uint childIndex = 0; childIndex < BVH_MAX_CHILDREN; ++childIndex) {
            if (childIndex >= emitTraverseCount) {
                break;
            }

            outNodes[childIndex] = childRecords[childIndex];
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_TRAVERSE_RECORDS, 1);
        }
    }

    if (emitBucket) {
        uint offset = UnpackMeshletFirstIndex(bucketRecord.meshletIndexAndCount);

        for (uint i = 0; i < n64; i++) {
            MeshletBucketRecord r = bucketRecord;
            r.meshletIndexAndCount = PackMeshletIndexAndCount(offset, 64);
            out64[i] = r;
            offset += 64;
        }
        for (uint i32 = 0; i32 < n32; i32++) {
            MeshletBucketRecord r = bucketRecord;
            r.meshletIndexAndCount = PackMeshletIndexAndCount(offset, 32);
            out32[i32] = r;
            offset += 32;
        }
        for (uint i16 = 0; i16 < n16; i16++) {
            MeshletBucketRecord r = bucketRecord;
            r.meshletIndexAndCount = PackMeshletIndexAndCount(offset, 16);
            out16[i16] = r;
            offset += 16;
        }
        for (uint i8 = 0; i8 < n8; i8++) {
            MeshletBucketRecord r = bucketRecord;
            r.meshletIndexAndCount = PackMeshletIndexAndCount(offset, 8);
            out8[i8] = r;
            offset += 8;
        }
        for (uint i4 = 0; i4 < n4; i4++) {
            MeshletBucketRecord r = bucketRecord;
            r.meshletIndexAndCount = PackMeshletIndexAndCount(offset, 4);
            out4[i4] = r;
            offset += 4;
        }
        for (uint i2 = 0; i2 < n2; i2++) {
            MeshletBucketRecord r = bucketRecord;
            r.meshletIndexAndCount = PackMeshletIndexAndCount(offset, 2);
            out2[i2] = r;
            offset += 2;
        }
        for (uint i1 = 0; i1 < n1; i1++) {
            MeshletBucketRecord r = bucketRecord;
            r.meshletIndexAndCount = PackMeshletIndexAndCount(offset, 1);
            out1[i1] = r;
            offset += 1;
        }
    }

    outNodes.OutputComplete();
    out64.OutputComplete();
    out32.OutputComplete();
    out16.OutputComplete();
    out8.OutputComplete();
    out4.OutputComplete();
    out2.OutputComplete();
    out1.OutputComplete();
}
#endif
#define CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP 32

// SW raster batch accumulator (groupshared, per ClusterCull variant)
// Worst case: every meshlet across all 32 threads goes SW.
// CL64 = 32 threads * 64 meshlets = 2048 entries = 8 KB groupshared (within 32 KB limit).
// Output is deferred to a single group-uniform GetGroupNodeOutputRecords call
// after the meshlet loop, satisfying the Work Graphs spec requirement that
// Get*NodeOutputRecords / OutputComplete are not inside varying flow control.
#define SW_BATCH_ACCUM_CAPACITY (CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 64)
#define SW_RASTER_GROUPS_PER_CLUSTER 1

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
groupshared uint gs_swBatchIndices[SW_BATCH_ACCUM_CAPACITY];
#endif

// Page-job batch accumulator (same capacity — worst case identical).
#define PAGEJOB_BATCH_ACCUM_CAPACITY SW_BATCH_ACCUM_CAPACITY
#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
groupshared uint gs_pageJobBatchIndices[PAGEJOB_BATCH_ACCUM_CAPACITY];
#endif

// Shared cluster-cull implementation called by each bucket-size variant.
// FIXED_LOOP_COUNT is the bucket size (1, 2, 4, 8, 16, 32, or 64) - all active lanes
// in a variant wave process the same number of iterations, minimizing WaveActiveMax divergence.
void ClusterCullBody(
    MeshletBucketRecord b,
    bool hasBucket,
    bool countReplayBucketRecord,
    uint GI,
    uint inputCount,
    uint FIXED_LOOP_COUNT,
    out uint swPendingOut,
    out uint pageJobPendingOut)
{
    // Telemetry (coalesced launch level)
    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_THREADS, 1);
    if (hasBucket) {
        const uint bucketMeshletCount = UnpackMeshletCount(b.meshletIndexAndCount);
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_IN_RANGE_THREADS, bucketMeshletCount);
        if (countReplayBucketRecord) {
            WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_BUCKET_RECORDS_DISPATCHED, 1);
        }
        if (countReplayBucketRecord && UnpackGroupSourceTag(b.groupIdPacked) == CLOD_RECORD_SOURCE_REPLAY) {
            WGTelemetryAdd(WG_COUNTER_PHASE2_REPLAY_CLUSTER_BUCKET_RECORDS_CONSUMED, 1);
        }
    }

    const uint4 allLaneMask = WaveActiveBallot(true);
    const uint allLeaderLane = WaveFirstLaneFromMask(allLaneMask);
    const bool isWaveLeader = (WaveGetLaneIndex() == allLeaderLane);
    if (isWaveLeader) {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_WAVES, 1);
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_ACTIVE_LANES, inputCount);
    }

    // Pre-load per-bucket data (loaded once, reused across meshlets).
    // Only the camera fields needed for the hot frustum-culling loop are loaded here
    // (view matrix + 6 clip planes).  Occlusion-specific camera matrices (projection,
    // prevView, prevUnjitteredProjection) and prevModelMatrix are deferred to the
    // occlusion branch to reduce register pressure.
    bool pageValid = false;
    bool replaySource = false;
    row_major matrix objectModelMatrix = (float4x4)0;
    row_major matrix viewMatrix = (float4x4)0;
    float4 frustumPlanes[6];
    uint pageSlabDesc = 0;
    uint pageSlabOff = 0;
    uint pageMeshletCount = 0;
    uint pageDescriptorOffset = 0;
    CLodPageHeader pageHeader = (CLodPageHeader)0;
    uint depthMapDescriptorIndex = 0;
    uint2 depthRes = uint2(0, 0);
    uint numDepthMips = 0;
    float2 hzbUVScale = float2(0, 0);
    float viewHeightPixels = 0.0f;
    float cullUniformScale = 0.0f;
    float lodUniformScale = 0.0f;
    CullingCameraInfo cullCam = (CullingCameraInfo)0;
    CullingCameraInfo lodCam = (CullingCameraInfo)0;
    bool cullCameraIsOrtho = false;
    bool lodCameraIsOrtho = false;
    uint groupsBase = 0;
    uint meshBufferIndex = 0;
    uint activeGroupScanCount = 0;
    float ownGroupErrorOverDistance = 0.0f;
    PerObjectBuffer instanceTransform = (PerObjectBuffer)0;
    bool isSkinned = false;
    bool reyesDisplacementCandidate = false;
    bool isAlphaTestedMaterial = false;
    bool forceLodDecision = false;
    uint skinningInstanceSlot = 0xFFFFFFFFu;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    bool objectInvalidatedThisFrame = false;
#endif

    if (hasBucket && b.pageSlabDescriptorIndex != 0) {
        pageValid = true;
        replaySource = (UnpackGroupSourceTag(b.groupIdPacked) == CLOD_RECORD_SOURCE_REPLAY);
        pageSlabDesc = b.pageSlabDescriptorIndex;
        pageSlabOff = b.pageSlabByteOffset;

        const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(b.instanceIndex);
        const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDrawRecord(drawRecord);
        instanceTransform = LoadInstanceTransformForDrawRecord(drawRecord);
        skinningInstanceSlot = instanceData.skinningInstanceSlot;
        objectModelMatrix = instanceTransform.model;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        objectInvalidatedThisFrame = CLodVirtualShadowInstanceInvalidatedThisFrame(b.instanceIndex);
#endif

        // Load only the camera fields needed for the hot culling loop.
        // Occlusion matrices are deferred to the occlusion branch.
        StructuredBuffer<Camera> cameras =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const uint lodViewId = CLodResolveLodViewId(b.viewId);
        viewMatrix = cameras[b.viewId].view;
        cullCameraIsOrtho = cameras[b.viewId].isOrtho;
        lodCameraIsOrtho = cameras[lodViewId].isOrtho;
        [unroll] for (uint p = 0; p < 6; p++)
            frustumPlanes[p] = cameras[b.viewId].clippingPlanes[p].plane;

        StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer =
            ResourceDescriptorHeap[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
        const ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[b.viewId];
        viewHeightPixels = float(viewRasterInfo.scissorMaxY - viewRasterInfo.scissorMinY);

        // Load the current page header layout through the shared helper.
        ByteAddressBuffer slab = ResourceDescriptorHeap[pageSlabDesc];
        pageHeader = LoadPageHeader(pageSlabDesc, pageSlabOff);
        pageMeshletCount = pageHeader.meshletCount;
        pageDescriptorOffset = pageHeader.descriptorOffset;

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (!cullCameraIsOrtho || CLOD_VSM_OCCLUSION_CULLING) {
            StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
                ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
            depthMapDescriptorIndex = viewDepthSRVIndices[b.viewId].linearDepthSRVIndex;
            depthRes = uint2(cameras[b.viewId].depthResX, cameras[b.viewId].depthResY);
            numDepthMips = cameras[b.viewId].numDepthMips;
            hzbUVScale = cameras[b.viewId].UVScaleToNextPowerOf2;
        }
#endif

        // Per-meshlet condition 2 + streaming fallback state
        const float objectUniformScale = MaxAxisScale_RowVector(objectModelMatrix);
        cullUniformScale = objectUniformScale;
        lodUniformScale = objectUniformScale;
        meshBufferIndex = instanceData.perMeshBufferIndex;
        StructuredBuffer<PerMeshBuffer> perMeshBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshBuffer perMesh = perMeshBuffer[meshBufferIndex];
        isSkinned = (perMesh.vertexFlags & VERTEX_SKINNED) != 0u;
        StructuredBuffer<MaterialInfo> materialDataBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
        const MaterialInfo materialInfo = materialDataBuffer[perMesh.materialDataIndex];
        isAlphaTestedMaterial = (materialInfo.materialFlags & MATERIAL_ALPHA_TEST) != 0u;
        const bool displacementEnabled = materialInfo.geometricDisplacementEnabled != 0u;
        const float displacementSpan = max(0.0f, materialInfo.geometricDisplacementMax - materialInfo.geometricDisplacementMin);
        reyesDisplacementCandidate = displacementEnabled && displacementSpan > 1e-5f;
        StructuredBuffer<CullingCameraInfo> cameraInfos =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
        cullCam = cameraInfos[b.viewId];
        lodCam = cameraInfos[lodViewId];

        StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
        const MeshInstanceClodOffsets clodOff = LoadCLodOffsetsForDrawRecord(drawRecord);
        const CLodMeshMetadata clodMeshMetadata = clodMeshMetadataBuffer[clodOff.clodMeshMetadataIndex];
        groupsBase = clodMeshMetadata.groupsBase;
        forceLodDecision = CLodForcedTraversalDepthRootEnabled(clodMeshMetadata);

        // Own group EOD for streaming request priority
        {
            StructuredBuffer<ClusterLODGroup> groups =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
            const ClusterLODGroup ownGrp = groups[groupsBase + UnpackGroupId(b.groupIdPacked)];
            const float3 ownWorldCenter = mul(float4(ownGrp.bounds.centerAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
            const float ownWorldRadius = ownGrp.bounds.centerAndRadius.w * lodUniformScale;
            ownGroupErrorOverDistance = ProjectedGeometricError(
                ownWorldCenter, ownWorldRadius, ownGrp.bounds.error, lodUniformScale,
                lodCam.positionWorldSpace.xyz, lodCam.zNear,
                lodCameraIsOrtho);
        }

        StructuredBuffer<CLodStreamingRuntimeState> runtimeState =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingRuntimeState)];
        activeGroupScanCount = runtimeState[0].activeGroupScanCount;
    }

    // Meshlet loop - fixed iteration count eliminates WaveActiveMax divergence.
    // Lanes with fewer meshlets (e.g. replay count=1) simply skip inactive iterations.
    const uint meshletCount = hasBucket ? UnpackMeshletCount(b.meshletIndexAndCount) : 0;

    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> visibleClusterCounter =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState =
        ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    const uint visibleClusterCapacity = CLOD_WG_VISIBLE_CLUSTERS_CAPACITY;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    RWTexture2DArray<uint> shadowPageTable = ResourceDescriptorHeap[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX];
#endif

    // Phase 2: read Phase 1's final HW count to offset writes and avoid overwriting Phase 1 entries.
    // Always bind the resource to avoid DXC ICE with conditional ResourceDescriptorHeap casts.
    StructuredBuffer<uint> phase1HWBaseCounter = ResourceDescriptorHeap[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX];
    const uint phase1HWBase = CLodWorkGraphIsPhase2() ? phase1HWBaseCounter.Load(0) : 0u;

#if CLOD_WG_ENABLE_SW_CLASSIFICATION
    // SW raster classification setup.
    const bool swRasterEnabled = CLodWorkGraphSWRasterEnabled();
    const float swDiameterThreshold = CLodSWRasterDiameterThreshold();
    RWStructuredBuffer<uint> swVisibleClusterCounter =
        ResourceDescriptorHeap[CLOD_WG_SW_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> swWriteBaseCounter = ResourceDescriptorHeap[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX];
    const uint swWriteBase = CLodWorkGraphIsPhase2() ? swWriteBaseCounter.Load(0) : 0u;
    const uint phase1SWBase = swWriteBase;
    const bool useDedicatedComputePageJobBuffer = CLodWorkGraphUseDedicatedComputePageJobBuffer();
    // Page-job classification setup.
    const bool pageJobEnabled = CLodPageJobEnabled();
    const float pageJobDiameterThreshold = (float)CLodPageJobDiameterThreshold();
    const bool pageJobForceAll = CLodPageJobForceAll();
#else
    StructuredBuffer<uint> phase1SWBaseCounter = ResourceDescriptorHeap[CLOD_WG_SW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX];
    const uint phase1SWBase = CLodWorkGraphIsPhase2() ? phase1SWBaseCounter.Load(0) : 0u;
#endif

    const uint sharedVisibleClusterWriteCapacity = CLodWorkGraphSharedVisibleClusterWriteCapacity(
        visibleClusterCapacity,
        phase1HWBase,
        phase1SWBase);
    const uint hwVisibleClusterWriteCapacity = sharedVisibleClusterWriteCapacity;
#if CLOD_WG_ENABLE_SW_CLASSIFICATION
    const uint swVisibleClusterWriteCapacity = sharedVisibleClusterWriteCapacity;
#endif

    uint totalSurvivors = 0;
    uint swPending = 0; // SW batch accumulator count (wave-uniform)
    uint pageJobPending = 0; // Page-job batch accumulator count (wave-uniform)

    for (uint m = 0; m < FIXED_LOOP_COUNT; m++) {
        const bool active = (m < meshletCount) && pageValid;
        uint localMeshletIndex = 0;
        bool survives = false;
        float3 meshletCenterViewSpace = float3(0, 0, -1); // default: behind camera
        float3 meshletCenterWorld = 0.0f.xxx;
        float meshletRadiusWorld = 0.0f;

        if (active) {
            const uint localMeshlet = UnpackMeshletFirstIndex(b.meshletIndexAndCount) + m;

            if (localMeshlet < pageMeshletCount) {
                localMeshletIndex = localMeshlet;

                // Load per-meshlet descriptor (5 x Load4 = 80 bytes)
                CLodMeshletDescriptor desc = LoadMeshletDescriptor(pageSlabDesc, pageSlabOff, pageDescriptorOffset, localMeshlet);
                BoundingSphere meshletBounds = { desc.bounds };
                if (isSkinned)
                {
                    meshletBounds = ComputeSkinnedMeshletBounds(
                        desc,
                        pageHeader,
                        pageSlabDesc,
                        pageSlabOff,
                        skinningInstanceSlot);
                }
                meshletCenterViewSpace = ToViewSpace(meshletBounds.sphere.xyz, objectModelMatrix, viewMatrix);
                meshletCenterWorld = mul(float4(meshletBounds.sphere.xyz, 1.0f), objectModelMatrix).xyz;
                meshletRadiusWorld = meshletBounds.sphere.w * cullUniformScale;
                survives =
                    !CLodWorkGraphFrustumCullingEnabled() ||
                    replaySource ||
                    !SphereOutsideFrustumViewSpace(meshletCenterViewSpace, meshletRadiusWorld, frustumPlanes);
                
                if (!survives) {
                    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_FRUSTUM, 1);
                }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                if (survives && !objectInvalidatedThisFrame)
                {
                    bool touchesDirtyPages = true;
                    if (CLodWorkGraphShadowDirtyPageCullingEnabled())
                    {
                        touchesDirtyPages = CLodVirtualShadowMeshletTouchesDirtyPages(meshletCenterWorld, meshletRadiusWorld, b.viewId);
                    }

                    if (!touchesDirtyPages)
                    {
                        survives = false;
                        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES, 1);
                    }
                }
#endif

                // Per-meshlet LOD condition 2: terminal meshlets pass
                // automatically; non-terminal meshlets are suppressed when
                // the refined child boundary is still above the threshold and
                // the refined child is resident.
                if (survives && !forceLodDecision) {
                    const int refinedGroupId = CLodDescRefinedGroupId(desc);
                    if (CLodRefinedChildSuppressesParent(
                        groupsBase,
                        (uint)refinedGroupId,
                        refinedGroupId >= 0,
                        objectModelMatrix,
                        lodUniformScale,
                        lodCam,
                        lodCameraIsOrtho,
                        b.instanceIndex,
                        meshBufferIndex,
                        b.viewId,
                        ownGroupErrorOverDistance,
                        meshletCenterWorld,
                        meshletRadiusWorld,
                        true)) {
                        survives = false;
                        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CONDITION2, 1);
                    }
                }

 #if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                if (survives && CLodWorkGraphOcclusionEnabled() && depthMapDescriptorIndex != 0) {
                    bool occlusionCulled = false;
                    // Load only the occlusion-specific camera matrices when needed,
                    // keeping them out of registers during the main frustum/LOD loop.
                    StructuredBuffer<Camera> occCameras =
                        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
                    if (replaySource) {
                        // Phase 2 replay: HZB is from this frame's Phase 1 depth,
                        // so test current-frame bounding spheres.
                        OcclusionCullingPerspectiveTexture2D(
                            occlusionCulled,
                            depthRes, numDepthMips, hzbUVScale,
                            occCameras[b.viewId].projection,
                            meshletCenterViewSpace,
                            -meshletCenterViewSpace.z,
                            meshletRadiusWorld,
                            depthMapDescriptorIndex);
                    } else {
                        // Phase 1: HZB is from previous frame's depth,
                        // so reproject bounding sphere into previous frame's camera space.
                        const row_major matrix prevModelMatrix = instanceTransform.prevModel;
                        const float prevMeshletScale = MaxAxisScale_RowVector(prevModelMatrix);
                        const float3 prevMeshletCenterViewSpace = ToViewSpace(meshletBounds.sphere.xyz, prevModelMatrix, occCameras[b.viewId].prevView);
                        const float prevMeshletRadiusWorld = meshletBounds.sphere.w * prevMeshletScale;
                        OcclusionCullingPerspectiveTexture2D(
                            occlusionCulled,
                            depthRes, numDepthMips, hzbUVScale,
                            occCameras[b.viewId].prevUnjitteredProjection,
                            prevMeshletCenterViewSpace,
                            -prevMeshletCenterViewSpace.z,
                            prevMeshletRadiusWorld,
                            depthMapDescriptorIndex);
                    }
                    if (occlusionCulled) {
                        if (!replaySource) {
                            ReplayTryAppendMeshlet(
                                b.instanceIndex,
                                b.viewId,
                                UnpackGroupId(b.groupIdPacked),
                                localMeshlet,
                                pageSlabDesc,
                                pageSlabOff);
                        }
                        survives = false;
                        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_OCCLUSION, 1);
                    }
                }
 #endif
            } else {
                WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_PAGE_BOUNDS, 1);
            }
        } else {
            WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_OUT_OF_RANGE, 1);
        }

        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_MESHLET_ITERATIONS, active ? 1 : 0);

        const bool contributes = active && survives;
        const uint visibleGroupId = UnpackGroupId(b.groupIdPacked);
        uint shadowClipmapIndex = CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX;
        CLodVirtualShadowClipmapInfo shadowClipmapInfo = (CLodVirtualShadowClipmapInfo)0;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (contributes)
        {
            if (!CLodVirtualShadowFindClipmapForView(b.viewId, shadowClipmapIndex, shadowClipmapInfo))
            {
                shadowClipmapIndex = CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX;
            }
        }
#endif

#if !CLOD_WG_ENABLE_SW_CLASSIFICATION
        uint hwLaneWriteCount = contributes ? 1u : 0u;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        uint2 hwMinPageCoord = uint2(0u, 0u);
        uint2 hwMaxPageCoord = uint2(0u, 0u);
        uint2 hwMinBlockCoord = uint2(0u, 0u);
        uint2 hwBlockCount = uint2(0u, 0u);
        const bool hwUsesVsmBlocks =
            contributes &&
            shadowClipmapIndex != CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX &&
            CLodVirtualShadowComputeMeshletBlockCoverage(
                meshletCenterWorld,
                meshletRadiusWorld,
                shadowClipmapIndex,
                shadowClipmapInfo,
                hwMinPageCoord,
                hwMaxPageCoord,
                hwMinBlockCoord,
                hwBlockCount);
        if (hwUsesVsmBlocks)
        {
            hwLaneWriteCount = CLodVirtualShadowCountVisibleClusterBlocksForMeshlet(
                shadowClipmapIndex,
                shadowClipmapInfo,
                shadowPageTable,
                hwMinPageCoord,
                hwMaxPageCoord,
                hwMinBlockCoord,
                hwBlockCount);
        }
#endif
        const uint4 hwMask = WaveActiveBallot(hwLaneWriteCount != 0u);
        const uint hwWriteCount = WaveActiveSum(hwLaneWriteCount);
        totalSurvivors += hwWriteCount;

        if (hwWriteCount > 0u) {
            const uint hwLeader = WaveFirstLaneFromMask(hwMask);
            const uint hwPrefix = WavePrefixSum(hwLaneWriteCount);

            uint hwBase = 0u;
            uint hwCombinedBase = 0u;
            if (WaveGetLaneIndex() == hwLeader) {
                InterlockedAdd(replayState[0].visibleClusterCombinedCount, hwWriteCount, hwCombinedBase);
            }
            hwCombinedBase = WaveReadLaneAt(hwCombinedBase, hwLeader);

            const uint hwAvail =
                (hwCombinedBase < hwVisibleClusterWriteCapacity)
                    ? min(hwWriteCount, hwVisibleClusterWriteCapacity - hwCombinedBase)
                    : 0u;

            if (WaveGetLaneIndex() == hwLeader) {
                InterlockedAdd(visibleClusterCounter[0], hwAvail, hwBase);
            }
            hwBase = WaveReadLaneAt(hwBase, hwLeader);

            const uint hwGlobalBase = phase1HWBase + hwBase;
            const uint hwLaneAvail =
                (hwPrefix < hwAvail)
                    ? min(hwLaneWriteCount, hwAvail - hwPrefix)
                    : 0u;

            if (WaveGetLaneIndex() == hwLeader && (hwCombinedBase + hwWriteCount > hwVisibleClusterWriteCapacity)) {
                InterlockedMin(replayState[0].visibleClusterCombinedCount, hwVisibleClusterWriteCapacity);
            }

            if (WaveGetLaneIndex() == hwLeader) {
                WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_VISIBLE_CLUSTER_WRITES, hwAvail);
            }

            if (hwLaneAvail != 0u) {
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                if (hwUsesVsmBlocks)
                {
                    CLodVirtualShadowEmitVisibleClusterBlocksForMeshlet(
                        visibleClusters,
                        hwGlobalBase + hwPrefix,
                        hwLaneAvail,
                        b.viewId,
                        b.instanceIndex,
                        localMeshletIndex,
                        visibleGroupId,
                        b.pageSlabDescriptorIndex,
                        b.pageSlabByteOffset,
                        shadowClipmapIndex,
                        shadowClipmapInfo,
                        shadowPageTable,
                        hwMinPageCoord,
                        hwMaxPageCoord,
                        hwMinBlockCoord,
                        hwBlockCount);
                }
                else
#endif
                {
                    CLodStoreVisibleClusterGloballyCoherent(
                        visibleClusters,
                        hwGlobalBase + hwPrefix,
                        b.viewId,
                        b.instanceIndex,
                        localMeshletIndex,
                        visibleGroupId,
                        b.pageSlabDescriptorIndex,
                        b.pageSlabByteOffset,
                        shadowClipmapIndex);
                }
            }
        }
#else
        // Benchmark mode: bypass all SW/HW classification and SW batch generation.
        // Survivors go straight through the HW path so the work-graph cost excludes
        // the software-raster routing logic itself.
        if (!swRasterEnabled) {
            uint hwLaneWriteCount = contributes ? 1u : 0u;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
            uint2 hwMinPageCoord = uint2(0u, 0u);
            uint2 hwMaxPageCoord = uint2(0u, 0u);
            uint2 hwMinBlockCoord = uint2(0u, 0u);
            uint2 hwBlockCount = uint2(0u, 0u);
            const bool hwUsesVsmBlocks =
                contributes &&
                shadowClipmapIndex != CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX &&
                CLodVirtualShadowComputeMeshletBlockCoverage(
                    meshletCenterWorld,
                    meshletRadiusWorld,
                    shadowClipmapIndex,
                    shadowClipmapInfo,
                    hwMinPageCoord,
                    hwMaxPageCoord,
                    hwMinBlockCoord,
                    hwBlockCount);
            if (hwUsesVsmBlocks)
            {
                hwLaneWriteCount = CLodVirtualShadowCountVisibleClusterBlocksForMeshlet(
                    shadowClipmapIndex,
                    shadowClipmapInfo,
                    shadowPageTable,
                    hwMinPageCoord,
                    hwMaxPageCoord,
                    hwMinBlockCoord,
                    hwBlockCount);
            }
#endif
            const uint4 hwMask = WaveActiveBallot(hwLaneWriteCount != 0u);
            const uint hwWriteCount = WaveActiveSum(hwLaneWriteCount);
            totalSurvivors += hwWriteCount;

            if (hwWriteCount > 0u) {
                const uint hwLeader = WaveFirstLaneFromMask(hwMask);
                const uint hwPrefix = WavePrefixSum(hwLaneWriteCount);

                uint hwBase = 0u;
                uint hwCombinedBase = 0u;
                if (WaveGetLaneIndex() == hwLeader) {
                    InterlockedAdd(replayState[0].visibleClusterCombinedCount, hwWriteCount, hwCombinedBase);
                }
                hwCombinedBase = WaveReadLaneAt(hwCombinedBase, hwLeader);

                const uint hwAvail =
                    (hwCombinedBase < hwVisibleClusterWriteCapacity)
                        ? min(hwWriteCount, hwVisibleClusterWriteCapacity - hwCombinedBase)
                        : 0u;

                if (WaveGetLaneIndex() == hwLeader) {
                    InterlockedAdd(visibleClusterCounter[0], hwAvail, hwBase);
                }
                hwBase = WaveReadLaneAt(hwBase, hwLeader);

                const uint hwGlobalBase = phase1HWBase + hwBase;
                const uint hwLaneAvail =
                    (hwPrefix < hwAvail)
                        ? min(hwLaneWriteCount, hwAvail - hwPrefix)
                        : 0u;

                if (WaveGetLaneIndex() == hwLeader && (hwCombinedBase + hwWriteCount > hwVisibleClusterWriteCapacity)) {
                    InterlockedMin(replayState[0].visibleClusterCombinedCount, hwVisibleClusterWriteCapacity);
                }

                if (WaveGetLaneIndex() == hwLeader) {
                    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_VISIBLE_CLUSTER_WRITES, hwAvail);
                }

                if (hwLaneAvail != 0u) {
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                    if (hwUsesVsmBlocks)
                    {
                        CLodVirtualShadowEmitVisibleClusterBlocksForMeshlet(
                            visibleClusters,
                            hwGlobalBase + hwPrefix,
                            hwLaneAvail,
                            b.viewId,
                            b.instanceIndex,
                            localMeshletIndex,
                            visibleGroupId,
                            b.pageSlabDescriptorIndex,
                            b.pageSlabByteOffset,
                            shadowClipmapIndex,
                            shadowClipmapInfo,
                            shadowPageTable,
                            hwMinPageCoord,
                            hwMaxPageCoord,
                            hwMinBlockCoord,
                            hwBlockCount);
                    }
                    else
#endif
                    {
                        CLodStoreVisibleClusterGloballyCoherent(
                            visibleClusters,
                            hwGlobalBase + hwPrefix,
                            b.viewId,
                            b.instanceIndex,
                            localMeshletIndex,
                            visibleGroupId,
                            b.pageSlabDescriptorIndex,
                            b.pageSlabByteOffset,
                            shadowClipmapIndex);
                    }
                }
            }

            continue;
        }

        // SW/HW/PageJob three-way classification.
        bool isSW = false;
        bool isPageJob = false;
        if (contributes) {
            WGTelemetryAdd(WG_COUNTER_CLASSIFY_CONTRIBUTING, 1);
        }
        if (contributes && !reyesDisplacementCandidate && (swRasterEnabled || pageJobEnabled)) {
            const float projectedDiameter = CLodProjectedDiameterPixels(
                meshletRadiusWorld,
                cullCam.projY,
                viewHeightPixels,
                meshletCenterViewSpace.z,
                cullCam.zNear,
                cullCameraIsOrtho);
            if (swRasterEnabled) {
                isSW = projectedDiameter < swDiameterThreshold;
            } else if (contributes) {
                WGTelemetryAdd(WG_COUNTER_CLASSIFY_SW_DISABLED, 1);
            }
            if (!isSW && pageJobEnabled && !isAlphaTestedMaterial
                && shadowClipmapIndex != CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX) {
                if ((projectedDiameter >= pageJobDiameterThreshold) || pageJobForceAll) {
                    isPageJob = true;
                } else {
                    WGTelemetryAdd(WG_COUNTER_CLASSIFY_PJ_REJECT_BELOW_THRESHOLD, 1);
                }
            } else if (!isSW && contributes) {
                // Diagnose why we didn't enter the page-job gate.
                if (!pageJobEnabled) {
                    WGTelemetryAdd(WG_COUNTER_CLASSIFY_PJ_REJECT_DISABLED, 1);
                } else if (isAlphaTestedMaterial) {
                    WGTelemetryAdd(WG_COUNTER_CLASSIFY_PJ_REJECT_ALPHA_TESTED, 1);
                } else if (shadowClipmapIndex == CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX) {
                    WGTelemetryAdd(WG_COUNTER_CLASSIFY_PJ_REJECT_NO_CLIPMAP_INDEX, 1);
                }
            } else if (isSW && contributes) {
                WGTelemetryAdd(WG_COUNTER_CLASSIFY_PJ_REJECT_ALREADY_SW, 1);
            }
        } else if (contributes && reyesDisplacementCandidate) {
            WGTelemetryAdd(WG_COUNTER_CLASSIFY_PJ_REJECT_REYES_DISPLACEMENT, 1);
        }

        const bool isHW = contributes && !isSW && !isPageJob;
        const bool outputSW = contributes && isSW;
        const bool outputPageJob = contributes && isPageJob;

        if (isHW)       WGTelemetryAdd(WG_COUNTER_CLASSIFY_ROUTED_HW, 1);
        if (outputSW)   WGTelemetryAdd(WG_COUNTER_CLASSIFY_ROUTED_SW, 1);
        if (outputPageJob) WGTelemetryAdd(WG_COUNTER_CLASSIFY_ROUTED_PAGEJOB, 1);

        // HW path: wave-cooperative bottom-up write
        {
            uint hwLaneWriteCount = isHW ? 1u : 0u;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
            uint2 hwMinPageCoord = uint2(0u, 0u);
            uint2 hwMaxPageCoord = uint2(0u, 0u);
            uint2 hwMinBlockCoord = uint2(0u, 0u);
            uint2 hwBlockCount = uint2(0u, 0u);
            const bool hwUsesVsmBlocks =
                isHW &&
                shadowClipmapIndex != CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX &&
                CLodVirtualShadowComputeMeshletBlockCoverage(
                    meshletCenterWorld,
                    meshletRadiusWorld,
                    shadowClipmapIndex,
                    shadowClipmapInfo,
                    hwMinPageCoord,
                    hwMaxPageCoord,
                    hwMinBlockCoord,
                    hwBlockCount);
            if (hwUsesVsmBlocks)
            {
                hwLaneWriteCount = CLodVirtualShadowCountVisibleClusterBlocksForMeshlet(
                    shadowClipmapIndex,
                    shadowClipmapInfo,
                    shadowPageTable,
                    hwMinPageCoord,
                    hwMaxPageCoord,
                    hwMinBlockCoord,
                    hwBlockCount);
            }
#endif
            const uint4 hwMask = WaveActiveBallot(hwLaneWriteCount != 0u);
            const uint hwWriteCount = WaveActiveSum(hwLaneWriteCount);
            totalSurvivors += hwWriteCount;

            if (hwWriteCount > 0u) {
                const uint hwLeader = WaveFirstLaneFromMask(hwMask);
                const uint hwPrefix = WavePrefixSum(hwLaneWriteCount);

                uint hwBase = 0u;
                uint hwCombinedBase = 0u;
                if (WaveGetLaneIndex() == hwLeader) {
                    InterlockedAdd(replayState[0].visibleClusterCombinedCount, hwWriteCount, hwCombinedBase);
                }
                hwCombinedBase = WaveReadLaneAt(hwCombinedBase, hwLeader);

                const uint hwAvail =
                    (hwCombinedBase < hwVisibleClusterWriteCapacity)
                        ? min(hwWriteCount, hwVisibleClusterWriteCapacity - hwCombinedBase)
                        : 0u;

                if (WaveGetLaneIndex() == hwLeader) {
                    InterlockedAdd(visibleClusterCounter[0], hwAvail, hwBase);
                }
                hwBase = WaveReadLaneAt(hwBase, hwLeader);

                const uint hwGlobalBase = phase1HWBase + hwBase;
                const uint hwLaneAvail =
                    (hwPrefix < hwAvail)
                        ? min(hwLaneWriteCount, hwAvail - hwPrefix)
                        : 0u;

                if (WaveGetLaneIndex() == hwLeader && (hwCombinedBase + hwWriteCount > hwVisibleClusterWriteCapacity)) {
                    InterlockedMin(replayState[0].visibleClusterCombinedCount, hwVisibleClusterWriteCapacity);
                }

                if (WaveGetLaneIndex() == hwLeader) {
                    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_VISIBLE_CLUSTER_WRITES, hwAvail);
                }

                if (hwLaneAvail != 0u) {
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                    if (hwUsesVsmBlocks)
                    {
                        CLodVirtualShadowEmitVisibleClusterBlocksForMeshlet(
                            visibleClusters,
                            hwGlobalBase + hwPrefix,
                            hwLaneAvail,
                            b.viewId,
                            b.instanceIndex,
                            localMeshletIndex,
                            visibleGroupId,
                            b.pageSlabDescriptorIndex,
                            b.pageSlabByteOffset,
                            shadowClipmapIndex,
                            shadowClipmapInfo,
                            shadowPageTable,
                            hwMinPageCoord,
                            hwMaxPageCoord,
                            hwMinBlockCoord,
                            hwBlockCount);
                    }
                    else
#endif
                    {
                        CLodStoreVisibleClusterGloballyCoherent(
                            visibleClusters,
                            hwGlobalBase + hwPrefix,
                            b.viewId,
                            b.instanceIndex,
                            localMeshletIndex,
                            visibleGroupId,
                            b.pageSlabDescriptorIndex,
                            b.pageSlabByteOffset,
                            shadowClipmapIndex);
                    }
                }
            }
        }

        // SW path: wave-cooperative top-down write + batch accumulate
        {
            const uint4 swMask = WaveActiveBallot(outputSW);
            const uint swIterCount = CountBits128(swMask);
            totalSurvivors += swIterCount;
            uint swAvail = 0;

            if (swIterCount > 0) {
                const uint swLeader = WaveFirstLaneFromMask(swMask);
                const uint swRank = GetLaneRankInGroup(swMask, WaveGetLaneIndex());

                uint swBase = 0;
                uint swCombinedBase = 0;
                if (WaveGetLaneIndex() == swLeader) {
                    InterlockedAdd(replayState[0].visibleClusterCombinedCount, swIterCount, swCombinedBase);
                }
                swCombinedBase = WaveReadLaneAt(swCombinedBase, swLeader);

                swAvail =
                    (swCombinedBase < swVisibleClusterWriteCapacity)
                        ? min(swIterCount, swVisibleClusterWriteCapacity - swCombinedBase)
                        : 0u;

                if (WaveGetLaneIndex() == swLeader) {
                    InterlockedAdd(swVisibleClusterCounter[0], swAvail, swBase);
                }
                swBase = WaveReadLaneAt(swBase, swLeader);

                if (WaveGetLaneIndex() == swLeader && (swCombinedBase + swIterCount > swVisibleClusterWriteCapacity)) {
                    InterlockedMin(replayState[0].visibleClusterCombinedCount, swVisibleClusterWriteCapacity);
                }

                if (outputSW && (swRank < swAvail)) {
                    // Write visible cluster top-down from the end of the buffer.
                    const uint swIndex = visibleClusterCapacity - 1 - (swWriteBase + swBase + swRank);
                    CLodStoreVisibleClusterGloballyCoherent(
                        visibleClusters,
                        swIndex,
                        b.viewId,
                        b.instanceIndex,
                        localMeshletIndex,
                        visibleGroupId,
                        b.pageSlabDescriptorIndex,
                        b.pageSlabByteOffset,
                        shadowClipmapIndex);

                    // Accumulate index into batch buffer.
#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
                    gs_swBatchIndices[swPending + swRank] = swIndex;
#endif
                }
            }
#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
            swPending += swAvail; // uniform, swAvail derived from wave-uniform values
#endif
        }

        // Page-job path: wave-cooperative top-down write + batch accumulate
        {
            const uint4 pjMask = WaveActiveBallot(outputPageJob);
            const uint pjIterCount = CountBits128(pjMask);
            totalSurvivors += pjIterCount;
            uint pjAvail = 0;

            if (pjIterCount > 0) {
                const uint pjLeader = WaveFirstLaneFromMask(pjMask);
                const uint pjRank = GetLaneRankInGroup(pjMask, WaveGetLaneIndex());

                uint pjBase = 0;
#if CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER
                if (useDedicatedComputePageJobBuffer) {
                    StructuredBuffer<uint4> pageJobDescriptorBuffer =
                        ResourceDescriptorHeap[ResourceDescriptorIndex(CLOD_WG_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER_ID)];
                    const uint2 descriptorPair = pageJobDescriptorBuffer[0].xy;
                    RWStructuredBuffer<uint> pageJobVisibleClusterCounter =
                        ResourceDescriptorHeap[descriptorPair.y];
                    if (WaveGetLaneIndex() == pjLeader) {
                        InterlockedAdd(pageJobVisibleClusterCounter[0], pjIterCount, pjBase);
                    }
                    pjBase = WaveReadLaneAt(pjBase, pjLeader);

                    pjAvail =
                        (pjBase < visibleClusterCapacity)
                            ? min(pjIterCount, visibleClusterCapacity - pjBase)
                            : 0u;

                    if (WaveGetLaneIndex() == pjLeader && (pjBase + pjIterCount > visibleClusterCapacity)) {
                        InterlockedMin(pageJobVisibleClusterCounter[0], visibleClusterCapacity);
                    }

                    if (outputPageJob && (pjRank < pjAvail)) {
                        globallycoherent RWByteAddressBuffer pageJobVisibleClusters =
                            ResourceDescriptorHeap[descriptorPair.x];
                        const uint pjIndex = pjBase + pjRank;
                        CLodStoreVisibleClusterGloballyCoherent(
                            pageJobVisibleClusters,
                            pjIndex,
                            b.viewId,
                            b.instanceIndex,
                            localMeshletIndex,
                            visibleGroupId,
                            b.pageSlabDescriptorIndex,
                            b.pageSlabByteOffset,
                            shadowClipmapIndex);
                    }
                } else
#endif
                {
                    uint pjCombinedBase = 0;
                    if (WaveGetLaneIndex() == pjLeader) {
                        InterlockedAdd(replayState[0].visibleClusterCombinedCount, pjIterCount, pjCombinedBase);
                    }
                    pjCombinedBase = WaveReadLaneAt(pjCombinedBase, pjLeader);

                    pjAvail =
                        (pjCombinedBase < swVisibleClusterWriteCapacity)
                            ? min(pjIterCount, swVisibleClusterWriteCapacity - pjCombinedBase)
                            : 0u;

                    if (WaveGetLaneIndex() == pjLeader) {
                        InterlockedAdd(swVisibleClusterCounter[0], pjAvail, pjBase);
                    }
                    pjBase = WaveReadLaneAt(pjBase, pjLeader);

                    if (WaveGetLaneIndex() == pjLeader && (pjCombinedBase + pjIterCount > swVisibleClusterWriteCapacity)) {
                        InterlockedMin(replayState[0].visibleClusterCombinedCount, swVisibleClusterWriteCapacity);
                    }

                    if (outputPageJob && (pjRank < pjAvail)) {
                        const uint pjIndex = visibleClusterCapacity - 1 - (swWriteBase + pjBase + pjRank);
                        CLodStoreVisibleClusterGloballyCoherent(
                            visibleClusters,
                            pjIndex,
                            b.viewId,
                            b.instanceIndex,
                            localMeshletIndex,
                            visibleGroupId,
                            b.pageSlabDescriptorIndex,
                            b.pageSlabByteOffset,
                            shadowClipmapIndex);

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
                        gs_pageJobBatchIndices[pageJobPending + pjRank] = pjIndex;
#endif
                    }
                }
            }
#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
#if CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER
            pageJobPending += useDedicatedComputePageJobBuffer ? 0u : pjAvail;
#else
            pageJobPending += pjAvail;
#endif
#endif
        }
#endif

    }

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
    // SWRaster re-reads visibleClusters through UAV indirection in the same work graph.
    // The Work Graphs spec requires globallycoherent accesses plus a device-scope
    // barrier before the node invocation request for this producer-consumer pattern.
    Barrier(visibleClusters, DEVICE_SCOPE | GROUP_SYNC);
#endif

    swPendingOut = swPending;
    pageJobPendingOut = pageJobPending;

    if (isWaveLeader) {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_SURVIVING_LANES, totalSurvivors);
        if (totalSurvivors == 0) {
            WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_ZERO_SURVIVOR_WAVES, 1);
        }
    }
}

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
#define CLOD_CLUSTER_CULL_SW_PARAM(MAX_RECORDS) \
    [NodeID("SWRaster")] [MaxRecords(MAX_RECORDS)] NodeOutput<SWRasterBatchRecord> swRasterOutput,

#define CLOD_CLUSTER_CULL_SW_EPILOGUE() \
    GroupMemoryBarrierWithGroupSync(); \
    const uint numBatches = CLodWorkGraphUseComputeSWRaster() ? 0u : ((swPending + SW_BATCH_MAX_CLUSTERS - 1) / SW_BATCH_MAX_CLUSTERS); \
    GroupNodeOutputRecords<SWRasterBatchRecord> swBatchOut = \
        swRasterOutput.GetGroupNodeOutputRecords(numBatches); \
    if (GI == 0) { \
        for (uint batch = 0; batch < numBatches; batch++) { \
            const uint batchStart = batch * SW_BATCH_MAX_CLUSTERS; \
            const uint batchSize = min(SW_BATCH_MAX_CLUSTERS, swPending - batchStart); \
            swBatchOut[batch].dispatchGrid = uint3(SW_RASTER_GROUPS_PER_CLUSTER * batchSize, 1, 1); \
            swBatchOut[batch].numClusters = batchSize; \
            for (uint i = 0; i < batchSize; i++) \
                swBatchOut[batch].clusterIndices[i] = gs_swBatchIndices[batchStart + i]; \
        } \
    } \
    swBatchOut.OutputComplete()
#else
#define CLOD_CLUSTER_CULL_SW_PARAM(MAX_RECORDS)
#define CLOD_CLUSTER_CULL_SW_EPILOGUE()
#endif

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
#define CLOD_CLUSTER_CULL_PAGEJOB_PARAM(MAX_RECORDS) \
    [NodeID("PageJobBuild")] [AllowSparseNodes] [MaxRecordsSharedWith(swRasterOutput)] NodeOutput<PageJobBuildBatchRecord> pageJobOutput,

#define CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE() \
    GroupMemoryBarrierWithGroupSync(); \
    const uint pjNumBatches = CLodWorkGraphUseComputeSWRaster() ? 0u : ((pageJobPending + PAGEJOB_BUILD_MAX_CLUSTERS - 1) / PAGEJOB_BUILD_MAX_CLUSTERS); \
    GroupNodeOutputRecords<PageJobBuildBatchRecord> pjBatchOut = \
        pageJobOutput.GetGroupNodeOutputRecords(pjNumBatches); \
    if (GI == 0) { \
        for (uint pjBatch = 0; pjBatch < pjNumBatches; pjBatch++) { \
            const uint pjBatchStart = pjBatch * PAGEJOB_BUILD_MAX_CLUSTERS; \
            const uint pjBatchSize = min(PAGEJOB_BUILD_MAX_CLUSTERS, pageJobPending - pjBatchStart); \
            pjBatchOut[pjBatch].dispatchGrid = uint3(pjBatchSize, 1, 1); \
            pjBatchOut[pjBatch].numClusters = pjBatchSize; \
            for (uint pji = 0; pji < pjBatchSize; pji++) \
                pjBatchOut[pjBatch].clusterIndices[pji] = gs_pageJobBatchIndices[pjBatchStart + pji]; \
        } \
    } \
    pjBatchOut.OutputComplete()
#else
#define CLOD_CLUSTER_CULL_PAGEJOB_PARAM(MAX_RECORDS)
#define CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE()
#endif

// ClusterCull variant entry points - one per bucket size.
// Each variant processes a fixed number of meshlets per lane, eliminating wave divergence.

#ifndef CLOD_COMPUTE_INCLUDE_ONLY
[Shader("node")]
[NodeID("ClusterCull1")]
[NodeLaunch("coalescing")]
[NodeIsProgramEntry]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull1(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<MeshletBucketRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 1 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 1 / PAGEJOB_BUILD_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    MeshletBucketRecord b = (MeshletBucketRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 1, swPending, pageJobPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull2")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull2(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<MeshletBucketRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 2 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 2 / PAGEJOB_BUILD_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    MeshletBucketRecord b = (MeshletBucketRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 2, swPending, pageJobPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull4")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull4(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<MeshletBucketRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 4 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 4 / PAGEJOB_BUILD_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    MeshletBucketRecord b = (MeshletBucketRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 4, swPending, pageJobPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull8")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull8(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<MeshletBucketRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 8 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 8 / PAGEJOB_BUILD_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    MeshletBucketRecord b = (MeshletBucketRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 8, swPending, pageJobPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull16")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull16(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<MeshletBucketRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 16 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 16 / PAGEJOB_BUILD_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    MeshletBucketRecord b = (MeshletBucketRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 16, swPending, pageJobPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull32")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull32(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<MeshletBucketRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 32 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 32 / PAGEJOB_BUILD_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    MeshletBucketRecord b = (MeshletBucketRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 32, swPending, pageJobPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull64")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull64(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<MeshletBucketRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 64 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 64 / PAGEJOB_BUILD_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    MeshletBucketRecord b = (MeshletBucketRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 64, swPending, pageJobPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
}

#endif

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
#define CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST 1
#include "ClusterLOD/softwareRaster.hlsl"
#undef CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#include "ClusterLOD/pageJobRaster.hlsl"
#endif
#endif
