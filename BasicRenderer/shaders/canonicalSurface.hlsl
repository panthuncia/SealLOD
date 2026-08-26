#include "include/clodResolveCommon.hlsli"
#include "include/debugPayload.hlsli"

void WriteCanonicalSurfaceSample(uint2 pixel, float2 motionVector, MaterialInputs material)
{
    RWTexture2D<float4> baseColorOpacity = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::BaseColorOpacity)];
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    RWTexture2D<float4> specularAo = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::SpecularAo)];
    RWTexture2D<float4> emissiveTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Emissive)];
    RWTexture2D<float2> motionVectorTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
    RWTexture2D<float4> payload0Texture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Payload0)];
    RWTexture2D<float4> payload1Texture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Payload1)];
    RWTexture2D<uint2> surfaceIdentity = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Identity)];
    RWStructuredBuffer<SARPSurfaceRecordV1> surfaceRecords = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Records)];

    const OpenPBRMaterialInfo openPBR = LoadOpenPBRMaterialInfo(material.openPBRMaterialDataIndex);
    const float baseWeight = saturate(openPBR.baseWeight);
    const float specularWeight = saturate(openPBR.specularWeight);
    const float3 specularColor = saturate(openPBR.specularColor);
    const float3 weightedBaseColor = saturate(material.albedo * baseWeight);
    const float weightedIor = OpenPBRApplySpecularWeightToIor(openPBR.specularIor, specularWeight);
    const float3 dielectricF0 = saturate(specularColor * OpenPBRIorToF0(weightedIor));
    const float3 metalF0 = saturate(weightedBaseColor * specularColor);
    const float metalness = saturate(material.metallic);
    baseColorOpacity[pixel] = float4(weightedBaseColor * (1.0f - metalness), saturate(material.opacity));
    normalRoughness[pixel] = float4(normalize(material.normalWS), saturate(material.roughness));
    specularAo[pixel] = float4(lerp(dielectricF0, metalF0, metalness), saturate(material.ambientOcclusion));
    emissiveTexture[pixel] = float4(material.emissive, 0.0f);
    payload0Texture[pixel] = float4(material.coatColor, material.coatWeight);
    payload1Texture[pixel] = float4(material.fuzzColor, material.fuzzRoughness);
    motionVectorTexture[pixel] = motionVector;

    uint width;
    uint height;
    surfaceIdentity.GetDimensions(width, height);
    const uint recordIndex = pixel.y * width + pixel.x;
    const uint hasCoat = material.coatWeight > 0.0f ? (1u << 9u) : 0u;
    const uint hasFuzz = material.fuzzWeight > 0.0f ? (1u << 10u) : 0u;
    const uint hasGlint = material.glintEnabled != 0u ? (1u << 12u) : 0u;
    const uint metallicWorkflow = material.metallic > 0.0f ? (1u << 8u) : 0u;
    const uint flags = material.surfaceFlags | hasCoat | hasFuzz | hasGlint | metallicWorkflow;
    const uint payloadProfile = hasGlint != 0u ? 3u : ((hasCoat | hasFuzz) != 0u ? 1u : 0u);
    surfaceIdentity[pixel] = uint2(
        recordIndex,
        (material.semanticFamily & 0xffu) | ((payloadProfile & 0xffu) << 8u) | ((flags & 0xffffu) << 16u));

    SARPSurfaceRecordV1 record = (SARPSurfaceRecordV1)0;
    record.sourceObjectId = material.sourceObjectId;
    record.sourceMaterialId = material.sourceMaterialId;
    record.materialTableIndex = material.materialTableIndex;
    record.semanticFamilyAndPayload = (material.semanticFamily & 0xffffu) | ((payloadProfile & 0xffffu) << 16u);
    record.flags = flags;
    record.diagnosticReason = material.diagnosticReason;
    surfaceRecords[recordIndex] = record;

    if (VISBUF_MATERIAL_PIXEL_TELEMETRY_ENABLED != 0u)
    {
        RWTexture2D<uint2> telemetry =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
        const uint compileFlagsBinAndFrame = telemetry[pixel].x;
        const float albedoMaximum = max(material.albedo.x, max(material.albedo.y, material.albedo.z));
        const float weightedMaximum = max(weightedBaseColor.x, max(weightedBaseColor.y, weightedBaseColor.z));
        // C2: material evaluation supplied zero albedo. C3: the OpenPBR table
        // supplied zero base weight. C4: both inputs were nonzero but their
        // weighted result still collapsed to zero. C1 is a healthy write.
        uint outcomeMarker = 0xC1000000u;
        if (baseWeight <= 0.0f) outcomeMarker = 0xC3000000u;
        else if (albedoMaximum <= 0.0f)
        {
            const uint zeroReason = material.diagnosticReason & 0xFF000000u;
            outcomeMarker = zeroReason == 0xD1000000u ? 0xC5000000u
                : (zeroReason == 0xD2000000u ? 0xC6000000u
                    : (zeroReason == 0xD3000000u ? 0xC7000000u
                        : (zeroReason == 0xD4000000u ? 0xC8000000u
                            : (zeroReason == 0xD5000000u ? 0xC9000000u : 0xC2000000u))));
        }
        else if (weightedMaximum <= 0.0f) outcomeMarker = 0xC4000000u;
        telemetry[pixel] = uint2(
            compileFlagsBinAndFrame,
            outcomeMarker | (material.materialTableIndex & 0x00FFFFFFu));
    }
}

bool CLodAssemblyPartDebugColorFromVisKey(uint64_t vis, out float3 debugColor)
{
    debugColor = 0.0f.xxx;

    if (vis == 0xFFFFFFFFFFFFFFFF ||
        VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return false;
    }

    float visDepth;
    uint visClusterIndex;
    uint visPrimitiveIndex;
    UnpackVisKey(vis, visDepth, visClusterIndex, visPrimitiveIndex);

    if (visClusterIndex >= VISBUF_SARP_GRASS_INDEX_BASE)
    {
        return false;
    }

    if (visClusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE)
    {
        if (VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
        {
            return false;
        }

        StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue =
            ResourceDescriptorHeap[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
        visClusterIndex = diceQueue[visClusterIndex - VISBUF_REYES_PATCH_INDEX_BASE].visibleClusterIndex;
    }

    StructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint assemblyTransformIndex = visibleClusterTransformIndices[visClusterIndex];

    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, visClusterIndex);
    const uint instanceIndex = CLodVisibleClusterInstanceID(packedCluster);
    const uint localGroupId = CLodVisibleClusterGroupID(packedCluster);

    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceIndex);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup group = groups[metadata.groupsBase + localGroupId];

    if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
    {
        // Parent assembly LOD selected: this voxel payload represents collapsed child instances.
        debugColor = float3(1.0f, 0.58f, 0.08f);
    }
    else if (assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
    {
        // Traversal crossed an instance root and is rendering the child part.
        debugColor = max(HashToColor(assemblyTransformIndex + 17u), 0.18f.xxx);
    }
    else if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u)
    {
        debugColor = float3(0.82f, 0.18f, 1.0f);
    }
    else
    {
        // Ordinary/root-space CLod, kept deliberately muted.
        debugColor = float3(0.08f, 0.10f, 0.12f);
    }

    return true;
}

bool CLodAssemblyDirectPartIdFromVisKey(uint64_t vis, out uint directPartId)
{
    directPartId = 0u;

    if (vis == 0xFFFFFFFFFFFFFFFF ||
        VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return false;
    }

    float visDepth;
    uint visClusterIndex;
    uint visPrimitiveIndex;
    UnpackVisKey(vis, visDepth, visClusterIndex, visPrimitiveIndex);

    if (visClusterIndex >= VISBUF_SARP_GRASS_INDEX_BASE)
    {
        return false;
    }

    if (visClusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE)
    {
        if (VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
        {
            return false;
        }

        StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue =
            ResourceDescriptorHeap[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
        visClusterIndex = diceQueue[visClusterIndex - VISBUF_REYES_PATCH_INDEX_BASE].visibleClusterIndex;
    }

    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, visClusterIndex);
    const uint instanceIndex = CLodVisibleClusterInstanceID(packedCluster);
    const uint localGroupId = CLodVisibleClusterGroupID(packedCluster);

    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceIndex);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup group = groups[metadata.groupsBase + localGroupId];

    if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
    {
        directPartId = 0x40000000u ^ localGroupId;
        return true;
    }

    StructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint assemblyTransformIndex = visibleClusterTransformIndices[visClusterIndex];
    directPartId = assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL
        ? 1u
        : (0x80000000u + assemblyTransformIndex);
    return true;
}

void EvaluateCanonicalSurfaceOptimized(uint2 pixel, uint64_t vis)
{
#if defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    ClodGBufferColorSample sample;
    if (!ResolveClodGBufferColorSampleFromVisKey(vis, pixel, sample))
    {
        return;
    }

    WriteCanonicalSurfaceSample(pixel, sample.motionVector, sample.materialInputs);
    return;
#else
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    uint outputType = perFrame.outputType;

    if (outputType == OUTPUT_COLOR)
    {
        ClodGBufferColorSample sample;
        if (!ResolveClodGBufferColorSampleFromVisKey(vis, pixel, sample))
        {
            return;
        }

        WriteCanonicalSurfaceSample(pixel, sample.motionVector, sample.materialInputs);
        return;
    }

    ClodGBufferDebugSample sample;
    if (!ResolveClodGBufferDebugSampleFromVisKey(vis, pixel, sample))
    {
        return;
    }

    WriteCanonicalSurfaceSample(pixel, sample.motionVector, sample.materialInputs);

    bool isReyesPatch = false;
    if (outputType == OUTPUT_REYES_GEOMETRY_PATH && vis != 0xFFFFFFFFFFFFFFFF)
    {
        float visDepth;
        uint visClusterIndex;
        uint visPrimitiveIndex;
        UnpackVisKey(vis, visDepth, visClusterIndex, visPrimitiveIndex);
        isReyesPatch =
            visClusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE &&
            VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu;
    }

    RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
    uint2 payload = uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
    switch (outputType) {
        case OUTPUT_NORMAL:
            payload = PackDebugFloat3(sample.materialInputs.normalWS * 0.5 + 0.5);
            break;
        case OUTPUT_ALBEDO:
        case OUTPUT_TERRAIN_GRASS_OVERLAY:
            payload = PackDebugFloat3(sample.materialInputs.albedo);
            break;
        case OUTPUT_METALLIC:
            payload = PackDebugFloat3(sample.materialInputs.metallic.xxx);
            break;
        case OUTPUT_ROUGHNESS:
            payload = PackDebugFloat3(sample.materialInputs.roughness.xxx);
            break;
        case OUTPUT_EMISSIVE:
            payload = PackDebugFloat3(sample.materialInputs.emissive);
            break;
        case OUTPUT_AO:
            payload = PackDebugFloat3(sample.materialInputs.ambientOcclusion.xxx);
            break;
        case OUTPUT_TERRAIN_GEOMETRIC_HEIGHT:
            payload = PackDebugFloat3(sample.materialInputs.geometricHeightDebug.xxx);
            break;
        case OUTPUT_MESHLETS:
            payload = PackDebugUint(sample.meshletIndex);
            break;
        case OUTPUT_GEOMETRY_GROUP:
            payload = PackDebugUint(sample.geometryGroupIndex);
            break;
        case OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE:
        {
            float3 assemblyPartDebugColor;
            if (CLodAssemblyPartDebugColorFromVisKey(vis, assemblyPartDebugColor))
            {
                payload = PackDebugFloat3(assemblyPartDebugColor);
            }
            break;
        }
        case OUTPUT_CLOD_ASSEMBLY_PARTS:
        {
            uint directPartId;
            if (CLodAssemblyDirectPartIdFromVisKey(vis, directPartId))
            {
                payload = PackDebugUint(directPartId);
            }
            break;
        }
        case OUTPUT_MODEL_NORMALS:
            payload = PackDebugFloat3(sample.normalOS * 0.5 + 0.5);
            break;
        case OUTPUT_MOTION_VECTORS:
            payload = PackDebugFloat3(float3(sample.motionVector * 0.5 + 0.5, 0.5));
            break;
        case OUTPUT_MATERIAL_UV:
            payload = PackDebugFloat3(float3(frac(sample.materialDebugUv), sample.materialDebugUvValid));
            break;
        case OUTPUT_MATERIAL_UV_DERIVATIVE:
        {
            const float2 derivativeDebug = saturate(log2(sample.materialDebugUvDerivative * 4096.0f + 1.0f) / 12.0f);
            payload = PackDebugFloat3(float3(derivativeDebug, sample.materialDebugUvValid));
            break;
        }
        case OUTPUT_VOXEL_UV_DENSITY:
        {
            const float2 densityDebug = saturate(0.5f.xx + log2(max(sample.voxelDebugUvDensity, 1.0e-12f.xx)) / 24.0f);
            payload = PackDebugFloat3(sample.isVoxelPath ? float3(densityDebug, 1.0f) : 0.0f.xxx);
            break;
        }
        case OUTPUT_REYES_SOURCE_BARYCENTRICS:
            payload = PackDebugFloat3(saturate(sample.reyesDebugSourceBarycentrics));
            break;
        case OUTPUT_MATERIAL_EVAL_FEATURES:
        {
            const float runtimeBaseColorTexture = (sample.materialDebugFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u ? 1.0f : 0.0f;
#if defined(PSO_BASE_COLOR_TEXTURE)
            const float compiledBaseColorTexture = 1.0f;
#else
            const float compiledBaseColorTexture = 0.0f;
#endif
#if defined(PSO_CLOD_REYES_PATCH)
            const float reyesPatchVariant = 1.0f;
#else
            const float reyesPatchVariant = 0.0f;
#endif
            payload = PackDebugFloat3(float3(runtimeBaseColorTexture, compiledBaseColorTexture, reyesPatchVariant));
            break;
        }
        case OUTPUT_REYES_GEOMETRY_PATH:
            payload = PackDebugFloat3(isReyesPatch ? float3(0.10, 0.95, 0.20) : float3(0.95, 0.15, 0.15));
            break;
        case OUTPUT_VOXEL_GEOMETRY_PATH:
            payload = PackDebugFloat3(sample.isVoxelPath ? float3(0.95, 0.15, 0.15) : float3(0.10, 0.95, 0.20));
            break;
        case OUTPUT_MATERIAL_SELECTED_MIP:
            if (sample.materialInputs.selectedMaterialMipLevel != MATERIAL_DEBUG_INVALID_MIP_LEVEL)
            {
                payload = PackDebugUint2(sample.materialInputs.selectedMaterialMipLevel, sample.materialInputs.selectedMaterialMipMaxLevel);
            }
            break;
        case OUTPUT_PARALLAX_PIXELS:
            payload = PackDebugFloat3(sample.materialInputs.parallaxApplied != 0u ? 1.0f.xxx : 0.0f.xxx);
            break;
        case OUTPUT_TERRAIN_RVT_HIT:
            payload = PackDebugFloat3(
                (sample.materialInputs.terrainRvtDebugFlags & 0x2u) != 0u ? float3(0.10f, 0.95f, 0.20f) :
                ((sample.materialInputs.terrainRvtDebugFlags & 0x1u) != 0u ? float3(0.95f, 0.15f, 0.10f) : 0.0f.xxx));
            break;
        case OUTPUT_TERRAIN_RVT_REQUESTED_MIP:
            if (sample.materialInputs.terrainRvtRequestedMip != MATERIAL_DEBUG_INVALID_MIP_LEVEL)
            {
                payload = PackDebugUint2(sample.materialInputs.terrainRvtRequestedMip, 15u);
            }
            break;
        case OUTPUT_TERRAIN_RVT_RESIDENT_MIP:
            if (sample.materialInputs.terrainRvtResidentMip != MATERIAL_DEBUG_INVALID_MIP_LEVEL)
            {
                payload = PackDebugUint2(sample.materialInputs.terrainRvtResidentMip, 15u);
            }
            break;
        case OUTPUT_TERRAIN_RVT_VIRTUAL_PAGE:
            if (sample.materialInputs.terrainRvtPageTableIndex != 0xffffffffu)
            {
                payload = PackDebugUint(sample.materialInputs.terrainRvtPageTableIndex);
            }
            break;
        case OUTPUT_TERRAIN_RVT_PHYSICAL_PAGE:
            if (sample.materialInputs.terrainRvtPhysicalPageIndex != 0xffffffffu)
            {
                payload = PackDebugUint(sample.materialInputs.terrainRvtPhysicalPageIndex);
            }
            break;
        case OUTPUT_TERRAIN_RVT_PAGE_UV:
            if (sample.materialInputs.terrainRvtPageTableIndex != 0xffffffffu)
            {
                payload = PackDebugFloat3(float3(sample.materialInputs.terrainRvtPageUv, 0.0f));
            }
            break;
        case OUTPUT_TERRAIN_RVT_ATLAS_POOL:
            if (sample.materialInputs.terrainRvtAtlasPoolIndex != 0xffffffffu)
            {
                payload = PackDebugUint(sample.materialInputs.terrainRvtAtlasPoolIndex);
            }
            break;
        case OUTPUT_TERRAIN_RVT_ATLAS_UV:
            if (sample.materialInputs.terrainRvtPageTableIndex != 0xffffffffu)
            {
                payload = PackDebugFloat3(sample.materialInputs.terrainRvtAtlasUv);
            }
            break;
        case OUTPUT_TERRAIN_RVT_SAMPLED_ALBEDO:
            if ((sample.materialInputs.terrainRvtDebugFlags & 0x2u) != 0u)
            {
                payload = PackDebugFloat3(sample.materialInputs.terrainRvtSampleAlbedo);
            }
            break;
        case OUTPUT_TERRAIN_RVT_FALLBACK_REASON:
            payload = PackDebugUint(sample.materialInputs.terrainRvtFallbackReason);
            break;
        case OUTPUT_TERRAIN_RVT_OWNER_PAGE:
            if (sample.materialInputs.terrainRvtOwnerPageTableIndex != 0xffffffffu)
            {
                payload = PackDebugUint(sample.materialInputs.terrainRvtOwnerPageTableIndex);
            }
            break;
        case OUTPUT_TERRAIN_RVT_PAGE_DELTA:
            if (sample.materialInputs.terrainRvtPageTableIndex != 0xffffffffu)
            {
                payload = PackDebugUint(sample.materialInputs.terrainRvtPageTableIndex ^ sample.materialInputs.terrainRvtOwnerPageTableIndex);
            }
            break;
        case OUTPUT_TERRAIN_RVT_PHYSICAL_TILE_UV:
            if (sample.materialInputs.terrainRvtPageTableIndex != 0xffffffffu)
            {
                payload = PackDebugFloat3(float3(sample.materialInputs.terrainRvtPhysicalTileUv, 0.0f));
            }
            break;
        case OUTPUT_TERRAIN_RVT_SAMPLED_NORMAL:
            if ((sample.materialInputs.terrainRvtDebugFlags & 0x2u) != 0u)
            {
                payload = PackDebugFloat3(sample.materialInputs.terrainRvtSampleNormal * 0.5f + 0.5f);
            }
            break;
        case OUTPUT_TERRAIN_RVT_SAMPLED_MATERIAL:
            if ((sample.materialInputs.terrainRvtDebugFlags & 0x2u) != 0u)
            {
                payload = PackDebugFloat3(sample.materialInputs.terrainRvtSampleMaterial);
            }
            break;
        case OUTPUT_TERRAIN_RVT_SAMPLED_ALBEDO_POINT:
            if ((sample.materialInputs.terrainRvtDebugFlags & 0x2u) != 0u)
            {
                payload = PackDebugFloat3(sample.materialInputs.terrainRvtSampleAlbedoPoint);
            }
            break;
        case OUTPUT_TERRAIN_RVT_HEIGHT_SCALE:
            if ((sample.materialInputs.terrainRvtDebugFlags & 0x2u) != 0u)
            {
                payload = PackDebugFloat3(sample.materialInputs.terrainRvtHeightScale.xxx);
            }
            break;
    }
    if (payload.x != DEBUG_SENTINEL) {
        WriteDebugPixel(debugVisTex, pixel, payload);
    }
#endif
}

void EvaluateCanonicalSurfaceOptimized(uint2 pixel)
{
    Texture2D<uint64_t> visibilityTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    EvaluateCanonicalSurfaceOptimized(pixel, visibilityTexture[pixel]);
}

[numthreads(8, 8, 1)]
void PerViewPrimaryDepthCopyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint screenW = UintRootConstant2;
    uint screenH = UintRootConstant3;

    if (dispatchThreadId.x >= screenW || dispatchThreadId.y >= screenH)
    {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    Texture2D<uint64_t> visibilityTexture = ResourceDescriptorHeap[UintRootConstant0];
    uint64_t vis = visibilityTexture[pixel];

    float depth;
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        depth = asfloat(0x7F7FFFFF);
    }
    else
    {
        uint clusterIndex;
        uint meshletTriangleIndex;
        UnpackVisKey(vis, depth, clusterIndex, meshletTriangleIndex);
    }

    RWTexture2D<float> linearDepthTexture = ResourceDescriptorHeap[UintRootConstant1];
    linearDepthTexture[pixel] = depth;

    uint projectedDepthUAVIndex = UintRootConstant4;
    if (projectedDepthUAVIndex != 0xFFFFFFFFu)
    {
        float projectedDepth;
        if (vis == 0xFFFFFFFFFFFFFFFF)
        {
            projectedDepth = 0.0f;
        }
        else
        {
            float M22 = asfloat(UintRootConstant5);
            float M32 = asfloat(UintRootConstant6);
            projectedDepth = -M22 + M32 / depth;
        }
        RWTexture2D<float> projectedDepthTexture = ResourceDescriptorHeap[projectedDepthUAVIndex];
        projectedDepthTexture[pixel] = projectedDepth;
        if (UintRootConstant7 != 0xFFFFFFFFu)
        {
            RWTexture2D<float> canonicalDeviceDepth = ResourceDescriptorHeap[UintRootConstant7];
            canonicalDeviceDepth[pixel] = projectedDepth;
        }
    }
}

[numthreads(8, 8, 1)]
void PrimaryDepthCopyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    uint screenW = perFrameBuffer.screenResX;
    uint screenH = perFrameBuffer.screenResY;

    if (dispatchThreadId.x >= screenW || dispatchThreadId.y >= screenH)
    {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    Texture2D<uint64_t> visibilityTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];

    uint64_t vis = visibilityTexture[pixel];
    float depth;
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        depth = asfloat(0x7F7FFFFF);
    }
    else
    {
        uint clusterIndex;
        uint meshletTriangleIndex;
        UnpackVisKey(vis, depth, clusterIndex, meshletTriangleIndex);
    }

    RWTexture2D<float> linearDepthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    linearDepthTexture[pixel] = depth;
}
