#ifndef CLOD_VISIBLE_CLUSTER_PACKING_HLSLI
#define CLOD_VISIBLE_CLUSTER_PACKING_HLSLI

#include "include/clodVirtualShadowClipmap.hlsli"

static const uint CLOD_PACKED_VISIBLE_CLUSTER_STRIDE = 16u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT = 14u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_LOW_MASK = 0x3FFu;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_MASK = 0xFu;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_SHIFT = 25u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_MASK =
    CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_MASK <<
    CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_SHIFT;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX = 0xFFFFFFFFu;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_BITS = 5u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_COORD_BITS = 5u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_LOCAL_PAGE_BITS = 2u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_MASK =
    (1u << CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_BITS) - 1u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_INVALID_CLIPMAP_BITS = CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_MASK;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_SHIFT = 0u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_X_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_SHIFT + CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_BITS;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_Y_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_X_SHIFT + CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_COORD_BITS;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_X_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_Y_SHIFT + CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_COORD_BITS;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_Y_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_X_SHIFT + CLOD_PACKED_VISIBLE_CLUSTER_VSM_LOCAL_PAGE_BITS;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_X_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_Y_SHIFT + CLOD_PACKED_VISIBLE_CLUSTER_VSM_LOCAL_PAGE_BITS;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_Y_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_X_SHIFT + CLOD_PACKED_VISIBLE_CLUSTER_VSM_LOCAL_PAGE_BITS;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_OVERFLOW_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_Y_SHIFT + CLOD_PACKED_VISIBLE_CLUSTER_VSM_LOCAL_PAGE_BITS;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_HAS_BLOCK_DATA_SHIFT =
    CLOD_PACKED_VISIBLE_CLUSTER_VSM_OVERFLOW_SHIFT + 1u;
// Bits 29-30 are not used by either the VSM payload or the high page-slab
// offset nibble. Keep cache-layer routing with the cluster instead of
// inferring it later from deformation type.
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VSM_DYNAMIC_LAYER_FLAG = 0x20000000u;
static const uint CLOD_PACKED_VISIBLE_CLUSTER_VOXEL_FLAG = 0x80000000u;

uint4 CLodLoadVisibleClusterPacked(ByteAddressBuffer buffer, uint clusterIndex)
{
    const uint byteOffset = clusterIndex * CLOD_PACKED_VISIBLE_CLUSTER_STRIDE;
    return uint4(
        buffer.Load(byteOffset + 0u),
        buffer.Load(byteOffset + 4u),
        buffer.Load(byteOffset + 8u),
        buffer.Load(byteOffset + 12u));
}

uint4 CLodLoadVisibleClusterPackedRW(RWByteAddressBuffer buffer, uint clusterIndex)
{
    const uint byteOffset = clusterIndex * CLOD_PACKED_VISIBLE_CLUSTER_STRIDE;
    return uint4(
        buffer.Load(byteOffset + 0u),
        buffer.Load(byteOffset + 4u),
        buffer.Load(byteOffset + 8u),
        buffer.Load(byteOffset + 12u));
}

uint4 CLodLoadVisibleClusterPackedGloballyCoherent(globallycoherent RWByteAddressBuffer buffer, uint clusterIndex)
{
    const uint byteOffset = clusterIndex * CLOD_PACKED_VISIBLE_CLUSTER_STRIDE;
    return uint4(
        buffer.Load(byteOffset + 0u),
        buffer.Load(byteOffset + 4u),
        buffer.Load(byteOffset + 8u),
        buffer.Load(byteOffset + 12u));
}

void CLodStoreVisibleClusterPackedWordsRW(RWByteAddressBuffer buffer, uint clusterIndex, uint4 packedCluster)
{
    const uint byteOffset = clusterIndex * CLOD_PACKED_VISIBLE_CLUSTER_STRIDE;
    buffer.Store(byteOffset + 0u, packedCluster.x);
    buffer.Store(byteOffset + 4u, packedCluster.y);
    buffer.Store(byteOffset + 8u, packedCluster.z);
    buffer.Store(byteOffset + 12u, packedCluster.w);
}

void CLodStoreVisibleClusterPackedWordsGloballyCoherent(globallycoherent RWByteAddressBuffer buffer, uint clusterIndex, uint4 packedCluster)
{
    const uint byteOffset = clusterIndex * CLOD_PACKED_VISIBLE_CLUSTER_STRIDE;
    buffer.Store(byteOffset + 0u, packedCluster.x);
    buffer.Store(byteOffset + 4u, packedCluster.y);
    buffer.Store(byteOffset + 8u, packedCluster.z);
    buffer.Store(byteOffset + 12u, packedCluster.w);
}

uint CLodVisibleClusterViewID(uint4 packedCluster)
{
    return packedCluster.x & 0xFFu;
}

uint CLodVisibleClusterInstanceID(uint4 packedCluster)
{
    return (packedCluster.x >> 8u) & 0xFFFFFFu;
}

uint CLodVisibleClusterLocalMeshletIndex(uint4 packedCluster)
{
    return packedCluster.y & 0x3FFFu;
}

uint CLodVisibleClusterGroupID(uint4 packedCluster)
{
    return ((packedCluster.y >> 14u) & 0x3FFFFu) | ((packedCluster.z & 0x3u) << 18u);
}

uint CLodVisibleClusterPageSlabDescriptorIndex(uint4 packedCluster)
{
    return (packedCluster.z >> 2u) & 0xFFFFFu;
}

uint CLodVisibleClusterVoxelClusterIndex(uint4 packedCluster)
{
    return CLodVisibleClusterLocalMeshletIndex(packedCluster) |
        (((packedCluster.z >> 2u) & 0xFFFFFu) << 14u);
}

uint CLodVisibleClusterVoxelCubeIndex(uint4 packedCluster)
{
    return CLodVisibleClusterVoxelClusterIndex(packedCluster);
}

uint CLodVisibleClusterPageSlabByteOffset(uint4 packedCluster)
{
    const uint pageOffsetUnits =
        ((packedCluster.z >> 22u) &
            CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_LOW_MASK) |
        (((packedCluster.w >>
            CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_SHIFT) &
            CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_MASK) << 10u);
    return pageOffsetUnits << CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT;
}

uint CLodVisibleClusterVoxelPageIndex(uint4 packedCluster)
{
    return (packedCluster.z >> 2u) & 0xFFFFFu;
}

uint CLodVisibleClusterVsmPayload(uint4 packedCluster)
{
    return packedCluster.w &
        ~CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_MASK;
}

bool CLodVisibleClusterIsVoxel(uint4 packedCluster)
{
    return (packedCluster.w & CLOD_PACKED_VISIBLE_CLUSTER_VOXEL_FLAG) != 0u;
}

bool CLodVisibleClusterIsVoxelCube(uint4 packedCluster)
{
    return CLodVisibleClusterIsVoxel(packedCluster);
}

uint CLodVisibleClusterMarkVoxelPayload(uint vsmPayload)
{
    return vsmPayload | CLOD_PACKED_VISIBLE_CLUSTER_VOXEL_FLAG;
}

uint CLodVisibleClusterMarkDynamicShadowPayload(uint vsmPayload)
{
    return vsmPayload | CLOD_PACKED_VISIBLE_CLUSTER_VSM_DYNAMIC_LAYER_FLAG;
}

bool CLodVisibleClusterUsesDynamicShadowLayerFromPayload(uint vsmPayload)
{
    return (vsmPayload & CLOD_PACKED_VISIBLE_CLUSTER_VSM_DYNAMIC_LAYER_FLAG) != 0u;
}

bool CLodVisibleClusterUsesDynamicShadowLayer(uint4 packedCluster)
{
    return CLodVisibleClusterUsesDynamicShadowLayerFromPayload(
        CLodVisibleClusterVsmPayload(packedCluster));
}

uint CLodBuildVisibleClusterVsmPayloadFromClipmapIndex(uint shadowClipmapIndex)
{
    const uint encodedClipmapIndex =
        shadowClipmapIndex == CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX
            ? CLOD_PACKED_VISIBLE_CLUSTER_VSM_INVALID_CLIPMAP_BITS
            : (shadowClipmapIndex & CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_MASK);
    return encodedClipmapIndex << CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_SHIFT;
}

uint CLodPackVisibleClusterVsmPayloadForBlock(
    uint shadowClipmapIndex,
    uint2 blockCoord,
    uint2 minLocalPageCoord,
    uint2 maxLocalPageCoord,
    bool overflowed)
{
    uint payload = CLodBuildVisibleClusterVsmPayloadFromClipmapIndex(shadowClipmapIndex);
    payload |= (blockCoord.x & 0x1Fu) << CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_X_SHIFT;
    payload |= (blockCoord.y & 0x1Fu) << CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_Y_SHIFT;
    payload |= (minLocalPageCoord.x & 0x3u) << CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_X_SHIFT;
    payload |= (minLocalPageCoord.y & 0x3u) << CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_Y_SHIFT;
    payload |= (maxLocalPageCoord.x & 0x3u) << CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_X_SHIFT;
    payload |= (maxLocalPageCoord.y & 0x3u) << CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_Y_SHIFT;
    payload |= (overflowed ? 1u : 0u) << CLOD_PACKED_VISIBLE_CLUSTER_VSM_OVERFLOW_SHIFT;
    payload |= 1u << CLOD_PACKED_VISIBLE_CLUSTER_VSM_HAS_BLOCK_DATA_SHIFT;
    return payload;
}

uint CLodVisibleClusterShadowClipmapIndexFromPayload(uint vsmPayload)
{
    const uint encodedClipmapIndex =
        (vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_SHIFT) & CLOD_PACKED_VISIBLE_CLUSTER_VSM_CLIPMAP_MASK;
    return encodedClipmapIndex == CLOD_PACKED_VISIBLE_CLUSTER_VSM_INVALID_CLIPMAP_BITS
        ? CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX
        : encodedClipmapIndex;
}

uint CLodVisibleClusterShadowClipmapIndex(uint4 packedCluster)
{
    return CLodVisibleClusterShadowClipmapIndexFromPayload(CLodVisibleClusterVsmPayload(packedCluster));
}

bool CLodVisibleClusterHasVsmBlockDataFromPayload(uint vsmPayload)
{
    return ((vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_HAS_BLOCK_DATA_SHIFT) & 0x1u) != 0u;
}

uint2 CLodVisibleClusterVsmBlockCoordFromPayload(uint vsmPayload)
{
    return uint2(
        (vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_X_SHIFT) & 0x1Fu,
        (vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_BLOCK_Y_SHIFT) & 0x1Fu);
}

uint2 CLodVisibleClusterVsmBlockOriginPageCoordFromPayload(uint vsmPayload)
{
    return CLodVirtualShadowBlockOriginFromBlockCoord(CLodVisibleClusterVsmBlockCoordFromPayload(vsmPayload));
}

uint CLodVisibleClusterVsmActiveRectFromPayload(uint vsmPayload)
{
    return CLodVirtualShadowPackBlockActiveRect(
        uint2(
            (vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_X_SHIFT) & 0x3u,
            (vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MIN_Y_SHIFT) & 0x3u),
        uint2(
            (vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_X_SHIFT) & 0x3u,
            (vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_RECT_MAX_Y_SHIFT) & 0x3u),
        ((vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_OVERFLOW_SHIFT) & 0x1u) != 0u);
}

bool CLodVisibleClusterVsmBlockOverflowedFromPayload(uint vsmPayload)
{
    return ((vsmPayload >> CLOD_PACKED_VISIBLE_CLUSTER_VSM_OVERFLOW_SHIFT) & 0x1u) != 0u;
}

uint4 CLodPackVisibleCluster(
    uint viewID,
    uint instanceID,
    uint localMeshletIndex,
    uint groupID,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint shadowClipmapIndex)
{
    const uint pageOffsetUnits =
        pageSlabByteOffset >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT;
    return uint4(
        (viewID & 0xFFu) | ((instanceID & 0xFFFFFFu) << 8u),
        (localMeshletIndex & 0x3FFFu) | ((groupID & 0x3FFFFu) << 14u),
        ((groupID >> 18u) & 0x3u) |
            ((pageSlabDescriptorIndex & 0xFFFFFu) << 2u) |
            ((pageOffsetUnits &
                CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_LOW_MASK) << 22u),
        CLodBuildVisibleClusterVsmPayloadFromClipmapIndex(shadowClipmapIndex) |
            (((pageOffsetUnits >> 10u) &
                CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_MASK) <<
                CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_SHIFT));
}

uint4 CLodPackVisibleClusterWithVsmPayload(
    uint viewID,
    uint instanceID,
    uint localMeshletIndex,
    uint groupID,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint vsmPayload)
{
    const uint pageOffsetUnits =
        pageSlabByteOffset >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT;
    return uint4(
        (viewID & 0xFFu) | ((instanceID & 0xFFFFFFu) << 8u),
        (localMeshletIndex & 0x3FFFu) | ((groupID & 0x3FFFFu) << 14u),
        ((groupID >> 18u) & 0x3u) |
            ((pageSlabDescriptorIndex & 0xFFFFFu) << 2u) |
            ((pageOffsetUnits &
                CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_LOW_MASK) << 22u),
        vsmPayload |
            (((pageOffsetUnits >> 10u) &
                CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_MASK) <<
                CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_SHIFT));
}

void CLodStoreVisibleClusterRW(
    RWByteAddressBuffer buffer,
    uint clusterIndex,
    uint viewID,
    uint instanceID,
    uint localMeshletIndex,
    uint groupID,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint shadowClipmapIndex)
{
    CLodStoreVisibleClusterPackedWordsRW(
        buffer,
        clusterIndex,
        CLodPackVisibleCluster(
            viewID,
            instanceID,
            localMeshletIndex,
            groupID,
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            shadowClipmapIndex));
}

void CLodStoreVisibleClusterWithVsmPayloadRW(
    RWByteAddressBuffer buffer,
    uint clusterIndex,
    uint viewID,
    uint instanceID,
    uint localMeshletIndex,
    uint groupID,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint vsmPayload)
{
    CLodStoreVisibleClusterPackedWordsRW(
        buffer,
        clusterIndex,
        CLodPackVisibleClusterWithVsmPayload(
            viewID,
            instanceID,
            localMeshletIndex,
            groupID,
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            vsmPayload));
}

void CLodStoreVisibleClusterGloballyCoherent(
    globallycoherent RWByteAddressBuffer buffer,
    uint clusterIndex,
    uint viewID,
    uint instanceID,
    uint localMeshletIndex,
    uint groupID,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint shadowClipmapIndex)
{
    CLodStoreVisibleClusterPackedWordsGloballyCoherent(
        buffer,
        clusterIndex,
        CLodPackVisibleCluster(
            viewID,
            instanceID,
            localMeshletIndex,
            groupID,
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            shadowClipmapIndex));
}

void CLodStoreVisibleClusterWithVsmPayloadGloballyCoherent(
    globallycoherent RWByteAddressBuffer buffer,
    uint clusterIndex,
    uint viewID,
    uint instanceID,
    uint localMeshletIndex,
    uint groupID,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint vsmPayload)
{
    CLodStoreVisibleClusterPackedWordsGloballyCoherent(
        buffer,
        clusterIndex,
        CLodPackVisibleClusterWithVsmPayload(
            viewID,
            instanceID,
            localMeshletIndex,
            groupID,
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            vsmPayload));
}

#endif
