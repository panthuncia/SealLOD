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

#if defined(VISUTIL_USE_CACHED_VIS_KEY)
    EvaluateCanonicalSurfaceOptimized(pixel, (uint64_t(ref.visibilityKey.y) << 32) | ref.visibilityKey.x);
#else
    EvaluateCanonicalSurfaceOptimized(pixel);
#endif
}
