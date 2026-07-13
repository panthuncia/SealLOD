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
    RWStructuredBuffer<uint> diagnostics = ResourceDescriptorHeap[UintRootConstant8];
    InterlockedAdd(diagnostics[18], 1u);
    return input.color;
}

struct WindDebugType {
    uint firstBone, boneCount, sourceSkinningSlot, bucketBase;
    uint bucketCapacity, diagnosticsDescriptor, activeEntriesDescriptor, transformCount;
    uint deferredEntriesDescriptor, processedTypeCountsDescriptor;
};
struct WindDebugBone {
    uint skinningSlot, jointIndex, parentEntry, simulationGroup;
    uint phaseSeed, flags; float influence, meanBend;
    float parallelAmplitude, perpendicularRatio, torsionRatio, frequencyScale;
    float maximumAngle, gustAttenuation; float2 pad0;
    float3 frequencies; float pad1; float3 weights; float pad2;
    float3 branchAxis; float pad3; float3 branchTangent; float pad4;
};
struct WindDebugActiveInstance { uint instanceTransformIndex, stableSceneId, transformOffsetMatrices, inverseSkinOffsetMatrices; };
typedef row_major float4x4 WindDebugMatrix;

static const uint kWindTypes = UintRootConstant0;
static const uint kWindBones = UintRootConstant1;
static const uint kWindActive = UintRootConstant2;
static const uint kWindForward = UintRootConstant3;
static const uint kWindInverseBind = UintRootConstant4;
static const uint kWindSkinInfo = UintRootConstant5;
static const uint kWindPerFrame = UintRootConstant6;
static const uint kWindCameras = UintRootConstant7;
static const uint kWindDiagnostics = UintRootConstant8;
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
    float3x3 r = (float3x3)m;
    const float det = determinant(r);
    if (abs(det) < 1.0e-8f) return m;
    float3x3 ri = transpose(float3x3(cross(r[1], r[2]), cross(r[2], r[0]), cross(r[0], r[1])) / det);
    WindDebugMatrix result = (WindDebugMatrix)0;
    result[0].xyz = ri[0]; result[1].xyz = ri[1]; result[2].xyz = ri[2];
    result[3].xyz = -mul(m[3].xyz, ri); result[3][3] = 1.0f;
    return result;
}

float3 WindDebugJointPosition(
    uint jointIndex,
    WindDebugActiveInstance active,
    SkinningInstanceGPUInfo source,
    StructuredBuffer<WindDebugMatrix> forward,
    StructuredBuffer<WindDebugMatrix> inverseBind)
{
    WindDebugMatrix invBind = inverseBind[source.invBindOffsetMatrices + jointIndex];
    WindDebugMatrix skin = forward[active.transformOffsetMatrices + jointIndex];
    if ((source.flags & 1u) == 0u) { invBind = transpose(invBind); skin = transpose(skin); }
    return mul(skin, WindDebugInverse(invBind))[3].xyz;
}

float4 WindDebugSimulationGroupColor(uint simulationGroup)
{
    if (simulationGroup == 0xFFFFFFFFu)
        return float4(0.55f, 0.55f, 0.55f, 1.0f);

    return float4(max(HashToColor(simulationGroup + 17u), 0.18f.xxx), 1.0f);
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
    RWStructuredBuffer<uint> diagnostics = ResourceDescriptorHeap[kWindDiagnostics];
    if (groupThreadID == 0u) {
        InterlockedAdd(diagnostics[14], 1u);
        InterlockedAdd(diagnostics[15], groupBoneCount);
    }
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
    StructuredBuffer<WindDebugMatrix> inverseBind = ResourceDescriptorHeap[kWindInverseBind];
    WindDebugBone parent = bone;
    if (emit) parent = bones[bone.parentEntry];
    float3 start = WindDebugJointPosition(parent.jointIndex, active, source, forward, inverseBind);
    float3 end = WindDebugJointPosition(bone.jointIndex, active, source, forward, inverseBind);
    StructuredBuffer<PerObjectBuffer> transforms =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerInstanceTransformBuffer)];
    const PerObjectBuffer objectData = transforms[active.instanceTransformIndex];
    start = mul(float4(start, 1.0f), objectData.model).xyz;
    end = mul(float4(end, 1.0f), objectData.model).xyz;

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[kWindPerFrame];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[kWindCameras];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const float4 color = WindDebugSimulationGroupColor(bone.simulationGroup);
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
    const bool finiteClip = all(isfinite(startVertex.position)) && all(isfinite(endVertex.position));
    if (emit && finiteClip && startVertex.position.w > 0.0f && endVertex.position.w > 0.0f) {
        InterlockedAdd(diagnostics[16], 1u);
        const bool startOnScreen = abs(startVertex.position.x) <= startVertex.position.w && abs(startVertex.position.y) <= startVertex.position.w;
        const bool endOnScreen = abs(endVertex.position.x) <= endVertex.position.w && abs(endVertex.position.y) <= endVertex.position.w;
        if (startOnScreen || endOnScreen) InterlockedAdd(diagnostics[17], 1u);
        if (groupID.x == 0u && groupID.y == 0u && groupThreadID == 1u) {
            diagnostics[19] = asuint(start.x); diagnostics[20] = asuint(start.y);
            diagnostics[21] = asuint(end.x); diagnostics[22] = asuint(end.y); diagnostics[23] = asuint(end.z);
            diagnostics[24] = asuint(startVertex.position.x); diagnostics[25] = asuint(startVertex.position.y);
            diagnostics[26] = asuint(startVertex.position.z); diagnostics[27] = asuint(startVertex.position.w);
            diagnostics[28] = asuint(endVertex.position.x); diagnostics[29] = asuint(endVertex.position.y);
            diagnostics[30] = asuint(endVertex.position.z); diagnostics[31] = asuint(endVertex.position.w);
        }
    }
}
