#ifndef CLOD_RAY_TRACING_HITS_HLSLI
#define CLOD_RAY_TRACING_HITS_HLSLI

// Tracing the CLod BVH from compute shaders, with the same alpha test raster uses.

#include "include/clodRayTracingScene.hlsli"
#include "include/clodRasterCluster.hlsli"
#include "include/materialFlags.hlsli"
#include "include/clodRayTracingAlpha.hlsli"

#ifndef CLOD_RT_FIXED_ALPHA_MIP
#define CLOD_RT_FIXED_ALPHA_MIP 0
#endif

// RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY. The current preview DXC has the behaviour but not the name, and a query
// must opt in to touch clusters.
#define CLOD_RT_RAYQUERY_FLAGS 2

struct CLodRtHit
{
    bool hit;
    uint drawRecord;
    uint transformIndex; // The part portal occurrence's assembly transform, or sentinel.
    uint clasSlot;
    uint primitive;
    float2 barycentrics;
    float t;
    bool frontFace;
};

// The visible cluster record a CLAS slot stands for, as raster's culling would have written it.
uint4 CLodRtPackHitCluster(uint viewID, uint drawRecord, CLodRtClasInfo info)
{
    return CLodPackVisibleCluster(
        viewID,
        drawRecord,
        info.localMeshlet,
        CLodRtClasGroup(info),
        info.slabDescriptor,
        info.pageOffsetUnits << CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT,
        CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX);
}

// Materials reach their real image through streaming, as raster's alpha test does. Normally it has no say in what streams in,
// since raster asks for what it draws. With no raster, a leaf rejected by a low mip would never reach the closest hit that asks
// for a better one, so primary rays ask here as well.
// testMip: the cluster has micromaps, so its any-hit answers must be theirs. A cluster without them (merge hierarchy
// node geometry has none yet) samples the footprint's mip instead: asking every primary hit for the test mip keeps
// distant foliage textures at full resolution.
float CLodRtSampleAlpha(uint textureIndex, uint samplerIndex, uint streamingTextureID, float2 uv, float footprintUv, bool testMip)
{
#if CLOD_RT_FIXED_ALPHA_MIP
    if (testMip) {
    // See clodRayTracingAlpha.hlsli: one mip whatever the distance, which opacity micromaps are baked against.
    const CLodRtAlphaImage alphaImage = CLodRtResolveAlphaImage(textureIndex, streamingTextureID);
    Texture2D<float4> fixedImage = ResourceDescriptorHeap[NonUniformResourceIndex(alphaImage.descriptor)];
    SamplerState fixedSampler = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];
#if defined(CLOD_RESOLVE_PATH_HIT)
    if (g_clodPathHitIsPrimary && streamingTextureID != 0u) {
        const TextureStreamingGPUInfo streamingInfo = LoadTextureStreamingInfo(streamingTextureID);
        if (streamingInfo.imageDescriptorIndex != 0xFFFFFFFFu) {
            uint residentWidth = 1u;
            uint residentHeight = 1u;
            fixedImage.GetDimensions(residentWidth, residentHeight);
            const float2 texelScale = ResolveTextureStreamingTexelScale(streamingInfo, residentWidth, residentHeight);
            const float testFootprint = exp2(float(alphaImage.absoluteMip)) / float(alphaImage.fullMaxDimension);
            RecordTextureStreamingFeedback(streamingInfo, streamingTextureID, float2(testFootprint, 0.0f) * texelScale, float2(0.0f, testFootprint) * texelScale);
        }
    }
#endif
    return fixedImage.SampleLevel(fixedSampler, uv, float(alphaImage.mip)).a;
    }
#endif
    if (streamingTextureID != 0u) {
        StructuredBuffer<TextureStreamingGPUInfo> streaming =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Material::TextureStreamingMetadataBuffer)];
        const uint streamedImage = streaming[streamingTextureID].imageDescriptorIndex;
        if (streamedImage != 0xFFFFFFFFu) {
            textureIndex = streamedImage;
        }
    }
    Texture2D<float4> image = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    SamplerState imageSampler = SamplerDescriptorHeap[NonUniformResourceIndex(samplerIndex)];
    uint width = 1u;
    uint height = 1u;
    image.GetDimensions(width, height);
#if defined(CLOD_RESOLVE_PATH_HIT)
    if (g_clodPathHitIsPrimary && streamingTextureID != 0u) {
        const TextureStreamingGPUInfo streamingInfo = LoadTextureStreamingInfo(streamingTextureID);
        if (streamingInfo.imageDescriptorIndex != 0xFFFFFFFFu) {
            const float2 texelScale = ResolveTextureStreamingTexelScale(streamingInfo, width, height);
            RecordTextureStreamingFeedback(streamingInfo, streamingTextureID, float2(footprintUv, 0.0f) * texelScale, float2(0.0f, footprintUv) * texelScale);
        }
    }
#endif
    const float footprintTexels = footprintUv * float(max(width, height));
    const float mipLevel = log2(max(footprintTexels, 1.0f));
    return image.SampleLevel(imageSampler, uv, mipLevel).a;
}

// Ray barycentrics are perspective correct already, so unlike raster nothing needs converting.
//
// The mip comes from a ray cone. Hardware raster samples alpha with screen derivatives, and at a distance those pick
// coarser, denser mips of a foliage texture; testing mip 0 instead would leave trees full of holes raster does not have.
bool CLodRtTriangleIsTransparentAt(CLodRtClasInfo info, uint drawRecord, uint transformIndex, uint primitive, float2 barycentrics, float3 rayDirection, float t, float coneSpread)
{
    CLodRasterClusterSource source = (CLodRasterClusterSource)0;
    source.packedCluster = CLodRtPackHitCluster(0u, drawRecord, info);
    source.assemblyTransformIndex = transformIndex;
    source.paletteIndex = CLOD_PALETTE_INDEX_FROM_PAGE;
    CLodRasterCluster cluster;
    if (CLodLoadRasterCluster(source, cluster) != CLOD_RASTER_CLUSTER_OK) {
        return true;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(cluster.slabDescriptorIndex)];
    const uint triangleAddress = CLodRasterTriangleStreamAddress(cluster) + primitive * 3u;
    const uint word = slab.Load(triangleAddress & ~3u);
    const uint nextWord = slab.Load((triangleAddress & ~3u) + 4u);
    const uint shift = (triangleAddress & 3u) * 8u;
    const uint packedIndices = shift == 0u ? word : ((word >> shift) | (nextWord << (32u - shift)));
    const uint3 indices = uint3(packedIndices & 0xFFu, (packedIndices >> 8u) & 0xFFu, (packedIndices >> 16u) & 0xFFu);

    const CLodUvStream uvStream = CLodRasterLoadUvStream(cluster, 0u);
    const float2 uv0 = CLodDecodeUv(slab, uvStream, indices.x);
    const float2 uv1 = CLodDecodeUv(slab, uvStream, indices.y);
    const float2 uv2 = CLodDecodeUv(slab, uvStream, indices.z);
    const float2 uv = uv0 * (1.0f - barycentrics.x - barycentrics.y) + uv1 * barycentrics.x + uv2 * barycentrics.y;

    const row_major matrix model = LoadInstanceTransformForDrawRecordWithAssemblyTransform(
        LoadInstanceDrawRecord(drawRecord), transformIndex).model;
    const float3 p0 = mul(float4(CLodRasterLoadPosition(cluster, slab, indices.x), 1.0f), model).xyz;
    const float3 edge1 = mul(float4(CLodRasterLoadPosition(cluster, slab, indices.y), 1.0f), model).xyz - p0;
    const float3 edge2 = mul(float4(CLodRasterLoadPosition(cluster, slab, indices.z), 1.0f), model).xyz - p0;
    const float3 areaVector = cross(edge1, edge2);
    const float worldArea = max(length(areaVector), 1e-12f);
    const float2 uvEdge1 = uv1 - uv0;
    const float2 uvEdge2 = uv2 - uv0;
    const float uvArea = abs(uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x);
    const float facing = max(abs(dot(areaVector / worldArea, rayDirection)), 0.05f);
    const float footprintUv = t * coneSpread * sqrt(uvArea / worldArea) / facing;

    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const MaterialInfo material = materials[cluster.materialDataIndex];
    // The material's KHR_texture_transform. Such a material has no micromaps (Material::SetUvTransform).
    const float2 materialUv = TransformMaterialUv(material, uv);
    const float materialFootprintUv = footprintUv * MaterialUvTransformFootprintScale(material.uvTransformLinearDelta);
    float alpha = material.baseColorFactor.a;
    if ((material.materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u) {
        alpha *= CLodRtSampleAlpha(material.baseColorTextureIndex, material.baseColorSamplerIndex, material.baseColorStreamingTextureID, materialUv, materialFootprintUv,
            CLodRtClasHasMicromaps(info));
    }
    if ((material.materialFlags & MATERIAL_OPACITY_TEXTURE) != 0u) {
        alpha *= CLodRtSampleAlpha(material.opacityTextureIndex, material.opacitySamplerIndex, material.opacityStreamingTextureID, materialUv, materialFootprintUv,
            CLodRtClasHasMicromaps(info));
    }
    return !CLodRtAlphaIsOpaque(alpha, material.alphaCutoff);
}

// coneSpread: how fast the ray footprint grows with distance, in radians.
// instanceID and instanceIndex are the candidate's InstanceID() and InstanceIndex().
bool CLodRtCandidateIsTransparent(StructuredBuffer<CLodRtClasInfo> clasInfo, uint instanceID, uint instanceIndex, uint slot, uint primitive, float2 barycentrics, float3 rayDirection, float t, float coneSpread)
{
    return CLodRtTriangleIsTransparentAt(clasInfo[slot], CLodRtInstanceDraw(instanceID), CLodRtInstanceTransform(instanceID, instanceIndex),
        primitive, barycentrics, rayDirection, t, coneSpread);
}

CLodRtHit CLodRtTrace(RaytracingAccelerationStructure scene, StructuredBuffer<CLodRtClasInfo> clasInfo, RayDesc ray, uint rayFlags, uint instanceMask, float coneSpread)
{
    RayQuery<RAY_FLAG_NONE, CLOD_RT_RAYQUERY_FLAGS> query;
    query.TraceRayInline(scene, rayFlags, instanceMask, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && !CLodRtCandidateIsTransparent(clasInfo, query.CandidateInstanceID(), query.CandidateInstanceIndex(), query.CandidateClusterID(),
                query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), ray.Direction, query.CandidateTriangleRayT(), coneSpread)) {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    CLodRtHit hit = (CLodRtHit)0;
    hit.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    if (hit.hit) {
        hit.drawRecord = CLodRtInstanceDraw(query.CommittedInstanceID());
        hit.transformIndex = CLodRtInstanceTransform(query.CommittedInstanceID(), query.CommittedInstanceIndex());
        hit.clasSlot = query.CommittedClusterID();
        hit.primitive = query.CommittedPrimitiveIndex();
        hit.barycentrics = query.CommittedTriangleBarycentrics();
        hit.t = query.CommittedRayT();
        hit.frontFace = query.CommittedTriangleFrontFace();
    }
    return hit;
}

// The angular size of one pixel, from the focal length of the projection.
float CLodRtPixelConeSpread(Camera camera, float screenHeight)
{
    return 2.0f / max(abs(camera.projection[1][1]) * screenHeight, 1e-3f);
}

// A camera ray through a pixel center, consistent with how the resolve projects triangles: same jittered matrices,
// same Y flip. TMin is the near plane, as raster clips there.
RayDesc CLodRtCameraRayAt(Camera camera, float2 pixelPosition, float2 resolution)
{
    const float2 uv = pixelPosition / resolution;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 viewPoint = mul(float4(ndc, 1.0f, 1.0f), camera.projectionInverse);
    viewPoint.xyz /= max(abs(viewPoint.w), 1e-6f);
    const float3 direction = normalize(mul(float4(viewPoint.xyz, 0.0f), camera.viewInverse).xyz);
    const float3 forward = -camera.viewInverse[2].xyz;

    RayDesc ray;
    ray.Origin = camera.positionWorldSpace.xyz;
    ray.Direction = direction;
    ray.TMin = camera.zNear / max(dot(direction, forward), 1e-3f);
    ray.TMax = 1e20f;
    return ray;
}

RayDesc CLodRtCameraRay(Camera camera, uint2 pixel, float2 resolution)
{
    return CLodRtCameraRayAt(camera, float2(pixel) + 0.5f, resolution);
}

#endif // CLOD_RAY_TRACING_HITS_HLSLI
