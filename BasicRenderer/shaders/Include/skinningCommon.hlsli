#ifndef __SKINNING_COMMON_HLSLI__
#define __SKINNING_COMMON_HLSLI__

#include "cbuffers.hlsli"
#include "structs.hlsli"
#include "vertex.hlsli"
#include "clodStructs.hlsli"
#include "instanceDrawRecordHelpers.hlsli"

typedef row_major float4x4 SkinningMatrix;

bool IsValidSkinningInstanceSlot(uint skinningInstanceSlot)
{
    return skinningInstanceSlot != 0xFFFFFFFFu;
}

uint ResolveProceduralWindSkinningSlot(uint drawRecordIndex, uint sourceSlot)
{
    if (!IsValidSkinningInstanceSlot(sourceSlot)) return sourceSlot;
    StructuredBuffer<SkinningInstanceGPUInfo> infos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
	uint infoCount = 0u;
	uint infoStride = 0u;
	infos.GetDimensions(infoCount, infoStride);
    StructuredBuffer<InstanceDrawRecordBuffer> drawRecords =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::InstanceDrawRecordBuffer)];
	uint drawRecordCount = 0u;
	uint drawRecordStride = 0u;
	drawRecords.GetDimensions(drawRecordCount, drawRecordStride);
	if (drawRecordIndex >= drawRecordCount) return 0xFFFFFFFFu;
	const InstanceDrawRecordBuffer drawRecord = drawRecords[drawRecordIndex];
	const uint effectiveSourceSlot = IsValidSkinningInstanceSlot(drawRecord.skinningTypeSlot)
		? drawRecord.skinningTypeSlot
		: sourceSlot;
	if (effectiveSourceSlot >= infoCount) return 0xFFFFFFFFu;
	const SkinningInstanceGPUInfo source = infos[effectiveSourceSlot];
	if ((source.flags & 2u) == 0u) return sourceSlot;
	if (drawRecord.instanceTransformIndex > 0xFFFFFFFFu - 65536u) return 0xFFFFFFFFu;
	const uint transientSlot = 65536u + drawRecord.instanceTransformIndex;
	if (transientSlot >= infoCount) return 0xFFFFFFFFu;
	const SkinningInstanceGPUInfo transientInfo = infos[transientSlot];
	return transientInfo.boneCount != 0u ? transientSlot : 0xFFFFFFFFu;
}

uint ResolveAssemblyProceduralWindSkinningSlot(
    uint drawRecordIndex,
    uint sourceSlot,
    uint assemblyTransformIndex)
{
    // Procedural-wind transient skeletons are created only for skinned
    // assembly placements. Keep ordinary rigid/skinned instances entirely
    // off the descriptor-heavy resolution path.
    if (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL ||
        !IsValidSkinningInstanceSlot(sourceSlot))
    {
        return sourceSlot;
    }
    return ResolveProceduralWindSkinningSlot(drawRecordIndex, sourceSlot);
}

uint ResolveAssemblyBoneIndex(
    uint localJointId,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex)
{
    if (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL ||
        metadata.assemblyBoneRemapCount == 0u ||
        assemblyTransformIndex < metadata.assemblyTransformBase)
    {
        return localJointId;
    }

    const uint localTransformIndex = assemblyTransformIndex - metadata.assemblyTransformBase;
    if (localTransformIndex >= metadata.assemblyBoneRemapCount)
    {
        return localJointId;
    }

    StructuredBuffer<ClusterLODAssemblyBoneRemap> remaps =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemaps)];
    const ClusterLODAssemblyBoneRemap remap = remaps[metadata.assemblyBoneRemapBase + localTransformIndex];
    if (remap.remapIndexBase == CLOD_ASSEMBLY_BONE_REMAP_SENTINEL ||
        localJointId >= remap.remapIndexCount)
    {
        return localJointId;
    }

    StructuredBuffer<uint> remapIndices =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemapIndices)];
    return remapIndices[remap.remapIndexBase + localJointId];
}

SkinningInfluences ResolveAssemblySkinningInfluences(
    SkinningInfluences skinning,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex)
{
    if (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL ||
        metadata.assemblyBoneRemapCount == 0u ||
        assemblyTransformIndex < metadata.assemblyTransformBase)
    {
        return skinning;
    }

    const uint localTransformIndex = assemblyTransformIndex - metadata.assemblyTransformBase;
    if (localTransformIndex >= metadata.assemblyBoneRemapCount)
    {
        return skinning;
    }

    StructuredBuffer<ClusterLODAssemblyBoneRemap> remaps =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemaps)];
    const ClusterLODAssemblyBoneRemap remap = remaps[metadata.assemblyBoneRemapBase + localTransformIndex];
    if (remap.remapIndexBase == CLOD_ASSEMBLY_BONE_REMAP_SENTINEL)
    {
        return skinning;
    }

    StructuredBuffer<uint> remapIndices =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemapIndices)];
    skinning.joints0.x = skinning.joints0.x < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints0.x] : skinning.joints0.x;
    skinning.joints0.y = skinning.joints0.y < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints0.y] : skinning.joints0.y;
    skinning.joints0.z = skinning.joints0.z < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints0.z] : skinning.joints0.z;
    skinning.joints0.w = skinning.joints0.w < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints0.w] : skinning.joints0.w;
    skinning.joints1.x = skinning.joints1.x < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints1.x] : skinning.joints1.x;
    skinning.joints1.y = skinning.joints1.y < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints1.y] : skinning.joints1.y;
    skinning.joints1.z = skinning.joints1.z < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints1.z] : skinning.joints1.z;
    skinning.joints1.w = skinning.joints1.w < remap.remapIndexCount ? remapIndices[remap.remapIndexBase + skinning.joints1.w] : skinning.joints1.w;
    return skinning;
}

float SkinningMaxAxisScale_RowVector(SkinningMatrix M)
{
    // Conservative upper bound for the spectral norm of the affine linear
    // part. Max row length is not conservative when assembly conjugation
    // introduces shear. Gershgorin on A*A^T is exact for orthogonal rows and
    // remains conservative for the general case.
    const float3 r0 = M[0].xyz;
    const float3 r1 = M[1].xyz;
    const float3 r2 = M[2].xyz;
    const float d0 = dot(r0, r0);
    const float d1 = dot(r1, r1);
    const float d2 = dot(r2, r2);
    const float o01 = abs(dot(r0, r1));
    const float o02 = abs(dot(r0, r2));
    const float o12 = abs(dot(r1, r2));
    return sqrt(max(
        d0 + o01 + o02,
        max(d1 + o01 + o12, d2 + o02 + o12)));
}

float4x4 IdentitySkinMatrix();

SkinningInstanceGPUInfo LoadSkinningInstanceInfo(uint skinningInstanceSlot)
{
    StructuredBuffer<SkinningInstanceGPUInfo> skinningInstanceBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::SkinningInstanceInfo)];
    return skinningInstanceBuffer[skinningInstanceSlot];
}

uint ResolveSkeletonLodBoneIndex(SkinningInstanceGPUInfo skinningInfo, uint jointIndex)
{
	if (skinningInfo.boneRemapDescriptor == 0xFFFFFFFFu) return jointIndex;
	if (jointIndex >= skinningInfo.sourceBoneCount) return 0xFFFFFFFFu;
	StructuredBuffer<uint> remap = ResourceDescriptorHeap[skinningInfo.boneRemapDescriptor];
	const uint compact = remap[skinningInfo.boneRemapOffset + jointIndex];
	return compact < skinningInfo.boneCount ? compact : 0xFFFFFFFFu;
}

float4x4 LoadBoneSkinMatrixFromInfo(SkinningInstanceGPUInfo skinningInfo, uint jointIndex)
{
    StructuredBuffer<SkinningMatrix> boneSkinMatricesBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];

	jointIndex = ResolveSkeletonLodBoneIndex(skinningInfo, jointIndex);
	if (jointIndex == 0xFFFFFFFFu) return IdentitySkinMatrix();
    SkinningMatrix skin = boneSkinMatricesBuffer[skinningInfo.transformOffsetMatrices + jointIndex];
    // CPU uploads and GPU writers both encode shader-native row-vector matrices.
    // Keep orientation conversion at the upload boundary, not in every consumer.
    return skin;
}

float3 TransformPositionByBoneSkinMatrixFromInfo(SkinningInstanceGPUInfo skinningInfo, uint jointIndex, float3 position)
{
    StructuredBuffer<SkinningMatrix> boneSkinMatricesBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];

	jointIndex = ResolveSkeletonLodBoneIndex(skinningInfo, jointIndex);
	if (jointIndex == 0xFFFFFFFFu) return position;
    SkinningMatrix skin = boneSkinMatricesBuffer[skinningInfo.transformOffsetMatrices + jointIndex];
    return mul(float4(position, 1.0f), skin).xyz;
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
	jointIndex = ResolveSkeletonLodBoneIndex(skinningInfo, jointIndex);
	if (jointIndex == 0xFFFFFFFFu) return IdentitySkinMatrix();
    SkinningMatrix inverseSkin = inverseSkinMatricesBuffer[skinningInfo.inverseSkinOffsetMatrices + jointIndex];
    return inverseSkin;
}

float4x4 LoadPreviousBoneSkinMatrixFromInfo(SkinningInstanceGPUInfo skinningInfo, uint jointIndex)
{
    StructuredBuffer<SkinningMatrix> boneSkinMatricesBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::SkeletonResources::BoneTransforms)];
	jointIndex = ResolveSkeletonLodBoneIndex(skinningInfo, jointIndex);
	return jointIndex == 0xFFFFFFFFu ? IdentitySkinMatrix() : boneSkinMatricesBuffer[skinningInfo.previousTransformOffsetMatrices + jointIndex];
}

void AddWeightedBoneSkinMatrix(
    inout float4x4 skinMatrix,
    inout float acceptedWeightSum,
    SkinningInstanceGPUInfo skinningInfo,
    uint jointIndex,
    float weight)
{
    if (weight > 0.0f && isfinite(weight) && jointIndex < skinningInfo.sourceBoneCount)
    {
        skinMatrix += weight * LoadBoneSkinMatrixFromInfo(skinningInfo, jointIndex);
        acceptedWeightSum += weight;
    }
}

void AddWeightedPreviousBoneSkinMatrix(
    inout float4x4 skinMatrix,
    inout float acceptedWeightSum,
    SkinningInstanceGPUInfo skinningInfo,
    uint jointIndex,
    float weight)
{
    if (weight > 0.0f && isfinite(weight) && jointIndex < skinningInfo.sourceBoneCount)
    {
        skinMatrix += weight * LoadPreviousBoneSkinMatrixFromInfo(skinningInfo, jointIndex);
        acceptedWeightSum += weight;
    }
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
    float weightSum = 0.0f;
    AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.x, skinning.weights0.x);
    if (any(skinning.weights0.yzw != 0.0f) || any(skinning.weights1 != 0.0f))
    {
        AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.y, skinning.weights0.y);
        AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.z, skinning.weights0.z);
        AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.w, skinning.weights0.w);
        AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.x, skinning.weights1.x);
        AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.y, skinning.weights1.y);
        AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.z, skinning.weights1.z);
        AddWeightedBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.w, skinning.weights1.w);
    }
    // USD permits non-normalized weights, and some assembly geometry has no
    // authored influence at all.  An all-zero weighted matrix collapses those
    // vertices to the part origin and stretches every adjacent triangle.  A
    // missing influence is rigid/bind-pose geometry; otherwise normalize the
    // affine blend so its homogeneous component remains one.
    if (!isfinite(weightSum) || abs(weightSum) <= 1.0e-8f)
    {
        return IdentitySkinMatrix();
    }
    return skinMatrix * rcp(weightSum);
}

float4x4 BuildPreviousSkinMatrix(uint skinningInstanceSlot, SkinningInfluences skinning)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        return IdentitySkinMatrix();
    }

    SkinningInstanceGPUInfo skinningInfo = LoadSkinningInstanceInfo(skinningInstanceSlot);
    float4x4 skinMatrix = (float4x4)0;
    float weightSum = 0.0f;
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.x, skinning.weights0.x);
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.y, skinning.weights0.y);
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.z, skinning.weights0.z);
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints0.w, skinning.weights0.w);
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.x, skinning.weights1.x);
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.y, skinning.weights1.y);
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.z, skinning.weights1.z);
    AddWeightedPreviousBoneSkinMatrix(skinMatrix, weightSum, skinningInfo, skinning.joints1.w, skinning.weights1.w);
    return (!isfinite(weightSum) || abs(weightSum) <= 1.0e-8f)
        ? IdentitySkinMatrix()
        : skinMatrix * rcp(weightSum);
}

// Expanded assembly skeletons are evaluated in assembly-root space, while the
// vertices stored in an individual CLod bucket remain local to that bucket's
// authored assembly transform.  The normal object transform subsequently
// applies localToAssembly, so conjugate the expanded skin matrix back into the
// bucket's local space before applying it to a vertex:
//
//   pLocal * (T * S * inverse(T)) * T == pLocal * T * S
//
// This also keeps rigid and skinned paths using the same object transform.
float4x4 ConvertExpandedSkinMatrixToAssemblyLocal(
    float4x4 expandedSkinMatrix,
    uint assemblyTransformIndex)
{
    if (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
    {
        return expandedSkinMatrix;
    }

    StructuredBuffer<ClusterLODAssemblyTransform> assemblyTransforms =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyTransforms)];
    const ClusterLODAssemblyTransform assemblyTransform = assemblyTransforms[assemblyTransformIndex];
    const row_major float4x4 localToAssembly = CLodAssemblyTransformToMatrix(assemblyTransform);
    const row_major float4x4 assemblyToLocal =
        CLodAssemblyTransformToMatrix(CLodInvertAssemblyTransform(assemblyTransform));
    return mul(mul(localToAssembly, expandedSkinMatrix), assemblyToLocal);
}

float4x4 BuildAssemblyLocalSkinMatrix(
    uint skinningInstanceSlot,
    SkinningInfluences skinning,
    uint assemblyTransformIndex)
{
    return ConvertExpandedSkinMatrixToAssemblyLocal(
        BuildSkinMatrix(skinningInstanceSlot, skinning),
        assemblyTransformIndex);
}

float4x4 BuildPreviousAssemblyLocalSkinMatrix(
    uint skinningInstanceSlot,
    SkinningInfluences skinning,
    uint assemblyTransformIndex)
{
    return ConvertExpandedSkinMatrixToAssemblyLocal(
        BuildPreviousSkinMatrix(skinningInstanceSlot, skinning),
        assemblyTransformIndex);
}

float4x4 LoadAssemblyLocalBoneSkinMatrix(
    uint skinningInstanceSlot,
    uint jointIndex,
    uint assemblyTransformIndex)
{
    return ConvertExpandedSkinMatrixToAssemblyLocal(
        LoadBoneSkinMatrix(skinningInstanceSlot, jointIndex),
        assemblyTransformIndex);
}

float4x4 LoadPreviousAssemblyLocalBoneSkinMatrix(
    uint skinningInstanceSlot,
    uint jointIndex,
    uint assemblyTransformIndex)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        return IdentitySkinMatrix();
    }
    return ConvertExpandedSkinMatrixToAssemblyLocal(
        LoadPreviousBoneSkinMatrixFromInfo(LoadSkinningInstanceInfo(skinningInstanceSlot), jointIndex),
        assemblyTransformIndex);
}

float4x4 LoadAssemblyLocalBoneInverseSkinMatrix(
    uint skinningInstanceSlot,
    uint jointIndex,
    uint assemblyTransformIndex)
{
    return ConvertExpandedSkinMatrixToAssemblyLocal(
        LoadBoneInverseSkinMatrix(skinningInstanceSlot, jointIndex),
        assemblyTransformIndex);
}

float3 ApplyAssemblySkinningToPosition(
    uint skinningInstanceSlot,
    SkinningInfluences skinning,
    uint assemblyTransformIndex,
    float3 position)
{
    const float4x4 skinMatrix = BuildAssemblyLocalSkinMatrix(
        skinningInstanceSlot,
        skinning,
        assemblyTransformIndex);
    return mul(float4(position, 1.0f), skinMatrix).xyz;
}

void ApplyAssemblySkinningToVertex(
    uint skinningInstanceSlot,
    SkinningInfluences skinning,
    uint assemblyTransformIndex,
    inout Vertex vertex)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        vertex.skinning = skinning;
        return;
    }

    const float4x4 skinMatrix = BuildAssemblyLocalSkinMatrix(
        skinningInstanceSlot,
        skinning,
        assemblyTransformIndex);
    vertex.position = mul(float4(vertex.position, 1.0f), skinMatrix).xyz;
    vertex.normal = normalize(mul(vertex.normal, (float3x3)skinMatrix));
    vertex.tangent = normalize(mul(vertex.tangent, (float3x3)skinMatrix));
    vertex.skinning = skinning;
}

float3 ApplySkinningToPosition(uint skinningInstanceSlot, SkinningInfluences skinning, float3 position)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        return position;
    }

    return mul(float4(position, 1.0f), BuildSkinMatrix(skinningInstanceSlot, skinning)).xyz;
}

void ApplySkinningToVertex(uint skinningInstanceSlot, SkinningInfluences skinning, inout Vertex vertex)
{
    if (!IsValidSkinningInstanceSlot(skinningInstanceSlot))
    {
        vertex.skinning = skinning;
        return;
    }

    const float4x4 skinMatrix = BuildSkinMatrix(skinningInstanceSlot, skinning);
    vertex.position = mul(float4(vertex.position, 1.0f), skinMatrix).xyz;
    vertex.normal = normalize(mul(vertex.normal, (float3x3)skinMatrix));
    vertex.tangent.xyz = normalize(mul(vertex.tangent.xyz, (float3x3)skinMatrix));
    vertex.skinning = skinning;
}

#endif // __SKINNING_COMMON_HLSLI__
