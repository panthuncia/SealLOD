// Software rasterizer for small CLod clusters. One 32-lane thread group handles one cluster:
//
//   SWLoadClusterSetup    lane 0 publishes the cluster's page, mesh and transform state
//   SWTransformVertices   each lane projects every 32nd vertex into groupshared memory
//   SWRasterTriangles     each lane rasterizes every 32nd triangle
//   SWRasterPixel         alpha test, then the visibility or virtual shadow write
//
// Entry points: SWRasterIndirectCSMain (compute, the default path) and WG_SWRaster (work graph
// node, embedded in workGraphCulling.hlsl). They only differ in how they find their cluster.

#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/clodVirtualShadowDepth.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/vertex.hlsli"
#include "include/materialFlags.hlsli"
#include "include/materialTextureBinding.hlsli"
#include "PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodClusterFormat.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/debugPayload.hlsli"

#ifndef CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#define CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW 0
#endif

#define SW_CLUSTER_RASTER_THREADS 32
#ifndef CLOD_WG_SW_RASTER_THREADS
#define CLOD_WG_SW_RASTER_THREADS SW_CLUSTER_RASTER_THREADS
#endif

#ifndef CLOD_WG_RIGID_SW_RASTER
#define CLOD_WG_RIGID_SW_RASTER CLOD_WG_RIGID_ONLY
#endif

#define CLOD_RASTER_CLUSTER_VIRTUAL_SHADOW CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#define CLOD_RASTER_CLUSTER_RIGID_ONLY CLOD_WG_RIGID_SW_RASTER
#include "include/clodRasterCluster.hlsli"

#if defined(PSO_ALPHA_TEST) || defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST)
#define CLOD_SW_RASTER_MAY_ALPHA_TEST 1
#else
#define CLOD_SW_RASTER_MAY_ALPHA_TEST 0
#endif

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#include "include/clodSwRasterVirtualShadow.hlsli"
#endif

// Work counters, compiled in only on request because they cost atomics per pixel. Enable them
// live with a pso.recompile define override, in a session launched with visibility telemetry.
#ifndef CLOD_SW_RASTER_TELEMETRY
#define CLOD_SW_RASTER_TELEMETRY 0
#endif

static const uint SW_COUNTER_CLUSTERS = 252u;
static const uint SW_COUNTER_INPUT_TRIANGLES = 253u;
static const uint SW_COUNTER_BACKFACE_CULLED = 254u;
static const uint SW_COUNTER_DEGENERATE_OR_OFFSCREEN = 255u;
static const uint SW_COUNTER_NO_SAMPLE_COVERED = 256u;
static const uint SW_COUNTER_COVERED_PIXELS = 257u;
static const uint SW_COUNTER_ALPHA_REJECTED_PIXELS = 258u;
static const uint SW_COUNTER_VISIBILITY_WRITES = 259u;

void SWTelemetryAdd(uint counter, uint value)
{
#if CLOD_SW_RASTER_TELEMETRY && !defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> telemetry = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
        InterlockedAdd(telemetry[counter], value);
    }
#endif
}

// What an entry point hands to SWRasterCluster.
struct SWClusterWork
{
    uint4 packedCluster;
    uint assemblyTransformIndex;
    uint paletteIndex;
    uint visibleClusterIndex; // Written to the visibility buffer, so unsorted.
    bool hasResolvedSkinningSlot;
    uint resolvedSkinningSlot;
    uint cachedPositionBase; // 0xFFFFFFFF unless the dynamic wind skin cache has this cluster.
};

// Published by lane 0. These are the parts only software raster uses.
struct SWClusterFlags
{
    uint alphaTestEnabled;
    uint doubleSided;
    uint groupFlags;
    uint debugOutputType;
};

#if defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
#define SW_RASTER_SETUP_SLOTS 1
#else
#if SW_RASTER_CLUSTERS_PER_GROUP > SW_CLUSTER_RASTER_THREADS
#error SW_RASTER_CLUSTERS_PER_GROUP is above one cluster per lane
#endif
#define SW_RASTER_SETUP_SLOTS SW_RASTER_CLUSTERS_PER_GROUP
#endif

groupshared CLodRasterCluster gs_clusters[SW_RASTER_SETUP_SLOTS];
groupshared SWClusterFlags gs_clusterFlags[SW_RASTER_SETUP_SLOTS];
groupshared SWClusterWork gs_clusterWork[SW_RASTER_SETUP_SLOTS];
groupshared uint gs_groupClusterCount;
// Groupshared memory limits this shader's occupancy (achieved warps times bytes per group stayed
// near 44 KB per SM as both changed), so each vertex is one entry: screen xy, linear depth and,
// for perspective views, 1/w. A virtual shadow view needs no 1/w: the sun's clips are orthographic,
// where w is 1, and a local light face's w is its linear depth.
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
typedef float3 SWVertex;
#else
typedef float4 SWVertex;
#endif
groupshared SWVertex gs_vertices[SW_RASTER_MAX_VERTS];

// ---------------------------------------------------------------------------------------------
// Stage 1: cluster setup, one lane per cluster
// ---------------------------------------------------------------------------------------------

void SWLoadClusterSetup(SWClusterWork work, uint slot)
{
    CLodRasterClusterSource source;
    source.packedCluster = work.packedCluster;
    source.assemblyTransformIndex = work.assemblyTransformIndex;
    source.paletteIndex = work.paletteIndex;
    source.hasResolvedSkinningSlot = work.hasResolvedSkinningSlot;
    source.resolvedSkinningSlot = work.resolvedSkinningSlot;
    CLodRasterCluster cluster;
    // A cluster that fails to load has no vertices or triangles, so the later stages do nothing.
    const uint loadResult = CLodLoadRasterCluster(source, cluster);
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && !defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
    if (loadResult != CLOD_RASTER_CLUSTER_OK)
    {
        // Its pages are not complete this frame: none may be taken as empty.
        RWStructuredBuffer<CLodVirtualShadowStats> vsmStats =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodStats)];
        InterlockedAdd(vsmStats[0].rasterWorkDroppedCount, 1u);
    }
#else
    (void)loadResult;
#endif
    gs_clusters[slot] = cluster;
    gs_clusterWork[slot] = work;

    SWClusterFlags flags;
    ConstantBuffer<PerFrameBuffer> perFrameBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    flags.debugOutputType = perFrameBuffer.outputType;

    flags.groupFlags = 0u;
    if (flags.debugOutputType == OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE ||
        flags.debugOutputType == OUTPUT_CLOD_ASSEMBLY_PARTS)
    {
        const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(cluster.drawRecordIndex);
        StructuredBuffer<CLodMeshMetadata> metadataBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
        StructuredBuffer<ClusterLODGroup> groups =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
        flags.groupFlags = groups[
            CLodVisibleClusterGroupID(work.packedCluster)].flags;
    }

#if defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST)
    // The work graph runs one pipeline for every material, so these come from the material.
    StructuredBuffer<MaterialInfo> materialDataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const uint materialFlags = materialDataBuffer[cluster.materialDataIndex].materialFlags;
    flags.alphaTestEnabled = (materialFlags & MATERIAL_ALPHA_TEST) != 0u ? 1u : 0u;
    flags.doubleSided = (materialFlags & MATERIAL_DOUBLE_SIDED) != 0u ? 1u : 0u;
#else
#if defined(PSO_ALPHA_TEST)
    flags.alphaTestEnabled = 1u;
#else
    flags.alphaTestEnabled = 0u;
#endif
#if defined(PSO_DOUBLE_SIDED)
    flags.doubleSided = 1u;
#else
    flags.doubleSided = 0u;
#endif
#endif
    gs_clusterFlags[slot] = flags;
}

// ---------------------------------------------------------------------------------------------
// Stage 2: vertices
// ---------------------------------------------------------------------------------------------

void SWRecordCacheTelemetry(SWClusterWork work, CLodRasterCluster cluster)
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && !defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return;
    }

    RWStructuredBuffer<uint> cacheTelemetry = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    if (work.cachedPositionBase != 0xFFFFFFFFu)
    {
        InterlockedAdd(cacheTelemetry[244u], cluster.vertexCount);
    }
    else if ((cluster.vertexFlags & VERTEX_SKINNED) != 0u)
    {
        InterlockedAdd(cacheTelemetry[245u], cluster.vertexCount);
    }
#endif
}

float3 SWLoadVertexPosition(SWClusterWork work, CLodRasterCluster cluster, ByteAddressBuffer slab, uint vertexIndex)
{
    if (work.cachedPositionBase != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<float3> cachedPositions =
            ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX];
        return cachedPositions[work.cachedPositionBase + vertexIndex];
    }

    const float3 position = CLodRasterLoadPosition(cluster, slab, vertexIndex);
#if defined(PSO_SKINNED)
    return CLodRasterSkinPosition(cluster, slab, vertexIndex, position);
#elif !CLOD_WG_RIGID_SW_RASTER && defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
    // Compute dispatches one PSO per bucket, so only the work graph's single library sees both.
    if ((cluster.vertexFlags & VERTEX_SKINNED) != 0u)
    {
        return CLodRasterSkinPosition(cluster, slab, vertexIndex, position);
    }
    return position;
#else
    return position;
#endif
}

void SWTransformVertices(SWClusterWork work, CLodRasterCluster cluster, ClodViewRasterInfo rasterInfo, uint GI)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[cluster.slabDescriptorIndex];

    const float visWidth = float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX);
    const float visHeight = float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY);
    const float scissorMinX = float(rasterInfo.scissorMinX);
    const float scissorMinY = float(rasterInfo.scissorMinY);

    for (uint v = GI; v < cluster.vertexCount; v += SW_CLUSTER_RASTER_THREADS)
    {
        const float3 position = SWLoadVertexPosition(work, cluster, slab, v);
        const float4 clipPosition = CLodRasterClipPosition(cluster, position);
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        // Exact for the sun's clips, where w is 1. A local light face's screen position is in its mip 0's pixels;
        // SWRasterTriangles maps it to each mip. Vertices behind the face have no meaningful position, and their
        // triangles are rejected by depth.
        const float2 ndc = clipPosition.xy / clipPosition.w;
#else
        const float invW = 1.0f / clipPosition.w;
        const float2 ndc = clipPosition.xy * invW;
#endif
        const float2 screenPosition = float2(
            (ndc.x + 1.0f) * 0.5f * visWidth + scissorMinX,
            (1.0f - ndc.y) * 0.5f * visHeight + scissorMinY);
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        gs_vertices[v] = float3(screenPosition, CLodRasterLinearDepth(cluster, position));
#else
        gs_vertices[v] = float4(screenPosition, CLodRasterLinearDepth(cluster, position), invW);
#endif
    }
}

// ---------------------------------------------------------------------------------------------
// Stage 3: triangles and pixels
// ---------------------------------------------------------------------------------------------

// Everything a covered pixel needs from its triangle.
struct SWRasterTriangle
{
    uint3 vertices;
    uint triangleIndex;
    // Linear depths; their reciprocals for a local light face (SWRasterOutput::vsmPerspective).
    float3 depths;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    SWVirtualShadowTrianglePage vsmPage;
#else
    float3 invW;
#endif
#if CLOD_SW_RASTER_MAY_ALPHA_TEST
    // Barycentric steps per pixel, for the alpha test's UV gradients.
    float3 barycentricsDx;
    float3 barycentricsDy;
#endif
};

// Per-cluster debug output. Only built when a debug view is on.
struct SWRasterDebug
{
    bool enabled;
    uint2 payload;
};

SWRasterDebug SWBuildRasterDebug(SWClusterWork work, SWClusterFlags flags)
{
    const uint outputType = flags.debugOutputType;
    SWRasterDebug debug = (SWRasterDebug)0;
    debug.enabled =
        outputType == OUTPUT_SW_RASTER ||
        outputType == OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE ||
        outputType == OUTPUT_CLOD_ASSEMBLY_PARTS;
    if (!debug.enabled)
    {
        return debug;
    }

    const bool isAssemblyVoxel = (flags.groupFlags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u;
    if (outputType == OUTPUT_SW_RASTER)
    {
        debug.payload = PackDebugUint(1u);
    }
    else if (outputType == OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE)
    {
        float3 color = float3(0.08f, 0.10f, 0.12f);
        if (isAssemblyVoxel)
        {
            color = float3(1.0f, 0.58f, 0.08f);
        }
        else if (work.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
        {
            color = max(HashToColor(work.assemblyTransformIndex + 17u), 0.18f.xxx);
        }
        else if ((flags.groupFlags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u)
        {
            color = float3(0.82f, 0.18f, 1.0f);
        }
        debug.payload = PackDebugFloat3(color);
    }
    else
    {
        debug.payload = PackDebugUint(isAssemblyVoxel
            ? (0x40000000u ^ CLodVisibleClusterGroupID(work.packedCluster))
            : (work.assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL
                ? 1u
                : (0x80000000u + work.assemblyTransformIndex)));
    }
    return debug;
}

#if CLOD_SW_RASTER_MAY_ALPHA_TEST
// Loaded once per lane. A thread group is one cluster, so its material and therefore these
// descriptor indices are uniform across the wave.
struct SWAlphaMaterial
{
    uint materialFlags;
    float baseAlpha;
    float alphaCutoff;
    uint baseColorTextureIndex;
    uint baseColorSamplerIndex;
    uint opacityTextureIndex;
    uint opacitySamplerIndex;
    float4 uvTransformLinearDelta; // The material's KHR_texture_transform (materialUvTransform.hlsli).
    float2 uvTransformOffset;
};

SWAlphaMaterial SWLoadAlphaMaterial(uint materialDataIndex)
{
    StructuredBuffer<MaterialInfo> materialDataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const MaterialInfo materialInfo = materialDataBuffer[materialDataIndex];

    SWAlphaMaterial material;
    material.materialFlags = materialInfo.materialFlags;
    material.baseAlpha = materialInfo.baseColorFactor.a;
    material.alphaCutoff = materialInfo.alphaCutoff;
    // The images the textures' streaming rows publish, as the hardware rasterizer's alpha test samples.
    material.baseColorTextureIndex = ResolveMaterialTextureDescriptor(
        materialInfo.baseColorTextureIndex, materialInfo.baseColorStreamingTextureID);
    material.baseColorSamplerIndex = materialInfo.baseColorSamplerIndex;
    material.opacityTextureIndex = ResolveMaterialTextureDescriptor(
        materialInfo.opacityTextureIndex, materialInfo.opacityStreamingTextureID);
    material.opacitySamplerIndex = materialInfo.opacitySamplerIndex;
    material.uvTransformLinearDelta = materialInfo.uvTransformLinearDelta;
    material.uvTransformOffset = materialInfo.uvTransformOffset;
    return material;
}

// Sampled with the texel's UV gradients, as the hardware raster's alpha test samples with its
// derivatives. Mip 0 cost a shadow texel covering many texture texels scattered, uncached samples.
bool SWAlphaTestFailed(SWAlphaMaterial material, float2 texcoords, float2 texcoordsDx, float2 texcoordsDy)
{
    float alpha = material.baseAlpha;
    texcoords = ApplyMaterialUvTransformToUv(material.uvTransformLinearDelta, material.uvTransformOffset, texcoords);
    texcoordsDx = ApplyMaterialUvTransformToGradient(material.uvTransformLinearDelta, texcoordsDx);
    texcoordsDy = ApplyMaterialUvTransformToGradient(material.uvTransformLinearDelta, texcoordsDy);

    if ((material.materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTextureIndex];
        SamplerState baseColorSamplerState = SamplerDescriptorHeap[material.baseColorSamplerIndex];
        alpha *= baseColorTexture.SampleGrad(baseColorSamplerState, texcoords, texcoordsDx, texcoordsDy).a;
    }

    if ((material.materialFlags & MATERIAL_OPACITY_TEXTURE) != 0u)
    {
        Texture2D<float4> opacityTexture = ResourceDescriptorHeap[material.opacityTextureIndex];
        SamplerState opacitySamplerState = SamplerDescriptorHeap[material.opacitySamplerIndex];
        alpha *= opacityTexture.SampleGrad(opacitySamplerState, texcoords, texcoordsDx, texcoordsDy).a;
    }

    return alpha < material.alphaCutoff;
}
#endif

// Everything a covered pixel writes to, resolved once per lane.
struct SWRasterOutput
{
    uint viewID;
    uint visibleClusterIndex;
    uint dynamicShadowLayer;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    // The slice of the mip being rastered.
    SWVirtualShadowTarget vsmTarget;
    // A local light face: a perspective view, so depth and UVs interpolate perspective-correctly.
    bool vsmPerspective;
#endif
    SWRasterDebug debug;
#if CLOD_SW_RASTER_MAY_ALPHA_TEST
    bool alphaTestEnabled;
    SWAlphaMaterial alphaMaterial;
    CLodUvStream uvStream;
#endif
};

void SWRasterPixel(
    SWRasterTriangle tri,
    SWRasterOutput output,
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    RWTexture2D<uint64_t> visBuffer,
#endif
    RWTexture2D<uint2> debugVisTex,
    ByteAddressBuffer slab,
    uint2 pixel,
    float3 barycentrics)
{
    SWTelemetryAdd(SW_COUNTER_COVERED_PIXELS, 1u);
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    // Under perspective 1/depth is affine in screen space.
    const float depth = output.vsmPerspective
        ? rcp(dot(barycentrics, tri.depths))
        : dot(barycentrics, tri.depths);
#else
    const float depth = dot(barycentrics, tri.depths);
#endif
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    // The texel's page and depth come before the alpha test: a texel on a page that is not being
    // rastered, or already holding nearer depth, needs neither its UVs nor its texture samples.
    const bool dynamicLayer = output.dynamicShadowLayer != 0u;
    SWVirtualShadowTexel texel;
    if (!SWResolveVirtualShadowTexel(pixel, output.vsmTarget, tri.vsmPage, dynamicLayer, texel))
    {
        return;
    }
    const uint encodedDepth = SWEncodeVirtualShadowTexelDepth(texel, output.vsmTarget, depth, dynamicLayer);
#endif
#if CLOD_SW_RASTER_MAY_ALPHA_TEST
    if (output.alphaTestEnabled)
    {
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (SWVirtualShadowTexelOccluded(texel, encodedDepth, dynamicLayer))
        {
            return;
        }
#endif
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        // Orthographic: the screen barycentrics are already the perspective-correct ones. A local light face weights
        // them by 1/depth, which is 1/w.
        const float3 perspective = output.vsmPerspective ? barycentrics * tri.depths : barycentrics;
        const float invSum = output.vsmPerspective ? rcp(perspective.x + perspective.y + perspective.z) : 1.0f;
#else
        const float3 perspective = barycentrics * tri.invW;
        const float invSum = rcp(perspective.x + perspective.y + perspective.z);
#endif
        // Decoded here because so few triangles cover a sample: decoding per vertex was a
        // large part of the cost of alpha-tested clusters.
        const float2 uv0 = CLodDecodeUv(slab, output.uvStream, tri.vertices.x);
        const float2 uv1 = CLodDecodeUv(slab, output.uvStream, tri.vertices.y);
        const float2 uv2 = CLodDecodeUv(slab, output.uvStream, tri.vertices.z);
        const float2 texcoord = (uv0 * perspective.x + uv1 * perspective.y + uv2 * perspective.z) * invSum;
        // Exact for orthographic views; perspective views take the affine gradient.
        const float2 texcoordDx =
            uv0 * tri.barycentricsDx.x + uv1 * tri.barycentricsDx.y + uv2 * tri.barycentricsDx.z;
        const float2 texcoordDy =
            uv0 * tri.barycentricsDy.x + uv1 * tri.barycentricsDy.y + uv2 * tri.barycentricsDy.z;
        if (SWAlphaTestFailed(output.alphaMaterial, texcoord, texcoordDx, texcoordDy))
        {
            SWTelemetryAdd(SW_COUNTER_ALPHA_REJECTED_PIXELS, 1u);
            return;
        }
    }
#endif

    if (output.debug.enabled)
    {
        WriteDebugPixel(debugVisTex, pixel, output.debug.payload);
    }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    SWCommitVirtualShadowTexel(texel, encodedDepth, dynamicLayer);
#else
    InterlockedMin(visBuffer[pixel], PackVisKey(depth, output.visibleClusterIndex, tri.triangleIndex));
#endif
    SWTelemetryAdd(SW_COUNTER_VISIBILITY_WRITES, 1u);
}

// Narrows [firstPixelOffset, lastPixelOffset] to where value + offset * step >= 0.
void SWClipScanlineConstraint(
    float value,
    float step,
    inout int firstPixelOffset,
    inout int lastPixelOffset,
    inout bool hasPixels)
{
    if (!hasPixels)
    {
        return;
    }

    if (step > 0.0f)
    {
        firstPixelOffset = max(firstPixelOffset, int(ceil(-value / step)));
    }
    else if (step < 0.0f)
    {
        lastPixelOffset = min(lastPixelOffset, int(floor(value / -step)));
    }
    else
    {
        hasPixels = value >= 0.0f;
    }

    hasPixels = hasPixels && firstPixelOffset <= lastPixelOffset;
}

void SWRasterTriangles(SWClusterWork work, CLodRasterCluster cluster, SWClusterFlags flags, ClodViewRasterInfo rasterInfo, uint GI)
{
    const uint viewID = cluster.viewID;
    ByteAddressBuffer slab = ResourceDescriptorHeap[cluster.slabDescriptorIndex];

    const uint triangleCount = cluster.triangleCount;
    const uint triangleStreamAddress = CLodRasterTriangleStreamAddress(cluster);
    const bool reverseWinding = cluster.reverseWinding != 0u;
    const int2 scissorMin = int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY);
    const int2 scissorMax = int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1);

    SWRasterOutput output;
    output.viewID = viewID;
    output.visibleClusterIndex = work.visibleClusterIndex;
    output.dynamicShadowLayer = cluster.dynamicShadowLayer;
    output.debug = SWBuildRasterDebug(work, flags);
    // Debug views show both faces too.
    const bool drawBackFaces = output.debug.enabled || flags.doubleSided != 0u;
#if CLOD_SW_RASTER_MAY_ALPHA_TEST
    output.alphaTestEnabled = flags.alphaTestEnabled != 0u;
    output.alphaMaterial = (SWAlphaMaterial)0;
    output.uvStream = (CLodUvStream)0;
    if (output.alphaTestEnabled)
    {
        output.alphaMaterial = SWLoadAlphaMaterial(cluster.materialDataIndex);
        output.uvStream = CLodRasterLoadUvStream(cluster, 0u);
    }
#endif

    RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_SW_VSM_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    const bool hasClipmapInfo = SWFindVirtualShadowClipmapInfo(viewID, clipmapInfos, clipmapInfo);
    // A local light face rasters each triangle into each of its mips (CLodVirtualShadowLocalMipPageOrigin): mip 0 on
    // the face's first slice, the rest on its second. The sun's clips are one pass over their slice.
    const bool localFace = hasClipmapInfo && CLodVirtualShadowClipmapIsLocal(clipmapInfo);
    const uint mipCount = localFace ? kCLodVirtualShadowLocalMipCount : 1u;
    const SWVirtualShadowTarget mip0Target = SWBuildVirtualShadowTarget(clipmapInfo);
    SWVirtualShadowTarget coarseMipsTarget = mip0Target;
    if (localFace)
    {
        coarseMipsTarget =
            SWBuildVirtualShadowTarget(clipmapInfos[clipmapInfo.pageTableLayer + CLodVirtualShadowLocalMipSliceOffset(1u)]);
    }
    output.vsmTarget = mip0Target;
    output.vsmPerspective = localFace;
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_SW_VSM_PAGE_TABLE_DESCRIPTOR_INDEX];
#else
    const uint mipCount = 1u;
    RWTexture2D<uint64_t> visBuffer =
        ResourceDescriptorHeap[NonUniformResourceIndex(rasterInfo.visibilityUAVDescriptorIndex)];
#endif

    for (uint triangleBlockBase = 0u; triangleBlockBase < triangleCount; triangleBlockBase += SW_CLUSTER_RASTER_THREADS)
    {
        // Every lane takes part in the wave load, including lanes past the last triangle.
        const uint triangleIndex = triangleBlockBase + GI;
        uint3 vertices = CLodDecodeTriangleWave32(slab, triangleStreamAddress, triangleBlockBase, GI);
        if (triangleIndex >= triangleCount)
        {
            continue;
        }
        if (reverseWinding)
        {
            vertices = vertices.xzy;
        }

        // One load per vertex: the depths (and 1/w) of the few surviving triangles come with it.
        SWVertex v0 = gs_vertices[vertices.x];
        SWVertex v1 = gs_vertices[vertices.y];
        SWVertex v2 = gs_vertices[vertices.z];
        float2 s0 = v0.xy;
        float2 s1 = v1.xy;
        float2 s2 = v2.xy;

        const float2 e01 = s1 - s0;
        const float2 e02 = s2 - s0;
        float twiceArea = e01.x * e02.y - e01.y * e02.x;
        if (twiceArea >= 0.0f)
        {
            if (!drawBackFaces || twiceArea == 0.0f)
            {
                SWTelemetryAdd(SW_COUNTER_BACKFACE_CULLED, 1u);
                continue;
            }
            // The rest of the rasterizer expects front-face winding. The triangle index is
            // unchanged, so the visibility buffer is the same as a hardware back face.
            vertices = vertices.xzy;
            const SWVertex swappedVertex = v1;
            v1 = v2;
            v2 = swappedVertex;
            s1 = v1.xy;
            s2 = v2.xy;
            twiceArea = -twiceArea;
        }

        // Behind-camera vertices project to meaningless positions.
        if (v0.z <= 0.0f || v1.z <= 0.0f || v2.z <= 0.0f)
        {
            SWTelemetryAdd(SW_COUNTER_DEGENERATE_OR_OFFSCREEN, 1u);
            continue;
        }

        // Positive scales keep the winding, so the face test above holds for every mip.
        const float2 faceS0 = s0;
        const float2 faceS1 = s1;
        const float2 faceS2 = s2;
        const float faceTwiceArea = twiceArea;
        [loop]
        for (uint mip = 0u; mip < mipCount; ++mip)
        {
        int2 targetMin = scissorMin;
        int2 targetMax = scissorMax;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (localFace)
        {
            // The mip's rectangle of its slice's canvas, which the face's mip 0 pixels scale into.
            const float mipScale = exp2(-(float)mip);
            const int2 mipOrigin = int2(CLodVirtualShadowLocalMipPageOrigin(mip) * kCLodVirtualShadowPhysicalPageSize);
            s0 = faceS0 * mipScale + float2(mipOrigin);
            s1 = faceS1 * mipScale + float2(mipOrigin);
            s2 = faceS2 * mipScale + float2(mipOrigin);
            twiceArea = faceTwiceArea * mipScale * mipScale;
            targetMin = mipOrigin;
            targetMax = mipOrigin + int(kCLodVirtualShadowFixedVirtualResolution >> mip) - 1;
            if (mip == 0u)
            {
                output.vsmTarget = mip0Target;
            }
            else
            {
                output.vsmTarget = coarseMipsTarget;
            }
        }
#endif

        // Pixels whose centers can be inside the triangle. Most triangles here are smaller than a
        // pixel and this range is empty for them, so reject before doing any setup.
        int2 minPixel = int2(ceil(min(min(s0, s1), s2) - 0.5f));
        int2 maxPixel = int2(floor(max(max(s0, s1), s2) - 0.5f));
        minPixel = max(max(minPixel, targetMin), int2(0, 0));
        maxPixel = min(maxPixel, targetMax);
        if (minPixel.x > maxPixel.x || minPixel.y > maxPixel.y)
        {
            SWTelemetryAdd(SW_COUNTER_DEGENERATE_OR_OFFSCREEN, 1u);
            continue;
        }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        SWVirtualShadowTrianglePage trianglePage;
        if (!hasClipmapInfo ||
            !SWVirtualShadowResolveTrianglePages(
                uint2(minPixel),
                uint2(maxPixel),
                output.vsmTarget,
                (cluster.vertexFlags & VERTEX_SKINNED) != 0u,
                pageTable,
                trianglePage))
        {
            continue;
        }
#endif

        // Only the few triangles that reach this point need these.
        SWRasterTriangle tri;
        tri.vertices = vertices;
        tri.triangleIndex = triangleIndex;
        tri.depths = float3(v0.z, v1.z, v2.z);
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        tri.vsmPage = trianglePage;
        if (output.vsmPerspective)
        {
            tri.depths = rcp(tri.depths);
        }
#else
        tri.invW = float3(v0.w, v1.w, v2.w);
#endif

        // Barycentrics at the first pixel center, and their steps.
        const float invTwiceArea = -1.0f / twiceArea;
        const float2 origin = float2(minPixel) + 0.5f;
        const float2 e12 = s2 - s1;
        const float2 e20 = s0 - s2;
        float scanlineB0 = ((origin.x - s1.x) * e12.y - (origin.y - s1.y) * e12.x) * invTwiceArea;
        float scanlineB1 = ((origin.x - s2.x) * e20.y - (origin.y - s2.y) * e20.x) * invTwiceArea;
        const float dxB0 = e12.y * invTwiceArea;
        const float dxB1 = e20.y * invTwiceArea;
        const float dxB2 = -(dxB0 + dxB1);
        const float dyB0 = -e12.x * invTwiceArea;
        const float dyB1 = -e20.x * invTwiceArea;
#if CLOD_SW_RASTER_MAY_ALPHA_TEST
        tri.barycentricsDx = float3(dxB0, dxB1, dxB2);
        tri.barycentricsDy = float3(dyB0, dyB1, -(dyB0 + dyB1));
#endif

        const int rectWidth = maxPixel.x - minPixel.x + 1;
        // Decided per wave to keep the lanes together.
        const bool useScanlineRanges = WaveActiveAnyTrue(rectWidth > 4);
#if CLOD_SW_RASTER_TELEMETRY
        bool coveredAnySample = false;
#endif

        for (int y = minPixel.y; y <= maxPixel.y; y++)
        {
            int firstPixelOffset = 0;
            int lastPixelOffset = rectWidth - 1;
            bool hasPixels = true;
            if (useScanlineRanges)
            {
                const float scanlineB2 = 1.0f - scanlineB0 - scanlineB1;
                SWClipScanlineConstraint(scanlineB0, dxB0, firstPixelOffset, lastPixelOffset, hasPixels);
                SWClipScanlineConstraint(scanlineB1, dxB1, firstPixelOffset, lastPixelOffset, hasPixels);
                SWClipScanlineConstraint(scanlineB2, dxB2, firstPixelOffset, lastPixelOffset, hasPixels);
            }

            if (hasPixels)
            {
                float b0 = scanlineB0 + float(firstPixelOffset) * dxB0;
                float b1 = scanlineB1 + float(firstPixelOffset) * dxB1;
                for (int x = minPixel.x + firstPixelOffset; x <= minPixel.x + lastPixelOffset; x++)
                {
                    const float b2 = 1.0f - b0 - b1;
                    // Scanline ranges already guarantee coverage up to rounding at the range ends.
                    if (useScanlineRanges || (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f))
                    {
#if CLOD_SW_RASTER_TELEMETRY
                        coveredAnySample = true;
#endif
                        SWRasterPixel(
                            tri,
                            output,
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                            visBuffer,
#endif
                            debugVisTex,
                            slab,
                            uint2(x, y),
                            float3(b0, b1, b2));
                    }
                    b0 += dxB0;
                    b1 += dxB1;
                }
            }

            scanlineB0 += dyB0;
            scanlineB1 += dyB1;
        }
#if CLOD_SW_RASTER_TELEMETRY
        if (!coveredAnySample)
        {
            SWTelemetryAdd(SW_COUNTER_NO_SAMPLE_COVERED, 1u);
        }
#endif
        } // mip
    }
}

// ---------------------------------------------------------------------------------------------
// One cluster
// ---------------------------------------------------------------------------------------------

// The setup of slot is in groupshared memory.
void SWRasterCluster(uint slot, StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer, uint GI)
{
    const SWClusterWork work = gs_clusterWork[slot];
    const CLodRasterCluster cluster = gs_clusters[slot];
    const SWClusterFlags flags = gs_clusterFlags[slot];
    const ClodViewRasterInfo rasterInfo = viewRasterInfoBuffer[CLodVisibleClusterViewID(work.packedCluster)];
    if (GI == 0u)
    {
        SWRecordCacheTelemetry(work, cluster);
        SWTelemetryAdd(SW_COUNTER_CLUSTERS, 1u);
        SWTelemetryAdd(SW_COUNTER_INPUT_TRIANGLES, cluster.triangleCount);
    }
    SWTransformVertices(work, cluster, rasterInfo, GI);
    GroupMemoryBarrierWithGroupSync();

    SWRasterTriangles(work, cluster, flags, rasterInfo, GI);
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

[Shader("node")]
[NodeID("SWRaster")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(SW_BATCH_MAX_CLUSTERS * SW_RASTER_GROUPS_PER_CLUSTER, 1, 1)]
[NumThreads(CLOD_WG_SW_RASTER_THREADS, 1, 1)]
void WG_SWRaster(
    DispatchNodeInputRecord<SWRasterBatchRecord> inputRecord,
    uint GI : SV_GroupIndex,
    uint3 groupId : SV_GroupID)
{
    const SWRasterBatchRecord batch = inputRecord.Get();

    const uint clusterIndex = groupId.x / SW_RASTER_GROUPS_PER_CLUSTER;
    if (clusterIndex >= batch.numClusters)
    {
        return;
    }

    const uint unsortedClusterIndex = batch.clusterIndices[clusterIndex];
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint2> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer =
        ResourceDescriptorHeap[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];

    SWClusterWork work;
    work.packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, unsortedClusterIndex);
    work.assemblyTransformIndex = visibleClusterTransformIndices[unsortedClusterIndex].x;
    work.paletteIndex = visibleClusterTransformIndices[unsortedClusterIndex].y;
    work.visibleClusterIndex = unsortedClusterIndex;
    work.hasResolvedSkinningSlot = false;
    work.resolvedSkinningSlot = 0xFFFFFFFFu;
    work.cachedPositionBase = 0xFFFFFFFFu;
    if (GI == 0u)
    {
        SWLoadClusterSetup(work, 0u);
    }
    GroupMemoryBarrierWithGroupSync();
    SWRasterCluster(0u, viewRasterInfoBuffer, GI);
}

#if !defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
// The compute raster's dispatch, as clodUtil's raster-bucket arguments lay it out.
SWClusterWork SWLoadComputeClusterWork(uint sortedClusterIndex)
{
    ByteAddressBuffer compactedVisibleClusters =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint2> compactedVisibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodSoftwareRasterMapping> sortedToUnsortedMapping =
        ResourceDescriptorHeap[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX];
    const CLodSoftwareRasterMapping mapping = sortedToUnsortedMapping[sortedClusterIndex];

    SWClusterWork work;
    work.packedCluster = CLodLoadVisibleClusterPacked(compactedVisibleClusters, sortedClusterIndex);
    work.assemblyTransformIndex = compactedVisibleClusterTransformIndices[sortedClusterIndex].x;
    work.paletteIndex = compactedVisibleClusterTransformIndices[sortedClusterIndex].y;
    work.visibleClusterIndex = mapping.unsortedClusterIndex;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    work.hasResolvedSkinningSlot = false;
    work.resolvedSkinningSlot = 0xFFFFFFFFu;
    work.cachedPositionBase = 0xFFFFFFFFu;
    if (CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> dynamicWindSkinCacheMapping =
            ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX];
        work.cachedPositionBase = dynamicWindSkinCacheMapping[sortedClusterIndex];
    }
#else
    work.hasResolvedSkinningSlot = true;
    work.resolvedSkinningSlot = mapping.skinningInstanceSlot;
    work.cachedPositionBase = 0xFFFFFFFFu;
#endif
    return work;
}

[shader("compute")]
[numthreads(SW_CLUSTER_RASTER_THREADS, 1, 1)]
void SWRasterIndirectCSMain(uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketID = IndirectCommandSignatureRootConstant2;
    const uint linearizedGroupID = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    const uint firstClusterIndex = linearizedGroupID * SW_RASTER_CLUSTERS_PER_GROUP;
    const uint bucketClusterCount = histogram[bucketID];
    if (firstClusterIndex >= bucketClusterCount)
    {
        return;
    }
    const uint groupClusterCount = min(SW_RASTER_CLUSTERS_PER_GROUP, bucketClusterCount - firstClusterIndex);
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer =
        ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];

    if (GI < groupClusterCount)
    {
        SWLoadClusterSetup(SWLoadComputeClusterWork(IndirectCommandSignatureRootConstant0 + firstClusterIndex + GI), GI);
    }
    if (GI == 0u)
    {
        gs_groupClusterCount = groupClusterCount;
    }
    GroupMemoryBarrierWithGroupSync();

    // The count comes back through groupshared memory to hide its bound, SW_RASTER_CLUSTERS_PER_GROUP,
    // from the driver's compiler: with the bound in sight, registers grew with it (80 at 1, 171 at 16).
    const uint loopClusterCount = gs_groupClusterCount;
    [loop]
    for (uint slot = 0u; slot < loopClusterCount; ++slot)
    {
        SWRasterCluster(slot, viewRasterInfoBuffer, GI);
        // The next cluster's vertices overwrite this one's.
        GroupMemoryBarrierWithGroupSync();
    }
}
#endif
