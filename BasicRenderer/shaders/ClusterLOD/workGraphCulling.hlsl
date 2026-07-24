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
#include "include/clodReyesTransition.hlsli"
#include "include/reyesPatchCommon.hlsli"

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

#ifndef CLOD_WG_SPLIT_LEAF_NODE
#define CLOD_WG_SPLIT_LEAF_NODE 0
#endif

#ifndef CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER
#define CLOD_WG_ENABLE_COMPUTE_PAGE_JOB_DESCRIPTOR_BUFFER 0
#endif

#ifndef CLOD_WG_ENABLE_REYES_VISIBILITY
#define CLOD_WG_ENABLE_REYES_VISIBILITY 0
#endif

#ifndef CLOD_WG_ENABLE_VOXEL_OUTPUT
#define CLOD_WG_ENABLE_VOXEL_OUTPUT 1
#endif

#ifndef CLOD_WG_RIGID_ONLY
#define CLOD_WG_RIGID_ONLY 0
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

static const uint CLOD_NODE_CULL_STATIC_BIND_POSE = 0u;
static const uint CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS = 1u;
static const uint CLOD_NODE_CULL_OVERFLOW_FALLBACK = 2u;
static const uint CLOD_NODE_CULL_ASSEMBLY_FALLBACK = 3u;
static const uint CLOD_NODE_CULL_INVALID_FALLBACK = 4u;

static const uint CLOD_MESHLET_BOUNDS_STATIC = 0u;
static const uint CLOD_MESHLET_BOUNDS_SKINNED_LIVE = 1u;
static const uint CLOD_MESHLET_BOUNDS_SKINNED_INVALID_SLOT_FALLBACK = 2u;
static const uint CLOD_MESHLET_BOUNDS_SKINNED_NO_VALID_BONE_FALLBACK = 3u;

void CLodTelemetryExplicitNodeBoneCount(uint boneCount);

uint CLodResolveAnimatedNodeCullSphereForPose(
    uint nodeLocalId,
    float4 bindSphere,
    CLodMeshMetadata metadata,
    uint drawRecordIndex,
    uint sourceSkinningInstanceSlot,
    uint assemblyTransformIndex,
    bool previousPose,
    out BoundingSphere resolvedBounds)
{
    resolvedBounds.sphere = bindSphere;
    if (nodeLocalId >= metadata.nodeSkinningInfoCount || metadata.nodeSkinningInfoCount == 0u)
        return CLOD_NODE_CULL_INVALID_FALLBACK;

    StructuredBuffer<ClusterLODNodeSkinningInfo> nodeInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::NodeSkinningInfos)];
	uint nodeInfoDimension = 0u;
	uint nodeInfoStride = 0u;
	nodeInfos.GetDimensions(nodeInfoDimension, nodeInfoStride);
	if (metadata.nodeSkinningInfoBase > nodeInfoDimension ||
		nodeLocalId >= nodeInfoDimension - metadata.nodeSkinningInfoBase)
		return CLOD_NODE_CULL_INVALID_FALLBACK;
    const ClusterLODNodeSkinningInfo nodeInfo = nodeInfos[metadata.nodeSkinningInfoBase + nodeLocalId];
    const uint boneCount = CLodNodeBoneCount(nodeInfo);
    const uint flags = CLodNodeSkinningFlags(nodeInfo);
	const uint validFlags = CLOD_NODE_SKINNING_FLAG_OVERFLOW | CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
	if ((flags & ~validFlags) != 0u || flags == validFlags || (flags != 0u && boneCount != 0u))
		return CLOD_NODE_CULL_INVALID_FALLBACK;
    if ((flags & CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK) != 0u)
        return CLOD_NODE_CULL_ASSEMBLY_FALLBACK;
    if ((flags & CLOD_NODE_SKINNING_FLAG_OVERFLOW) != 0u)
        return CLOD_NODE_CULL_OVERFLOW_FALLBACK;
    if (boneCount == 0u)
        return CLOD_NODE_CULL_STATIC_BIND_POSE;
    const uint skinningInstanceSlot =
        ResolveAssemblyProceduralWindSkinningSlot(
            drawRecordIndex, sourceSkinningInstanceSlot, assemblyTransformIndex);
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot) ||
        boneCount > min(metadata.nodeBoneLimit, CLOD_NODE_BONE_LIMIT_HARD_MAX) ||
        nodeInfo.boneListOffset > metadata.nodeBoneIndexCount ||
        boneCount > metadata.nodeBoneIndexCount - nodeInfo.boneListOffset)
        return CLOD_NODE_CULL_INVALID_FALLBACK;

    StructuredBuffer<uint> nodeBoneIndices =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::NodeBoneIndices)];
	uint nodeBoneDimension = 0u;
	uint nodeBoneStride = 0u;
	nodeBoneIndices.GetDimensions(nodeBoneDimension, nodeBoneStride);
	const uint globalBoneListOffset = metadata.nodeBoneIndexBase + nodeInfo.boneListOffset;
	if (globalBoneListOffset < metadata.nodeBoneIndexBase ||
		globalBoneListOffset > nodeBoneDimension || boneCount > nodeBoneDimension - globalBoneListOffset)
		return CLOD_NODE_CULL_INVALID_FALLBACK;
	StructuredBuffer<SkinningInstanceGPUInfo> skinningInfos =
		ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
	uint skinningInfoDimension = 0u;
	uint skinningInfoStride = 0u;
	skinningInfos.GetDimensions(skinningInfoDimension, skinningInfoStride);
	if (skinningInstanceSlot >= skinningInfoDimension)
		return CLOD_NODE_CULL_INVALID_FALLBACK;
    const SkinningInstanceGPUInfo skinningInfo = LoadSkinningInstanceInfo(skinningInstanceSlot);
	StructuredBuffer<SkinningMatrix> boneMatrices =
		ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];
	uint boneMatrixDimension = 0u;
	uint boneMatrixStride = 0u;
	boneMatrices.GetDimensions(boneMatrixDimension, boneMatrixStride);
	if (assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL &&
		(assemblyTransformIndex < metadata.assemblyTransformBase ||
		 assemblyTransformIndex - metadata.assemblyTransformBase >= metadata.assemblyTransformCount))
		return CLOD_NODE_CULL_INVALID_FALLBACK;
	ClusterLODAssemblyBoneRemap activeBoneRemap = (ClusterLODAssemblyBoneRemap)0;
	bool hasActiveBoneRemap = false;
	if (assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL && metadata.assemblyBoneRemapCount != 0u)
	{
		const uint localTransformIndex = assemblyTransformIndex - metadata.assemblyTransformBase;
		if (localTransformIndex >= metadata.assemblyBoneRemapCount)
			return CLOD_NODE_CULL_INVALID_FALLBACK;
		StructuredBuffer<ClusterLODAssemblyBoneRemap> boneRemaps =
			ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemaps)];
		uint boneRemapDimension = 0u;
		uint boneRemapStride = 0u;
		boneRemaps.GetDimensions(boneRemapDimension, boneRemapStride);
		const uint globalRemapIndex = metadata.assemblyBoneRemapBase + localTransformIndex;
		if (globalRemapIndex < metadata.assemblyBoneRemapBase || globalRemapIndex >= boneRemapDimension)
			return CLOD_NODE_CULL_INVALID_FALLBACK;
		activeBoneRemap = boneRemaps[globalRemapIndex];
		hasActiveBoneRemap = activeBoneRemap.remapIndexBase != CLOD_ASSEMBLY_BONE_REMAP_SENTINEL;
	}
	StructuredBuffer<uint> assemblyBoneRemapIndices =
		ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemapIndices)];
	uint assemblyBoneRemapIndexDimension = 0u;
	uint assemblyBoneRemapIndexStride = 0u;
	assemblyBoneRemapIndices.GetDimensions(assemblyBoneRemapIndexDimension, assemblyBoneRemapIndexStride);
    CLodTelemetryExplicitNodeBoneCount(boneCount);
    float3 mergedCenter = 0.0f.xxx;
    float mergedRadius = 0.0f;
    bool hasValidBone = false;
    [loop]
    for (uint bone = 0u; bone < boneCount; ++bone)
    {
		const uint localJoint = nodeBoneIndices[globalBoneListOffset + bone];
		uint jointIndex = localJoint;
		if (hasActiveBoneRemap)
		{
			const uint remapIndex = activeBoneRemap.remapIndexBase + localJoint;
			if (localJoint >= activeBoneRemap.remapIndexCount ||
				remapIndex < activeBoneRemap.remapIndexBase || remapIndex >= assemblyBoneRemapIndexDimension)
				return CLOD_NODE_CULL_INVALID_FALLBACK;
			jointIndex = assemblyBoneRemapIndices[remapIndex];
		}
		const uint compactJointIndex = ResolveSkeletonLodBoneIndex(skinningInfo, jointIndex);
        const uint matrixBase = previousPose
            ? skinningInfo.previousTransformOffsetMatrices
            : skinningInfo.transformOffsetMatrices;
		const uint matrixIndex = matrixBase + compactJointIndex;
		if (compactJointIndex == 0xFFFFFFFFu || jointIndex >= skinningInfo.sourceBoneCount || matrixIndex < matrixBase ||
			matrixIndex >= boneMatrixDimension)
            return CLOD_NODE_CULL_INVALID_FALLBACK;
        const float4x4 boneSkinMatrix = ConvertExpandedSkinMatrixToAssemblyLocal(
            boneMatrices[matrixIndex],
            assemblyTransformIndex);
        const float3 transformedCenter = mul(float4(bindSphere.xyz, 1.0f), boneSkinMatrix).xyz;
        const float transformedRadius = bindSphere.w * SkinningMaxAxisScale_RowVector(boneSkinMatrix);
        if (!all(isfinite(transformedCenter)) || !isfinite(transformedRadius))
            return CLOD_NODE_CULL_INVALID_FALLBACK;
        if (!hasValidBone)
        {
            mergedCenter = transformedCenter;
            mergedRadius = transformedRadius;
            hasValidBone = true;
        }
        else
        {
            CLodMergeBoundingSphere(mergedCenter, mergedRadius, transformedCenter, transformedRadius);
        }
    }
    if (!hasValidBone) return CLOD_NODE_CULL_INVALID_FALLBACK;
    resolvedBounds.sphere = float4(mergedCenter, mergedRadius * (1.0f + 1.0e-5f));
    return CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS;
}

uint CLodResolveAnimatedNodeCullSphere(
    uint nodeLocalId,
    float4 bindSphere,
    CLodMeshMetadata metadata,
    uint drawRecordIndex,
    uint sourceSkinningInstanceSlot,
    uint assemblyTransformIndex,
    out BoundingSphere resolvedBounds)
{
    return CLodResolveAnimatedNodeCullSphereForPose(
        nodeLocalId,
        bindSphere,
        metadata,
        drawRecordIndex,
        sourceSkinningInstanceSlot,
        assemblyTransformIndex,
        false,
        resolvedBounds);
}

bool CLodNodeBoundsSupportOcclusion(uint classification)
{
    return classification == CLOD_NODE_CULL_STATIC_BIND_POSE ||
        classification == CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS;
}

BoundingSphere CLodNodeOcclusionSphere(
    float4 bindSphere,
    BoundingSphere resolvedBounds,
    uint classification)
{
    if (classification == CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS)
    {
        float3 mergedCenter = resolvedBounds.sphere.xyz;
        float mergedRadius = resolvedBounds.sphere.w;
        CLodMergeBoundingSphere(
            mergedCenter,
            mergedRadius,
            bindSphere.xyz,
            bindSphere.w);
        resolvedBounds.sphere = float4(
            mergedCenter,
            mergedRadius * (1.0f + 1.0e-5f));
    }
    return resolvedBounds;
}

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
static const uint WG_COUNTER_TRAVERSE_VOXEL_SEGMENT_PAGE_HITS = 105;
static const uint WG_COUNTER_TRAVERSE_VOXEL_SEGMENT_PAGE_MISSES = 106;
static const uint WG_COUNTER_TRAVERSE_VOXEL_RASTER_WORK_RECORDS = 107;
static const uint WG_COUNTER_TRAVERSE_VOXEL_RASTER_WORK_DROPPED = 108;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB = 122u;
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_STALE_GENERATION = 133u;
static const uint WG_COUNTER_VOXEL_RASTER_WORK_GROUPS = 134u;
static const uint WG_COUNTER_VOXEL_RASTER_INVALID_PACKED_CLUSTER = 135u;
static const uint WG_COUNTER_VOXEL_RASTER_SEGMENT_PAGE_MISSES = 136u;
static const uint WG_COUNTER_VOXEL_RASTER_INVALID_CLUSTER = 137u;
static const uint WG_COUNTER_VOXEL_RASTER_INVALID_VOXEL_WIDTH = 138u;
static const uint WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED = 139u;
static const uint WG_COUNTER_VOXEL_RASTER_SCISSOR_REJECTED = 140u;
static const uint WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED = 141u;
static const uint WG_COUNTER_VOXEL_RASTER_DDA_MISSES = 142u;
static const uint WG_COUNTER_VOXEL_RASTER_VISIBILITY_WRITES = 143u;
static const uint WG_COUNTER_VOXEL_RASTER_PROJECTED_PIXELS = 144u;
static const uint WG_COUNTER_VOXEL_RASTER_QUEUED_PIXELS = 145u;
static const uint WG_COUNTER_VOXEL_RASTER_QUEUE_OVERFLOW = 146u;
static const uint WG_COUNTER_VOXEL_RASTER_NON_POSITIVE_DEPTH = 147u;
static const uint WG_COUNTER_VOXEL_RASTER_VISIBILITY_WINS = 148u;
static const uint WG_COUNTER_VOXEL_RASTER_VISIBILITY_LOSSES = 149u;
static const uint WG_COUNTER_VOXEL_OBJECT_CANDIDATES = 150u;
static const uint WG_COUNTER_VOXEL_OBJECT_FRUSTUM_REJECTED = 151u;
static const uint WG_COUNTER_VOXEL_OBJECT_VISIBLE = 152u;
static const uint WG_COUNTER_VOXEL_OBJECT_TRAVERSE_RECORDS = 153u;
static const uint WG_COUNTER_VOXEL_ROOT_INTERNAL_RECORDS = 154u;
static const uint WG_COUNTER_VOXEL_ROOT_LEAF_RECORDS = 155u;
static const uint WG_COUNTER_ASSEMBLY_INSTANCE_ROOT_RECORDS = 156u;
static const uint WG_COUNTER_ASSEMBLY_PART_INSTANCE_ROOT_RECORDS = 157u;
static const uint WG_COUNTER_ASSEMBLY_VOXEL_LEAF_RECORDS = 158u;
static const uint WG_COUNTER_ASSEMBLY_VOXEL_REJECTED_BY_ERROR_RECORDS = 163u;
static const uint WG_COUNTER_ASSEMBLY_VOXEL_SUPPRESSED_BY_CHILD_RECORDS = 164u;
static const uint WG_COUNTER_ASSEMBLY_VOXEL_NONRESIDENT_RECORDS = 165u;
static const uint WG_COUNTER_ASSEMBLY_VOXEL_RASTER_WORK_RECORDS = 170u;
static const uint WG_COUNTER_ASSEMBLY_PART_TRAVERSAL_RECORDS = 175u;
static const uint WG_COUNTER_ASSEMBLY_PART_VOXEL_LEAF_RECORDS = 176u;
static const uint WG_COUNTER_ASSEMBLY_PART_VOXEL_RASTER_WORK_RECORDS = 177u;
static const uint WG_COUNTER_ASSEMBLY_PART_TRIANGLE_BUCKET_RECORDS = 178u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_EVALUATIONS = 179u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_FRUSTUM_REJECTED = 180u;
static const uint WG_COUNTER_NODE_BOUNDS_OVERFLOW_FALLBACKS = 181u;
static const uint WG_COUNTER_NODE_BOUNDS_ASSEMBLY_FALLBACKS = 182u;
static const uint WG_COUNTER_NODE_BOUNDS_INVALID_FALLBACKS = 183u;
static const uint WG_COUNTER_MESHLET_BOUNDS_SKINNED_LIVE_EVALUATIONS = 184u;
static const uint WG_COUNTER_MESHLET_BOUNDS_SKINNED_INVALID_SLOT_FALLBACKS = 185u;
static const uint WG_COUNTER_MESHLET_BOUNDS_SKINNED_NO_VALID_BONE_FALLBACKS = 186u;
static const uint WG_COUNTER_MESHLET_BOUNDS_SKINNED_FALLBACK_FRUSTUM_REJECTED = 187u;
static const uint WG_COUNTER_TRAVERSE_WAVES = 240u;
static const uint WG_COUNTER_TRAVERSE_ACTIVE_LANES = 241u;
static const uint WG_COUNTER_TRAVERSE_RIGID_LANES = 242u;
static const uint WG_COUNTER_TRAVERSE_SKINNED_LANES = 243u;
static const uint WG_COUNTER_TRAVERSE_INTERNAL_ONLY_WAVES = 244u;
static const uint WG_COUNTER_TRAVERSE_LEAF_ONLY_WAVES = 245u;
static const uint WG_COUNTER_TRAVERSE_MIXED_NODE_TYPE_WAVES = 246u;
static const uint WG_COUNTER_TRAVERSE_RIGID_ONLY_WAVES = 247u;
static const uint WG_COUNTER_TRAVERSE_SKINNED_ONLY_WAVES = 248u;
static const uint WG_COUNTER_TRAVERSE_MIXED_SKINNING_WAVES = 249u;
static const uint WG_COUNTER_TRAVERSE_CHILD_LOOP_NODES = 250u;
static const uint WG_COUNTER_TRAVERSE_CHILD_LOOP_SLOTS = 251u;
static const uint WG_COUNTER_TRAVERSE_CHILD_RECORDS_EMITTED = 252u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT = 253u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_1 = 254u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_2 = 255u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_3_TO_4 = 256u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_5_TO_8 = 257u;
static const uint WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_9_PLUS = 258u;
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
static const uint WG_COUNTER_OBJECT_CULL_REJECTED_OCCLUSION = 57;
static const uint WG_COUNTER_OBJECT_REPLAY_REJECTED_OCCLUSION = 58;
static const uint WG_COUNTER_STREAM_REQUEST_ATTEMPTS = 59;
static const uint WG_COUNTER_STREAM_REQUEST_RANGE_REJECTS = 60;
static const uint WG_COUNTER_STREAM_RESIDENT_HITS = 61;
static const uint WG_COUNTER_STREAM_REQUEST_APPENDS = 62;
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

bool CLodMeshHasVoxelRootGroup(CLodMeshMetadata clodMeshMetadata)
{
    if (clodMeshMetadata.groupChunkTableCount == 0u)
    {
        return false;
    }

    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup firstGroup = groups[clodMeshMetadata.groupsBase];
    return (firstGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
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
    const uint3 descriptorPair = pageJobDescriptorBuffer[0].xyz;
    // Shadow PageJob/Reyes large-cluster routing consumes the dedicated side-channel
    // buffer after culling regardless of whether visibility SW raster runs in compute
    // or work-graph mode. If descriptors are present, keep emitting into that buffer.
    return
        descriptorPair.x != 0xFFFFFFFFu &&
        descriptorPair.y != 0xFFFFFFFFu &&
        descriptorPair.z != 0xFFFFFFFFu;
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
    RWStructuredBuffer<uint> visibleClusterTransformIndices,
    uint writeBase,
    uint maxWriteCount,
    uint assemblyTransformIndex,
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
                    visibleClusterTransformIndices[writeBase] = assemblyTransformIndex;
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
            visibleClusterTransformIndices[writeBase + emittedCount] = assemblyTransformIndex;
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

void CLodTelemetryTraverseWaveLaunch(bool slotActive)
{
    if (!CLodWorkGraphTelemetryEnabled())
    {
        return;
    }

    const uint activeLanes = WaveActiveCountBits(slotActive);
    if (WaveIsFirstLane())
    {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_WAVES, 1u);
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_ACTIVE_LANES, activeLanes);
    }
}

void CLodTelemetryTraverseWaveClassification(bool isInternal, bool isSkinned)
{
    if (!CLodWorkGraphTelemetryEnabled())
    {
        return;
    }

    const uint internalLanes = WaveActiveCountBits(isInternal);
    const uint leafLanes = WaveActiveCountBits(!isInternal);
    const uint skinnedLanes = WaveActiveCountBits(isSkinned);
    const uint rigidLanes = WaveActiveCountBits(!isSkinned);
    if (WaveIsFirstLane())
    {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_RIGID_LANES, rigidLanes);
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_SKINNED_LANES, skinnedLanes);
        WGTelemetryAdd(
            internalLanes != 0u && leafLanes != 0u
                ? WG_COUNTER_TRAVERSE_MIXED_NODE_TYPE_WAVES
                : (internalLanes != 0u
                    ? WG_COUNTER_TRAVERSE_INTERNAL_ONLY_WAVES
                    : WG_COUNTER_TRAVERSE_LEAF_ONLY_WAVES),
            1u);
        WGTelemetryAdd(
            rigidLanes != 0u && skinnedLanes != 0u
                ? WG_COUNTER_TRAVERSE_MIXED_SKINNING_WAVES
                : (skinnedLanes != 0u
                    ? WG_COUNTER_TRAVERSE_SKINNED_ONLY_WAVES
                    : WG_COUNTER_TRAVERSE_RIGID_ONLY_WAVES),
            1u);
    }
}

void CLodTelemetryExplicitNodeBoneCount(uint boneCount)
{
    if (!CLodWorkGraphTelemetryEnabled())
    {
        return;
    }

    WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT, boneCount);
    const uint histogramCounter =
        boneCount == 1u ? WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_1 :
        boneCount == 2u ? WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_2 :
        boneCount <= 4u ? WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_3_TO_4 :
        boneCount <= 8u ? WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_5_TO_8 :
        WG_COUNTER_NODE_BOUNDS_EXPLICIT_BONE_COUNT_9_PLUS;
    WGTelemetryAdd(histogramCounter, 1u);
}

void CLodTelemetryNodeCullClassification(uint classification)
{
    if (classification == CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS)
        WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_EXPLICIT_EVALUATIONS, 1u);
    else if (classification == CLOD_NODE_CULL_OVERFLOW_FALLBACK)
        WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_OVERFLOW_FALLBACKS, 1u);
    else if (classification == CLOD_NODE_CULL_ASSEMBLY_FALLBACK)
        WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_ASSEMBLY_FALLBACKS, 1u);
    else if (classification == CLOD_NODE_CULL_INVALID_FALLBACK)
        WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_INVALID_FALLBACKS, 1u);
}

float ProjectedGeometricError(
    float3 worldCenter,
    float worldRadius,
    float errorMeshSpace,
    float errorScale,
    float4 worldToViewZ,
    float zNear,
    bool isOrtho);

bool SphereOutsideFrustumViewSpace(float3 viewSpaceCenter, float radius, Camera camera);

#if CLOD_WG_ENABLE_VOXEL_OUTPUT
void CLodAppendVoxelRasterClusterWork(
    CLodMeshMetadata clodMeshMetadata,
    uint instanceIndex,
    uint assemblyTransformIndex,
    uint viewId,
    uint localGroupId,
    ClusterLODGroup voxelGroup,
    ClusterLODGroupSegment voxelSegment,
    float4x4 objectModelMatrix,
    float lodUniformScale,
    Camera cullCamera,
    bool clusterDirtyPageCullingEnabled)
{
    if (voxelSegment.meshletCount == 0u)
    {
        return;
    }

    GroupPageMapEntry voxelPageEntry;
    CLodVoxelPageHeader voxelPageHeader;
    if (!CLodTryLoadVoxelPageForSegment(clodMeshMetadata, voxelGroup, voxelSegment, voxelPageEntry, voxelPageHeader))
    {
        WGTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SEGMENT_PAGE_MISSES, 1);
        return;
    }

    const PerMeshInstanceBuffer voxelInstanceData = LoadMeshTemplateForDraw(instanceIndex);
    const uint voxelSkinningInstanceSlot =
        ResolveAssemblyProceduralWindSkinningSlot(
            instanceIndex, voxelInstanceData.skinningInstanceSlot, assemblyTransformIndex);

    StructuredBuffer<CLodVoxelRasterQueueDescriptors> queueDescriptorBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(CLOD_WG_VOXEL_RASTER_QUEUE_DESCRIPTOR_BUFFER_ID)];
    const CLodVoxelRasterQueueDescriptors queueDescriptors = queueDescriptorBuffer[0];
    if (queueDescriptors.rigidWorkRecordsUAVDescriptorIndex == 0xFFFFFFFFu ||
        queueDescriptors.rigidWorkRecordCounterUAVDescriptorIndex == 0xFFFFFFFFu ||
        queueDescriptors.skinnedWorkRecordsUAVDescriptorIndex == 0xFFFFFFFFu ||
        queueDescriptors.skinnedWorkRecordCounterUAVDescriptorIndex == 0xFFFFFFFFu ||
        queueDescriptors.workRecordCapacity == 0u)
    {
        WGTelemetryAdd(WG_COUNTER_VOXEL_RASTER_QUEUE_OVERFLOW, 1);
        return;
    }

    RWStructuredBuffer<CLodVoxelRasterWorkRecord> rigidWorkRecords =
        ResourceDescriptorHeap[queueDescriptors.rigidWorkRecordsUAVDescriptorIndex];
    RWStructuredBuffer<uint> rigidWorkRecordCounter =
        ResourceDescriptorHeap[queueDescriptors.rigidWorkRecordCounterUAVDescriptorIndex];
    RWStructuredBuffer<CLodVoxelRasterWorkRecord> skinnedWorkRecords =
        ResourceDescriptorHeap[queueDescriptors.skinnedWorkRecordsUAVDescriptorIndex];
    RWStructuredBuffer<uint> skinnedWorkRecordCounter =
        ResourceDescriptorHeap[queueDescriptors.skinnedWorkRecordCounterUAVDescriptorIndex];
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
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

    for (uint clusterIndex = 0u; clusterIndex < voxelSegment.meshletCount; ++clusterIndex)
    {
        const uint pageLocalClusterIndex = voxelSegment.firstMeshletInPage + clusterIndex;
        const CLodVoxelClusterRecord voxelCluster = CLodLoadVoxelClusterFromPage(
            voxelPageEntry.slabDescriptorIndex,
            voxelPageEntry.slabByteOffset,
            voxelPageHeader.descriptorOffset,
            pageLocalClusterIndex);
        const CLodClusterCullHeader clusterCullHeader = CLodVoxelCullHeader(voxelCluster);
        const uint clusterPrimitiveCount = clusterCullHeader.primitiveCountAndRefinedGroup & 0xFFFFu;
        if (clusterPrimitiveCount == 0u || clusterPrimitiveCount > CLOD_VOXEL_MAX_CUBES_PER_CLUSTER)
        {
            continue;
        }

        const uint clusterCullFlags = CLodClusterCullFlags(clusterCullHeader);
        const uint clusterBoneCount = CLodClusterCullBoneCount(clusterCullHeader);
        const bool clusterHasSkinnedCubes =
            (clusterCullFlags & CLOD_CLUSTER_CULL_FLAG_ANIMATED) != 0u;
        const bool clusterBoneOverflow =
            (clusterCullFlags & CLOD_CLUSTER_CULL_FLAG_BONE_OVERFLOW) != 0u;
        bool hasConservativeClusterBounds = !clusterHasSkinnedCubes;
        BoundingSphere evaluatedClusterBounds = { clusterCullHeader.bounds };
        if (clusterHasSkinnedCubes && !clusterBoneOverflow && clusterBoneCount != 0u &&
            IsValidSkinningInstanceSlot(voxelSkinningInstanceSlot))
        {
            CLodMeshletDescriptor boundsDescriptor = (CLodMeshletDescriptor)0;
            boundsDescriptor.bounds = clusterCullHeader.bounds;
            boundsDescriptor.boneListOffset = clusterCullHeader.boneListOffset;
            boundsDescriptor.boneCount = clusterCullHeader.kindFlagsAndBoneCount;
            CLodPageHeader boundsPageHeader = (CLodPageHeader)0;
            boundsPageHeader.boneIndexStreamOffset = voxelPageHeader.boneIndexStreamOffset;
            const BoundingSphere liveBounds = CLodComputeSkinnedMeshletBounds(
                boundsDescriptor,
                boundsPageHeader,
                voxelPageEntry.slabDescriptorIndex,
                voxelPageEntry.slabByteOffset,
                voxelSkinningInstanceSlot,
                clodMeshMetadata,
                assemblyTransformIndex);
            if (!all(liveBounds.sphere == clusterCullHeader.bounds))
            {
                evaluatedClusterBounds = liveBounds;
                hasConservativeClusterBounds = true;
                if ((clusterCullFlags & CLOD_CLUSTER_CULL_FLAG_RIGID_COMPONENT) != 0u)
                {
                    float3 mergedCenter = evaluatedClusterBounds.sphere.xyz;
                    float mergedRadius = evaluatedClusterBounds.sphere.w;
                    CLodMergeBoundingSphere(
                        mergedCenter,
                        mergedRadius,
                        clusterCullHeader.bounds.xyz,
                        clusterCullHeader.bounds.w);
                    evaluatedClusterBounds.sphere = float4(mergedCenter, mergedRadius * (1.0f + 1.0e-5f));
                }
            }
        }
        float3 clusterWorldCenter = 0.0f.xxx;
        float clusterWorldRadius = 0.0f;
        bool clusterOutside = false;
        if (hasConservativeClusterBounds || clusterDirtyPageCullingEnabled)
        {
            clusterWorldCenter = mul(float4(evaluatedClusterBounds.sphere.xyz, 1.0f), objectModelMatrix).xyz;
            clusterWorldRadius = evaluatedClusterBounds.sphere.w * lodUniformScale;
        }
        if (hasConservativeClusterBounds && CLodWorkGraphFrustumCullingEnabled())
        {
            const float3 clusterViewCenter = mul(float4(clusterWorldCenter, 1.0f), cullCamera.view).xyz;
            clusterOutside = SphereOutsideFrustumViewSpace(clusterViewCenter, clusterWorldRadius, cullCamera);
        }
        if (clusterOutside)
        {
            WGTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED, 1);
            continue;
        }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (clusterDirtyPageCullingEnabled &&
            !CLodVirtualShadowBoundsTouchDirtyPages(clusterWorldCenter, clusterWorldRadius, viewId))
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
        if (clusterHasSkinnedCubes)
        {
            InterlockedAdd(skinnedWorkRecordCounter[0], 1u, baseSlot);
        }
        else
        {
            InterlockedAdd(rigidWorkRecordCounter[0], 1u, baseSlot);
        }
        if (baseSlot >= queueDescriptors.workRecordCapacity)
        {
            if (clusterHasSkinnedCubes)
            {
                InterlockedMin(skinnedWorkRecordCounter[0], queueDescriptors.workRecordCapacity);
            }
            else
            {
                InterlockedMin(rigidWorkRecordCounter[0], queueDescriptors.workRecordCapacity);
            }
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
            pageLocalClusterIndex & 0x3FFFu,
            localGroupId,
            voxelSegment.pageIndex,
            0u,
            CLodVisibleClusterMarkVoxelPayload(CLodBuildVisibleClusterVsmPayloadFromClipmapIndex(CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX)));
        visibleClusterTransformIndices[visibleClusterIndex] = assemblyTransformIndex;

        CLodVoxelRasterWorkRecord record;
        record.visibleClusterIndex = visibleClusterIndex;
        record.instanceIndex = instanceIndex;
        record.viewId = viewId;
        record.assemblyTransformIndex = assemblyTransformIndex;
        record.skinningInstanceSlot = voxelSkinningInstanceSlot;
        record.slabDescriptorIndex = voxelPageEntry.slabDescriptorIndex;
        record.slabByteOffset = voxelPageEntry.slabByteOffset;
        record.cubeRecordsOffset = voxelPageHeader.cubeRecordsOffset;
        record.firstCube = voxelCluster.firstCube;
        record.cubeCount = voxelCluster.cubeCount;
        record.assemblyTransformBase = clodMeshMetadata.assemblyTransformBase;
        record.assemblyBoneRemapBase = clodMeshMetadata.assemblyBoneRemapBase;
        record.assemblyBoneRemapCount = clodMeshMetadata.assemblyBoneRemapCount;
        record.aabbMinAndVoxelWidth = voxelCluster.aabbMinAndVoxelWidth;
        if (clusterHasSkinnedCubes)
        {
            skinnedWorkRecords[baseSlot] = record;
        }
        else
        {
            rigidWorkRecords[baseSlot] = record;
        }
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
#endif

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

uint PackTraverseNodeId(uint nodeId, uint sourceTag, uint allowRefine, uint boundsTested)
{
    return
        (sourceTag << 31u) |
        (allowRefine << 30u) |
        (boundsTested << 29u) |
        (nodeId & 0x1FFFFFFFu);
}

uint UnpackNodeId(uint packed)       { return packed & 0x1FFFFFFFu; }
uint UnpackSourceTag(uint packed)    { return packed >> 31u; }
uint UnpackAllowRefine(uint packed)  { return (packed >> 30u) & 1u; }
uint UnpackBoundsTested(uint packed) { return (packed >> 29u) & 1u; }

uint PackGroupId(uint groupId, uint sourceTag)
{
    return (sourceTag << 31u) | (groupId & 0x7FFFFFFFu);
}

uint UnpackGroupId(uint packed)        { return packed & 0x7FFFFFFFu; }
uint UnpackGroupSourceTag(uint packed) { return packed >> 31u; }

uint PackClusterIndexAndCount(uint firstIndex, uint count)
{
    return (count << 16u) | (firstIndex & 0xFFFFu);
}

uint UnpackClusterFirstIndex(uint packed) { return packed & 0xFFFFu; }
uint UnpackClusterCount(uint packed)      { return packed >> 16u; }

// Write a TraverseNodeRecord directly to the node replay region.
bool ReplayTryAppendNode(uint instanceIndex, uint viewId, uint nodeId, uint assemblyTransformIndex)
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

    // TraverseNodeRecord layout: instanceIndex, nodeIdPacked, viewId, assemblyTransformIndex.
    const uint byteOffset = slot * CLOD_NODE_REPLAY_STRIDE_BYTES;
    const uint packed = PackTraverseNodeId(nodeId, CLOD_RECORD_SOURCE_REPLAY, 1u, 0u);
    replayBuffer.Store4(byteOffset, uint4(instanceIndex, packed, viewId, assemblyTransformIndex));
    return true;
}

// Write a CLodClusterRunRecord directly to the meshlet replay region.
bool ReplayTryAppendMeshlet(uint instanceIndex, uint viewId, uint groupId, uint localMeshletIndex,
                            uint pageSlabDescriptorIndex, uint pageSlabByteOffset, uint assemblyTransformIndex)
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

    // CLodClusterRunRecord layout: instanceIndex, viewId, groupIdPacked,
    // clusterIndexAndCount, pageSlabDescriptorIndex, pageSlabByteOffset, assemblyTransformIndex, pad.
    const uint byteOffset = CLOD_REPLAY_MESHLET_REGION_OFFSET + slot * CLOD_MESHLET_REPLAY_STRIDE_BYTES;
    replayBuffer.Store4(byteOffset,       uint4(instanceIndex, viewId,
                                                PackGroupId(groupId, CLOD_RECORD_SOURCE_REPLAY),
                                                PackClusterIndexAndCount(localMeshletIndex, 1u)));
    replayBuffer.Store4(byteOffset + 16u, uint4(pageSlabDescriptorIndex, pageSlabByteOffset, assemblyTransformIndex, 0u));
    return true;
}

#if CLOD_WG_ENABLE_REYES_VISIBILITY
void ReplayStoreReyesSplitEntry(RWByteAddressBuffer replayBuffer, uint byteOffset, CLodReyesSplitQueueEntry entry)
{
    replayBuffer.Store4(byteOffset + 0u, uint4(
        entry.visibleClusterIndex,
        entry.instanceID,
        entry.localMeshletIndex,
        entry.materialIndex));
    replayBuffer.Store4(byteOffset + 16u, uint4(
        entry.viewID,
        entry.splitLevel,
        entry.quantizedTessFactor,
        entry.flags));
    replayBuffer.Store4(byteOffset + 32u, uint4(
        entry.sourcePrimitiveAndSplitConfig,
        asuint(entry.domainVertex0UV.x),
        asuint(entry.domainVertex0UV.y),
        asuint(entry.domainVertex1UV.x)));
    replayBuffer.Store3(byteOffset + 48u, uint3(
        asuint(entry.domainVertex1UV.y),
        asuint(entry.domainVertex2UV.x),
        asuint(entry.domainVertex2UV.y)));
}

void ReplayStoreReyesDiceEntry(RWByteAddressBuffer replayBuffer, uint byteOffset, CLodReyesDiceQueueEntry entry)
{
    replayBuffer.Store4(byteOffset + 0u, uint4(
        entry.visibleClusterIndex,
        entry.instanceID,
        entry.localMeshletIndex,
        entry.materialIndex));
    replayBuffer.Store4(byteOffset + 16u, uint4(
        entry.viewID,
        entry.splitLevel,
        entry.quantizedTessFactor,
        entry.flags));
    replayBuffer.Store4(byteOffset + 32u, uint4(
        entry.sourcePrimitiveAndSplitConfig,
        asuint(entry.domainVertex0UV.x),
        asuint(entry.domainVertex0UV.y),
        asuint(entry.domainVertex1UV.x)));
    replayBuffer.Store4(byteOffset + 48u, uint4(
        asuint(entry.domainVertex1UV.y),
        asuint(entry.domainVertex2UV.x),
        asuint(entry.domainVertex2UV.y),
        entry.tessTableConfigIndex));
    replayBuffer.Store(byteOffset + 64u, entry.reserved);
}

bool ReplayTryAppendReyesSplit(CLodReyesSplitQueueEntry entry)
{
    RWByteAddressBuffer replayBuffer = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX];

    uint slot = 0u;
    InterlockedAdd(replayState[0].reyesSplitWriteCount, 1u, slot);
    if (slot >= CLOD_REYES_SPLIT_REPLAY_CAPACITY) {
        InterlockedAdd(replayState[0].reyesSplitDropped, 1u);
        InterlockedAdd(telemetryBuffer[0].replaySplitQueueOverflowCount, 1u);
        return false;
    }

    const uint byteOffset = CLOD_REPLAY_REYES_SPLIT_REGION_OFFSET + slot * CLOD_REYES_SPLIT_REPLAY_STRIDE_BYTES;
    ReplayStoreReyesSplitEntry(replayBuffer, byteOffset, entry);
    InterlockedAdd(telemetryBuffer[0].splitOcclusionDeferCount, 1u);
    return true;
}

bool ReplayTryAppendReyesDice(CLodReyesDiceQueueEntry entry)
{
    RWByteAddressBuffer replayBuffer = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX];

    uint slot = 0u;
    InterlockedAdd(replayState[0].reyesDiceWriteCount, 1u, slot);
    if (slot >= CLOD_REYES_DICE_REPLAY_CAPACITY) {
        InterlockedAdd(replayState[0].reyesDiceDropped, 1u);
        InterlockedAdd(telemetryBuffer[0].replayDiceQueueOverflowCount, 1u);
        return false;
    }

    const uint byteOffset = CLOD_REPLAY_REYES_DICE_REGION_OFFSET + slot * CLOD_REYES_DICE_REPLAY_STRIDE_BYTES;
    ReplayStoreReyesDiceEntry(replayBuffer, byteOffset, entry);
    InterlockedAdd(telemetryBuffer[0].diceOcclusionDeferCount, 1u);
    return true;
}
#endif

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
    uint nodeIdPacked; // [31]=sourceTag, [30]=allowRefine, [29]=boundsTested, [28:0]=nodeId
    uint viewId;
    uint assemblyTransformIndex;
};

struct CLodClusterRunRecord
{
    uint instanceIndex;
    uint viewId;
    uint groupIdPacked;         // [31]=sourceTag, [30:0]=groupId
    uint clusterIndexAndCount;  // [31:16]=count, [15:0]=firstLocalMeshletIndex
    uint pageSlabDescriptorIndex;
    uint pageSlabByteOffset;
    uint assemblyTransformIndex;
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

bool CLodReplayRootOcclusionCandidate(
    TraverseNodeRecord rec,
    CLodMeshMetadata clodMeshMetadata)
{
    return
        UnpackSourceTag(rec.nodeIdPacked) == CLOD_RECORD_SOURCE_REPLAY &&
        rec.assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL &&
        UnpackNodeId(rec.nodeIdPacked) == CLodResolveTraversalRootNode(clodMeshMetadata) &&
        CLodWorkGraphOcclusionEnabled();
}

bool CLodReplayRootOccluded(
    TraverseNodeRecord rec,
    CLodMeshMetadata clodMeshMetadata,
    InstanceDrawRecordBuffer drawRecord,
    PerMeshInstanceBuffer instanceData,
    PerObjectBuffer instanceTransform,
    Camera cullCamera)
{
    if (!CLodReplayRootOcclusionCandidate(rec, clodMeshMetadata) || cullCamera.isOrtho)
    {
        return false;
    }

    StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
        ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
    const uint depthMapDescriptorIndex =
        viewDepthSRVIndices[rec.viewId].linearDepthSRVIndex;
    if (depthMapDescriptorIndex == 0u)
    {
        return false;
    }

    float coarseBoundsScale = 1.0f;
    const BoundingSphere coarseBounds =
        LoadCoarseCullBoundsForDrawRecord(drawRecord, instanceData, coarseBoundsScale);
    const row_major matrix modelMatrix = instanceTransform.model;
    const float3 viewSpaceCenter =
        ToViewSpace(coarseBounds.sphere.xyz, modelMatrix, cullCamera.view);
    const float worldRadius = coarseBounds.sphere.w * coarseBoundsScale *
        MaxAxisScale_RowVector(modelMatrix);
    if (any(isnan(viewSpaceCenter)) || any(isinf(viewSpaceCenter)) ||
        isnan(worldRadius) || isinf(worldRadius))
    {
        return false;
    }

    bool occlusionCulled = false;
    OcclusionCullingPerspectiveTexture2D(
        occlusionCulled,
        cullCamera,
        viewSpaceCenter,
        -viewSpaceCenter.z,
        worldRadius,
        depthMapDescriptorIndex,
        cullCamera.projection);
    if (occlusionCulled)
    {
        WGTelemetryAdd(WG_COUNTER_OBJECT_REPLAY_REJECTED_OCCLUSION, 1u);
    }
    return occlusionCulled;
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

bool CLodNodeBoundsOutsideFrustum(
    uint classification,
    float4 bindSphere,
    BoundingSphere resolvedSphere,
    row_major matrix objectModelMatrix,
    float uniformScale,
    Camera camera)
{
    if (classification == CLOD_NODE_CULL_OVERFLOW_FALLBACK ||
        classification == CLOD_NODE_CULL_ASSEMBLY_FALLBACK ||
        classification == CLOD_NODE_CULL_INVALID_FALLBACK)
    {
        return false;
    }

    const float3 resolvedCenterView = ToViewSpace(
        resolvedSphere.sphere.xyz,
        objectModelMatrix,
        camera.view);
    const bool resolvedOutside = SphereOutsideFrustumViewSpace(
        resolvedCenterView,
        resolvedSphere.sphere.w * uniformScale,
        camera);
    if (classification != CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS)
    {
        return resolvedOutside;
    }

    // Animated voxel leaves can contain rigid static-sentinel cubes alongside
    // skinned cubes. The accepted node bound must cover both copies because it
    // authorizes inline emission without a second per-cluster frustum test.
    const float3 bindCenterView = ToViewSpace(bindSphere.xyz, objectModelMatrix, camera.view);
    const bool bindOutside = SphereOutsideFrustumViewSpace(
        bindCenterView,
        bindSphere.w * uniformScale,
        camera);
    return bindOutside && resolvedOutside;
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

// Perspective views attenuate projected geometric error by view-space depth.
// Orthographic views keep a constant world-to-screen scale, so the projected
// error reduces to the world-space error directly.
float ProjectedGeometricError(
    float3 worldCenter,
    float worldRadius,
    float errorMeshSpace,
    float errorScale,
    float4 worldToViewZ,
    float zNear,
    bool isOrtho)
{
    const float worldSpaceError = errorMeshSpace * errorScale;
    if (isOrtho) {
        return worldSpaceError;
    }

    // Use view-space depth, not radial camera distance. Radial distance
    // underestimates projected error near frustum edges and allows coarse LODs
    // to switch in too early toward screen corners.
    const float centerViewZ = dot(float4(worldCenter, 1.0f), worldToViewZ);
    const float denom = max(-centerViewZ - worldRadius, zNear);

    return worldSpaceError / denom;
}

bool CLodTouchAndRequestGroupResident(
    uint groupGlobalIndex,
    uint instanceIndex,
    uint meshBufferIndex,
    uint viewId,
    float requestPriorityErrorOverDistance);

bool CLodInstanceRootWantsTraversal(
    CLodMeshMetadata clodMeshMetadata,
    ClusterLODNode node,
    bool parentAllowsRefine,
    float4x4 objectModelMatrix,
    float lodUniformScale,
    CullingCameraInfo lodCam,
    bool lodCameraIsOrtho,
    bool forceLodDecision,
    uint instanceIndex,
    uint meshBufferIndex,
    uint viewId)
{
    if (forceLodDecision)
    {
        return true;
    }

    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup proxyGroup = groups[clodMeshMetadata.groupsBase + node.range.ownerGroupId];
    const bool structuralRootProxy = proxyGroup.parentGroupId < 0 && proxyGroup.pageCount == 0u;
    if (!parentAllowsRefine)
    {
        return structuralRootProxy;
    }

    const float3 proxyWorldCenter = mul(float4(proxyGroup.bounds.centerAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
    const float proxyWorldRadius = proxyGroup.bounds.centerAndRadius.w * lodUniformScale;
    const float proxyErrorOverDistance = ProjectedGeometricError(
        proxyWorldCenter,
        proxyWorldRadius,
        proxyGroup.bounds.error,
        lodUniformScale,
        lodCam.viewZ,
        lodCam.zNear,
        lodCameraIsOrtho);
    if (proxyErrorOverDistance >= lodCam.errorOverDistanceThreshold)
    {
        return true;
    }

    if (structuralRootProxy)
    {
        return true;
    }

    if (proxyGroup.parentGroupId >= 0)
    {
        const uint parentGroupLocalIndex = uint(proxyGroup.parentGroupId);
        const uint parentGroupGlobalIndex = clodMeshMetadata.groupsBase + parentGroupLocalIndex;
        if (!CLodTouchAndRequestGroupResident(
                parentGroupGlobalIndex,
                instanceIndex,
                meshBufferIndex,
                viewId,
                proxyErrorOverDistance))
        {
            return true;
        }
    }

    return false;
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
    WGTelemetryAdd(WG_COUNTER_STREAM_REQUEST_ATTEMPTS, 1u);
    StructuredBuffer<CLodStreamingRuntimeState> runtimeState =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingRuntimeState)];
    const uint activeGroupScanCount = runtimeState[0].activeGroupScanCount;
    if (groupGlobalIndex >= activeGroupScanCount)
    {
        WGTelemetryAdd(WG_COUNTER_STREAM_REQUEST_RANGE_REJECTS, 1u);
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
        WGTelemetryAdd(WG_COUNTER_STREAM_REQUEST_APPENDS, 1u);
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
    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup group = groups[groupGlobalIndex];
    if (group.pageCount == 0u)
    {
        return true;
    }

    if (CLodGroupIsResident(groupGlobalIndex))
    {
        WGTelemetryAdd(WG_COUNTER_STREAM_RESIDENT_HITS, 1u);
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
    bool allowVoxelChildSuppression,
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
    if (!allowVoxelChildSuppression && ((childGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u))
    {
        return false;
    }

    const float3 childWorldCenter = mul(float4(childGroup.bounds.centerAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
    const float childWorldRadius = childGroup.bounds.centerAndRadius.w * lodUniformScale;
    const float childBoundaryEOD = ProjectedGeometricError(
        childWorldCenter,
        childWorldRadius,
        childGroup.bounds.error,
        lodUniformScale,
        lodCam.viewZ,
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
        if ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_VOXEL_LEAF_RECORDS, 1);
        }
    }

    const float3 groupWorldCenter = mul(float4(leaf.group.bounds.centerAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
    const float groupWorldRadius = leaf.group.bounds.centerAndRadius.w * lodUniformScale;
    leaf.errorOverDistance = ProjectedGeometricError(
        groupWorldCenter,
        groupWorldRadius,
        node.metric.maxQuadricError,
        lodUniformScale,
        lodCam.viewZ,
        lodCam.zNear,
        lodCameraIsOrtho);
    const bool wantsRender = forceLodDecision || (parentAllowsRefine && (leaf.errorOverDistance >= lodCam.errorOverDistanceThreshold));
    if (!wantsRender)
    {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_REJECTED_BY_ERROR_RECORDS, 1);
        if (leaf.isVoxel)
        {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_REJECTED_BY_ERROR_RECORDS, 1);
            if ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
            {
                WGTelemetryAdd(WG_COUNTER_ASSEMBLY_VOXEL_REJECTED_BY_ERROR_RECORDS, 1);
            }
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
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_SEGMENT_PAGE_MISSES, 1);
        }
        else
        {
            WGTelemetryAdd(WG_COUNTER_SEGMENT_EVALUATE_NON_RESIDENT_REFINED_CHILD_THREADS, 1);
        }
    }

    leaf.valid = true;
    return true;
}

#if CLOD_WG_ENABLE_VOXEL_OUTPUT
void CLodAppendVoxelRasterWorkForLeaf(
    CLodMeshMetadata clodMeshMetadata,
    uint instanceIndex,
    uint assemblyTransformIndex,
    uint viewId,
    ClusterLODNode node,
    ClusterLODGroup voxelGroup,
    row_major matrix objectModelMatrix,
    float lodUniformScale,
    Camera cullCamera,
    bool dirtyPageCullingEnabled)
{
    StructuredBuffer<ClusterLODGroupSegment> segments =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];

    if ((voxelGroup.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
    {
        WGTelemetryAdd(WG_COUNTER_ASSEMBLY_VOXEL_RASTER_WORK_RECORDS, 1);
    }

    if (node.range.isLeaf == CLOD_NODE_VOXEL_LEAF)
    {
		const uint firstLocalSegment = node.range.indexOrOffset;
		const int sectionRefinedGroup = int(node.range.countMinusOne) - 1;

        [loop]
        for (uint localSegment = firstLocalSegment; localSegment < voxelGroup.segmentCount; ++localSegment)
        {
            const uint segGlobalIndex = clodMeshMetadata.segmentsBase + voxelGroup.firstSegment + localSegment;
            const ClusterLODGroupSegment seg = segments[segGlobalIndex];
            if (seg.refinedGroup != sectionRefinedGroup)
            {
                break;
            }

            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_SEGMENT_PAGE_HITS, 1);
            CLodAppendVoxelRasterClusterWork(
                clodMeshMetadata,
                instanceIndex,
                assemblyTransformIndex,
                viewId,
                node.range.ownerGroupId,
                voxelGroup,
                seg,
                objectModelMatrix,
                lodUniformScale,
                cullCamera,
                dirtyPageCullingEnabled);
        }
        return;
    }

    const uint segGlobalIndex = clodMeshMetadata.segmentsBase + node.range.indexOrOffset;
    const ClusterLODGroupSegment seg = segments[segGlobalIndex];
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_SEGMENT_PAGE_HITS, 1);
    CLodAppendVoxelRasterClusterWork(
        clodMeshMetadata,
        instanceIndex,
        assemblyTransformIndex,
        viewId,
        node.range.ownerGroupId,
        voxelGroup,
        seg,
        objectModelMatrix,
        lodUniformScale,
        cullCamera,
        dirtyPageCullingEnabled);
}
#endif

void CLodHandleRenderableLeaf(
    TraverseNodeRecord rec,
    bool parentAllowsRefine,
    CLodMeshMetadata clodMeshMetadata,
    ClusterLODNode node,
    row_major matrix objectModelMatrix,
    float lodUniformScale,
    CullingCameraInfo lodCam,
    bool lodCameraIsOrtho,
    Camera cullCamera,
    bool forceLodDecision,
    PerMeshInstanceBuffer instanceData,
    bool nodeTouchesDirtyPages,
    bool dirtyPageCullingEnabled,
    out CLodClusterRunRecord bucketRecord,
    out uint n64,
    out uint n32,
    out uint n16,
    out uint n8,
    out uint n4,
    out uint n2,
    out uint n1,
    out bool emitBucket)
{
    bucketRecord = (CLodClusterRunRecord)0;
    n64 = 0;
    n32 = 0;
    n16 = 0;
    n8 = 0;
    n4 = 0;
    n2 = 0;
    n1 = 0;
    emitBucket = false;

    CLodRenderableLeaf leaf;
    if (!CLodPrepareRenderableLeaf(
        clodMeshMetadata,
        node,
        parentAllowsRefine,
        objectModelMatrix,
        lodUniformScale,
        lodCam,
        lodCameraIsOrtho,
        nodeTouchesDirtyPages,
        forceLodDecision,
        rec.instanceIndex,
        instanceData.perMeshBufferIndex,
        rec.viewId,
        leaf))
    {
        return;
    }
    const bool assemblyPartTraversal = rec.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
    const bool assemblyPartVoxelLeaf =
        assemblyPartTraversal &&
        leaf.isVoxel &&
        ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) == 0u);
    if (assemblyPartVoxelLeaf)
    {
        WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_VOXEL_LEAF_RECORDS, 1);
    }

    if (!forceLodDecision && CLodRefinedChildSuppressesParent(
        clodMeshMetadata.groupsBase,
        node.range.countMinusOne - 1u,
        node.range.countMinusOne != 0u,
        objectModelMatrix,
        lodUniformScale,
        lodCam,
        lodCameraIsOrtho,
        rec.instanceIndex,
        instanceData.perMeshBufferIndex,
        rec.viewId,
        leaf.errorOverDistance,
        0.0f.xxx,
        -1.0f,
        true,
        false))
    {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CONDITION2, 1);
        if ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_VOXEL_SUPPRESSED_BY_CHILD_RECORDS, 1);
        }
    }
    else if (!leaf.canRender)
    {
        // The group was requested above; keep traversal/request side effects
        // but do not emit render work until the page data is resident.
        if ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_VOXEL_NONRESIDENT_RECORDS, 1);
        }
    }
    else if (leaf.isVoxel)
    {
#if CLOD_WG_ENABLE_VOXEL_OUTPUT
        if (assemblyPartVoxelLeaf)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_VOXEL_RASTER_WORK_RECORDS, 1);
        }
        CLodAppendVoxelRasterWorkForLeaf(
            clodMeshMetadata,
            rec.instanceIndex,
            rec.assemblyTransformIndex,
            rec.viewId,
            node,
            leaf.group,
            objectModelMatrix,
            lodUniformScale,
            cullCamera,
            dirtyPageCullingEnabled);
#endif
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
            if (assemblyPartTraversal)
            {
                WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_TRIANGLE_BUCKET_RECORDS, 1);
            }
            const GroupPageMapEntry pageEntry = LoadGroupPageMapEntry(clodMeshMetadata.pageMapBase, seg.pageIndex);
            if (pageEntry.slabDescriptorIndex == 0u)
            {
                WGTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB, 1);
                emitBucket = false;
                return;
            }

            bucketRecord.instanceIndex = rec.instanceIndex;
            bucketRecord.viewId = rec.viewId;
            bucketRecord.groupIdPacked = PackGroupId(node.range.ownerGroupId, UnpackSourceTag(rec.nodeIdPacked));
            bucketRecord.clusterIndexAndCount = PackClusterIndexAndCount(seg.firstMeshletInPage, 0); // count set per-record below
            bucketRecord.pageSlabDescriptorIndex = pageEntry.slabDescriptorIndex;
            bucketRecord.pageSlabByteOffset = pageEntry.slabByteOffset;
            bucketRecord.assemblyTransformIndex = rec.assemblyTransformIndex;
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
            WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_STALE_GENERATION, 1);
            entryVisible = false;
        }
    }

    if (entryVisible) {
        const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(drawRecordIndex);
        const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDrawRecord(drawRecord);
        const PerObjectBuffer instanceTransform =
            LoadInstanceTransformForDrawRecordWithAssemblyTransform(drawRecord, CLOD_ASSEMBLY_TRANSFORM_SENTINEL);
        StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
                        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
        const MeshInstanceClodOffsets off = LoadCLodOffsetsForDrawRecord(drawRecord);
        const CLodMeshMetadata clodMeshMetadata = clodMeshMetadataBuffer[off.clodMeshMetadataIndex];
        const uint rootNodeId = CLodResolveTraversalRootNode(clodMeshMetadata);
        const bool voxelRootCandidate = CLodMeshHasVoxelRootGroup(clodMeshMetadata);
        if (voxelRootCandidate)
        {
            WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_CANDIDATES, 1);
        }

        StructuredBuffer<PerMeshBuffer> perMeshBuffer =
                    ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshBuffer perMesh = perMeshBuffer[instanceData.perMeshBufferIndex];

        const row_major matrix objectModelMatrix = instanceTransform.model;

        StructuredBuffer<Camera> cameras =
                    ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const Camera camera = cameras[hdr.viewDataIndex];

        float coarseBoundsScale = 1.0f;
        const BoundingSphere coarseBounds =
            LoadCoarseCullBoundsForDrawRecord(drawRecord, instanceData, coarseBoundsScale);
        const float3 objectSpaceCenter = coarseBounds.sphere.xyz;
        const float3 viewSpaceCenter = ToViewSpace(objectSpaceCenter, objectModelMatrix, camera.view);
        // Assembly material/subset draws share one fitted conservative sphere.
        // Final clusters are culled with their live palette-deformed spheres.
        const float worldRadius = coarseBounds.sphere.w * coarseBoundsScale *
            MaxAxisScale_RowVector(objectModelMatrix);

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
                    if (voxelRootCandidate)
                    {
                        WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_FRUSTUM_REJECTED, 1);
                    }
                    culled = true;
                    break;
                }
            }
        }
        if (!culled) {
            bool occlusionCulled = false;
            if (CLodWorkGraphOcclusionEnabled() && !camera.isOrtho) {
                StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
                    ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
                const uint depthMapDescriptorIndex =
                    viewDepthSRVIndices[hdr.viewDataIndex].linearDepthSRVIndex;
                if (depthMapDescriptorIndex != 0u) {
                    const row_major matrix prevModelMatrix = instanceTransform.prevModel;
                    const float3 prevViewSpaceCenter =
                        ToViewSpace(objectSpaceCenter, prevModelMatrix, camera.prevView);
                    const float prevWorldRadius = coarseBounds.sphere.w * coarseBoundsScale *
                        MaxAxisScale_RowVector(prevModelMatrix);
                    OcclusionCullingPerspectiveTexture2D(
                        occlusionCulled,
                        camera,
                        prevViewSpaceCenter,
                        -prevViewSpaceCenter.z,
                        prevWorldRadius,
                        depthMapDescriptorIndex,
                        camera.prevUnjitteredProjection);
                }
            }
            if (occlusionCulled) {
                WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_OCCLUSION, 1u);
                ReplayTryAppendNode(
                    drawRecordIndex,
                    hdr.viewDataIndex,
                    rootNodeId,
                    CLOD_ASSEMBLY_TRANSFORM_SENTINEL);
                entryVisible = false;
            }
        }
        if (!culled && entryVisible) {
            outRecord.viewId = hdr.viewDataIndex;
            outRecord.instanceIndex = drawRecordIndex;
            outRecord.nodeIdPacked = PackTraverseNodeId(rootNodeId, CLOD_RECORD_SOURCE_PASS1, 1u, 0u);
            outRecord.assemblyTransformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
            outCount = 1;

            WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_VISIBLE_THREADS, 1);
            WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_TRAVERSE_RECORDS, 1);
            if (voxelRootCandidate)
            {
                WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_VISIBLE, 1);
                WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_TRAVERSE_RECORDS, 1);
            }
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
[NodeMaxRecursionDepth(19)] // Could be higher when Reyes is in pure-compute mode
void WG_TraverseNodes(
    [MaxRecords(TRAVERSE_RECORDS_PER_GROUP)] GroupNodeInputRecords<TraverseNodeRecord> inRecs,
    uint GI : SV_GroupIndex,
    [MaxRecords(TRAVERSE_RECORDS_PER_GROUP * BVH_MAX_CHILDREN)] NodeOutput<TraverseNodeRecord> TraverseNodes,
#if CLOD_WG_SPLIT_LEAF_NODE
    [NodeID("LeafNodes")] [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<TraverseNodeRecord> LeafNodes)
#else
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<CLodClusterRunRecord> ClusterCull1,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<CLodClusterRunRecord> ClusterCull2,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<CLodClusterRunRecord> ClusterCull4,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<CLodClusterRunRecord> ClusterCull8,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<CLodClusterRunRecord> ClusterCull16,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<CLodClusterRunRecord> ClusterCull32,
    [MaxRecordsSharedWith(TraverseNodes)] NodeOutput<CLodClusterRunRecord> ClusterCull64)
#endif
{
    const uint slot = GI;
    const uint inputCount = inRecs.Count();
    const bool slotActive = slot < inputCount;

    WGTelemetryAdd(WG_COUNTER_TRAVERSE_THREADS, 1);
    CLodTelemetryTraverseWaveLaunch(slotActive);
    if (slot == 0) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_COALESCED_LAUNCHES, 1);
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_COALESCED_INPUT_RECORDS, inputCount);
        if (inputCount > 0 && inputCount <= COALESCED_INPUT_COUNT_HISTOGRAM_BUCKETS) {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_COALESCED_INPUT_COUNT_1 + (inputCount - 1), 1);
        }
    }

    uint emitTraverseCount = 0;
    TraverseNodeRecord childRecords[BVH_MAX_CHILDREN];
#if CLOD_WG_SPLIT_LEAF_NODE
    uint emitLeafCount = 0;
    TraverseNodeRecord leafRecords[BVH_MAX_CHILDREN];
#endif
    CLodClusterRunRecord bucketRecord = (CLodClusterRunRecord)0;
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
        const PerObjectBuffer instanceTransform =
            LoadInstanceTransformForDrawRecordWithAssemblyTransform(drawRecord, rec.assemblyTransformIndex);
        // Mesh upload creates one node-skinning sidecar entry per CLOD node for
        // skinned meshes and none for rigid meshes. Keep the original source for
        // telemetry so live PSO replacement retains the same resource interface.
#if CLOD_WG_RIGID_ONLY
        bool isSkinned = false;
#else
        bool isSkinned = clodMeshMetadata.nodeSkinningInfoCount != 0u;
        if (CLodWorkGraphTelemetryEnabled())
        {
            StructuredBuffer<PerMeshBuffer> perMeshBuffer =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
            const PerMeshBuffer perMesh = perMeshBuffer[instanceData.perMeshBufferIndex];
            isSkinned = (perMesh.vertexFlags & VERTEX_SKINNED) != 0u;
        }
#endif
        const row_major matrix objectModelMatrix = instanceTransform.model;
        StructuredBuffer<Camera> cameras =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const uint cullViewId = rec.viewId;
        const uint lodViewId = CLodResolveLodViewId(cullViewId);
        const Camera cullCamera = cameras[cullViewId];
        const bool replayRootOcclusionCulled = CLodReplayRootOccluded(
            rec,
            clodMeshMetadata,
            drawRecord,
            instanceData,
            instanceTransform,
            cullCamera);
        StructuredBuffer<CullingCameraInfo> cameraInfos =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
        const CullingCameraInfo lodCam = cameraInfos[lodViewId];
        const bool lodCameraIsOrtho = lodCam.isOrtho != 0u;
        StructuredBuffer<ClusterLODNode> lodNodes =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Nodes)];

        const uint nodeLocalId = UnpackNodeId(rec.nodeIdPacked);
        const ClusterLODNode node = lodNodes[clodMeshMetadata.lodNodesBase + nodeLocalId];
        CLodTelemetryTraverseWaveClassification(
            node.range.isLeaf == CLOD_NODE_INTERNAL,
            isSkinned);
        const bool skinnedAssemblyPortal =
            node.range.isLeaf == CLOD_NODE_INSTANCE_ROOT &&
            isSkinned &&
            clodMeshMetadata.assemblyInstanceCount != 0u;
        const bool assemblyPortalTraversal = rec.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
        if (assemblyPortalTraversal)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_TRAVERSAL_RECORDS, 1);
        }
        const bool voxelRootCandidate = CLodMeshHasVoxelRootGroup(clodMeshMetadata);
        if (voxelRootCandidate && nodeLocalId == CLodResolveTraversalRootNode(clodMeshMetadata))
        {
            WGTelemetryAdd(
                node.range.isLeaf == CLOD_NODE_INTERNAL
                    ? WG_COUNTER_VOXEL_ROOT_INTERNAL_RECORDS
                    : WG_COUNTER_VOXEL_ROOT_LEAF_RECORDS,
                1);
        }

        if (node.range.isLeaf == CLOD_NODE_INTERNAL) {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_INTERNAL_NODE_RECORDS, 1);
        }
#if !CLOD_WG_SPLIT_LEAF_NODE
        else {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_LEAF_NODE_RECORDS, 1);
        }
#endif

        const float objectUniformScale = MaxAxisScale_RowVector(objectModelMatrix);
        const float cullUniformScale = objectUniformScale;
        const float lodUniformScale = objectUniformScale;
        BoundingSphere nodeCullBounds = { node.metric.cullCenterAndRadius };
        const uint nodeCullClassification = isSkinned
            ? CLodResolveAnimatedNodeCullSphere(
                nodeLocalId, node.metric.cullCenterAndRadius, clodMeshMetadata,
                rec.instanceIndex, instanceData.skinningInstanceSlot,
                rec.assemblyTransformIndex, nodeCullBounds)
            : CLOD_NODE_CULL_STATIC_BIND_POSE;
        CLodTelemetryNodeCullClassification(nodeCullClassification);
        const float3 nodeCullCenterObjectSpace = nodeCullBounds.sphere.xyz;
        const float nodeCullRadiusObjectSpace = nodeCullBounds.sphere.w;
        const float3 nodeLodCenterObjectSpace = node.metric.lodCenterAndRadius.xyz;
        const float nodeLodRadiusObjectSpace = node.metric.lodCenterAndRadius.w;
        const float3 nodeCenterViewSpace = ToViewSpace(nodeCullCenterObjectSpace, objectModelMatrix, cullCamera.view);
        const float nodeRadiusWorld = nodeCullRadiusObjectSpace * cullUniformScale;
        const bool nodeCulled =
            replayRootOcclusionCulled ||
            (CLodWorkGraphFrustumCullingEnabled() &&
             !replaySource &&
             UnpackBoundsTested(rec.nodeIdPacked) == 0u &&
             CLodNodeBoundsOutsideFrustum(
                 nodeCullClassification,
                 node.metric.cullCenterAndRadius,
                 nodeCullBounds,
                 objectModelMatrix,
                 cullUniformScale,
                 cullCamera));
    #if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        const bool objectInvalidatedThisFrame = CLodVirtualShadowInstanceInvalidatedThisFrame(rec.instanceIndex);
        const bool dirtyPageCullingEnabled =
            !isSkinned && CLodWorkGraphShadowDirtyPageCullingEnabled() && !objectInvalidatedThisFrame;
    #else
        const bool dirtyPageCullingEnabled = false;
    #endif

        if (nodeCulled) {
            if (nodeCullClassification == CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS)
                WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_EXPLICIT_FRUSTUM_REJECTED, 1u);
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_CULLED_NODE_RECORDS, 1);
        }
        else {
            // LOD pre-filter: for segment-leaves, check if the own group's
            // error-over-distance exceeds threshold (condition 1 of the
            // meshoptimizer rendering rule).  Uses the actual group sphere
            // for accuracy; the BVH node sphere is only for frustum culling.
            // For internal nodes, the BVH node sphere and propagated max
            // error provide a conservative bound.

            if (node.range.isLeaf == CLOD_NODE_INSTANCE_ROOT) {
                WGTelemetryAdd(WG_COUNTER_ASSEMBLY_INSTANCE_ROOT_RECORDS, 1);
                if (rec.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
                {
                    WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_INSTANCE_ROOT_RECORDS, 1);
                }
                // Bind-pose portal proxy bounds are not valid LOD gates for an
                // animated part. Open the portal and let meshlet bounds cull it.
                const bool portalWantsTraversal = skinnedAssemblyPortal || CLodInstanceRootWantsTraversal(
                    clodMeshMetadata,
                    node,
                    parentAllowsRefine,
                    objectModelMatrix,
                    lodUniformScale,
                    lodCam,
                    lodCameraIsOrtho,
                    forceLodDecision,
                    rec.instanceIndex,
                    instanceData.perMeshBufferIndex,
                    rec.viewId);
                if (!portalWantsTraversal)
                {
                    WGTelemetryAdd(WG_COUNTER_TRAVERSE_REJECTED_BY_ERROR_RECORDS, 1);
                }
                else
                {
                    StructuredBuffer<ClusterLODAssemblyInstance> assemblyInstances =
                        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyInstances)];
                    const bool validAssemblyInstance =
                        node.range.indexOrOffset < clodMeshMetadata.assemblyInstanceCount;
                    if (validAssemblyInstance) {
                        const ClusterLODAssemblyInstance assemblyInstance =
                            assemblyInstances[clodMeshMetadata.assemblyInstanceBase + node.range.indexOrOffset];
                        if (assemblyInstance.stackDepth <= CLOD_ASSEMBLY_MAX_STACK_DEPTH) {
                            TraverseNodeRecord instanceChildRecord = (TraverseNodeRecord)0;
                            instanceChildRecord.instanceIndex = rec.instanceIndex;
                            instanceChildRecord.viewId = rec.viewId;
                            instanceChildRecord.nodeIdPacked = PackTraverseNodeId(
                                assemblyInstance.targetRootNode,
                                UnpackSourceTag(rec.nodeIdPacked),
                                1u,
                                0u);
                            instanceChildRecord.assemblyTransformIndex =
                                assemblyInstance.transformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL
                                    ? rec.assemblyTransformIndex
                                    : clodMeshMetadata.assemblyTransformBase + assemblyInstance.transformIndex;
                            childRecords[emitTraverseCount] = instanceChildRecord;
                            emitTraverseCount++;
                        }
                    }
                }
            }
            else if (node.range.isLeaf != CLOD_NODE_INTERNAL) {
#if CLOD_WG_SPLIT_LEAF_NODE
                leafRecords[emitLeafCount] = rec;
                emitLeafCount++;
#else
                bool leafOcclusionCulled = false;
                if (CLodWorkGraphOcclusionEnabled() &&
                    (!cullCamera.isOrtho || CLOD_VSM_OCCLUSION_CULLING) &&
                    CLodNodeBoundsSupportOcclusion(nodeCullClassification))
                {
                    StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
                        ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
                    const uint depthMapDescriptorIndex =
                        viewDepthSRVIndices[cullViewId].linearDepthSRVIndex;
                    if (depthMapDescriptorIndex != 0u)
                    {
                        BoundingSphere leafOcclusionBounds = nodeCullBounds;
                        row_major matrix leafOcclusionModel = objectModelMatrix;
                        row_major matrix leafOcclusionView = cullCamera.view;
                        row_major matrix leafOcclusionProjection = cullCamera.projection;
                        uint leafOcclusionClassification = nodeCullClassification;

                        if (!replaySource)
                        {
                            leafOcclusionModel = instanceTransform.prevModel;
                            leafOcclusionView = cullCamera.prevView;
                            leafOcclusionProjection = cullCamera.prevUnjitteredProjection;
                            if (isSkinned)
                            {
                                leafOcclusionClassification =
                                    CLodResolveAnimatedNodeCullSphereForPose(
                                        nodeLocalId,
                                        node.metric.cullCenterAndRadius,
                                        clodMeshMetadata,
                                        rec.instanceIndex,
                                        instanceData.skinningInstanceSlot,
                                        rec.assemblyTransformIndex,
                                        true,
                                        leafOcclusionBounds);
                            }
                        }

                        if (CLodNodeBoundsSupportOcclusion(leafOcclusionClassification))
                        {
                            leafOcclusionBounds = CLodNodeOcclusionSphere(
                                node.metric.cullCenterAndRadius,
                                leafOcclusionBounds,
                                leafOcclusionClassification);
                            const float leafOcclusionScale =
                                MaxAxisScale_RowVector(leafOcclusionModel);
                            const float3 leafOcclusionCenterView = ToViewSpace(
                                leafOcclusionBounds.sphere.xyz,
                                leafOcclusionModel,
                                leafOcclusionView);
                            OcclusionCullingPerspectiveTexture2D(
                                leafOcclusionCulled,
                                cullCamera,
                                leafOcclusionCenterView,
                                -leafOcclusionCenterView.z,
                                leafOcclusionBounds.sphere.w * leafOcclusionScale,
                                depthMapDescriptorIndex,
                                leafOcclusionProjection);
                        }
                    }
                }

                if (leafOcclusionCulled)
                {
                    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_OCCLUSION, 1u);
                    if (!replaySource)
                    {
                        ReplayTryAppendNode(
                            rec.instanceIndex,
                            rec.viewId,
                            nodeLocalId,
                            rec.assemblyTransformIndex);
                    }
                }
                else
                {
                bool nodeTouchesDirtyPages = true;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                if (dirtyPageCullingEnabled)
                {
                    const float3 nodeCullCenterWorld = mul(float4(nodeCullCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
                    nodeTouchesDirtyPages = CLodVirtualShadowBoundsTouchDirtyPages(nodeCullCenterWorld, nodeRadiusWorld, rec.viewId);
                }
#endif

                CLodHandleRenderableLeaf(
                    rec,
                    parentAllowsRefine,
                    clodMeshMetadata,
                    node,
                    objectModelMatrix,
                    lodUniformScale,
                    lodCam,
                    lodCameraIsOrtho,
                    cullCamera,
                    forceLodDecision,
                    instanceData,
                    nodeTouchesDirtyPages,
                    dirtyPageCullingEnabled,
                    bucketRecord,
                    n64,
                    n32,
                    n16,
                    n8,
                    n4,
                    n2,
                    n1,
                    emitBucket);
                }
#endif
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
                    lodCam.viewZ,
                    lodCam.zNear,
                    lodCameraIsOrtho);
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
                        if (!isSkinned && CLodWorkGraphOcclusionEnabled() && (!cullCamera.isOrtho || CLOD_VSM_OCCLUSION_CULLING)) {
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
                                    UnpackNodeId(rec.nodeIdPacked),
                                    rec.assemblyTransformIndex);
                            }
                        }
                        else {
                            const uint childCount = min(node.range.countMinusOne + 1u, BVH_MAX_CHILDREN);
                            const uint sourceTag = UnpackSourceTag(rec.nodeIdPacked);
                            WGTelemetryAdd(WG_COUNTER_TRAVERSE_CHILD_LOOP_NODES, 1u);
                            WGTelemetryAdd(WG_COUNTER_TRAVERSE_CHILD_LOOP_SLOTS, childCount);

                            // Pre-filter children: load each child, frustum cull + LOD check,
                            // and only emit records for survivors.
                            [loop]
                            for (uint childIndex = 0; childIndex < childCount; ++childIndex) {
                                const uint childNodeId = node.range.indexOrOffset + childIndex;
                                const ClusterLODNode child = lodNodes[clodMeshMetadata.lodNodesBase + childNodeId];

                                // LOD pre-filter for internal children only.
                                // Leaf children use the group sphere for LOD (different from node sphere),
                                // so we skip the LOD check here and let the leaf thread handle it.
                                if (!forceLodDecision && child.range.isLeaf == CLOD_NODE_INTERNAL) {
                                    const float3 childWorldCenter = mul(float4(child.metric.lodCenterAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
                                    const float childLodRadiusWorld = child.metric.lodCenterAndRadius.w * lodUniformScale;
                                    const float childEOD = ProjectedGeometricError(
                                        childWorldCenter, childLodRadiusWorld,
                                        child.metric.maxQuadricError, lodUniformScale,
                                        lodCam.viewZ, lodCam.zNear,
                                        lodCameraIsOrtho);
                                    if (childEOD < lodCam.errorOverDistanceThreshold) {
                                        WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_LOD_REJECTED, 1);
                                        continue;
                                    }
                                }

                                // Skinned children resolve this same live sphere
                                // when their emitted record is consumed. Defer
                                // their frustum test instead of evaluating every
                                // animated child twice. Static bind bounds remain
                                // cheap enough to prefilter here.
                                if (!isSkinned) {
                                    BoundingSphere childCullBounds = { child.metric.cullCenterAndRadius };
                                    const uint childCullClassification = CLOD_NODE_CULL_STATIC_BIND_POSE;
                                    CLodTelemetryNodeCullClassification(childCullClassification);
                                    const float3 childCullCenterOS = childCullBounds.sphere.xyz;
                                    const float childCullRadiusOS = childCullBounds.sphere.w;
                                    const float childRadiusWorld = childCullRadiusOS * cullUniformScale;
                                    if (CLodWorkGraphFrustumCullingEnabled() &&
                                        !replaySource &&
                                        CLodNodeBoundsOutsideFrustum(
                                            childCullClassification,
                                            child.metric.cullCenterAndRadius,
                                            childCullBounds,
                                            objectModelMatrix,
                                            cullUniformScale,
                                            cullCamera)) {
                                        WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_FRUSTUM_CULLED, 1);
                                        continue;
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
                                }

                                TraverseNodeRecord childRecord = (TraverseNodeRecord)0;
                                childRecord.instanceIndex = rec.instanceIndex;
                                childRecord.viewId = rec.viewId;
                                childRecord.nodeIdPacked = PackTraverseNodeId(
                                    childNodeId,
                                    sourceTag,
                                    1u,
                                    isSkinned ? 0u : 1u);
                                childRecord.assemblyTransformIndex = rec.assemblyTransformIndex;
#if CLOD_WG_SPLIT_LEAF_NODE
                                if (child.range.isLeaf != CLOD_NODE_INTERNAL && child.range.isLeaf != CLOD_NODE_INSTANCE_ROOT) {
                                    leafRecords[emitLeafCount] = childRecord;
                                    emitLeafCount++;
                                }
                                else
#endif
                                {
                                    childRecords[emitTraverseCount] = childRecord;
                                    emitTraverseCount++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Allocate output records- all calls must be uniform across threads.
    ThreadNodeOutputRecords<TraverseNodeRecord>  outNodes = TraverseNodes.GetThreadNodeOutputRecords(emitTraverseCount);
#if CLOD_WG_SPLIT_LEAF_NODE
    ThreadNodeOutputRecords<TraverseNodeRecord> outLeafNodes = LeafNodes.GetThreadNodeOutputRecords(emitLeafCount);
#else
    ThreadNodeOutputRecords<CLodClusterRunRecord> out64 = ClusterCull64.GetThreadNodeOutputRecords(n64);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out32 = ClusterCull32.GetThreadNodeOutputRecords(n32);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out16 = ClusterCull16.GetThreadNodeOutputRecords(n16);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out8  = ClusterCull8.GetThreadNodeOutputRecords(n8);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out4  = ClusterCull4.GetThreadNodeOutputRecords(n4);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out2  = ClusterCull2.GetThreadNodeOutputRecords(n2);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out1  = ClusterCull1.GetThreadNodeOutputRecords(n1);
#endif

    if (emitTraverseCount > 0) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_ACTIVE_CHILD_THREADS, emitTraverseCount);
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_CHILD_RECORDS_EMITTED, emitTraverseCount);
        [unroll]
        for (uint childIndex = 0; childIndex < BVH_MAX_CHILDREN; ++childIndex) {
            if (childIndex >= emitTraverseCount) {
                break;
            }

            outNodes[childIndex] = childRecords[childIndex];
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_TRAVERSE_RECORDS, 1);
        }
    }

#if CLOD_WG_SPLIT_LEAF_NODE
    if (emitLeafCount > 0) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_ACTIVE_CHILD_THREADS, emitLeafCount);
        [unroll]
        for (uint leafIndex = 0; leafIndex < BVH_MAX_CHILDREN; ++leafIndex) {
            if (leafIndex >= emitLeafCount) {
                break;
            }

            outLeafNodes[leafIndex] = leafRecords[leafIndex];
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_TRAVERSE_RECORDS, 1);
        }
    }
#else
    if (emitBucket) {
        uint offset = UnpackClusterFirstIndex(bucketRecord.clusterIndexAndCount);

        for (uint i = 0; i < n64; i++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 64);
            out64[i] = r;
            offset += 64;
        }
        for (uint i32 = 0; i32 < n32; i32++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 32);
            out32[i32] = r;
            offset += 32;
        }
        for (uint i16 = 0; i16 < n16; i16++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 16);
            out16[i16] = r;
            offset += 16;
        }
        for (uint i8 = 0; i8 < n8; i8++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 8);
            out8[i8] = r;
            offset += 8;
        }
        for (uint i4 = 0; i4 < n4; i4++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 4);
            out4[i4] = r;
            offset += 4;
        }
        for (uint i2 = 0; i2 < n2; i2++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 2);
            out2[i2] = r;
            offset += 2;
        }
        for (uint i1 = 0; i1 < n1; i1++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 1);
            out1[i1] = r;
            offset += 1;
        }
    }
#endif

    outNodes.OutputComplete();
#if CLOD_WG_SPLIT_LEAF_NODE
    outLeafNodes.OutputComplete();
#else
    out64.OutputComplete();
    out32.OutputComplete();
    out16.OutputComplete();
    out8.OutputComplete();
    out4.OutputComplete();
    out2.OutputComplete();
    out1.OutputComplete();
#endif
}

#if CLOD_WG_SPLIT_LEAF_NODE
[Shader("node")]
[NodeID("LeafNodes")]
[NodeLaunch("coalescing")]
[NumThreads(TRAVERSE_THREADS_PER_GROUP, 1, 1)]
void WG_LeafNodes(
    [MaxRecords(TRAVERSE_RECORDS_PER_GROUP)] GroupNodeInputRecords<TraverseNodeRecord> inRecs,
    uint GI : SV_GroupIndex,
    [MaxRecords(TRAVERSE_RECORDS_PER_GROUP * MAX_RECORDS_PER_SEGMENT)] NodeOutput<CLodClusterRunRecord> ClusterCull1,
    [MaxRecordsSharedWith(ClusterCull1)] NodeOutput<CLodClusterRunRecord> ClusterCull2,
    [MaxRecordsSharedWith(ClusterCull1)] NodeOutput<CLodClusterRunRecord> ClusterCull4,
    [MaxRecordsSharedWith(ClusterCull1)] NodeOutput<CLodClusterRunRecord> ClusterCull8,
    [MaxRecordsSharedWith(ClusterCull1)] NodeOutput<CLodClusterRunRecord> ClusterCull16,
    [MaxRecordsSharedWith(ClusterCull1)] NodeOutput<CLodClusterRunRecord> ClusterCull32,
    [MaxRecordsSharedWith(ClusterCull1)] NodeOutput<CLodClusterRunRecord> ClusterCull64)
{
    const uint slot = GI;
    const uint inputCount = inRecs.Count();
    const bool slotActive = slot < inputCount;

    CLodClusterRunRecord bucketRecord = (CLodClusterRunRecord)0;
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
        const PerObjectBuffer instanceTransform =
            LoadInstanceTransformForDrawRecordWithAssemblyTransform(drawRecord, rec.assemblyTransformIndex);
        // Mesh upload creates one node-skinning sidecar entry per CLOD node for
        // skinned meshes and none for rigid meshes. Keep the original source for
        // telemetry so live PSO replacement retains the same resource interface.
#if CLOD_WG_RIGID_ONLY
        bool isSkinned = false;
#else
        bool isSkinned = clodMeshMetadata.nodeSkinningInfoCount != 0u;
        if (CLodWorkGraphTelemetryEnabled())
        {
            StructuredBuffer<PerMeshBuffer> perMeshBuffer =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
            const PerMeshBuffer perMesh = perMeshBuffer[instanceData.perMeshBufferIndex];
            isSkinned = (perMesh.vertexFlags & VERTEX_SKINNED) != 0u;
        }
#endif
        const row_major matrix objectModelMatrix = instanceTransform.model;
        StructuredBuffer<Camera> cameras =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const uint cullViewId = rec.viewId;
        const uint lodViewId = CLodResolveLodViewId(cullViewId);
        const Camera cullCamera = cameras[cullViewId];
        StructuredBuffer<CullingCameraInfo> cameraInfos =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
        const CullingCameraInfo lodCam = cameraInfos[lodViewId];
        const bool lodCameraIsOrtho = lodCam.isOrtho != 0u;
        StructuredBuffer<ClusterLODNode> lodNodes =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Nodes)];

		const uint nodeLocalId = UnpackNodeId(rec.nodeIdPacked);
		const ClusterLODNode node = lodNodes[clodMeshMetadata.lodNodesBase + nodeLocalId];
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_LEAF_NODE_RECORDS, 1);

        const float objectUniformScale = MaxAxisScale_RowVector(objectModelMatrix);
        const float cullUniformScale = objectUniformScale;
        const float lodUniformScale = objectUniformScale;
        BoundingSphere nodeCullBounds = { node.metric.cullCenterAndRadius };
        const uint nodeCullClassification = isSkinned
            ? CLodResolveAnimatedNodeCullSphere(
                nodeLocalId, node.metric.cullCenterAndRadius, clodMeshMetadata,
                rec.instanceIndex, instanceData.skinningInstanceSlot,
                rec.assemblyTransformIndex, nodeCullBounds)
            : CLOD_NODE_CULL_STATIC_BIND_POSE;
        CLodTelemetryNodeCullClassification(nodeCullClassification);
        const float3 nodeCullCenterObjectSpace = nodeCullBounds.sphere.xyz;
        const float nodeCullRadiusObjectSpace = nodeCullBounds.sphere.w;
        const float3 nodeCenterViewSpace = ToViewSpace(nodeCullCenterObjectSpace, objectModelMatrix, cullCamera.view);
        const float nodeRadiusWorld = nodeCullRadiusObjectSpace * cullUniformScale;
        const bool nodeCulled =
            CLodWorkGraphFrustumCullingEnabled() &&
            !replaySource &&
            UnpackBoundsTested(rec.nodeIdPacked) == 0u &&
            CLodNodeBoundsOutsideFrustum(
                nodeCullClassification,
                node.metric.cullCenterAndRadius,
                nodeCullBounds,
                objectModelMatrix,
                cullUniformScale,
                cullCamera);
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        const bool objectInvalidatedThisFrame = CLodVirtualShadowInstanceInvalidatedThisFrame(rec.instanceIndex);
        const bool dirtyPageCullingEnabled =
            !isSkinned && CLodWorkGraphShadowDirtyPageCullingEnabled() && !objectInvalidatedThisFrame;
#else
        const bool dirtyPageCullingEnabled = false;
#endif

        if (nodeCulled) {
            if (nodeCullClassification == CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS)
                WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_EXPLICIT_FRUSTUM_REJECTED, 1u);
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_CULLED_NODE_RECORDS, 1);
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

            CLodHandleRenderableLeaf(
                rec,
                parentAllowsRefine,
                clodMeshMetadata,
                node,
                objectModelMatrix,
                lodUniformScale,
                lodCam,
                lodCameraIsOrtho,
                cullCamera,
                forceLodDecision,
                instanceData,
                nodeTouchesDirtyPages,
                dirtyPageCullingEnabled,
                bucketRecord,
                n64,
                n32,
                n16,
                n8,
                n4,
                n2,
                n1,
                emitBucket);
        }
    }

    ThreadNodeOutputRecords<CLodClusterRunRecord> out64 = ClusterCull64.GetThreadNodeOutputRecords(n64);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out32 = ClusterCull32.GetThreadNodeOutputRecords(n32);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out16 = ClusterCull16.GetThreadNodeOutputRecords(n16);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out8  = ClusterCull8.GetThreadNodeOutputRecords(n8);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out4  = ClusterCull4.GetThreadNodeOutputRecords(n4);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out2  = ClusterCull2.GetThreadNodeOutputRecords(n2);
    ThreadNodeOutputRecords<CLodClusterRunRecord> out1  = ClusterCull1.GetThreadNodeOutputRecords(n1);

    if (emitBucket) {
        uint offset = UnpackClusterFirstIndex(bucketRecord.clusterIndexAndCount);

        for (uint i = 0; i < n64; i++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 64);
            out64[i] = r;
            offset += 64;
        }
        for (uint i32 = 0; i32 < n32; i32++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 32);
            out32[i32] = r;
            offset += 32;
        }
        for (uint i16 = 0; i16 < n16; i16++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 16);
            out16[i16] = r;
            offset += 16;
        }
        for (uint i8 = 0; i8 < n8; i8++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 8);
            out8[i8] = r;
            offset += 8;
        }
        for (uint i4 = 0; i4 < n4; i4++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 4);
            out4[i4] = r;
            offset += 4;
        }
        for (uint i2 = 0; i2 < n2; i2++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 2);
            out2[i2] = r;
            offset += 2;
        }
        for (uint i1 = 0; i1 < n1; i1++) {
            CLodClusterRunRecord r = bucketRecord;
            r.clusterIndexAndCount = PackClusterIndexAndCount(offset, 1);
            out1[i1] = r;
            offset += 1;
        }
    }

    out64.OutputComplete();
    out32.OutputComplete();
    out16.OutputComplete();
    out8.OutputComplete();
    out4.OutputComplete();
    out2.OutputComplete();
    out1.OutputComplete();
}
#endif
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

#define REYES_SEED_BATCH_MAX_CLUSTERS 8
#define REYES_SEED_BATCH_ACCUM_CAPACITY SW_BATCH_ACCUM_CAPACITY
#if CLOD_WG_ENABLE_REYES_VISIBILITY
groupshared uint gs_reyesSeedBatchIndices[REYES_SEED_BATCH_ACCUM_CAPACITY];
#endif

struct ReyesSeedBatchRecord
{
    uint3 dispatchGrid : SV_DispatchGrid;
    uint numClusters;
    uint clusterIndices[REYES_SEED_BATCH_MAX_CLUSTERS];
};

struct ReyesRasterBatchRecord
{
    uint3 dispatchGrid : SV_DispatchGrid;
    CLodReyesDiceQueueEntry diceEntry;
    uint diceQueueIndex;
    uint microTriangleOffset;
    uint microTriangleCount;
    uint pad0;
};

// Shared cluster-cull implementation called by each bucket-size variant.
// FIXED_LOOP_COUNT is the bucket size (1, 2, 4, 8, 16, 32, or 64) - all active lanes
// in a variant wave process the same number of iterations, minimizing WaveActiveMax divergence.
void ClusterCullBody(
    CLodClusterRunRecord b,
    bool hasBucket,
    bool countReplayBucketRecord,
    uint GI,
    uint inputCount,
    uint FIXED_LOOP_COUNT,
    out uint swPendingOut,
    out uint pageJobPendingOut,
    out uint reyesPendingOut)
{
    bool commonPageValid = false;
    CLodClusterPagePrefix commonPagePrefix = (CLodClusterPagePrefix)0;
    if (hasBucket && b.pageSlabDescriptorIndex != 0u)
    {
        commonPagePrefix =
            CLodLoadClusterPagePrefix(b.pageSlabDescriptorIndex, b.pageSlabByteOffset);
        const uint firstCluster = UnpackClusterFirstIndex(b.clusterIndexAndCount);
        const uint runClusterCount = UnpackClusterCount(b.clusterIndexAndCount);
        commonPageValid =
            commonPagePrefix.formatAndKind == CLOD_TRIANGLE_PAGE_MAGIC &&
            commonPagePrefix.descriptorOffset != 0u &&
            runClusterCount != 0u &&
            firstCluster < commonPagePrefix.clusterCount &&
            runClusterCount <= commonPagePrefix.clusterCount - firstCluster;
    }

    hasBucket = hasBucket && commonPageValid;

    // Telemetry (coalesced launch level)
    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_THREADS, 1);
    if (hasBucket) {
        const uint bucketMeshletCount = UnpackClusterCount(b.clusterIndexAndCount);
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
    uint meshVertexFlags = 0u;
    uint activeGroupScanCount = 0;
    float ownGroupErrorOverDistance = 0.0f;
    PerObjectBuffer instanceTransform = (PerObjectBuffer)0;
    CLodMeshMetadata clodMeshMetadata = (CLodMeshMetadata)0;
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
        instanceTransform = LoadInstanceTransformForDrawRecordWithAssemblyTransform(drawRecord, b.assemblyTransformIndex);
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

        // Culling only needs the common 16-byte page prefix. Skinning-bound
        // evaluation consumes boneIndexStreamOffset but none of the remaining
        // 48 bytes in CLodPageHeader, so avoid fetching them for every lane.
        pageHeader.formatAndKind = commonPagePrefix.formatAndKind;
        pageHeader.meshletCount = commonPagePrefix.clusterCount;
        pageHeader.descriptorOffset = commonPagePrefix.descriptorOffset;
        pageHeader.boneIndexStreamOffset = commonPagePrefix.boneIndexStreamOffset;
        pageMeshletCount = commonPagePrefix.clusterCount;
        pageDescriptorOffset = commonPagePrefix.descriptorOffset;

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
        meshVertexFlags = perMesh.vertexFlags;
#if !CLOD_WG_RIGID_ONLY
        if ((meshVertexFlags & VERTEX_SKINNED) != 0u)
        {
            skinningInstanceSlot = ResolveAssemblyProceduralWindSkinningSlot(
                b.instanceIndex, instanceData.skinningInstanceSlot, b.assemblyTransformIndex);
        }
#endif
        StructuredBuffer<MaterialInfo> materialDataBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
        const MaterialInfo materialInfo = materialDataBuffer[perMesh.materialDataIndex];
        isAlphaTestedMaterial = (materialInfo.materialFlags & MATERIAL_ALPHA_TEST) != 0u;
        const bool displacementEnabled = ReyesGeometricDisplacementEnabled(materialInfo);
        const float displacementSpan = max(0.0f, materialInfo.geometricDisplacementMax - materialInfo.geometricDisplacementMin);
        reyesDisplacementCandidate = displacementEnabled && displacementSpan > 1e-5f;
        StructuredBuffer<CullingCameraInfo> cameraInfos =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
        cullCam = cameraInfos[b.viewId];
        lodCam = cameraInfos[lodViewId];

        StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
        const MeshInstanceClodOffsets clodOff = LoadCLodOffsetsForDrawRecord(drawRecord);
        clodMeshMetadata = clodMeshMetadataBuffer[clodOff.clodMeshMetadataIndex];
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
                lodCam.viewZ, lodCam.zNear,
                lodCameraIsOrtho);
        }

        StructuredBuffer<CLodStreamingRuntimeState> runtimeState =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingRuntimeState)];
        activeGroupScanCount = runtimeState[0].activeGroupScanCount;
    }

    // Meshlet loop - fixed iteration count eliminates WaveActiveMax divergence.
    // Lanes with fewer meshlets (e.g. replay count=1) simply skip inactive iterations.
    const uint meshletCount = hasBucket ? UnpackClusterCount(b.clusterIndexAndCount) : 0;

    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> visibleClusterCounter =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState =
        ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    ConstantBuffer<PerFrameBuffer> perFrame =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
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
    uint reyesPending = 0; // WG Reyes seed batch accumulator count (wave-uniform)

    for (uint m = 0; m < FIXED_LOOP_COUNT; m++) {
        const bool active = (m < meshletCount) && pageValid;
        uint localMeshletIndex = 0;
        bool survives = false;
        float3 meshletCenterViewSpace = float3(0, 0, -1); // default: behind camera
        float3 meshletCenterWorld = 0.0f.xxx;
        float meshletRadiusWorld = 0.0f;
        bool meshletNeedsReyesDisplacement = reyesDisplacementCandidate;

        if (active) {
            const uint localMeshlet = UnpackClusterFirstIndex(b.clusterIndexAndCount) + m;

            if (localMeshlet < pageMeshletCount) {
                localMeshletIndex = localMeshlet;

                CLodClusterCullHeader clusterCullHeader;
                CLodMeshletDescriptor desc = (CLodMeshletDescriptor)0;
#if CLOD_WG_RIGID_ONLY
                const bool skinnedMesh = false;
#else
                const bool skinnedMesh = (meshVertexFlags & VERTEX_SKINNED) != 0u;
#endif
                if (skinnedMesh)
                {
                    desc = LoadMeshletDescriptor(
                        pageSlabDesc,
                        pageSlabOff,
                        pageDescriptorOffset,
                        localMeshlet);
                    clusterCullHeader = CLodMeshletCullHeader(desc);
                }
                else
                {
                    clusterCullHeader =
                        LoadMeshletCullHeader(pageSlabDesc, pageSlabOff, pageDescriptorOffset, localMeshlet);
                }
                uint meshletBoundsClassification = CLOD_MESHLET_BOUNDS_STATIC;
                BoundingSphere meshletBounds;
                const uint clusterCullFlags = CLodClusterCullFlags(clusterCullHeader);
                const bool animatedCluster =
                    skinnedMesh &&
                    (clusterCullFlags & CLOD_CLUSTER_CULL_FLAG_ANIMATED) != 0u;
                if (animatedCluster)
                {
                    if ((clusterCullFlags & CLOD_CLUSTER_CULL_FLAG_BONE_OVERFLOW) != 0u ||
                        !IsValidSkinningInstanceSlot(skinningInstanceSlot))
                    {
                        meshletBounds.sphere = clusterCullHeader.bounds;
                        meshletBoundsClassification = CLOD_MESHLET_BOUNDS_SKINNED_INVALID_SLOT_FALLBACK;
                        WGTelemetryAdd(WG_COUNTER_MESHLET_BOUNDS_SKINNED_INVALID_SLOT_FALLBACKS, 1u);
                    }
                    else
                    {
                        meshletBounds = CLodComputeSkinnedMeshletBounds(
                            desc,
                            pageHeader,
                            pageSlabDesc,
                            pageSlabOff,
                            skinningInstanceSlot,
                            clodMeshMetadata,
                            b.assemblyTransformIndex);
                        // The helper returns the descriptor sphere only when no remapped
                        // palette entry can be evaluated. Exact float4 equality is valid
                        // here because that fallback copies desc.bounds verbatim.
                        const bool usedBindFallback = all(meshletBounds.sphere == clusterCullHeader.bounds);
                        meshletBoundsClassification = usedBindFallback
                            ? CLOD_MESHLET_BOUNDS_SKINNED_NO_VALID_BONE_FALLBACK
                            : CLOD_MESHLET_BOUNDS_SKINNED_LIVE;
                        WGTelemetryAdd(
                            usedBindFallback
                                ? WG_COUNTER_MESHLET_BOUNDS_SKINNED_NO_VALID_BONE_FALLBACKS
                                : WG_COUNTER_MESHLET_BOUNDS_SKINNED_LIVE_EVALUATIONS,
                            1u);
                    }
                }
                else
                {
                    meshletBounds.sphere = clusterCullHeader.bounds;
                }
                meshletCenterViewSpace = ToViewSpace(meshletBounds.sphere.xyz, objectModelMatrix, viewMatrix);
                meshletCenterWorld = mul(float4(meshletBounds.sphere.xyz, 1.0f), objectModelMatrix).xyz;
                meshletRadiusWorld = meshletBounds.sphere.w * cullUniformScale;
                if (meshletNeedsReyesDisplacement &&
                    CLodReyesSphereFullyBeyondCutoff(
                        perFrame.heightFadeStartDistance,
                        perFrame.heightFadeEndDistance,
                        cullCam,
                        meshletCenterWorld,
                        meshletRadiusWorld))
                {
                    meshletNeedsReyesDisplacement = false;
                }
                survives =
                    !CLodWorkGraphFrustumCullingEnabled() ||
                    replaySource ||
                    meshletBoundsClassification == CLOD_MESHLET_BOUNDS_SKINNED_INVALID_SLOT_FALLBACK ||
                    meshletBoundsClassification == CLOD_MESHLET_BOUNDS_SKINNED_NO_VALID_BONE_FALLBACK ||
                    !SphereOutsideFrustumViewSpace(meshletCenterViewSpace, meshletRadiusWorld, frustumPlanes);
                
                if (!survives) {
                    if (meshletBoundsClassification == CLOD_MESHLET_BOUNDS_SKINNED_INVALID_SLOT_FALLBACK ||
                        meshletBoundsClassification == CLOD_MESHLET_BOUNDS_SKINNED_NO_VALID_BONE_FALLBACK)
                    {
                        WGTelemetryAdd(WG_COUNTER_MESHLET_BOUNDS_SKINNED_FALLBACK_FRUSTUM_REJECTED, 1u);
                    }
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
                    const int refinedGroupId =
                        (int)(clusterCullHeader.primitiveCountAndRefinedGroup >> 16u) - 1;
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
                        true,
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
                        // so use both the previous object transform and previous bone pose.
                        BoundingSphere previousMeshletBounds;
                        if (skinnedMesh)
                        {
                            previousMeshletBounds = CLodComputePreviousMeshletBounds(
                                desc,
                                pageHeader,
                                pageSlabDesc,
                                pageSlabOff,
                                meshVertexFlags,
                                skinningInstanceSlot,
                                clodMeshMetadata,
                                b.assemblyTransformIndex);
                        }
                        else
                        {
                            previousMeshletBounds.sphere = clusterCullHeader.bounds;
                        }
                        const row_major matrix prevModelMatrix = instanceTransform.prevModel;
                        const float prevMeshletScale = MaxAxisScale_RowVector(prevModelMatrix);
                        const float3 prevMeshletCenterViewSpace = ToViewSpace(previousMeshletBounds.sphere.xyz, prevModelMatrix, occCameras[b.viewId].prevView);
                        const float prevMeshletRadiusWorld = previousMeshletBounds.sphere.w * prevMeshletScale;
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
                                pageSlabOff,
                                b.assemblyTransformIndex);
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
                        visibleClusterTransformIndices,
                        hwGlobalBase + hwPrefix,
                        hwLaneAvail,
                        b.assemblyTransformIndex,
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
                    visibleClusterTransformIndices[hwGlobalBase + hwPrefix] = b.assemblyTransformIndex;
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
                            visibleClusterTransformIndices,
                            hwGlobalBase + hwPrefix,
                            hwLaneAvail,
                            b.assemblyTransformIndex,
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
                        visibleClusterTransformIndices[hwGlobalBase + hwPrefix] = b.assemblyTransformIndex;
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
        const bool outputReyes =
#if CLOD_WG_ENABLE_REYES_VISIBILITY
            contributes && meshletNeedsReyesDisplacement;
#else
            false;
#endif

        if (contributes && !meshletNeedsReyesDisplacement && (swRasterEnabled || pageJobEnabled)) {
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
        } else if (contributes && meshletNeedsReyesDisplacement && !outputReyes) {
            WGTelemetryAdd(WG_COUNTER_CLASSIFY_PJ_REJECT_REYES_DISPLACEMENT, 1);
        }

        const bool isHW = contributes && !isSW && !isPageJob && !outputReyes;
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
                            visibleClusterTransformIndices,
                            hwGlobalBase + hwPrefix,
                            hwLaneAvail,
                            b.assemblyTransformIndex,
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
                        visibleClusterTransformIndices[hwGlobalBase + hwPrefix] = b.assemblyTransformIndex;
                    }
                }
            }
        }

#if CLOD_WG_ENABLE_REYES_VISIBILITY
        // WG Reyes path: write visible clusters into the top-down SW-owned region so the
        // later HW raster compaction never sees them as regular hardware clusters.
        {
            const uint4 reyesMask = WaveActiveBallot(outputReyes);
            const uint reyesIterCount = CountBits128(reyesMask);
            totalSurvivors += reyesIterCount;
            uint reyesAvail = 0u;

            if (reyesIterCount > 0u) {
                const uint reyesLeader = WaveFirstLaneFromMask(reyesMask);
                const uint reyesRank = GetLaneRankInGroup(reyesMask, WaveGetLaneIndex());

                uint reyesBase = 0u;
                uint reyesCombinedBase = 0u;
                if (WaveGetLaneIndex() == reyesLeader) {
                    InterlockedAdd(replayState[0].visibleClusterCombinedCount, reyesIterCount, reyesCombinedBase);
                }
                reyesCombinedBase = WaveReadLaneAt(reyesCombinedBase, reyesLeader);

                reyesAvail =
                    (reyesCombinedBase < swVisibleClusterWriteCapacity)
                        ? min(reyesIterCount, swVisibleClusterWriteCapacity - reyesCombinedBase)
                        : 0u;

                if (WaveGetLaneIndex() == reyesLeader) {
                    InterlockedAdd(swVisibleClusterCounter[0], reyesAvail, reyesBase);
                }
                reyesBase = WaveReadLaneAt(reyesBase, reyesLeader);

                if (WaveGetLaneIndex() == reyesLeader && (reyesCombinedBase + reyesIterCount > swVisibleClusterWriteCapacity)) {
                    InterlockedMin(replayState[0].visibleClusterCombinedCount, swVisibleClusterWriteCapacity);
                }

                if (outputReyes && (reyesRank < reyesAvail)) {
                    const uint reyesIndex = visibleClusterCapacity - 1u - (swWriteBase + reyesBase + reyesRank);
                    CLodStoreVisibleClusterGloballyCoherent(
                        visibleClusters,
                        reyesIndex,
                        b.viewId,
                        b.instanceIndex,
                        localMeshletIndex,
                        visibleGroupId,
                        b.pageSlabDescriptorIndex,
                        b.pageSlabByteOffset,
                        shadowClipmapIndex);
                    visibleClusterTransformIndices[reyesIndex] = b.assemblyTransformIndex;
                    gs_reyesSeedBatchIndices[reyesPending + reyesRank] = reyesIndex;
                }
            }

            reyesPending += reyesAvail;
        }
#endif

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
                    visibleClusterTransformIndices[swIndex] = b.assemblyTransformIndex;

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
                    const uint3 descriptorPair = pageJobDescriptorBuffer[0].xyz;
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
                        RWStructuredBuffer<uint> pageJobVisibleClusterTransformIndices =
                            ResourceDescriptorHeap[descriptorPair.z];
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
                        pageJobVisibleClusterTransformIndices[pjIndex] = b.assemblyTransformIndex;
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
                        visibleClusterTransformIndices[pjIndex] = b.assemblyTransformIndex;

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

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT || CLOD_WG_ENABLE_REYES_VISIBILITY
    // SWRaster re-reads visibleClusters through UAV indirection in the same work graph.
    // The Work Graphs spec requires globallycoherent accesses plus a device-scope
    // barrier before the node invocation request for this producer-consumer pattern.
    Barrier(visibleClusters, DEVICE_SCOPE | GROUP_SYNC);
#endif

    swPendingOut = swPending;
    pageJobPendingOut = pageJobPending;
    reyesPendingOut = reyesPending;

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

#if CLOD_WG_ENABLE_REYES_VISIBILITY
#define CLOD_CLUSTER_CULL_REYES_PARAM(MAX_RECORDS) \
    [NodeID("ReyesSeed")] [MaxRecordsSharedWith(swRasterOutput)] NodeOutput<ReyesSeedBatchRecord> reyesSeedOutput,

#define CLOD_CLUSTER_CULL_REYES_EPILOGUE() \
    GroupMemoryBarrierWithGroupSync(); \
    const uint reyesNumBatches = (reyesPending + REYES_SEED_BATCH_MAX_CLUSTERS - 1) / REYES_SEED_BATCH_MAX_CLUSTERS; \
    GroupNodeOutputRecords<ReyesSeedBatchRecord> reyesBatchOut = \
        reyesSeedOutput.GetGroupNodeOutputRecords(reyesNumBatches); \
    if (GI == 0) { \
        for (uint reyesBatch = 0; reyesBatch < reyesNumBatches; reyesBatch++) { \
            const uint reyesBatchStart = reyesBatch * REYES_SEED_BATCH_MAX_CLUSTERS; \
            const uint reyesBatchSize = min(REYES_SEED_BATCH_MAX_CLUSTERS, reyesPending - reyesBatchStart); \
            reyesBatchOut[reyesBatch].dispatchGrid = uint3(reyesBatchSize, 1, 1); \
            reyesBatchOut[reyesBatch].numClusters = reyesBatchSize; \
            for (uint ri = 0; ri < reyesBatchSize; ri++) \
                reyesBatchOut[reyesBatch].clusterIndices[ri] = gs_reyesSeedBatchIndices[reyesBatchStart + ri]; \
        } \
    } \
    reyesBatchOut.OutputComplete()
#else
#define CLOD_CLUSTER_CULL_REYES_PARAM(MAX_RECORDS)
#define CLOD_CLUSTER_CULL_REYES_EPILOGUE()
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
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<CLodClusterRunRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 1 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 1 / PAGEJOB_BUILD_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_REYES_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 1 / REYES_SEED_BATCH_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    CLodClusterRunRecord b = (CLodClusterRunRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    uint reyesPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 1, swPending, pageJobPending, reyesPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
    CLOD_CLUSTER_CULL_REYES_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull2")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull2(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<CLodClusterRunRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 2 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 2 / PAGEJOB_BUILD_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_REYES_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 2 / REYES_SEED_BATCH_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    CLodClusterRunRecord b = (CLodClusterRunRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    uint reyesPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 2, swPending, pageJobPending, reyesPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
    CLOD_CLUSTER_CULL_REYES_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull4")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull4(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<CLodClusterRunRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 4 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 4 / PAGEJOB_BUILD_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_REYES_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 4 / REYES_SEED_BATCH_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    CLodClusterRunRecord b = (CLodClusterRunRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    uint reyesPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 4, swPending, pageJobPending, reyesPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
    CLOD_CLUSTER_CULL_REYES_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull8")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull8(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<CLodClusterRunRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 8 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 8 / PAGEJOB_BUILD_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_REYES_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 8 / REYES_SEED_BATCH_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    CLodClusterRunRecord b = (CLodClusterRunRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    uint reyesPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 8, swPending, pageJobPending, reyesPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
    CLOD_CLUSTER_CULL_REYES_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull16")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull16(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<CLodClusterRunRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 16 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 16 / PAGEJOB_BUILD_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_REYES_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 16 / REYES_SEED_BATCH_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    CLodClusterRunRecord b = (CLodClusterRunRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    uint reyesPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 16, swPending, pageJobPending, reyesPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
    CLOD_CLUSTER_CULL_REYES_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull32")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull32(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<CLodClusterRunRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 32 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 32 / PAGEJOB_BUILD_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_REYES_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 32 / REYES_SEED_BATCH_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    CLodClusterRunRecord b = (CLodClusterRunRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    uint reyesPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 32, swPending, pageJobPending, reyesPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
    CLOD_CLUSTER_CULL_REYES_EPILOGUE();
}

[Shader("node")]
[NodeID("ClusterCull64")]
[NodeLaunch("coalescing")]
[NumThreads(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP, 1, 1)]
void WG_ClusterCull64(
    [MaxRecords(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP)] GroupNodeInputRecords<CLodClusterRunRecord> inRecs,
    CLOD_CLUSTER_CULL_SW_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 64 / SW_BATCH_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_PAGEJOB_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 64 / PAGEJOB_BUILD_MAX_CLUSTERS)
    CLOD_CLUSTER_CULL_REYES_PARAM(CLUSTER_CULL_BUCKETS_THREADS_PER_GROUP * 64 / REYES_SEED_BATCH_MAX_CLUSTERS)
    uint GI : SV_GroupIndex)
{
    const uint inputCount = inRecs.Count();
    const bool hasBucket = GI < inputCount;
    CLodClusterRunRecord b = (CLodClusterRunRecord)0;
    if (hasBucket) b = inRecs[GI];
    uint swPending = 0;
    uint pageJobPending = 0;
    uint reyesPending = 0;
    ClusterCullBody(b, hasBucket, true, GI, inputCount, 64, swPending, pageJobPending, reyesPending);
    CLOD_CLUSTER_CULL_SW_EPILOGUE();
    CLOD_CLUSTER_CULL_PAGEJOB_EPILOGUE();
    CLOD_CLUSTER_CULL_REYES_EPILOGUE();
}

#if CLOD_WG_ENABLE_REYES_VISIBILITY
#define CLOD_REYES_PATCH_RASTER_HELPERS_ONLY 1
#include "ClusterLOD/reyesPatchRaster.hlsl"
#undef CLOD_REYES_PATCH_RASTER_HELPERS_ONLY

static const uint REYES_WG_SEED_THREADS = 64u;
static const uint REYES_WG_SPLIT_THREADS = 64u;
static const uint REYES_WG_DICE_THREADS = 8u;
static const uint REYES_WG_RASTER_THREADS = 16u;
static const uint REYES_WG_MAX_SOURCE_TRIANGLES_PER_CLUSTER = 128u;
static const uint REYES_WG_MAX_RASTER_WORK_ITEMS_PER_PATCH =
    (CLodReyesMaxVisibilityMicroTrianglesPerPatch + CLodReyesRasterBatchMicroTriangleCount - 1u) / CLodReyesRasterBatchMicroTriangleCount;

float3 WGReyesInterpolateTriangle(float3 p0, float3 p1, float3 p2, float3 barycentrics)
{
    precise float3 result = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
    return result;
}

uint3 WGReyesComputeSplitFactors(float3 edgeFactors)
{
    return clamp(
        uint3(ceil(edgeFactors / float(REYES_TESS_TABLE_MAX_SEGMENTS))),
        uint3(1u, 1u, 1u),
        uint3(REYES_TESS_TABLE_MAX_SEGMENTS, REYES_TESS_TABLE_MAX_SEGMENTS, REYES_TESS_TABLE_MAX_SEGMENTS));
}

CLodReyesDiceQueueEntry WGReyesMakeDiceEntry(CLodReyesSplitQueueEntry splitEntry, uint nextSplitLevel, uint quantizedTessFactor, uint tessTableConfigIndex)
{
    CLodReyesDiceQueueEntry diceEntry;
    diceEntry.visibleClusterIndex = splitEntry.visibleClusterIndex;
    diceEntry.instanceID = splitEntry.instanceID;
    diceEntry.localMeshletIndex = splitEntry.localMeshletIndex;
    diceEntry.materialIndex = splitEntry.materialIndex;
    diceEntry.viewID = splitEntry.viewID;
    diceEntry.splitLevel = nextSplitLevel;
    diceEntry.quantizedTessFactor = quantizedTessFactor;
    diceEntry.flags = splitEntry.flags;
    diceEntry.sourcePrimitiveAndSplitConfig = (splitEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu) | ((tessTableConfigIndex & 0xFFFFu) << 16u);
    diceEntry.domainVertex0UV = splitEntry.domainVertex0UV;
    diceEntry.domainVertex1UV = splitEntry.domainVertex1UV;
    diceEntry.domainVertex2UV = splitEntry.domainVertex2UV;
    diceEntry.tessTableConfigIndex = tessTableConfigIndex;
    diceEntry.reserved = 0u;
    return diceEntry;
}

bool WGReyesPatchHZBOccluded(
    float3 objectPosition0,
    float3 objectPosition1,
    float3 objectPosition2,
    float displacementMagnitude,
    PerObjectBuffer objectData,
    Camera camera,
    uint viewID,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer,
    bool diceStage)
{
    if (!CLodWorkGraphOcclusionEnabled() || camera.isOrtho) {
        return false;
    }

    StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
        ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
    const uint depthMapDescriptorIndex = viewDepthSRVIndices[viewID].linearDepthSRVIndex;
    if (depthMapDescriptorIndex == 0u) {
        return false;
    }

    const float3 objectCenter = (objectPosition0 + objectPosition1 + objectPosition2) * (1.0f / 3.0f);
    float objectRadius = max(
        length(objectPosition0 - objectCenter),
        max(length(objectPosition1 - objectCenter), length(objectPosition2 - objectCenter)));
    objectRadius += abs(displacementMagnitude);
    if (!all(isfinite(objectCenter)) || !isfinite(objectRadius) || objectRadius <= 0.0f) {
        return false;
    }

    const bool phase2 = CLodWorkGraphIsPhase2();
    const row_major matrix modelMatrix = phase2 ? objectData.model : objectData.prevModel;
    const row_major matrix viewMatrix = phase2 ? camera.view : camera.prevView;
    const row_major matrix projectionMatrix = phase2 ? camera.projection : camera.prevUnjitteredProjection;
    const float scaledRadius = objectRadius * MaxAxisScale_RowVector(modelMatrix);
    const float3 viewSpaceCenter = mul(mul(float4(objectCenter, 1.0f), modelMatrix), viewMatrix).xyz;
    const float boundingSphereDepth = -viewSpaceCenter.z;
    if (!all(isfinite(viewSpaceCenter)) || !isfinite(scaledRadius) || boundingSphereDepth <= scaledRadius) {
        return false;
    }

    bool occluded = false;
    if (diceStage) {
        InterlockedAdd(telemetryBuffer[0].diceOcclusionTestCount, 1u);
    } else {
        InterlockedAdd(telemetryBuffer[0].splitOcclusionTestCount, 1u);
    }
    OcclusionCullingPerspectiveTexture2D(
        occluded,
        camera,
        viewSpaceCenter,
        boundingSphereDepth,
        scaledRadius,
        depthMapDescriptorIndex,
        projectionMatrix);
    return occluded;
}

bool WGReyesDiceHZBOccluded(
    CLodReyesDiceQueueEntry diceEntry,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer)
{
    globallycoherent RWByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, diceEntry.visibleClusterIndex);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, diceEntry.localMeshletIndex);
    const uint sourceTriangleIndex = diceEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
    if (sourceTriangleIndex >= CLodDescTriangleCount(meshletDesc)) {
        return false;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
    const uint3 sourceTriangle = DecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, meshletDesc.triangleByteOffset, sourceTriangleIndex);
    const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
    const float3 sourcePosition0OS = DecodeCompressedPosition(
        sourceTriangle.x, positionBitstreamBase, meshletDesc.positionBitOffset,
        CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
        hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
    const float3 sourcePosition1OS = DecodeCompressedPosition(
        sourceTriangle.y, positionBitstreamBase, meshletDesc.positionBitOffset,
        CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
        hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
    const float3 sourcePosition2OS = DecodeCompressedPosition(
        sourceTriangle.z, positionBitstreamBase, meshletDesc.positionBitOffset,
        CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
        hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
    const float3 bary0 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex0UV);
    const float3 bary1 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex1UV);
    const float3 bary2 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex2UV);
    if (!ReyesPatchDomainHasValidSimplex(bary0, bary1, bary2)) {
        return false;
    }

    const float3 patchPosition0OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary0);
    const float3 patchPosition1OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary1);
    const float3 patchPosition2OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary2);
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const MaterialInfo materialInfo = materials[diceEntry.materialIndex];
    const float displacementMagnitude = ReyesGeometricDisplacementMagnitude(materialInfo);
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(diceEntry.instanceID);
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[diceEntry.viewID];
    return WGReyesPatchHZBOccluded(
        patchPosition0OS,
        patchPosition1OS,
        patchPosition2OS,
        displacementMagnitude,
        objectData,
        camera,
        diceEntry.viewID,
        telemetryBuffer,
        true);
}

[Shader("node")]
[NodeID("ReyesSeed")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(REYES_SEED_BATCH_MAX_CLUSTERS, 1, 1)]
[NumThreads(REYES_WG_SEED_THREADS, 1, 1)]
void WG_ReyesSeed(
    DispatchNodeInputRecord<ReyesSeedBatchRecord> inputRecord,
    [NodeID("ReyesSplit1")] [MaxRecords(CLodReyesMaxVisibilityMicroTrianglesPerPatch)] NodeOutput<CLodReyesSplitQueueEntry> splitOutput,
    uint GI : SV_GroupIndex,
    uint3 groupId : SV_GroupID)
{
    ReyesSeedBatchRecord batch = inputRecord.Get();
    const uint clusterSlot = groupId.x;
    uint triangleCount = 0u;
    uint visibleClusterIndex = 0u;
    uint instanceID = 0u;
    uint localMeshletIndex = 0u;
    uint materialIndex = 0u;
    uint viewID = 0u;
    uint flags = CLOD_REYES_ROUTE_VISIBILITY << CLOD_REYES_FLAG_ROUTE_SHIFT;

    if (clusterSlot < batch.numClusters) {
        visibleClusterIndex = batch.clusterIndices[clusterSlot];
        globallycoherent RWByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
        const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, visibleClusterIndex);
        instanceID = CLodVisibleClusterInstanceID(packedCluster);
        localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
        viewID = CLodVisibleClusterViewID(packedCluster);
        const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
        const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
        const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
        const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, localMeshletIndex);
        triangleCount = min(CLodDescTriangleCount(meshletDesc), REYES_WG_MAX_SOURCE_TRIANGLES_PER_CLUSTER);

        const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(instanceID);
        StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
        materialIndex = perMesh.materialDataIndex;
        if ((perMesh.vertexFlags & VERTEX_SKINNED) != 0u) {
            flags |= CLOD_REYES_FLAG_SKINNED;
        }
        flags |= CLOD_REYES_FLAG_DISPLACEMENT_ENABLED;
    }

    GroupNodeOutputRecords<CLodReyesSplitQueueEntry> splitOut = splitOutput.GetGroupNodeOutputRecords(triangleCount);
    for (uint triangleIndex = GI; triangleIndex < triangleCount; triangleIndex += REYES_WG_SEED_THREADS) {
        CLodReyesSplitQueueEntry splitEntry;
        splitEntry.visibleClusterIndex = visibleClusterIndex;
        splitEntry.instanceID = instanceID;
        splitEntry.localMeshletIndex = localMeshletIndex;
        splitEntry.materialIndex = materialIndex;
        splitEntry.viewID = viewID;
        splitEntry.splitLevel = 0u;
        splitEntry.quantizedTessFactor = 0u;
        splitEntry.flags = flags;
        splitEntry.sourcePrimitiveAndSplitConfig = triangleIndex & 0xFFFFu;
        splitEntry.domainVertex0UV = float2(0.0f, 0.0f);
        splitEntry.domainVertex1UV = float2(1.0f, 0.0f);
        splitEntry.domainVertex2UV = float2(0.0f, 1.0f);
        splitOut[triangleIndex] = splitEntry;
    }
    splitOut.OutputComplete();
}

void WGReyesSplitCommon(
    CLodReyesSplitQueueEntry splitEntry,
    uint stageIndex,
    bool forceDice,
    NodeOutput<CLodReyesSplitQueueEntry> nextSplitOutput,
    NodeOutput<CLodReyesDiceQueueEntry> diceOutput,
    uint GI)
{
    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_WG_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableVertices = ResourceDescriptorHeap[CLOD_WG_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableTriangles = ResourceDescriptorHeap[CLOD_WG_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX];
    globallycoherent RWByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, splitEntry.visibleClusterIndex);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, splitEntry.localMeshletIndex);
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
    const uint sourceTriangleIndex = splitEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
    bool valid = sourceTriangleIndex < CLodDescTriangleCount(meshletDesc);

    uint childCount = 0u;
    bool routeToDice = true;
    bool deferredSplitReplay = false;
    CLodReyesDiceQueueEntry diceEntry = (CLodReyesDiceQueueEntry)0;
    float2 childDomain0UV[CLodReyesMaxVisibilityMicroTrianglesPerPatch];
    float2 childDomain1UV[CLodReyesMaxVisibilityMicroTrianglesPerPatch];
    float2 childDomain2UV[CLodReyesMaxVisibilityMicroTrianglesPerPatch];

    if (valid) {
        const uint3 sourceTriangle = DecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, meshletDesc.triangleByteOffset, sourceTriangleIndex);
        const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
        const float3 sourcePosition0OS = DecodeCompressedPosition(
            sourceTriangle.x, positionBitstreamBase, meshletDesc.positionBitOffset,
            CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
            hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
        const float3 sourcePosition1OS = DecodeCompressedPosition(
            sourceTriangle.y, positionBitstreamBase, meshletDesc.positionBitOffset,
            CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
            hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
        const float3 sourcePosition2OS = DecodeCompressedPosition(
            sourceTriangle.z, positionBitstreamBase, meshletDesc.positionBitOffset,
            CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
            hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
        const float3 bary0 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex0UV);
        const float3 bary1 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex1UV);
        const float3 bary2 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex2UV);
        valid = ReyesPatchDomainHasValidSimplex(bary0, bary1, bary2);

        if (valid) {
            const float3 currentPosition0OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary0);
            const float3 currentPosition1OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary1);
            const float3 currentPosition2OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary2);
            const PerObjectBuffer objectData = LoadInstanceTransformForDraw(splitEntry.instanceID);
            const float3 currentPosition0WS = mul(float4(currentPosition0OS, 1.0f), objectData.model).xyz;
            const float3 currentPosition1WS = mul(float4(currentPosition1OS, 1.0f), objectData.model).xyz;
            const float3 currentPosition2WS = mul(float4(currentPosition2OS, 1.0f), objectData.model).xyz;
            StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
            const CullingCameraInfo cullingCamera = cameras[splitEntry.viewID];
            StructuredBuffer<Camera> sceneCameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
            const Camera sceneCamera = sceneCameras[splitEntry.viewID];
            StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
            const MaterialInfo materialInfo = materials[splitEntry.materialIndex];
            const float displacementMagnitude = ReyesGeometricDisplacementMagnitude(materialInfo);
            if (!CLodWorkGraphIsPhase2() &&
                WGReyesPatchHZBOccluded(
                    currentPosition0OS,
                    currentPosition1OS,
                    currentPosition2OS,
                    displacementMagnitude,
                    objectData,
                    sceneCamera,
                    splitEntry.viewID,
                    telemetryBuffer,
                    false))
            {
                deferredSplitReplay = true;
                valid = false;
            }
            if (valid) {
                const float3 edgeFactors = ReyesComputeVisibilityEdgeTessFactors(currentPosition0WS, currentPosition1WS, currentPosition2WS, cullingCamera);
                const float maxEdgeFactor = max(edgeFactors.x, max(edgeFactors.y, edgeFactors.z));
                const uint nextSplitLevel = splitEntry.splitLevel + 1u;
                const uint nextQuantizedTessFactor = (uint)min(65535.0f, ceil(maxEdgeFactor * 256.0f));

                float2 diceDomain0UV = splitEntry.domainVertex0UV;
                float2 diceDomain1UV = splitEntry.domainVertex1UV;
                float2 diceDomain2UV = splitEntry.domainVertex2UV;
                uint diceTessConfigIndex = ReyesEncodeCanonicalTessTableConfig(edgeFactors, diceDomain0UV, diceDomain1UV, diceDomain2UV);
                CLodReyesSplitQueueEntry diceSplitEntry = splitEntry;
                diceSplitEntry.domainVertex0UV = diceDomain0UV;
                diceSplitEntry.domainVertex1UV = diceDomain1UV;
                diceSplitEntry.domainVertex2UV = diceDomain2UV;
                routeToDice = forceDice || maxEdgeFactor <= float(REYES_TESS_TABLE_MAX_SEGMENTS);
                diceEntry = WGReyesMakeDiceEntry(diceSplitEntry, nextSplitLevel, nextQuantizedTessFactor, diceTessConfigIndex);

                if (!routeToDice) {
                    const uint3 splitFactors = WGReyesComputeSplitFactors(edgeFactors);
                    float2 splitDomain0UV = splitEntry.domainVertex0UV;
                    float2 splitDomain1UV = splitEntry.domainVertex1UV;
                    float2 splitDomain2UV = splitEntry.domainVertex2UV;
                    const uint splitConfigIndex = ReyesEncodeCanonicalTessTableConfig(float3(splitFactors), splitDomain0UV, splitDomain1UV, splitDomain2UV);
                    const CLodReyesTessTableConfigEntry splitConfig = ReyesGetTessTableConfigEntry(tessTableConfigs, splitConfigIndex);
                    childCount = min(splitConfig.numTriangles, CLodReyesMaxVisibilityMicroTrianglesPerPatch);
                    const float3 parentDomain0 = ReyesPatchDomainUVToBarycentrics(splitDomain0UV);
                    const float3 parentDomain1 = ReyesPatchDomainUVToBarycentrics(splitDomain1UV);
                    const float3 parentDomain2 = ReyesPatchDomainUVToBarycentrics(splitDomain2UV);
                    for (uint childIndex = 0u; childIndex < childCount; ++childIndex) {
                        const uint3 triIndices = ReyesGetTessTableConfigTriangleVertexIndices(tessTableConfigs, tessTableTriangles, splitConfigIndex, childIndex);
                        const float3 microBary0 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.x);
                        const float3 microBary1 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.y);
                        const float3 microBary2 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.z);
                        precise float3 childDomain0 = parentDomain0 * microBary0.x + parentDomain1 * microBary0.y + parentDomain2 * microBary0.z;
                        precise float3 childDomain1 = parentDomain0 * microBary1.x + parentDomain1 * microBary1.y + parentDomain2 * microBary1.z;
                        precise float3 childDomain2 = parentDomain0 * microBary2.x + parentDomain1 * microBary2.y + parentDomain2 * microBary2.z;
                        childDomain0UV[childIndex] = ReyesPatchBarycentricsToUV(childDomain0);
                        childDomain1UV[childIndex] = ReyesPatchBarycentricsToUV(childDomain1);
                        childDomain2UV[childIndex] = ReyesPatchBarycentricsToUV(childDomain2);
                    }
                    InterlockedAdd(telemetryBuffer[0].splitChildOutputCounts[min(stageIndex, 4u)], childCount);
                }
            }
        }
    }

    const uint diceCount = (valid && routeToDice) ? 1u : 0u;
    const uint splitCount = (valid && !routeToDice) ? childCount : 0u;
    GroupNodeOutputRecords<CLodReyesDiceQueueEntry> diceOut = diceOutput.GetGroupNodeOutputRecords(diceCount);
    GroupNodeOutputRecords<CLodReyesSplitQueueEntry> splitOut = nextSplitOutput.GetGroupNodeOutputRecords(splitCount);
    if (GI == 0u) {
        if (deferredSplitReplay) {
            ReplayTryAppendReyesSplit(splitEntry);
        }
        if (diceCount != 0u) {
            diceOut[0] = diceEntry;
            InterlockedAdd(telemetryBuffer[0].splitDiceOutputCounts[min(stageIndex, 4u)], 1u);
            InterlockedAdd(telemetryBuffer[0].finalDiceQueueEntryCount, 1u);
        }
        for (uint childIndex = 0u; childIndex < splitCount; ++childIndex) {
            CLodReyesSplitQueueEntry childEntry = splitEntry;
            childEntry.splitLevel = splitEntry.splitLevel + 1u;
            childEntry.domainVertex0UV = childDomain0UV[childIndex];
            childEntry.domainVertex1UV = childDomain1UV[childIndex];
            childEntry.domainVertex2UV = childDomain2UV[childIndex];
            splitOut[childIndex] = childEntry;
        }
    }
    diceOut.OutputComplete();
    splitOut.OutputComplete();
}

#define DECLARE_REYES_SPLIT_NODE(FUNC, NODE_ID, NEXT_ID, STAGE, FORCE_DICE) \
[Shader("node")] \
[NodeID(NODE_ID)] \
[NodeLaunch("broadcasting")] \
[NodeDispatchGrid(1, 1, 1)] \
[NumThreads(REYES_WG_SPLIT_THREADS, 1, 1)] \
void FUNC( \
    DispatchNodeInputRecord<CLodReyesSplitQueueEntry> inputRecord, \
    [NodeID(NEXT_ID)] [MaxRecords(CLodReyesMaxVisibilityMicroTrianglesPerPatch)] NodeOutput<CLodReyesSplitQueueEntry> nextSplitOutput, \
    [NodeID("ReyesDice")] [MaxRecordsSharedWith(nextSplitOutput)] NodeOutput<CLodReyesDiceQueueEntry> diceOutput, \
    uint GI : SV_GroupIndex) \
{ \
    WGReyesSplitCommon(inputRecord.Get(), STAGE, FORCE_DICE, nextSplitOutput, diceOutput, GI); \
}

DECLARE_REYES_SPLIT_NODE(WG_ReyesSplit1, "ReyesSplit1", "ReyesSplit2", 0u, false)
DECLARE_REYES_SPLIT_NODE(WG_ReyesSplit2, "ReyesSplit2", "ReyesSplit3", 1u, false)
DECLARE_REYES_SPLIT_NODE(WG_ReyesSplit3, "ReyesSplit3", "ReyesSplit4", 2u, false)
DECLARE_REYES_SPLIT_NODE(WG_ReyesSplit4, "ReyesSplit4", "ReyesSplit5", 3u, false)

[Shader("node")]
[NodeID("ReyesSplit5")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(REYES_WG_SPLIT_THREADS, 1, 1)]
void WG_ReyesSplit5(
    DispatchNodeInputRecord<CLodReyesSplitQueueEntry> inputRecord,
    [NodeID("ReyesDice")] [MaxRecords(1)] NodeOutput<CLodReyesDiceQueueEntry> diceOutput,
    uint GI : SV_GroupIndex)
{
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX];
    globallycoherent RWByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];

    CLodReyesSplitQueueEntry splitEntry = inputRecord.Get();
    const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, splitEntry.visibleClusterIndex);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, splitEntry.localMeshletIndex);
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
    const uint sourceTriangleIndex = splitEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
    bool valid = sourceTriangleIndex < CLodDescTriangleCount(meshletDesc);
    CLodReyesDiceQueueEntry diceEntry = (CLodReyesDiceQueueEntry)0;

    if (valid) {
        const uint3 sourceTriangle = DecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, meshletDesc.triangleByteOffset, sourceTriangleIndex);
        const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
        const float3 sourcePosition0OS = DecodeCompressedPosition(
            sourceTriangle.x, positionBitstreamBase, meshletDesc.positionBitOffset,
            CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
            hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
        const float3 sourcePosition1OS = DecodeCompressedPosition(
            sourceTriangle.y, positionBitstreamBase, meshletDesc.positionBitOffset,
            CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
            hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
        const float3 sourcePosition2OS = DecodeCompressedPosition(
            sourceTriangle.z, positionBitstreamBase, meshletDesc.positionBitOffset,
            CLodDescBitsX(meshletDesc), CLodDescBitsY(meshletDesc), CLodDescBitsZ(meshletDesc),
            hdr.compressedPositionQuantExp, int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz), pageSlabDescriptorIndex);
        const float3 bary0 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex0UV);
        const float3 bary1 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex1UV);
        const float3 bary2 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex2UV);
        valid = ReyesPatchDomainHasValidSimplex(bary0, bary1, bary2);

        if (valid) {
            const float3 currentPosition0OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary0);
            const float3 currentPosition1OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary1);
            const float3 currentPosition2OS = WGReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary2);
            const PerObjectBuffer objectData = LoadInstanceTransformForDraw(splitEntry.instanceID);
            const float3 currentPosition0WS = mul(float4(currentPosition0OS, 1.0f), objectData.model).xyz;
            const float3 currentPosition1WS = mul(float4(currentPosition1OS, 1.0f), objectData.model).xyz;
            const float3 currentPosition2WS = mul(float4(currentPosition2OS, 1.0f), objectData.model).xyz;
            StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
            const CullingCameraInfo camera = cameras[splitEntry.viewID];
            const float3 edgeFactors = ReyesComputeVisibilityEdgeTessFactors(currentPosition0WS, currentPosition1WS, currentPosition2WS, camera);
            const float maxEdgeFactor = max(edgeFactors.x, max(edgeFactors.y, edgeFactors.z));
            const uint nextSplitLevel = splitEntry.splitLevel + 1u;
            const uint nextQuantizedTessFactor = (uint)min(65535.0f, ceil(maxEdgeFactor * 256.0f));
            float2 diceDomain0UV = splitEntry.domainVertex0UV;
            float2 diceDomain1UV = splitEntry.domainVertex1UV;
            float2 diceDomain2UV = splitEntry.domainVertex2UV;
            const uint diceTessConfigIndex = ReyesEncodeCanonicalTessTableConfig(edgeFactors, diceDomain0UV, diceDomain1UV, diceDomain2UV);
            CLodReyesSplitQueueEntry diceSplitEntry = splitEntry;
            diceSplitEntry.domainVertex0UV = diceDomain0UV;
            diceSplitEntry.domainVertex1UV = diceDomain1UV;
            diceSplitEntry.domainVertex2UV = diceDomain2UV;
            diceEntry = WGReyesMakeDiceEntry(diceSplitEntry, nextSplitLevel, nextQuantizedTessFactor, diceTessConfigIndex);
        }
    }

    GroupNodeOutputRecords<CLodReyesDiceQueueEntry> diceOut = diceOutput.GetGroupNodeOutputRecords(valid ? 1u : 0u);
    if (GI == 0u && valid) {
        diceOut[0] = diceEntry;
        InterlockedAdd(telemetryBuffer[0].splitDiceOutputCounts[4], 1u);
        InterlockedAdd(telemetryBuffer[0].finalDiceQueueEntryCount, 1u);
    }
    diceOut.OutputComplete();
}

[Shader("node")]
[NodeID("ReyesDice")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(REYES_WG_DICE_THREADS, 1, 1)]
void WG_ReyesDice(
    DispatchNodeInputRecord<CLodReyesDiceQueueEntry> inputRecord,
    [NodeID("ReyesRaster")] [MaxRecords(REYES_WG_MAX_RASTER_WORK_ITEMS_PER_PATCH)] NodeOutput<ReyesRasterBatchRecord> rasterOutput,
    uint GI : SV_GroupIndex)
{
    CLodReyesDiceQueueEntry diceEntry = inputRecord.Get();
    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_WG_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[CLOD_WG_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> diceQueueCounter = ResourceDescriptorHeap[CLOD_WG_REYES_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> diceQueueOverflow = ResourceDescriptorHeap[CLOD_WG_REYES_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX];

    const bool deferForReplay =
        !CLodWorkGraphIsPhase2() &&
        WGReyesDiceHZBOccluded(diceEntry, telemetryBuffer);
    if (deferForReplay) {
        GroupNodeOutputRecords<ReyesRasterBatchRecord> rasterOut = rasterOutput.GetGroupNodeOutputRecords(0u);
        if (GI == 0u) {
            ReplayTryAppendReyesDice(diceEntry);
        }
        rasterOut.OutputComplete();
        return;
    }

    const uint microTriangleCount = ReyesGetDicePatchMicroTriangleCount(tessTableConfigs, diceEntry);
    const uint rasterBatchCount = (microTriangleCount + CLodReyesRasterBatchMicroTriangleCount - 1u) / CLodReyesRasterBatchMicroTriangleCount;
    uint diceQueueIndex = 0u;
    if (GI == 0u) {
        InterlockedAdd(diceQueueCounter[0], 1u, diceQueueIndex);
        if (diceQueueIndex < CLOD_WG_REYES_DICE_QUEUE_CAPACITY) {
            diceQueue[diceQueueIndex] = diceEntry;
        } else {
            InterlockedAdd(diceQueueOverflow[0], 1u);
        }
    }
    diceQueueIndex = WaveReadLaneFirst(diceQueueIndex);
    const bool validDice = diceQueueIndex < CLOD_WG_REYES_DICE_QUEUE_CAPACITY && microTriangleCount != 0u;
    GroupNodeOutputRecords<ReyesRasterBatchRecord> rasterOut =
        rasterOutput.GetGroupNodeOutputRecords(validDice ? rasterBatchCount : 0u);
    for (uint batchIndex = GI; batchIndex < rasterBatchCount && validDice; batchIndex += REYES_WG_DICE_THREADS) {
        ReyesRasterBatchRecord rec;
        rec.dispatchGrid = uint3(1, 1, 1);
        rec.diceEntry = diceEntry;
        rec.diceQueueIndex = diceQueueIndex;
        rec.microTriangleOffset = batchIndex * CLodReyesRasterBatchMicroTriangleCount;
        rec.microTriangleCount = min(CLodReyesRasterBatchMicroTriangleCount, microTriangleCount - rec.microTriangleOffset);
        rec.pad0 = 0u;
        rasterOut[batchIndex] = rec;
    }
    if (GI == 0u && validDice) {
        InterlockedAdd(telemetryBuffer[0].patchRasterizedPatchCount, 1u);
        InterlockedAdd(telemetryBuffer[0].rasterWorkEntryCount, rasterBatchCount);
    }
    rasterOut.OutputComplete();
}

[Shader("node")]
[NodeID("ReyesRaster")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(REYES_WG_RASTER_THREADS, 1, 1)]
void WG_ReyesRaster(
    DispatchNodeInputRecord<ReyesRasterBatchRecord> inputRecord,
    uint GI : SV_GroupIndex)
{
    ReyesRasterBatchRecord rec = inputRecord.Get();
    if (GI >= rec.microTriangleCount) {
        return;
    }

    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_WG_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableVertices = ResourceDescriptorHeap[CLOD_WG_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableTriangles = ResourceDescriptorHeap[CLOD_WG_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_WG_REYES_TELEMETRY_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    globallycoherent RWByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    CLodReyesDiceQueueEntry diceEntry = rec.diceEntry;
    uint viewRasterInfoCount = 0u;
    uint viewRasterInfoStride = 0u;
    viewRasterInfoBuffer.GetDimensions(viewRasterInfoCount, viewRasterInfoStride);
    if (diceEntry.viewID >= viewRasterInfoCount) {
        return;
    }
    const ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[diceEntry.viewID];
    if (viewRasterInfo.visibilityUAVDescriptorIndex == 0xFFFFFFFFu) {
        return;
    }

    const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, diceEntry.visibleClusterIndex);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, diceEntry.localMeshletIndex);
    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(diceEntry.instanceID);
    const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(diceEntry.instanceID);
    const CullingCameraInfo camera = cameras[diceEntry.viewID];
    const MaterialInfo materialInfo = materials[perMesh.materialDataIndex];
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
    const uint sourceTriangleIndex = diceEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
    if (sourceTriangleIndex >= CLodDescTriangleCount(meshletDesc)) {
        return;
    }

    const uint3 sourceTriangle = DecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, meshletDesc.triangleByteOffset, sourceTriangleIndex);
    const float3 sourcePosition0 = DecodeSkinnedPosition(sourceTriangle.x, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const float3 sourcePosition1 = DecodeSkinnedPosition(sourceTriangle.y, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const float3 sourcePosition2 = DecodeSkinnedPosition(sourceTriangle.z, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const bool displacementEnabled =
        ReyesGeometricDisplacementEnabled(materialInfo) &&
        materialInfo.heightUvSetIndex < hdr.uvSetCount;
    float3 sourceNormal0 = float3(0.0f, 0.0f, 1.0f);
    float3 sourceNormal1 = float3(0.0f, 0.0f, 1.0f);
    float3 sourceNormal2 = float3(0.0f, 0.0f, 1.0f);
    float2 sourceUv0 = float2(0.0f, 0.0f);
    float2 sourceUv1 = float2(0.0f, 0.0f);
    float2 sourceUv2 = float2(0.0f, 0.0f);
    if (displacementEnabled) {
        sourceNormal0 = DecodeSkinnedNormal(sourceTriangle.x, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceNormal1 = DecodeSkinnedNormal(sourceTriangle.y, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceNormal2 = DecodeSkinnedNormal(sourceTriangle.z, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceUv0 = DecodeCompressedUV(sourceTriangle.x, materialInfo.heightUvSetIndex, hdr, meshletDesc, diceEntry.localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
        sourceUv1 = DecodeCompressedUV(sourceTriangle.y, materialInfo.heightUvSetIndex, hdr, meshletDesc, diceEntry.localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
        sourceUv2 = DecodeCompressedUV(sourceTriangle.z, materialInfo.heightUvSetIndex, hdr, meshletDesc, diceEntry.localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
    }

    const float3 domain0 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex0UV);
    const float3 domain1 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex1UV);
    const float3 domain2 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex2UV);
    const uint microTriangleIndex = rec.microTriangleOffset + GI;
    const uint microTriangleCount = ReyesGetDicePatchMicroTriangleCount(tessTableConfigs, diceEntry);
    if (microTriangleIndex >= microTriangleCount) {
        return;
    }

    row_major matrix modelViewProjection = mul(objectData.model, camera.viewProjection);
    float4 modelViewZ = mul(objectData.model, camera.viewZ);
    const float patchDepth = max(
        (-dot(float4(sourcePosition0, 1.0f), modelViewZ)
        + -dot(float4(sourcePosition1, 1.0f), modelViewZ)
        + -dot(float4(sourcePosition2, 1.0f), modelViewZ)) / 3.0f,
        max(camera.zNear, 1.0e-3f));

    float3 patchBary0;
    float3 patchBary1;
    float3 patchBary2;
    ReyesDecodeMicroTrianglePatchDomain(tessTableConfigs, tessTableVertices, tessTableTriangles, microTriangleIndex, diceEntry, patchBary0, patchBary1, patchBary2);

    float3 sourceBary0;
    float3 sourceBary1;
    float3 sourceBary2;
    float3 patchPosition0;
    float3 patchPosition1;
    float3 patchPosition2;
    ReyesEvaluateDisplacedPatchTriangle(
        materialInfo,
        displacementEnabled,
        camera,
        perFrame.heightFadeStartDistance,
        perFrame.heightFadeEndDistance,
        objectData.model,
        patchDepth,
        sourcePosition0,
        sourcePosition1,
        sourcePosition2,
        sourceNormal0,
        sourceNormal1,
        sourceNormal2,
        sourceUv0,
        sourceUv1,
        sourceUv2,
        domain0,
        domain1,
        domain2,
        patchBary0,
        patchBary1,
        patchBary2,
        sourceBary0,
        sourceBary1,
        sourceBary2,
        patchPosition0,
        patchPosition1,
        patchPosition2);

    const float4 clip0 = mul(float4(patchPosition0, 1.0f), modelViewProjection);
    const float4 clip1 = mul(float4(patchPosition1, 1.0f), modelViewProjection);
    const float4 clip2 = mul(float4(patchPosition2, 1.0f), modelViewProjection);
    const float depth0 = -dot(float4(patchPosition0, 1.0f), modelViewZ);
    const float depth1 = -dot(float4(patchPosition1, 1.0f), modelViewZ);
    const float depth2 = -dot(float4(patchPosition2, 1.0f), modelViewZ);
    const uint patchVisibilityIndex = CLOD_WG_VISIBLE_CLUSTERS_CAPACITY + rec.diceQueueIndex;
    RWTexture2D<uint64_t> visBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(viewRasterInfo.visibilityUAVDescriptorIndex)];
    uint2 visDims;
    visBuffer.GetDimensions(visDims.x, visDims.y);
    ReyesRasterizeMicroTriangle(
        visBuffer,
        visDims,
        telemetryBuffer,
        viewRasterInfo,
        clip0,
        clip1,
        clip2,
        depth0,
        depth1,
        depth2,
        camera.zNear,
        patchVisibilityIndex,
        microTriangleIndex);
    InterlockedAdd(telemetryBuffer[0].patchRasterizedMicroTriangleCount, 1u);
}

[Shader("node")]
[NodeID("ReyesSplitReplay")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NodeIsProgramEntry]
[NumThreads(REYES_WG_SPLIT_THREADS, 1, 1)]
void WG_ReyesSplitReplay(
    DispatchNodeInputRecord<CLodReyesSplitQueueEntry> inputRecord,
    [NodeID("ReyesSplit1")] [MaxRecords(1)] NodeOutput<CLodReyesSplitQueueEntry> splitOutput,
    uint GI : SV_GroupIndex)
{
    GroupNodeOutputRecords<CLodReyesSplitQueueEntry> splitOut = splitOutput.GetGroupNodeOutputRecords(1u);
    if (GI == 0u) {
        splitOut[0] = inputRecord.Get();
    }
    splitOut.OutputComplete();
}

[Shader("node")]
[NodeID("ReyesDiceReplay")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NodeIsProgramEntry]
[NumThreads(REYES_WG_DICE_THREADS, 1, 1)]
void WG_ReyesDiceReplay(
    DispatchNodeInputRecord<CLodReyesDiceQueueEntry> inputRecord,
    [NodeID("ReyesDice")] [MaxRecords(1)] NodeOutput<CLodReyesDiceQueueEntry> diceOutput,
    uint GI : SV_GroupIndex)
{
    GroupNodeOutputRecords<CLodReyesDiceQueueEntry> diceOut = diceOutput.GetGroupNodeOutputRecords(1u);
    if (GI == 0u) {
        diceOut[0] = inputRecord.Get();
    }
    diceOut.OutputComplete();
}
#endif

#endif

#if CLOD_WG_ENABLE_SW_NODE_OUTPUT
#define CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST 1
#include "ClusterLOD/softwareRaster.hlsl"
#undef CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#include "ClusterLOD/pageJobRaster.hlsl"
#endif
#endif
