#ifndef CLOD_REYES_TRANSITION_HLSLI
#define CLOD_REYES_TRANSITION_HLSLI

#include "include/skinningCommon.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/vertexFlags.hlsli"

float CLodReyesFadeStartDistance(float fadeStartDistance)
{
    return max(fadeStartDistance, 0.0f);
}

float CLodReyesFadeEndDistance(float fadeEndDistance)
{
    return max(fadeEndDistance, 0.0f);
}

bool CLodReyesDisplacementFadeEnabled(float fadeStartDistance, float fadeEndDistance)
{
    return CLodReyesFadeEndDistance(fadeEndDistance) > CLodReyesFadeStartDistance(fadeStartDistance);
}

float CLodReyesDisplacementFade(float fadeStartDistance, float fadeEndDistance, CullingCameraInfo camera, float3 positionWorld)
{
    const float fadeStart = CLodReyesFadeStartDistance(fadeStartDistance);
    const float fadeEnd = CLodReyesFadeEndDistance(fadeEndDistance);
    if (fadeEnd <= fadeStart)
    {
        return 1.0f;
    }

    const float distanceToCamera = distance(positionWorld, camera.positionWorldSpace.xyz);
    const float t = saturate((distanceToCamera - fadeStart) / max(fadeEnd - fadeStart, 1.0e-6f));
    return 1.0f - (t * t * (3.0f - 2.0f * t));
}

bool CLodReyesSphereFullyBeyondCutoff(
    float fadeStartDistance,
    float fadeEndDistance,
    CullingCameraInfo camera,
    float3 centerWorld,
    float radiusWorld)
{
    if (!CLodReyesDisplacementFadeEnabled(fadeStartDistance, fadeEndDistance))
    {
        return false;
    }

    return distance(centerWorld, camera.positionWorldSpace.xyz) - max(radiusWorld, 0.0f) >= CLodReyesFadeEndDistance(fadeEndDistance);
}

float CLodMaxAxisScale_RowVector(row_major matrix m)
{
    const float3 row0 = float3(m._11, m._12, m._13);
    const float3 row1 = float3(m._21, m._22, m._23);
    const float3 row2 = float3(m._31, m._32, m._33);
    return sqrt(max(dot(row0, row0), max(dot(row1, row1), dot(row2, row2))));
}

BoundingSphere CLodComputeSkinnedMeshletBounds(
    CLodMeshletDescriptor desc,
    CLodPageHeader pageHeader,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint skinningInstanceSlot,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex)
{
    BoundingSphere staticBounds = { desc.bounds };
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot) || CLodDescBoneCount(desc) == 0u)
    {
        return staticBounds;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
    const uint boneListBase = pageSlabByteOffset + pageHeader.boneIndexStreamOffset + desc.boneListOffset * 4u;

    float3 mergedCenter = float3(0.0f, 0.0f, 0.0f);
    float mergedRadius = 0.0f;
    bool mergedInitialized = false;

    [loop]
    for (uint boneIndex = 0; boneIndex < CLodDescBoneCount(desc); ++boneIndex)
    {
        const uint jointIndex = ResolveAssemblyBoneIndex(
            slab.Load(boneListBase + boneIndex * 4u),
            metadata,
            assemblyTransformIndex);
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
        const float t = (newRadius - mergedRadius) / max(dist, 1.0e-12f);
        mergedCenter += delta * t;
        mergedRadius = newRadius;
    }

    if (!mergedInitialized)
    {
        return staticBounds;
    }

    BoundingSphere result = { float4(mergedCenter, mergedRadius * (1.0f + 1.0e-5f)) };
    return result;
}

BoundingSphere CLodComputeMeshletBounds(
    CLodMeshletDescriptor desc,
    CLodPageHeader pageHeader,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint vertexFlags,
    uint skinningInstanceSlot,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex)
{
    if ((vertexFlags & VERTEX_SKINNED) != 0u)
    {
        return CLodComputeSkinnedMeshletBounds(
            desc,
            pageHeader,
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            skinningInstanceSlot,
            metadata,
            assemblyTransformIndex);
    }

    BoundingSphere bounds = { desc.bounds };
    return bounds;
}

#endif // CLOD_REYES_TRANSITION_HLSLI
