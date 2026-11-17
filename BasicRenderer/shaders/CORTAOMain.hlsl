// CORTAO main pass (shaders/CORTAO/CORTAOMainPass.h, MIT, Daniel Isheden): traces CORTAO_RAY_COUNT rays per pixel against
// circular occluders built from the packed depth/normal mips, and writes visibility and the view-space bent normal packed as
// snorm8x4 into an R32_UINT, which the filter pass gathers.
//
// CORTAO_SAMPLE_COUNT is set per pipeline (the quality setting). The lookup tables are structured buffers rather than a 3D
// texture and a typed buffer, so the occluder table's nearest/clamp sampling is done here by hand.

#define CORTAO_HLSL 1
#define CORTAO_PERSPECTIVE_PROJECTION 1
#define CORTAO_16_BIT 0
#ifndef CORTAO_SAMPLE_COUNT
#define CORTAO_SAMPLE_COUNT 8
#endif
#define CORTAO_RAY_COUNT 16
#define CORTAO_OCCLUDER_LOOKUP_TABLE 1
#define CORTAO_BENT_NORMAL_LOOKUP_TABLE 1

#include "CORTAO/CORTAOMainPass.h"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/cortaoCommon.hlsli"

// Root constants
#define CORTAO_MAIN_PACKED_SRV UintRootConstant0
#define CORTAO_MAIN_NORMALS_SRV UintRootConstant1
#define CORTAO_MAIN_OCCLUDER_LUT_SRV UintRootConstant2
#define CORTAO_MAIN_BENT_NORMAL_LUT_SRV UintRootConstant3
#define CORTAO_MAIN_BORDER_SAMPLER UintRootConstant4
#define CORTAO_MAIN_OUTPUT_UAV UintRootConstant5

// CORTAOOccluderLookupTable16Rays.h: 24x24x16, x fastest.
static const uint3 CORTAO_OCCLUDER_LUT_SIZE = uint3(24u, 24u, 16u);

// The prepass writes mips 0 to mipCount - 1 of the packed data; the texture has a full chain, so the rest are never read.
static float g_cortaoMaxMip;

cortao_uint cortao_samplePackedData(cortao_float2 texCoords, cortao_float mipmap) {
	Texture2D<uint> packed = ResourceDescriptorHeap[CORTAO_MAIN_PACKED_SRV];
	SamplerState nearestBorder = SamplerDescriptorHeap[CORTAO_MAIN_BORDER_SAMPLER];
	// Integer sampling (SM 6.7). The border is 0, which CORTAO reads as "no occluder" (linear depth 0).
	return packed.SampleLevel(nearestBorder, texCoords, min(mipmap, g_cortaoMaxMip));
}

cortao_half3 cortao_loadNormal(cortao_int2 pixelCoords) {
	return CortaoLoadViewSpaceNormal(pixelCoords, CORTAO_MAIN_NORMALS_SRV);
}

cortao_uint cortao_sampleOccluderLookupTable(cortao_float3 texCoords) {
	// Nearest filtering with clamp addressing, as the reference samples its R16_UINT 3D texture.
	StructuredBuffer<uint> table = ResourceDescriptorHeap[CORTAO_MAIN_OCCLUDER_LUT_SRV];
	const uint3 texel = min(uint3(saturate(texCoords) * float3(CORTAO_OCCLUDER_LUT_SIZE)), CORTAO_OCCLUDER_LUT_SIZE - 1u);
	return table[(texel.z * CORTAO_OCCLUDER_LUT_SIZE.y + texel.y) * CORTAO_OCCLUDER_LUT_SIZE.x + texel.x];
}

cortao_half3 cortao_loadBentNormalLookupTable(cortao_uint rayBits) {
	// 65536 R8G8_SNORM entries, two per word, x in the low byte.
	StructuredBuffer<uint> table = ResourceDescriptorHeap[CORTAO_MAIN_BENT_NORMAL_LUT_SRV];
	const uint entry = table[rayBits >> 1u] >> ((rayBits & 1u) * 16u);
	const int2 bytes = int2(uint2(entry << 24u, entry << 16u)) >> 24;
	const float2 bentNormal = max(float2(bytes) * (1.0f / 127.0f), -1.0f);
	return cortao_half3(bentNormal, sqrt(max(0.0f, 1.0f - dot(bentNormal, bentNormal))));
}

uint CortaoPackSnorm4x8(float4 v) {
	const uint4 encoded = uint4(int4(round(clamp(v, -1.0f, 1.0f) * 127.0f))) & 0xFFu;
	return encoded.x | (encoded.y << 8u) | (encoded.z << 16u) | (encoded.w << 24u);
}

void cortao_writeResult(cortao_int2 pixelCoords, cortao_half occlusion, cortao_half3 bentNormal, cortao_half3x3 tangentSpaceTransform, cortao_uint rayBits) {
	RWTexture2D<uint> output = ResourceDescriptorHeap[CORTAO_MAIN_OUTPUT_UAV];
	output[pixelCoords] = CortaoPackSnorm4x8(float4(occlusion, bentNormal));
}

// Reorders the threads of a group into Morton order, which is more cache coherent (the reference's deinterleave()).
uint2 CortaoDeinterleave(uint index) {
	uint2 p = uint2(index, index) >> uint2(0u, 1u);
	p &= 0x5555u;
	p = (p ^ (p >> 1u)) & 0x3333u;
	p = (p ^ (p >> 2u)) & 0x0f0fu;
	p = (p ^ (p >> 4u)) & 0x00ffu;
	return p;
}

#define CORTAO_MAIN_GROUP_SIZE 16
[numthreads(CORTAO_MAIN_GROUP_SIZE, CORTAO_MAIN_GROUP_SIZE, 1)]
void CSCortaoMain(uint2 groupId : SV_GroupID, uint localIndex : SV_GroupIndex) {
	ConstantBuffer<CORTAOConstants> constants = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CORTAO::ConstantsBuffer)];
	g_cortaoMaxMip = float(max(constants.prepass.mipCount, 1u) - 1u);
	CORTAOMainPassParameters parameters;
	parameters.packedResolution = constants.mainPass.packedResolution;
	parameters.randomSeed = constants.mainPass.randomSeed;
	parameters.normalBias = constants.mainPass.normalBias;
	parameters.radius = constants.mainPass.radius;
	parameters.unprojectParams = constants.mainPass.unprojectParams;
	parameters.projectionParams = constants.mainPass.projectionParams;
	parameters.maxApparentCircleRadiusCoefficient = constants.mainPass.maxApparentCircleRadiusCoefficient;
	parameters.mipBias = constants.mainPass.mipBias;
	parameters.inverseMaxSlopeCompensation = constants.mainPass.inverseMaxSlopeCompensation;
	parameters.backfaceThreshold = constants.mainPass.backfaceThreshold;
	parameters.backfaceBoost = constants.mainPass.backfaceBoost;
	parameters._padding0 = 0u;
	parameters._padding1 = 0u;
	parameters._padding2 = 0u;

	const uint2 pixelCoords = groupId * uint2(CORTAO_MAIN_GROUP_SIZE, CORTAO_MAIN_GROUP_SIZE) + CortaoDeinterleave(localIndex);
	cortao_mainPass(pixelCoords, parameters);
}
