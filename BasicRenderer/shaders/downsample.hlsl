#include "include/cbuffers.hlsli"

#define A_GPU
#define A_HLSL

struct SpdGlobalAtomicBuffer
{
    uint counter[6];
};

#include "FidelityFX/ffx_a.h"

groupshared AU1 spdCounter;

#ifndef SPD_PACKED_ONLY
groupshared AF1 spdIntermediateR[16][16];
groupshared AF1 spdIntermediateG[16][16];
groupshared AF1 spdIntermediateB[16][16];
groupshared AF1 spdIntermediateA[16][16];

struct spdConstants
{
    uint2 srcSize;
    uint mips;
    uint numWorkGroups;
    
    uint2 workGroupOffset;
    float2 invInputSize;
    
    unsigned int mipUavDescriptorIndices[12];
    uint2 validSourceSize;
    uint2 pad;
};

// UintRootConstant0 is the index of the global atomic buffer
// UintRootConstant1 is the index of the source image
// UintRootConstant2 is the index of the spdConstants structured buffer
// UintRootConstant3 is the index of the constants in the spdConstants structured buffer

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
    StructuredBuffer<spdConstants> constants = ResourceDescriptorHeap[UintRootConstant2];
    spdConstants downsampleConstants = constants[UintRootConstant3];
    if (any(p >= downsampleConstants.validSourceSize))
    {
        // The linear-depth texture is padded to power-of-two dimensions for
        // SPD. Padding must be the neutral value for max-depth reduction;
        // otherwise its FLT_MAX clear value poisons every coarse HZB mip.
        return AF4(0.0f, 0.0f, 0.0f, 0.0f);
    }

#if defined (DOWNSAMPLE_ARRAY)
    Texture2DArray<float> imgSrc = ResourceDescriptorHeap[UintRootConstant1];
    float result = imgSrc.Load(int4((int)p.x, (int)p.y, (int)slice, 0));
#else
    Texture2D<float> imgSrc = ResourceDescriptorHeap[UintRootConstant1];
    float result = imgSrc.Load(int3((int) p.x, (int) p.y, 0));
#endif
    return AF4(result, result, result, result);
}
AF4 SpdLoad(ASU2 tex, AU1 slice)
{
    StructuredBuffer<spdConstants> constants = ResourceDescriptorHeap[UintRootConstant2];
#if defined (DOWNSAMPLE_ARRAY)
    globallycoherent RWTexture2DArray<float> imgDst5 = ResourceDescriptorHeap[constants[UintRootConstant3].mipUavDescriptorIndices[5]];
    return imgDst5[float3(tex, slice)].rrrr;
#else
    globallycoherent RWTexture2D<float> imgDst5 = ResourceDescriptorHeap[constants[UintRootConstant3].mipUavDescriptorIndices[5]];
    return imgDst5[tex].rrrr;
#endif
}
void SpdStore(ASU2 pix, AF4 outValue, AU1 index, AU1 slice)
{
    StructuredBuffer<spdConstants> constants = ResourceDescriptorHeap[UintRootConstant2];
    if (index == 5)
    {
#if defined (DOWNSAMPLE_ARRAY)
        globallycoherent RWTexture2DArray<float> imgDst5 = ResourceDescriptorHeap[constants[UintRootConstant3].mipUavDescriptorIndices[5]];
        imgDst5[float3(pix, slice)] = outValue.r;
#else
        globallycoherent RWTexture2D<float> imgDst5 = ResourceDescriptorHeap[constants[UintRootConstant3].mipUavDescriptorIndices[5]];
        imgDst5[pix] = outValue.r;
#endif
        return;
    }
#if defined (DOWNSAMPLE_ARRAY)
    RWTexture2DArray<float> imgDst = ResourceDescriptorHeap[NonUniformResourceIndex(constants[UintRootConstant3].mipUavDescriptorIndices[index])];
    imgDst[float3(pix, slice)] = outValue.r;
#else
    RWTexture2D<float> imgDst = ResourceDescriptorHeap[NonUniformResourceIndex(constants[UintRootConstant3].mipUavDescriptorIndices[index])];
    imgDst[pix] = outValue.r;
#endif
}

AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
    float m = max(max(v0.x, v1.x), max(v2.x, v3.x));
    return AF4(m, m, m, m);
}

void SpdIncreaseAtomicCounter(AU1 slice)
{
    globallycoherent RWStructuredBuffer<SpdGlobalAtomicBuffer> spdGlobalAtomic = ResourceDescriptorHeap[UintRootConstant0];
    InterlockedAdd(spdGlobalAtomic[0].counter[slice], 1, spdCounter);
}
AU1 SpdGetAtomicCounter()
{
    return spdCounter;
}
void SpdResetAtomicCounter(AU1 slice)
{
    globallycoherent RWStructuredBuffer<SpdGlobalAtomicBuffer> spdGlobalAtomic = ResourceDescriptorHeap[UintRootConstant0];
    spdGlobalAtomic[0].counter[slice] = 0;
}
AF4 SpdLoadIntermediate(AU1 x, AU1 y)
{
    return AF4(
    spdIntermediateR[x][y],
    spdIntermediateG[x][y],
    spdIntermediateB[x][y],
    spdIntermediateA[x][y]);
}
void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value)
{
    spdIntermediateR[x][y] = value.x;
    spdIntermediateG[x][y] = value.y;
    spdIntermediateB[x][y] = value.z;
    spdIntermediateA[x][y] = value.w;
}
#endif

#include "FidelityFX/ffx_spd.h"

// Main function
[numthreads(256, 1, 1)]
void DownsampleCSMain(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    StructuredBuffer<spdConstants> constantsBuf = ResourceDescriptorHeap[UintRootConstant2];
    spdConstants constants = constantsBuf[UintRootConstant3];
    SpdDownsample(
        AU2(WorkGroupId.xy),
        AU1(LocalThreadIndex),
        AU1(constants.mips),
        AU1(constants.numWorkGroups),
        AU1(WorkGroupId.z),
        AU2(constants.workGroupOffset));
}
