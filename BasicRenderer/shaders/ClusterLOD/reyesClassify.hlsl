#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/vertexFlags.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodReyesTransition.hlsli"
#include "include/clodStructs.hlsli"
#include "PerPassRootConstants/clodReyesRootConstants.h"

static const uint REYES_CLASSIFY_GROUP_SIZE = 64u;
static const uint REYES_BARYCENTRIC_COORD_MAX = 0xFFFFu;
static const uint REYES_CLASSIFY_MODE_DEFAULT = 0u;
static const uint REYES_CLASSIFY_MODE_SHADOW_FINE_DISPLACED_ONLY = 1u;
static const uint REYES_CLASSIFY_MODE_SHADOW_COARSE_LARGE_ONLY = 2u;

bool TryAppendBudgetedEntry(RWStructuredBuffer<uint> counterBuffer, uint capacity, out uint dst)
{
    InterlockedAdd(counterBuffer[0], 1u, dst);
    if (dst < capacity)
    {
        return true;
    }

    InterlockedAdd(counterBuffer[0], 0xFFFFFFFFu);
    dst = 0u;
    return false;
}

uint GetReyesClassifyVisibleClusterReadIndex(uint linearizedID)
{
    uint readBase = 0u;
    if (CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_READ_BASE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> readBaseCounter = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_READ_BASE_COUNTER_DESCRIPTOR_INDEX];
        readBase = readBaseCounter.Load(0);
    }

    return readBase + linearizedID;
}

void MarkVisibleClusterOwnedByReyes(uint visibleClusterReadIndex)
{
    if (CLOD_REYES_CLASSIFY_OWNERSHIP_BITSET_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return;
    }

    RWStructuredBuffer<uint> ownershipWords = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_OWNERSHIP_BITSET_DESCRIPTOR_INDEX];
    const uint ownershipWordIndex = visibleClusterReadIndex >> 5u;
    const uint ownershipBitMask = 1u << (visibleClusterReadIndex & 31u);
    InterlockedOr(ownershipWords[ownershipWordIndex], ownershipBitMask);
}

[shader("compute")]
[numthreads(REYES_CLASSIFY_GROUP_SIZE, 1, 1)]
void ReyesClassifyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> visibleClusterCounter = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    const uint clusterCount = visibleClusterCounter[0];
    const uint clusterLinearIndex = dispatchThreadId.x;
    if (clusterLinearIndex >= clusterCount)
    {
        return;
    }

    const uint clusterIndex = GetReyesClassifyVisibleClusterReadIndex(clusterLinearIndex);

    ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, clusterIndex);
    if (CLodVisibleClusterIsVoxel(packedCluster))
    {
        return;
    }

    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);

    StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];

    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(instanceID);
    const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
    const uint materialIndex = perMesh.materialDataIndex;
    const MaterialInfo materialInfo = materialDataBuffer[materialIndex];

    const bool displacementEnabled = materialInfo.geometricDisplacementEnabled != 0u;
    const float displacementSpan = max(0.0f, materialInfo.geometricDisplacementMax - materialInfo.geometricDisplacementMin);
    const bool skinned = (perMesh.vertexFlags & VERTEX_SKINNED) != 0u;
    const bool hasMeaningfulDisplacement = displacementEnabled && displacementSpan > 1e-5f;
    bool fullyBeyondReyesCutoff = false;
    if (hasMeaningfulDisplacement)
    {
        const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
        const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
        const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
        const CLodPageHeader pageHeader = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
        const CLodMeshletDescriptor meshletDesc =
            LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, pageHeader.descriptorOffset, localMeshletIndex);
        const BoundingSphere meshletBounds = CLodComputeMeshletBounds(
            meshletDesc,
            pageHeader,
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            perMesh.vertexFlags,
            meshInstance.skinningInstanceSlot);
        const PerObjectBuffer objectData = LoadInstanceTransformForDraw(instanceID);
        const float uniformScale = CLodMaxAxisScale_RowVector(objectData.model);
        const float3 centerWorld = mul(float4(meshletBounds.sphere.xyz, 1.0f), objectData.model).xyz;
        const float radiusWorld = meshletBounds.sphere.w * uniformScale;
        StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
        ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
        fullyBeyondReyesCutoff = CLodReyesSphereFullyBeyondCutoff(
            perFrame.clodReyesDisplacementFadeStartDistance,
            perFrame.clodReyesDisplacementFadeEndDistance,
            cameras[CLodVisibleClusterViewID(packedCluster)],
            centerWorld,
            radiusWorld);
    }
    const bool hasEffectiveReyesDisplacement = hasMeaningfulDisplacement && !fullyBeyondReyesCutoff;
    const uint classifyMode = CLOD_REYES_CLASSIFY_MODE;
    uint routeKind = CLOD_REYES_ROUTE_VISIBILITY;
    bool emitOwnedCluster = false;
    bool emitFullCluster = false;

    if (classifyMode == REYES_CLASSIFY_MODE_DEFAULT)
    {
        routeKind = CLOD_REYES_ROUTE_VISIBILITY;
        emitOwnedCluster = hasEffectiveReyesDisplacement;
        emitFullCluster = !emitOwnedCluster;
    }
    else if (classifyMode == REYES_CLASSIFY_MODE_SHADOW_FINE_DISPLACED_ONLY)
    {
        routeKind = CLOD_REYES_ROUTE_FINE_MICROPOLY_VSM;
        emitOwnedCluster = hasEffectiveReyesDisplacement;
    }
    else if (classifyMode == REYES_CLASSIFY_MODE_SHADOW_COARSE_LARGE_ONLY)
    {
        routeKind = CLOD_REYES_ROUTE_COARSE_HARDWARE_VSM;
        emitOwnedCluster = !hasEffectiveReyesDisplacement;
    }

    const uint commonFlags =
        (skinned ? CLOD_REYES_FLAG_SKINNED : 0u) |
        (displacementEnabled ? CLOD_REYES_FLAG_DISPLACEMENT_ENABLED : 0u) |
        ((routeKind << CLOD_REYES_FLAG_ROUTE_SHIFT) & CLOD_REYES_FLAG_ROUTE_MASK);

    if (clusterLinearIndex == 0u)
    {
        RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_TELEMETRY_DESCRIPTOR_INDEX];
        telemetryBuffer[0].visibleClusterInputCount = clusterCount;
        telemetryBuffer[0].phaseIndex = CLOD_REYES_CLASSIFY_PHASE_INDEX;
    }

    if (emitFullCluster)
    {
        RWStructuredBuffer<uint> fullCounter = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
        RWStructuredBuffer<CLodReyesFullClusterOutput> fullClusters = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_FULL_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
        RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_TELEMETRY_DESCRIPTOR_INDEX];

        uint dst = 0u;
        if (!TryAppendBudgetedEntry(fullCounter, CLOD_REYES_CLASSIFY_FULL_CLUSTERS_CAPACITY, dst))
        {
            return;
        }

        fullClusters[dst].visibleClusterIndex = clusterIndex;
        fullClusters[dst].instanceID = instanceID;
        fullClusters[dst].materialIndex = materialIndex;
        fullClusters[dst].flags = commonFlags;
        InterlockedAdd(telemetryBuffer[0].fullClusterOutputCount, 1u);
        return;
    }

    if (!emitOwnedCluster)
    {
        return;
    }

    RWStructuredBuffer<uint> ownedClusterCounter = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesOwnedClusterEntry> ownedClusters = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];

    uint dst = 0u;
    if (!TryAppendBudgetedEntry(ownedClusterCounter, CLOD_REYES_CLASSIFY_OWNED_CLUSTERS_CAPACITY, dst))
    {
        return;
    }

    MarkVisibleClusterOwnedByReyes(clusterIndex);
    ownedClusters[dst].visibleClusterIndex = clusterIndex;
    ownedClusters[dst].instanceID = instanceID;
    ownedClusters[dst].materialIndex = materialIndex;
    ownedClusters[dst].flags = commonFlags;

    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_REYES_CLASSIFY_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryBuffer[0].ownedClusterOutputCount, 1u);
}
