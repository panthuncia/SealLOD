#include "../Include/cbuffers.hlsli"

// FSR's reconstruct-and-dilate pass uses the motion vector belonging to the
// nearest depth sample in a 3x3 neighborhood. SARP's projected depth is
// reverse-Z, so the nearest sample has the greatest depth value.
[shader("compute")]
[numthreads(8, 8, 1)]
void DilateMotionVectorsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 dimensions = uint2(UintRootConstant3, UintRootConstant4);
    if (any(dispatchThreadId.xy >= dimensions)) {
        return;
    }

    Texture2D<float2> sourceMotion = ResourceDescriptorHeap[UintRootConstant0];
    Texture2D<float> sourceDepth = ResourceDescriptorHeap[UintRootConstant1];
    RWTexture2D<float2> destinationMotion = ResourceDescriptorHeap[UintRootConstant2];

    const int2 center = int2(dispatchThreadId.xy);
    int2 nearestPixel = center;
    float nearestDepth = sourceDepth[center];

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const int2 candidate = clamp(center + int2(x, y), int2(0, 0), int2(dimensions) - 1);
            const float candidateDepth = sourceDepth[candidate];
            if (candidateDepth > nearestDepth) {
                nearestDepth = candidateDepth;
                nearestPixel = candidate;
            }
        }
    }

    destinationMotion[center] = sourceMotion[nearestPixel];
}
