#ifndef RHI_RTAS_ARGS_HLSLI
#define RHI_RTAS_ARGS_HLSLI

// GPU argument layouts for acceleration structure operations. D3D12 and the Vulkan NV extensions disagree on
// these, so shaders write through this header. rhi.h carries the same layouts: PackedDxr2* for D3D12 (asserted
// against d3d12.h in rhi_dx12.cpp) and PackedRayTracing* for Vulkan.

#define RTAS_INDEX_FORMAT_UINT8 1u
#define RTAS_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT (1u << 30u)
#define RTAS_GEOMETRY_FLAG_OPAQUE (1u << 31u)
#define RTAS_CLUSTER_FLAG_ALLOW_DISABLE_OMMS 0x1u

// Special opacity micromap indices (two's complement, as written into a 32-bit OMM index buffer).
#define RTAS_OMM_SPECIAL_INDEX_FULLY_TRANSPARENT 0xFFFFFFFFu         // -1
#define RTAS_OMM_SPECIAL_INDEX_FULLY_OPAQUE 0xFFFFFFFEu              // -2
#define RTAS_OMM_SPECIAL_INDEX_FULLY_UNKNOWN_TRANSPARENT 0xFFFFFFFDu // -3
#define RTAS_OMM_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE 0xFFFFFFFCu      // -4
#define RTAS_OMM_SPECIAL_INDEX_CLUSTER_SKIP_OMM 0xFFFFFFFBu          // -5: this triangle uses the geometry opaque flag

#if defined(BASICRENDERER_SHADER_API_VULKAN)

struct RtasClusterBuildTriangleInfo
{
    uint clusterID;
    uint clusterFlags;
    uint countsAndFormats;
    uint baseGeometryIndexAndFlags;
    uint indexAndVertexBufferStride;
    uint geometryAndOpacityStride;
    uint64_t indexBuffer;
    uint64_t vertexBuffer;
    uint64_t geometryIndexAndFlagsBuffer;
    uint64_t opacityMicromapArray;
    uint64_t opacityMicromapIndexBuffer;
};

RtasClusterBuildTriangleInfo RtasMakeClusterBuildTriangleInfo(
    uint clusterID, uint triangleCount, uint vertexCount, uint geometryFlags, uint vertexStride, uint indexStride,
    uint64_t vertexBuffer, uint64_t indexBuffer)
{
    RtasClusterBuildTriangleInfo info = (RtasClusterBuildTriangleInfo)0;
    info.clusterID = clusterID;
    info.countsAndFormats = (triangleCount & 0x1FFu) | ((vertexCount & 0x1FFu) << 9u) | (RTAS_INDEX_FORMAT_UINT8 << 24u);
    info.baseGeometryIndexAndFlags = geometryFlags;
    info.indexAndVertexBufferStride = indexStride | (vertexStride << 16u);
    info.indexBuffer = indexBuffer;
    info.vertexBuffer = vertexBuffer;
    return info;
}

// VK_NV_cluster_acceleration_structure carries the OMM index format per argument and has no base location, so
// baseLocation must be 0 here (it is ignored), and the index format is taken from the stride (1, 2 or 4 bytes).
// Untested: the Vulkan backend reports no opacity micromap support.
void RtasClusterBuildTriangleInfoSetOpacityMicromaps(inout RtasClusterBuildTriangleInfo info, uint64_t ommArray, uint64_t ommIndexBuffer,
    uint ommIndexStrideBytes, uint baseLocation, bool allowDisable)
{
    const uint indexFormat = (ommIndexStrideBytes == 1u || ommIndexStrideBytes == 2u) ? ommIndexStrideBytes : 4u;
    info.countsAndFormats = (info.countsAndFormats & 0x0FFFFFFFu) | (indexFormat << 28u);
    info.geometryAndOpacityStride = (info.geometryAndOpacityStride & 0xFFFFu) | ((ommIndexStrideBytes & 0xFFFFu) << 16u);
    info.opacityMicromapArray = ommArray;
    info.opacityMicromapIndexBuffer = ommIndexBuffer;
    info.clusterFlags = allowDisable ? (info.clusterFlags | RTAS_CLUSTER_FLAG_ALLOW_DISABLE_OMMS) : (info.clusterFlags & ~RTAS_CLUSTER_FLAG_ALLOW_DISABLE_OMMS);
}

#else

struct RtasClusterBuildTriangleInfo
{
    uint clusterID;
    uint clusterFlags;
    uint countsPacked; // TriangleCount | UniqueVertexIndexCount << 16
    uint baseGeometryIndexAndFlags;
    uint opacityMicromapBaseLocation;
    uint strides0;     // VertexBufferStride | IndexBufferStride << 16
    uint strides1;     // OpacityMicromapIndexBufferStride | GeometryIndexAndFlagsArrayStride << 16
    uint truncate;     // PositionTruncateBitCount | ReservedPadding << 16
    uint64_t vertexBuffer;
    uint64_t indexBuffer;
    uint64_t geometryIndexAndFlagsArray;
    uint64_t geometryIndexAndFlagsIndexBuffer;
    uint64_t opacityMicromapArray;
    uint64_t opacityMicromapIndexBuffer;
};

RtasClusterBuildTriangleInfo RtasMakeClusterBuildTriangleInfo(
    uint clusterID, uint triangleCount, uint vertexCount, uint geometryFlags, uint vertexStride, uint indexStride,
    uint64_t vertexBuffer, uint64_t indexBuffer)
{
    RtasClusterBuildTriangleInfo info = (RtasClusterBuildTriangleInfo)0;
    info.clusterID = clusterID;
    info.countsPacked = triangleCount | (vertexCount << 16u);
    info.baseGeometryIndexAndFlags = geometryFlags;
    info.strides0 = vertexStride | (indexStride << 16u);
    info.vertexBuffer = vertexBuffer;
    info.indexBuffer = indexBuffer;
    return info;
}

// Links a cluster's triangles to an OMM array. ommIndexBuffer holds one index per triangle (signed; the special values
// RTAS_OMM_SPECIAL_INDEX_* are allowed) at ommIndexStrideBytes, which must be a multiple of the index size the batch
// declares in RayTracingClusterTriangleInputsDesc::opacityMicromapIndexFormat, and the buffer aligned to that size.
// baseLocation is added to the non-special indices. ommArray must be 128-byte aligned. The CLAS build and every
// operation that later touches the CLAS (BLAS-from-CLAS build, moves) must set RayTracingRtasOperationInputs::allowOpacityMicromaps.
// allowDisable lets instances turn OMMs off with RTAS_INSTANCE_FLAG_DISABLE_OMMS; it must be set on every OMM CLAS in the BLAS.
void RtasClusterBuildTriangleInfoSetOpacityMicromaps(inout RtasClusterBuildTriangleInfo info, uint64_t ommArray, uint64_t ommIndexBuffer,
    uint ommIndexStrideBytes, uint baseLocation, bool allowDisable)
{
    info.opacityMicromapBaseLocation = baseLocation;
    info.strides1 = (info.strides1 & 0xFFFF0000u) | (ommIndexStrideBytes & 0xFFFFu);
    info.opacityMicromapArray = ommArray;
    info.opacityMicromapIndexBuffer = ommIndexBuffer;
    info.clusterFlags = allowDisable ? (info.clusterFlags | RTAS_CLUSTER_FLAG_ALLOW_DISABLE_OMMS) : (info.clusterFlags & ~RTAS_CLUSTER_FLAG_ALLOW_DISABLE_OMMS);
}

#endif

// The same on both backends.
struct RtasClusterBuildBottomLevelInfo
{
    uint clusterReferencesCount;
    uint clusterReferencesStride;
    uint64_t clusterReferences;
};

// D3D12_RAYTRACING_INSTANCE_DESC / VkAccelerationStructureInstanceKHR.
struct RtasInstanceDesc
{
    float4 transformRows[3];
    uint instanceIDAndMask;
    uint contributionAndFlags;
    uint64_t accelerationStructure;
};

#define RTAS_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE 0x1u
#define RTAS_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE 0x2u
#define RTAS_INSTANCE_FLAG_FORCE_OPAQUE 0x4u
#define RTAS_INSTANCE_FLAG_FORCE_NON_OPAQUE 0x8u
#define RTAS_INSTANCE_FLAG_FORCE_OMM_2_STATE 0x10u
#define RTAS_INSTANCE_FLAG_DISABLE_OMMS 0x20u

#endif // RHI_RTAS_ARGS_HLSLI
