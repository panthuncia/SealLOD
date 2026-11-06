#ifndef CLOD_RESOLVE_VOXEL_HLSLI
#define CLOD_RESOLVE_VOXEL_HLSLI

// Resolves a pixel written by the voxel rasterizer: re-traces the voxel and derives its surface.
// Requires clodResolveMeshlet.hlsli.

uint3 CLodDecodeVoxelCubeCoord(uint packedCoord)
{
    return uint3(packedCoord & 0x3FFu, (packedCoord >> 10u) & 0x3FFu, (packedCoord >> 20u) & 0x3FFu);
}

bool ResolveVoxelMaskTest(uint2 mask, uint bitIndex)
{
    return bitIndex < 32u ? ((mask.x & (1u << bitIndex)) != 0u) : ((mask.y & (1u << (bitIndex - 32u))) != 0u);
}

bool ResolveRayBoxIntersect(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tEnter, out float tExit)
{
    tEnter = 0.0f;
    tExit = 3.402823e+38f;

    [unroll]
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        const float origin = rayOrigin[axis];
        const float dir = rayDir[axis];
        const float bMin = boxMin[axis];
        const float bMax = boxMax[axis];

        if (abs(dir) <= 1.0e-8f)
        {
            if (origin < bMin || origin > bMax)
            {
                return false;
            }
            continue;
        }

        const float invDir = 1.0f / dir;
        float t0 = (bMin - origin) * invDir;
        float t1 = (bMax - origin) * invDir;
        if (t0 > t1)
        {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }

        tEnter = max(tEnter, t0);
        tExit = min(tExit, t1);
        if (tExit < tEnter)
        {
            return false;
        }
    }

    return tExit >= 0.0f;
}

bool ResolveRaycastVoxelCubeDDA(float3 rayOrigin, float3 rayDir, uint2 occupancyMask, out float tHit)
{
    tHit = 0.0f;

    float tEnter = 0.0f;
    float tExit = 0.0f;
    if (!ResolveRayBoxIntersect(rayOrigin, rayDir, float3(0.0f, 0.0f, 0.0f), float3(4.0f, 4.0f, 4.0f), tEnter, tExit))
    {
        return false;
    }

    float currentT = max(tEnter, 0.0f) + 1.0e-4f;
    const float3 p = clamp(rayOrigin + rayDir * currentT, float3(0.0f, 0.0f, 0.0f), float3(3.9999f, 3.9999f, 3.9999f));
    int3 cell = int3(floor(p));
    const int3 stepDir = int3(rayDir.x >= 0.0f ? 1 : -1, rayDir.y >= 0.0f ? 1 : -1, rayDir.z >= 0.0f ? 1 : -1);

    const float largeT = 3.402823e+38f;
    float3 nextBoundary = float3(
        stepDir.x > 0 ? float(cell.x + 1) : float(cell.x),
        stepDir.y > 0 ? float(cell.y + 1) : float(cell.y),
        stepDir.z > 0 ? float(cell.z + 1) : float(cell.z));
    float3 tMax = float3(
        abs(rayDir.x) > 1.0e-8f ? (nextBoundary.x - rayOrigin.x) / rayDir.x : largeT,
        abs(rayDir.y) > 1.0e-8f ? (nextBoundary.y - rayOrigin.y) / rayDir.y : largeT,
        abs(rayDir.z) > 1.0e-8f ? (nextBoundary.z - rayOrigin.z) / rayDir.z : largeT);
    float3 tDelta = float3(
        abs(rayDir.x) > 1.0e-8f ? abs(1.0f / rayDir.x) : largeT,
        abs(rayDir.y) > 1.0e-8f ? abs(1.0f / rayDir.y) : largeT,
        abs(rayDir.z) > 1.0e-8f ? abs(1.0f / rayDir.z) : largeT);

    [loop]
    for (uint iter = 0u; iter < 16u; ++iter)
    {
        if (any(cell < 0) || any(cell >= 4))
        {
            break;
        }

        const uint cellIndex = (uint)cell.x | ((uint)cell.y << 2u) | ((uint)cell.z << 4u);
        if (ResolveVoxelMaskTest(occupancyMask, cellIndex))
        {
            tHit = currentT;
            return true;
        }

        if (tMax.x <= tMax.y && tMax.x <= tMax.z)
        {
            if (tMax.x > tExit)
            {
                break;
            }
            currentT = tMax.x + 1.0e-4f;
            cell.x += stepDir.x;
            tMax.x += tDelta.x;
        }
        else if (tMax.y <= tMax.z)
        {
            if (tMax.y > tExit)
            {
                break;
            }
            currentT = tMax.y + 1.0e-4f;
            cell.y += stepDir.y;
            tMax.y += tDelta.y;
        }
        else
        {
            if (tMax.z > tExit)
            {
                break;
            }
            currentT = tMax.z + 1.0e-4f;
            cell.z += stepDir.z;
            tMax.z += tDelta.z;
        }
    }

    return false;
}

MaterialInputs BuildVoxelMaterialInputs(
    MaterialInfo materialInfo,
    uint materialFlags,
    float3 normalWS,
    float3 posWS,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    float opacity)
{
    MaterialUvCache uvCache = BuildSingleUvCache(uv, dUVdx, dUVdy);
    const uint voxelMaterialFlags = materialFlags & ~(MATERIAL_NORMAL_MAP | MATERIAL_PARALLAX | MATERIAL_GEOMETRIC_DISPLACEMENT);
    MaterialUvBindings uvBindings = BuildMaterialUvBindings(materialInfo, voxelMaterialFlags, uvCache);

    MaterialInputs inputs;
    SampleMaterialFromUvCacheRuntime(
        uvCache,
        uvBindings,
        normalWS,
        posWS,
        float3(1.0f, 1.0f, 1.0f),
        materialInfo,
        voxelMaterialFlags,
        float3(0.0f, 0.0f, 0.0f),
        float3(0.0f, 0.0f, 0.0f),
        inputs);
    inputs.opacity *= opacity;
    return inputs;
}

float CLodVoxelMaxWorldUnitsPerObjectUnit(float4x4 localToWorld)
{
    const float3 axisScales = float3(
        length(mul(float4(1.0f, 0.0f, 0.0f, 0.0f), localToWorld).xyz),
        length(mul(float4(0.0f, 1.0f, 0.0f, 0.0f), localToWorld).xyz),
        length(mul(float4(0.0f, 0.0f, 1.0f, 0.0f), localToWorld).xyz));
    const float3 finiteAxisScales = float3(
        isfinite(axisScales.x) ? axisScales.x : 0.0f,
        isfinite(axisScales.y) ? axisScales.y : 0.0f,
        isfinite(axisScales.z) ? axisScales.z : 0.0f);
    return max(max(finiteAxisScales.x, finiteAxisScales.y), max(finiteAxisScales.z, 1.0e-6f));
}

float CLodVoxelEstimateObjectUnitsPerPixel(
    float3 worldPosition,
    float worldUnitsPerObjectUnit,
    float4x4 view,
    float projectionY,
    float zNear,
    bool isOrtho,
    float viewportHeight)
{
    const float projectionScale = max(abs(projectionY), 1.0e-5f) *
        (0.5f * max(viewportHeight, 1.0f));
    float worldUnitsPerPixel = rcp(max(projectionScale, 1.0e-5f));
    if (!isOrtho)
    {
        const float viewDepth = abs(mul(float4(worldPosition, 1.0f), view).z);
        worldUnitsPerPixel *= max(viewDepth, max(zNear, 1.0e-3f));
    }
    return worldUnitsPerPixel / max(worldUnitsPerObjectUnit, 1.0e-6f);
}

uint CLodVoxelHash(uint v)
{
    v ^= v >> 16u;
    v *= 0x7FEB352Du;
    v ^= v >> 15u;
    v *= 0x846CA68Bu;
    v ^= v >> 16u;
    return v;
}

float CLodVoxelHashToUnitFloat(uint v)
{
    return (float)(CLodVoxelHash(v) & 0x00FFFFFFu) / 16777216.0f;
}

uint CLodVoxelHilbertIndex(uint posX, uint posY)
{
    const uint width = 64u;
    uint index = 0u;
    posX &= width - 1u;
    posY &= width - 1u;
    for (uint curLevel = width / 2u; curLevel > 0u; curLevel /= 2u)
    {
        const uint regionX = (posX & curLevel) != 0u;
        const uint regionY = (posY & curLevel) != 0u;
        index += curLevel * curLevel * ((3u * regionX) ^ regionY);
        if (regionY == 0u)
        {
            if (regionX != 0u)
            {
                posX = (width - 1u) - posX;
                posY = (width - 1u) - posY;
            }

            const uint temp = posX;
            posX = posY;
            posY = temp;
        }
    }
    return index;
}

float2 CLodVoxelR2Sequence(uint n)
{
    return frac(float2(
        0.5f + (float)n * 0.7548776662466927f,
        0.5f + (float)n * 0.5698402909980532f));
}

float2 CLodVoxelSGGXSpatiotemporalSample(uint2 pixel, uint frameIndex, uint stableSeed)
{
    const uint hilbertIndex = CLodVoxelHilbertIndex(pixel.x, pixel.y);
    const uint temporalIndex = frameIndex & 63u;
    const float2 screenTemporal = CLodVoxelR2Sequence(hilbertIndex + 288u * temporalIndex);
    const float2 stableOffset = float2(
        CLodVoxelHashToUnitFloat(stableSeed),
        CLodVoxelHashToUnitFloat(stableSeed ^ 0xD1B54A35u));
    return frac(screenTemporal + stableOffset);
}

void CLodVoxelBuildOrthonormalBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    SGGXBuildOrthonormalBasis(direction, tangent, bitangent);
}

float3 CLodVoxelMulSGGX(float3 sDiag, float3 sOff, float3 v)
{
    return SGGXMul(sDiag, sOff, v);
}

float3 CLodVoxelDecodeOctAxis(float2 encoded)
{
    return SGGXDecodeOctAxis(encoded);
}

float3 CLodVoxelMulAxialSGGX(float4 sggxAxisAndSigmas, float3 v)
{
    return SGGXMulAxial(sggxAxisAndSigmas, v);
}

float CLodVoxelDetSGGX(float3 sDiag, float3 sOff)
{
    return SGGXDet(sDiag, sOff);
}

float3 CLodVoxelDominantSGGXNormal(float3 sDiag, float3 sOff)
{
    return SGGXDominantNormal(sDiag, sOff);
}

float3 CLodVoxelDominantAxialSGGXNormal(float4 sggxAxisAndSigmas)
{
    return SGGXDominantAxialNormal(sggxAxisAndSigmas);
}

float3 CLodVoxelSampleSGGXVNDF(float4 sggxAxisAndSigmas, float3 wi, float u1, float u2)
{
    return SGGXSampleVNDF(sggxAxisAndSigmas, wi, u1, u2);
}

float3x3 CLodAssemblyAwareNormalMatrix(PerObjectBuffer obj)
{
    return transpose((float3x3)obj.modelInverse);
}

bool ResolveClodVoxelCommonSampleFromPackedCluster(
    uint4 packedCluster,
    uint visibleClusterIndex,
    uint primID,
    float linearDepth,
    uint2 pixel,
    Camera cam,
    out ClodResolvedCommonSample sample)
{
    sample = (ClodResolvedCommonSample)0;

    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<CLodMeshMetadata> metadataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];

    const uint instanceIndex = CLodVisibleClusterInstanceID(packedCluster);
    const uint localGroupId = CLodVisibleClusterGroupID(packedCluster);
    const uint localPageIndex = CLodVisibleClusterVoxelPageIndex(packedCluster);
    const uint pageLocalClusterIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);

    if (VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return false;
    }
    StructuredBuffer<uint2> visibleClusterTransformIndices =
        ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint assemblyTransformIndex = visibleClusterTransformIndices[visibleClusterIndex].x;

    PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDraw(instanceIndex);
    // Match voxel rasterization: reconstruct the hit using the same transient
    // procedural-wind palette that positioned the cube in the visibility pass.
    instanceData.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(
        instanceIndex, instanceData.skinningInstanceSlot);
    const PerObjectBuffer obj = LoadInstanceTransformForDrawWithAssemblyTransform(instanceIndex, assemblyTransformIndex);
    const PerMeshBuffer mesh = perMeshBuffer[instanceData.perMeshBufferIndex];
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceIndex);
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup group = groups[localGroupId];
    if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
        localPageIndex < group.pageMapBase ||
        localPageIndex >= group.pageMapBase + group.pageCount)
    {
        return false;
    }

    GroupPageMapEntry pageEntry = CLodLoadVoxelPageMapEntry(metadata, group, localPageIndex);
    CLodVoxelPageHeader pageHeader = CLodLoadVoxelPageHeader(pageEntry.slabDescriptorIndex, pageEntry.slabByteOffset);
    if (pageHeader.formatAndKind != CLOD_VOXEL_PAGE_MAGIC ||
        pageLocalClusterIndex >= pageHeader.clusterCount)
    {
        return false;
    }

    const CLodVoxelClusterRecord voxelCluster = CLodLoadVoxelClusterFromPage(
        pageEntry.slabDescriptorIndex,
        pageEntry.slabByteOffset,
        pageHeader.descriptorOffset,
        pageLocalClusterIndex);
    const uint cubeLocalIndex = primID;
    if (cubeLocalIndex >= voxelCluster.cubeCount)
    {
        return false;
    }
    const CLodVoxelCubeRecord cube = CLodLoadVoxelCubeFromPage(
        pageEntry.slabDescriptorIndex,
        pageEntry.slabByteOffset,
        pageHeader.cubeRecordsOffset,
        voxelCluster.firstCube + cubeLocalIndex);
    const uint3 cubeCoord = CLodDecodeVoxelCubeCoord(cube.cubeCoord);
    const float voxelWidth = voxelCluster.aabbMinAndVoxelWidth.w;
    if (voxelWidth <= 0.0f)
    {
        return false;
    }
    const float3 cubeMinObject = voxelCluster.aabbMinAndVoxelWidth.xyz + float3(cubeCoord) * (voxelWidth * 4.0f);

    float4x4 skinMatrix = IdentitySkinMatrix();
    float4x4 previousSkinMatrix = IdentitySkinMatrix();
    float4x4 inverseSkinMatrix = IdentitySkinMatrix();
    if (cube.dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
    {
        const uint expandedBoneIndex = ResolveAssemblyBoneIndex(
            cube.dominantBoneIndex,
            metadata,
            assemblyTransformIndex);
        skinMatrix = LoadAssemblyLocalBoneSkinMatrix(
            instanceData.skinningInstanceSlot, expandedBoneIndex, assemblyTransformIndex);
        previousSkinMatrix = LoadPreviousAssemblyLocalBoneSkinMatrix(
            instanceData.skinningInstanceSlot, expandedBoneIndex, assemblyTransformIndex);
        inverseSkinMatrix = LoadAssemblyLocalBoneInverseSkinMatrix(
            instanceData.skinningInstanceSlot, expandedBoneIndex, assemblyTransformIndex);
    }
    const row_major matrix localToWorld = mul(skinMatrix, obj.model);
    const row_major matrix worldToLocal = mul(obj.modelInverse, inverseSkinMatrix);

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const float2 winSize = float2(perFrame.screenResX, perFrame.screenResY);
    const float2 pixelUv = (float2(pixel) + 0.5f) / winSize;
    const float2 ndc = float2(pixelUv.x * 2.0f - 1.0f, 1.0f - pixelUv.y * 2.0f);
    // Match voxelSoftwareRaster's ray construction. With reverse-Z, clip Z=0
    // can be the infinite far plane; unprojecting it as a point and subtracting
    // two transformed positions loses substantial precision for large instances.
    // Clip Z=1 is finite, and transforming the ray as a vector also avoids the
    // translation/subtraction cancellation entirely.
    float4 viewNear = mul(float4(ndc, 1.0f, 1.0f), cam.projectionInverse);
    viewNear.xyz /= max(viewNear.w, 1e-6f);
    const float3 rayOriginWS = cam.positionWorldSpace.xyz;
    const float3 rayOriginObject = mul(float4(rayOriginWS, 1.0f), worldToLocal).xyz;
    const float3 rayDirWS = mul(float4(viewNear.xyz, 0.0f), cam.viewInverse).xyz;
    const float3 rayDirObject = normalize(mul(float4(rayDirWS, 0.0f), worldToLocal).xyz);
    const float3 rayOriginCube = (rayOriginObject - cubeMinObject) / voxelWidth;
    const float3 rayDirCube = rayDirObject / voxelWidth;

    float tHitCube = 0.0f;
    if (!ResolveRaycastVoxelCubeDDA(rayOriginCube, rayDirCube, cube.occupancyMask, tHitCube))
    {
        return false;
    }

    const float3 objectPosition = rayOriginObject + rayDirObject * tHitCube;
    const float3 previousSkinnedObjectPosition = mul(float4(objectPosition, 1.0f), previousSkinMatrix).xyz;
    const float3 worldPosition = mul(float4(objectPosition, 1.0f), localToWorld).xyz;

    const int3 cell = clamp(int3(floor((objectPosition - cubeMinObject) / voxelWidth)), int3(0, 0, 0), int3(3, 3, 3));
    const uint cellIndex = (uint)cell.x | ((uint)cell.y << 2u) | ((uint)cell.z << 4u);

    CLodVoxelAttributeSample attributeSample = CLodLoadVoxelAttributeSampleFromPage(pageEntry, pageHeader, cube, cellIndex);
    const float4 sggxAxisAndSigmas = float4(
        attributeSample.sggxAxisAndSigmas.xy,
        max(attributeSample.sggxAxisAndSigmas.zw, float2(1.0e-4f, 1.0e-4f)));
    const uint sampleSeed = CLodVoxelHash(
        localGroupId * 0x9E3779B9u ^
        localPageIndex * 0x85EBCA6Bu ^
        pageLocalClusterIndex * 0xC2B2AE35u ^
        cube.cubeCoord * 0x27D4EB2Fu ^
        primID * 0x68BC21EBu ^
        cellIndex * 0xB5297A4Du);
    const float2 sggxXi = CLodVoxelSGGXSpatiotemporalSample(pixel, perFrame.frameIndex, sampleSeed);
    const float3 viewDirObject = -rayDirObject;
    float3 normalOS = CLodVoxelSampleSGGXVNDF(
        sggxAxisAndSigmas,
        viewDirObject,
        sggxXi.x,
        sggxXi.y);
    normalOS = dot(normalOS, viewDirObject) >= 0.0f ? normalOS : -normalOS;
    normalOS = normalize(mul(normalOS, (float3x3)skinMatrix));
    const float3x3 normalMatrix = CLodAssemblyAwareNormalMatrix(obj);
    const float3 normalWS = normalize(mul(normalOS, normalMatrix));

    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[mesh.materialDataIndex];
    const uint materialFlags = materialInfo.materialFlags;
    const float2 uvDensity = float2(
        isfinite(voxelCluster.uvDensity.x) ? max(voxelCluster.uvDensity.x, 0.0f) : 0.0f,
        isfinite(voxelCluster.uvDensity.y) ? max(voxelCluster.uvDensity.y, 0.0f) : 0.0f);
    const float worldUnitsPerObjectUnit = CLodVoxelMaxWorldUnitsPerObjectUnit(localToWorld);
    const float objectUnitsPerPixel = CLodVoxelEstimateObjectUnitsPerPixel(
        worldPosition,
        worldUnitsPerObjectUnit,
        cam.view,
        cam.projection._22,
        cam.zNear,
        cam.isOrtho,
        winSize.y);
    const float2 uvPerPixel = uvDensity * objectUnitsPerPixel;
    const float2 voxelDUdx = float2(uvPerPixel.x, 0.0f);
    const float2 voxelDUdy = float2(0.0f, uvPerPixel.y);

    sample.linearDepth = linearDepth;
    sample.clusterIndex = visibleClusterIndex;
    sample.meshletTriangleIndex = cubeLocalIndex;
    sample.meshletIndex = pageLocalClusterIndex;
    sample.geometryGroupIndex = localGroupId;
    sample.isVoxelPath = true;
    sample.positionWS = worldPosition;
    sample.positionVS = mul(float4(worldPosition, 1.0f), cam.view).xyz;
    sample.normalWSBase = normalWS;
    sample.normalOS = normalOS;
    sample.vertexColor = 1.0f.xxx;
    sample.materialDebugUv = attributeSample.uv;
    sample.materialDebugUvValid = 1.0f;
    sample.materialDebugUvDerivative = float2(length(voxelDUdx), length(voxelDUdy));
    sample.voxelDebugUvDensity = uvDensity;
    sample.dpdxWS = 0.0f.xxx;
    sample.dpdyWS = 0.0f.xxx;
    sample.motionVector = ComputeClodMotionVector(
        previousSkinnedObjectPosition,
        worldPosition,
        obj.prevModel,
        mul(cam.view, cam.unjitteredProjection),
        mul(cam.prevView, cam.prevUnjitteredProjection));
#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
    sample.materialInfo = (MaterialInfo)0;
#else
    sample.materialInfo = materialInfo;
#endif
    sample.materialFlags = materialFlags;
    sample.materialInputs = BuildVoxelMaterialInputs(
        materialInfo,
        materialFlags,
        normalWS,
        worldPosition,
        attributeSample.uv,
        voxelDUdx,
        voxelDUdy,
        saturate(attributeSample.opacity));
    return true;
}

#endif // CLOD_RESOLVE_VOXEL_HLSLI
