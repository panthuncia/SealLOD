#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/visUtilCommon.hlsli"
#include "canonicalSurface.hlsl"

// Root constants (via ExecuteIndirect / command signature):
//   IndirectCommandSignatureRootConstant0 = materialId
//   IndirectCommandSignatureRootConstant1 = baseOffset into PixelListBuffer
//   IndirectCommandSignatureRootConstant2 = count (number of pixels for this material)
[shader("compute")]
[numthreads(MATERIAL_EXECUTION_GROUP_SIZE, 1, 1)]
void EvaluateMaterialGroupCS(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint groupIndex : SV_GroupIndex
)
{
    uint baseOffset = IndirectCommandSignatureRootConstant1;
    uint count = IndirectCommandSignatureRootConstant2;
    uint dispatchXDimension = IndirectCommandSignatureRootConstant3;

    uint idx = dispatchThreadId.y * dispatchXDimension + dispatchThreadId.x;
    if (idx >= count)
    {
        return;
    }

    StructuredBuffer<PixelRef> pixelList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::PixelListBuffer)];

    PixelRef ref = pixelList[baseOffset + idx];

    uint2 pixel;
    pixel.x = ref.pixelXY & 0xFFFFu;
    pixel.y = ref.pixelXY >> 16;

    if (VISBUF_MATERIAL_PIXEL_TELEMETRY_ENABLED != 0u)
    {
        // E1 means the indirect compile-flags group consumed this PixelRef. A
        // successful canonical resolve replaces y with C1 + material-table row.
        RWTexture2D<uint2> telemetry =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
        ConstantBuffer<PerFrameBuffer> telemetryFrame =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
        telemetry[pixel] = uint2(
            ((telemetryFrame.frameIndex & 0xFFu) << 24u) |
                (IndirectCommandSignatureRootConstant0 & 0x00FFFFFFu),
            0xE1000000u | (baseOffset & 0x00FFFFFFu));
    }

#if defined(VISUTIL_USE_CACHED_VIS_KEY)
    EvaluateCanonicalSurfaceOptimized(pixel, (uint64_t(ref.visibilityKey.y) << 32) | ref.visibilityKey.x);
#else
    EvaluateCanonicalSurfaceOptimized(pixel);
#endif
}
