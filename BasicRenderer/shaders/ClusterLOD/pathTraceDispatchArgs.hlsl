// Writes the D3D12_DISPATCH_RAYS_DESC that PathTracePass launches. Constants 0-25 are its words, and 26 is the output.
#include "include/cbuffers.hlsli"

[numthreads(1, 1, 1)]
void PathTraceDispatchArgsCS()
{
    RWByteAddressBuffer output = ResourceDescriptorHeap[UintRootConstant26];
    const uint words[26] = {
        UintRootConstant0, UintRootConstant1, UintRootConstant2, UintRootConstant3, UintRootConstant4, UintRootConstant5, UintRootConstant6,
        UintRootConstant7, UintRootConstant8, UintRootConstant9, UintRootConstant10, UintRootConstant11, UintRootConstant12, UintRootConstant13,
        UintRootConstant14, UintRootConstant15, UintRootConstant16, UintRootConstant17, UintRootConstant18, UintRootConstant19, UintRootConstant20,
        UintRootConstant21, UintRootConstant22, UintRootConstant23, UintRootConstant24, UintRootConstant25,
    };
    for (uint index = 0u; index < 26u; ++index) {
        output.Store(index * 4u, words[index]);
    }
}
