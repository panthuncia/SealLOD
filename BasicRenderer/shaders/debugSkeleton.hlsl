#include "include/cbuffers.hlsli"
#include "include/debugPayload.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"

struct SkeletonDebugLine
{
    float4 startWorld;
    float4 endWorld;
    float4 color;
};

struct SkeletonLineVertex
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
};

static const uint kLineBufferDescriptorIndex = UintRootConstant0;
static const uint kPerFrameBufferDescriptorIndex = UintRootConstant1;
static const uint kCameraBufferDescriptorIndex = UintRootConstant2;
static const uint kLineOffset = UintRootConstant3;
static const uint kLineCount = UintRootConstant4;
static const uint kLinesPerGroup = 32u;

[outputtopology("line")]
[numthreads(kLinesPerGroup, 1, 1)]
void MSMain(
    uint groupThreadID : SV_GroupThreadID,
    uint3 groupID : SV_GroupID,
    out vertices SkeletonLineVertex outputVertices[64],
    out indices uint2 outputLines[32])
{
    const uint groupLineBase = groupID.x * kLinesPerGroup;
    const uint outputLineCount = groupLineBase < kLineCount
        ? min(kLinesPerGroup, kLineCount - groupLineBase)
        : 0u;
    SetMeshOutputCounts(outputLineCount * 2u, outputLineCount);

    if (groupThreadID >= outputLineCount)
    {
        return;
    }

    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[kPerFrameBufferDescriptorIndex];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[kCameraBufferDescriptorIndex];
    StructuredBuffer<SkeletonDebugLine> lines = ResourceDescriptorHeap[kLineBufferDescriptorIndex];

    const Camera camera = cameras[perFrameBuffer.mainCameraIndex];
    const SkeletonDebugLine skeletonLine = lines[kLineOffset + groupLineBase + groupThreadID];

    const uint vertexBase = groupThreadID * 2u;
    SkeletonLineVertex startVertex;
    SkeletonLineVertex endVertex;
    startVertex.position = mul(mul(skeletonLine.startWorld, camera.view), camera.projection);
    endVertex.position = mul(mul(skeletonLine.endWorld, camera.view), camera.projection);
    startVertex.color = skeletonLine.color;
    endVertex.color = skeletonLine.color;

    outputVertices[vertexBase] = startVertex;
    outputVertices[vertexBase + 1u] = endVertex;
    outputLines[groupThreadID] = uint2(vertexBase, vertexBase + 1u);
}

float4 PSMain(SkeletonLineVertex input) : SV_Target
{
    return input.color;
}

float4 PSWindMain(SkeletonLineVertex input) : SV_Target
{
    return input.color;
}

typedef row_major float4x4 WindDebugMatrix;

struct WindDebugType {
    uint firstBone, boneCount, sourceSkinningSlot, bucketBase;
    uint bucketCapacity, diagnosticsDescriptor, activeEntriesDescriptor, transformCount;
    uint deferredEntriesDescriptor, processedTypeCountsDescriptor;
	uint remapDescriptor, remapOffset, sourceBoneCount, lodLevel;
	uint baseTypeLookupDescriptor, baseTypeLookupCount;
	uint variantCount;
	float normalizedQuality, collapseError, qualityBias;
};
struct WindDebugBone {
    uint skinningSlot, jointIndex, parentEntry, simulationGroup;
    uint phaseSeed, flags; float influence, meanBend;
    float parallelAmplitude, perpendicularRatio, torsionRatio, frequencyScale;
    float maximumAngle, gustAttenuation; float2 pad0;
    float3 frequencies; float pad1; float3 weights; float pad2;
    float3 branchAxis; float pad3; float3 branchTangent; float pad4;
    WindDebugMatrix bindGlobal;
    WindDebugMatrix inverseBind;
};
struct WindDebugActiveInstance {
	uint instanceTransformIndex, stableSceneId, transformOffsetMatrices, inverseSkinOffsetMatrices;
	float screenFraction;
	uint priorityKey;
};
static const uint kWindTypes = UintRootConstant0;
static const uint kWindBones = UintRootConstant1;
static const uint kWindActive = UintRootConstant2;
static const uint kWindForward = UintRootConstant3;
static const uint kWindInverseBind = UintRootConstant4;
static const uint kWindSkinInfo = UintRootConstant5;
static const uint kWindPerFrame = UintRootConstant6;
static const uint kWindCameras = UintRootConstant7;
static const uint kWindTypeId = IndirectCommandSignatureRootConstant0;
static const uint kInvalidWindBone = 0xffffffffu;

struct WindDebugPlacement {
    uint instanceTransformIndex, skinningTypeSlot, stableSceneId, generation;
    float4 localBoundingSphere;
    float boundsScale;
    uint3 pad;
};

struct WindSphereVertex {
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
};

static const uint kWindPlacements = UintRootConstant9;
static const uint kWindActivePlacementEntries = UintRootConstant10;
static const uint kWindPlacementCount = UintRootConstant11;

float WindDebugMaxAxisScale(row_major float4x4 m)
{
    return max(length(m[0].xyz), max(length(m[1].xyz), length(m[2].xyz)));
}

float3 WindDebugSphereUnitVertex(uint vertexIndex)
{
    if (vertexIndex == 0u) return float3(0.0f, 0.0f, 1.0f);
    if (vertexIndex == 41u) return float3(0.0f, 0.0f, -1.0f);
    const uint ring = (vertexIndex - 1u) / 8u;
    const uint slice = (vertexIndex - 1u) % 8u;
    const float theta = 3.14159265359f * float(ring + 1u) / 6.0f;
    const float phi = 6.28318530718f * float(slice) / 8.0f;
    const float ringRadius = sin(theta);
    return float3(ringRadius * cos(phi), ringRadius * sin(phi), cos(theta));
}

uint3 WindDebugSphereTriangle(uint triangleIndex)
{
    if (triangleIndex < 8u) {
        const uint slice = triangleIndex;
        return uint3(0u, 1u + slice, 1u + ((slice + 1u) & 7u));
    }
    if (triangleIndex < 72u) {
        const uint localIndex = triangleIndex - 8u;
        const uint band = localIndex / 16u;
        const uint sliceAndTriangle = localIndex % 16u;
        const uint slice = sliceAndTriangle / 2u;
        const uint nextSlice = (slice + 1u) & 7u;
        const uint firstRing = 1u + band * 8u;
        const uint secondRing = firstRing + 8u;
        return (sliceAndTriangle & 1u) == 0u
            ? uint3(firstRing + slice, secondRing + slice, secondRing + nextSlice)
            : uint3(firstRing + slice, secondRing + nextSlice, firstRing + nextSlice);
    }
    const uint slice = triangleIndex - 72u;
    return uint3(41u, 33u + ((slice + 1u) & 7u), 33u + slice);
}

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSWindAssemblySphereMain(
    uint groupThreadID : SV_GroupThreadID,
    uint3 groupID : SV_GroupID,
    out vertices WindSphereVertex outputVertices[42],
    out indices uint3 outputTriangles[80])
{
    StructuredBuffer<uint2> activeEntries = ResourceDescriptorHeap[kWindActivePlacementEntries];
    const uint2 activeEntry = activeEntries[groupID.x];
    StructuredBuffer<WindDebugPlacement> placements = ResourceDescriptorHeap[kWindPlacements];
    const WindDebugPlacement placement = placements[activeEntry.x];
    const bool validPlacement = placement.generation == activeEntry.y;
    SetMeshOutputCounts(validPlacement ? 42u : 0u, validPlacement ? 80u : 0u);
    if (!validPlacement) return;

    StructuredBuffer<PerObjectBuffer> transforms =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerInstanceTransformBuffer)];
    const PerObjectBuffer objectData = transforms[placement.instanceTransformIndex];
    const float3 centerWS = mul(float4(placement.localBoundingSphere.xyz, 1.0f), objectData.model).xyz;
    const float radiusWS = placement.localBoundingSphere.w * placement.boundsScale * WindDebugMaxAxisScale(objectData.model);
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[kWindPerFrame];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[kWindCameras];
    const Camera camera = cameras[perFrame.mainCameraIndex];

    if (groupThreadID < 42u) {
        const float3 positionWS = centerWS + WindDebugSphereUnitVertex(groupThreadID) * radiusWS;
        WindSphereVertex vertex;
        vertex.position = mul(mul(float4(positionWS, 1.0f), camera.view), camera.projection);
        vertex.color = float4(0.1f, 0.9f, 1.0f, 1.0f);
        outputVertices[groupThreadID] = vertex;
    }
    if (groupThreadID < 80u) outputTriangles[groupThreadID] = WindDebugSphereTriangle(groupThreadID);
}

float4 PSWindAssemblySphereMain(WindSphereVertex input) : SV_Target
{
    return input.color;
}

WindDebugMatrix WindDebugInverse(WindDebugMatrix m)
{
    const float a00=m[0][0], a01=m[0][1], a02=m[0][2];
    const float a10=m[1][0], a11=m[1][1], a12=m[1][2];
    const float a20=m[2][0], a21=m[2][1], a22=m[2][2];
    const float det = a00*(a11*a22-a12*a21) - a01*(a10*a22-a12*a20) + a02*(a10*a21-a11*a20);
    if (abs(det) < 1.0e-8f) return m;
    const float invDet = 1.0f / det;
    WindDebugMatrix result = (WindDebugMatrix)0;
    result[0][0]=(a11*a22-a12*a21)*invDet;
    result[0][1]=(a02*a21-a01*a22)*invDet;
    result[0][2]=(a01*a12-a02*a11)*invDet;
    result[1][0]=(a12*a20-a10*a22)*invDet;
    result[1][1]=(a00*a22-a02*a20)*invDet;
    result[1][2]=(a02*a10-a00*a12)*invDet;
    result[2][0]=(a10*a21-a11*a20)*invDet;
    result[2][1]=(a01*a20-a00*a21)*invDet;
    result[2][2]=(a00*a11-a01*a10)*invDet;
    result[3].xyz = -float3(
        dot(m[3].xyz, float3(result[0][0], result[1][0], result[2][0])),
        dot(m[3].xyz, float3(result[0][1], result[1][1], result[2][1])),
        dot(m[3].xyz, float3(result[0][2], result[1][2], result[2][2])));
    result[3][3] = 1.0f;
    return result;
}

float3 WindDebugJointPosition(
    WindDebugBone bone,
    uint compactJointIndex,
    WindDebugActiveInstance active,
    SkinningInstanceGPUInfo source,
    StructuredBuffer<WindDebugMatrix> forward)
{
    // WindBone::jointIndex identifies the source/base skeleton joint used to
    // derive simulation parameters. Transient palettes are compact, so matrix
    // addressing must use the variant-local joint index instead.
    WindDebugMatrix skin = forward[active.transformOffsetMatrices + compactJointIndex];
    // Forward skin matrices map bind-space points into the animated pose.
    // With row-vector matrices, the bind origin therefore precedes skin.
    return mul(bone.bindGlobal, skin)[3].xyz;
}

float4 WindDebugSimulationGroupColor(uint simulationGroup)
{
    if (simulationGroup == 0xFFFFFFFFu)
        return float4(0.32f, 0.34f, 0.38f, 1.0f);

    // Golden-ratio hue stepping keeps adjacent and arbitrary group IDs distinct.
    // Fixed high saturation avoids the pale low-contrast colors produced by
    // hashing the three RGB channels independently.
    const float hue = frac((float(simulationGroup) + 1.0f) * 0.61803398875f);
    const float3 hueRgb = saturate(
        abs(frac(hue + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
    const float3 color = 0.95f * lerp(1.0f.xxx, hueRgb, 0.88f);
    return float4(color, 1.0f);
}

float4 WindDebugLodColor(uint lodLevel)
{
	// Stable golden-ratio palette for all sixteen supported variants.
	const float hue = frac((float(lodLevel) + 0.5f) * 0.61803398875f);
	const float3 hueRgb = saturate(
		abs(frac(hue + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
	return float4(lerp(1.0f.xxx, hueRgb, 0.86f), 1.0f);
}

[outputtopology("line")]
[numthreads(64, 1, 1)]
void MSWindMain(
    uint groupThreadID : SV_GroupThreadID,
    uint3 groupID : SV_GroupID,
    out vertices SkeletonLineVertex outputVertices[128],
    out indices uint2 outputLines[64])
{
    StructuredBuffer<WindDebugType> types = ResourceDescriptorHeap[kWindTypes];
    const WindDebugType type = types[kWindTypeId];
    const uint boneIndex = groupID.x * 64u + groupThreadID;
    const uint groupBoneBase = groupID.x * 64u;
    const uint groupBoneCount = groupBoneBase < type.boneCount ? min(64u, type.boneCount - groupBoneBase) : 0u;
    SetMeshOutputCounts(groupBoneCount * 2u, groupBoneCount);
    if (boneIndex >= type.boneCount) return;

    StructuredBuffer<WindDebugBone> bones = ResourceDescriptorHeap[kWindBones];
    const WindDebugBone bone = bones[type.firstBone + boneIndex];
    const bool emit = bone.parentEntry != kInvalidWindBone && bone.parentEntry >= type.firstBone && bone.parentEntry < type.firstBone + type.boneCount;
    const uint outputIndex = groupThreadID;

    StructuredBuffer<WindDebugActiveInstance> instances = ResourceDescriptorHeap[kWindActive];
    const WindDebugActiveInstance active = instances[type.bucketBase + groupID.y];
    StructuredBuffer<SkinningInstanceGPUInfo> infos = ResourceDescriptorHeap[kWindSkinInfo];
    const SkinningInstanceGPUInfo source = infos[type.sourceSkinningSlot];
    StructuredBuffer<WindDebugMatrix> forward = ResourceDescriptorHeap[kWindForward];
    WindDebugBone parent = bone;
    if (emit) parent = bones[bone.parentEntry];
    const uint parentCompactIndex = emit ? bone.parentEntry - type.firstBone : boneIndex;
    float3 start = WindDebugJointPosition(parent, parentCompactIndex, active, source, forward);
    float3 end = WindDebugJointPosition(bone, boneIndex, active, source, forward);
    StructuredBuffer<PerObjectBuffer> transforms =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerInstanceTransformBuffer)];
    const PerObjectBuffer objectData = transforms[active.instanceTransformIndex];
    start = mul(float4(start, 1.0f), objectData.model).xyz;
    end = mul(float4(end, 1.0f), objectData.model).xyz;

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[kWindPerFrame];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[kWindCameras];
    const Camera camera = cameras[perFrame.mainCameraIndex];
	const float4 color = WindDebugLodColor(type.lodLevel);
    const uint vertexBase = outputIndex * 2u;
    SkeletonLineVertex startVertex;
    SkeletonLineVertex endVertex;
    startVertex.position = mul(mul(float4(start, 1), camera.view), camera.projection);
    startVertex.color = color;
    endVertex.position = mul(mul(float4(end, 1), camera.view), camera.projection);
    endVertex.color = color;
    outputVertices[vertexBase] = startVertex;
    outputVertices[vertexBase + 1u] = endVertex;
    outputLines[outputIndex] = uint2(vertexBase, vertexBase + 1u);
}
