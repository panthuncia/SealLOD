#include "include/cbuffers.hlsli"
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

struct WindDebugType { uint firstBone, boneCount, sourceSkinningSlot, bucketBase, bucketCapacity; uint3 pad; };
struct WindDebugBone {
    uint skinningSlot, jointIndex, parentEntry, simulationGroup, phaseSeed;
    float influence, meanBend, parallelAmplitude, perpendicularRatio, torsionRatio, frequencyScale, maximumAngle;
    float3 frequencies; float pad0; float3 weights; float pad1;
};
struct WindDebugActiveInstance { uint drawRecordIndex, stableSceneId, transformOffsetMatrices, inverseSkinOffsetMatrices; };
typedef row_major float4x4 WindDebugMatrix;

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
    StructuredBuffer<WindDebugMatrix> inverseBind = ResourceDescriptorHeap[kWindInverseBind];
    WindDebugBone parent = bone;
    if (emit) parent = bones[bone.parentEntry];
    float3 start = WindDebugJointPosition(parent.jointIndex, active, source, forward, inverseBind);
    float3 end = WindDebugJointPosition(bone.jointIndex, active, source, forward, inverseBind);
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(active.drawRecordIndex);
    start = mul(float4(start, 1.0f), objectData.model).xyz;
    end = mul(float4(end, 1.0f), objectData.model).xyz;

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[kWindPerFrame];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[kWindCameras];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const float4 color = float4(1.0f, 0.2f + 0.6f * frac(kWindTypeId * 0.37f), 0.1f, 1.0f);
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
