// CORTAO filter pass (shaders/CORTAO/CORTAOFilterPass.h, MIT, Daniel Isheden): a sparse depth- and normal-aware blur of
// the main pass's visibility and bent normal, each thread writing a 2x2 block.
//
// CORTAO leaves temporal filtering to the integrator: here that is the upscaler (DLSS, FSR, Ray Reconstruction), and the
// random seed only changes per frame while one runs. Without an upscaler the seed is fixed and the large (13-tap) kernel is
// used, selected by CORTAO_LARGE_FILTER per pipeline.
//
// The output is Builtin::CORTAO::OutputAO (R8G8B8A8_SNORM): rgb the world-space bent normal, a the visibility.

#define CORTAO_HLSL 1
#define CORTAO_PERSPECTIVE_PROJECTION 1
#define CORTAO_16_BIT 0
#define CORTAO_FILTER_BENT_NORMAL 1
#ifndef CORTAO_LARGE_FILTER
#define CORTAO_LARGE_FILTER 0
#endif
#define CORTAO_FILTER_SYMMETRY 1

#include "CORTAO/CORTAOFilterPass.h"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/cortaoCommon.hlsli"

// Root constants
#define CORTAO_FILTER_PACKED_SRV UintRootConstant0
#define CORTAO_FILTER_WORKING_SRV UintRootConstant1
#define CORTAO_FILTER_BORDER_SAMPLER UintRootConstant2
#define CORTAO_FILTER_OUTPUT_UAV UintRootConstant3

static float3x3 g_cortaoViewToWorld;

float4 CortaoUnpackSnorm4x8(uint p) {
	const int4 i = int4(uint4(p, p, p, p) << uint4(24u, 16u, 8u, 0u)) >> 24;
	return max(float4(i) * (1.0f / 127.0f), -1.0f);
}

cortao_uint4 cortao_gatherPackedData(cortao_float2 texCoords, cortao_int2 offset) {
	Texture2D<uint> packed = ResourceDescriptorHeap[CORTAO_FILTER_PACKED_SRV];
	SamplerState nearestBorder = SamplerDescriptorHeap[CORTAO_FILTER_BORDER_SAMPLER];
	return packed.Gather(nearestBorder, texCoords, offset);
}

cortao_half4x4 cortao_gatherSSAO(cortao_float2 texCoords, cortao_int2 offset) {
	Texture2D<uint> working = ResourceDescriptorHeap[CORTAO_FILTER_WORKING_SRV];
	SamplerState nearestBorder = SamplerDescriptorHeap[CORTAO_FILTER_BORDER_SAMPLER];
	const uint4 packed = working.Gather(nearestBorder, texCoords, offset);
	return cortao_half4x4(
		CortaoUnpackSnorm4x8(packed[0]),
		CortaoUnpackSnorm4x8(packed[1]),
		CortaoUnpackSnorm4x8(packed[2]),
		CortaoUnpackSnorm4x8(packed[3]));
}

void cortao_writeFilterOutput(cortao_int2 pixelCoords, cortao_half4 ssao) {
	// ssao is (visibility, view-space bent normal), blurred, so the normal is renormalized. A zero normal (the blur
	// cancelled it out) is written as zero, and readers fall back to the shading normal.
	const float3 bentNormalVS = ssao.yzw;
	float3 bentNormalWS = 0.0f.xxx;
	if (dot(bentNormalVS, bentNormalVS) > 1.0e-8f) {
		bentNormalWS = normalize(mul(bentNormalVS, g_cortaoViewToWorld));
	}
	RWTexture2D<float4> output = ResourceDescriptorHeap[CORTAO_FILTER_OUTPUT_UAV];
	output[pixelCoords] = float4(bentNormalWS, saturate(ssao.x));
}

[numthreads(8, 8, 1)] // each thread writes a 2x2 block
void CSCortaoFilter(uint2 threadId : SV_DispatchThreadID) {
	g_cortaoViewToWorld = (float3x3)CortaoMainCamera().viewInverse;
	ConstantBuffer<CORTAOConstants> constants = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CORTAO::ConstantsBuffer)];
	CORTAOFilterPassParameters parameters;
	parameters.packedResolution = constants.filter.packedResolution;
	parameters.depthSensitivity = constants.filter.depthSensitivity;
	parameters.normalSensitivity = constants.filter.normalSensitivity;
	parameters._padding0 = 0u;
	cortao_filterPass(threadId, parameters);
}
