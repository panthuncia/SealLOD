#ifndef __SKINNING_COMMON_HLSLI__
#define __SKINNING_COMMON_HLSLI__

#include "cbuffers.hlsli"
#include "structs.hlsli"
#include "vertex.hlsli"

typedef row_major float4x4 SkinningMatrix;

bool IsValidSkinningInstanceSlot(uint skinningInstanceSlot)
{
    return skinningInstanceSlot != 0xFFFFFFFFu;
}

float SkinningMaxAxisScale_RowVector(SkinningMatrix M)
{
    float sx = length(M[0].xyz);
    float sy = length(M[1].xyz);
    float sz = length(M[2].xyz);
    return max(sx, max(sy, sz));
}

float4x4 IdentitySkinMatrix();

SkinningInstanceGPUInfo LoadSkinningInstanceInfo(uint skinningInstanceSlot)
{
    StructuredBuffer<SkinningInstanceGPUInfo> skinningInstanceBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
    return skinningInstanceBuffer[skinningInstanceSlot];
}

float4x4 LoadBoneSkinMatrixFromInfo(SkinningInstanceGPUInfo skinningInfo, uint jointIndex)
{
    StructuredBuffer<SkinningMatrix> boneSkinMatricesBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];

    SkinningMatrix skin = boneSkinMatricesBuffer[skinningInfo.transformOffsetMatrices + jointIndex];
    if ((skinningInfo.flags & SkinningInstanceFlag_RowVectorSkinMatrix) != 0u)
    {
        return skin;
    }
    return transpose(skin);
}

float3 TransformPositionByBoneSkinMatrixFromInfo(SkinningInstanceGPUInfo skinningInfo, uint jointIndex, float3 position)
{
    StructuredBuffer<SkinningMatrix> boneSkinMatricesBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];

    SkinningMatrix skin = boneSkinMatricesBuffer[skinningInfo.transformOffsetMatrices + jointIndex];
    if ((skinningInfo.flags & SkinningInstanceFlag_RowVectorSkinMatrix) != 0u)
    {
        return position.x * skin[0].xyz +
            position.y * skin[1].xyz +
            position.z * skin[2].xyz +
            skin[3].xyz;
    }

    return mul(float4(position, 1.0f), transpose(skin)).xyz;
}

float4x4 LoadBoneSkinMatrix(uint skinningInstanceSlot, uint jointIndex)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        return IdentitySkinMatrix();
    }

    return LoadBoneSkinMatrixFromInfo(LoadSkinningInstanceInfo(skinningInstanceSlot), jointIndex);
}

float4x4 LoadBoneInverseSkinMatrix(uint skinningInstanceSlot, uint jointIndex)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        return IdentitySkinMatrix();
    }

    StructuredBuffer<SkinningInstanceGPUInfo> skinningInstanceBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
    StructuredBuffer<SkinningMatrix> inverseSkinMatricesBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::InverseSkinMatrices)];

    SkinningInstanceGPUInfo skinningInfo = skinningInstanceBuffer[skinningInstanceSlot];
    SkinningMatrix inverseSkin = inverseSkinMatricesBuffer[skinningInfo.inverseSkinOffsetMatrices + jointIndex];
    if ((skinningInfo.flags & SkinningInstanceFlag_RowVectorSkinMatrix) != 0u)
    {
        return inverseSkin;
    }
    return transpose(inverseSkin);
}

float4x4 AddWeightedBoneSkinMatrix(
    float4x4 skinMatrix,
    SkinningInstanceGPUInfo skinningInfo,
    uint jointIndex,
    float weight)
{
    if (weight != 0.0f)
    {
        skinMatrix += weight * LoadBoneSkinMatrixFromInfo(skinningInfo, jointIndex);
    }
    return skinMatrix;
}

float4x4 IdentitySkinMatrix()
{
    return float4x4(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
}

float4x4 BuildSkinMatrix(uint skinningInstanceSlot, SkinningInfluences skinning)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        return IdentitySkinMatrix();
    }

    SkinningInstanceGPUInfo skinningInfo = LoadSkinningInstanceInfo(skinningInstanceSlot);
    float4x4 skinMatrix = (float4x4)0;
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints0.x, skinning.weights0.x);
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints0.y, skinning.weights0.y);
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints0.z, skinning.weights0.z);
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints0.w, skinning.weights0.w);
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints1.x, skinning.weights1.x);
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints1.y, skinning.weights1.y);
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints1.z, skinning.weights1.z);
    skinMatrix = AddWeightedBoneSkinMatrix(skinMatrix, skinningInfo, skinning.joints1.w, skinning.weights1.w);
    return skinMatrix;
}

void AccumulateSkinnedPositionInfluence(
    SkinningInstanceGPUInfo skinningInfo,
    uint jointIndex,
    float weight,
    float3 sourcePosition,
    inout float3 skinnedPosition)
{
    if (weight != 0.0f)
    {
        skinnedPosition += weight * TransformPositionByBoneSkinMatrixFromInfo(skinningInfo, jointIndex, sourcePosition);
    }
}

float3 ApplySkinningToPosition(uint skinningInstanceSlot, SkinningInfluences skinning, float3 position)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        return position;
    }

    SkinningInstanceGPUInfo skinningInfo = LoadSkinningInstanceInfo(skinningInstanceSlot);
    float3 skinnedPosition = float3(0.0f, 0.0f, 0.0f);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints0.x, skinning.weights0.x, position, skinnedPosition);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints0.y, skinning.weights0.y, position, skinnedPosition);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints0.z, skinning.weights0.z, position, skinnedPosition);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints0.w, skinning.weights0.w, position, skinnedPosition);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints1.x, skinning.weights1.x, position, skinnedPosition);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints1.y, skinning.weights1.y, position, skinnedPosition);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints1.z, skinning.weights1.z, position, skinnedPosition);
    AccumulateSkinnedPositionInfluence(skinningInfo, skinning.joints1.w, skinning.weights1.w, position, skinnedPosition);
    return skinnedPosition;
}

void AccumulateSkinnedVertexInfluence(
    SkinningInstanceGPUInfo skinningInfo,
    uint jointIndex,
    float weight,
    float3 sourcePosition,
    float3 sourceNormal,
    float3 sourceTangent,
    inout float3 skinnedPosition,
    inout float3 skinnedNormal,
    inout float3 skinnedTangent)
{
    if (weight != 0.0f)
    {
        float4x4 skinMatrix = LoadBoneSkinMatrixFromInfo(skinningInfo, jointIndex);
        skinnedPosition += weight * mul(float4(sourcePosition, 1.0f), skinMatrix).xyz;
        skinnedNormal += weight * mul(sourceNormal, (float3x3)skinMatrix);
        skinnedTangent += weight * mul(sourceTangent, (float3x3)skinMatrix);
    }
}

void ApplySkinningToVertex(uint skinningInstanceSlot, SkinningInfluences skinning, inout Vertex vertex)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        vertex.skinning = skinning;
        return;
    }

    SkinningInstanceGPUInfo skinningInfo = LoadSkinningInstanceInfo(skinningInstanceSlot);
    const float3 sourcePosition = vertex.position;
    const float3 sourceNormal = vertex.normal;
    const float3 sourceTangent = vertex.tangent.xyz;
    float3 skinnedPosition = float3(0.0f, 0.0f, 0.0f);
    float3 skinnedNormal = float3(0.0f, 0.0f, 0.0f);
    float3 skinnedTangent = float3(0.0f, 0.0f, 0.0f);

    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints0.x, skinning.weights0.x, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);
    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints0.y, skinning.weights0.y, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);
    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints0.z, skinning.weights0.z, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);
    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints0.w, skinning.weights0.w, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);
    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints1.x, skinning.weights1.x, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);
    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints1.y, skinning.weights1.y, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);
    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints1.z, skinning.weights1.z, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);
    AccumulateSkinnedVertexInfluence(skinningInfo, skinning.joints1.w, skinning.weights1.w, sourcePosition, sourceNormal, sourceTangent, skinnedPosition, skinnedNormal, skinnedTangent);

    vertex.position = skinnedPosition;
    vertex.normal = skinnedNormal;
    vertex.tangent.xyz = skinnedTangent;
    vertex.skinning = skinning;
}

#endif // __SKINNING_COMMON_HLSLI__
