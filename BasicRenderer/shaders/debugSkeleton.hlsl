#include "include/cbuffers.hlsli"

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
