// CORTAO prepass (shaders/CORTAO/CORTAOPrepass.h, MIT, Daniel Isheden): linear depth and a dithered spheremap normal packed
// into one R32_UINT per pixel, written to up to 8 mips in one dispatch.
//
// Depth comes from the primary camera's linear depth map (positive view depth, FLT_MAX where nothing was drawn) rather
// than a hardware depth buffer. The prepass computes linear depth as 1 / (depth * depthScale + depthOffset), so the CPU sets
// depthScale 1 and depthOffset 0 and cortao_loadDepth returns 1 / linear depth, or 0 for the sky, which is CORTAO's skybox
// depth when depthScale is positive.

#define CORTAO_HLSL 1
#define CORTAO_PERSPECTIVE_PROJECTION 1

#include "CORTAO/CORTAOPrepass.h"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/cortaoCommon.hlsli"

// Root constants
#define CORTAO_PREPASS_DEPTH_SRV UintRootConstant0
#define CORTAO_PREPASS_NORMALS_SRV UintRootConstant1
// UintRootConstant2 .. UintRootConstant9: the packed data's mips 0 to 7 (unused mips past mipCount are never written)

static uint g_cortaoPackedUavs[8];

cortao_float cortao_loadDepth(cortao_int2 pixelCoords) {
	Texture2D<float> linearDepth = ResourceDescriptorHeap[CORTAO_PREPASS_DEPTH_SRV];
	const float depth = linearDepth.Load(int3(pixelCoords, 0));
	return depth >= CORTAO_SKY_LINEAR_DEPTH ? 0.0f : 1.0f / depth;
}

cortao_float3 cortao_loadNormal(cortao_int2 pixelCoords) {
	return CortaoLoadViewSpaceNormal(pixelCoords, CORTAO_PREPASS_NORMALS_SRV);
}

void cortao_writePackedData(cortao_int2 pixelCoords, cortao_int mipLevel, cortao_uint data) {
	// The mip loop is unrolled, so mipLevel is a constant here.
	RWTexture2D<uint> packed = ResourceDescriptorHeap[g_cortaoPackedUavs[mipLevel]];
	packed[pixelCoords] = data;
}

[numthreads(8, 8, 1)]
void CSCortaoPrepass(uint2 pixelCoords : SV_DispatchThreadID) {
	g_cortaoPackedUavs[0] = UintRootConstant2;
	g_cortaoPackedUavs[1] = UintRootConstant3;
	g_cortaoPackedUavs[2] = UintRootConstant4;
	g_cortaoPackedUavs[3] = UintRootConstant5;
	g_cortaoPackedUavs[4] = UintRootConstant6;
	g_cortaoPackedUavs[5] = UintRootConstant7;
	g_cortaoPackedUavs[6] = UintRootConstant8;
	g_cortaoPackedUavs[7] = UintRootConstant9;
	ConstantBuffer<CORTAOConstants> constants = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CORTAO::ConstantsBuffer)];
	CORTAOPrepassParameters parameters;
	parameters.packedResolution = constants.prepass.packedResolution;
	parameters.depthScale = constants.prepass.depthScale;
	parameters.depthOffset = constants.prepass.depthOffset;
	parameters.randomSeed = constants.prepass.randomSeed;
	parameters.mipCount = constants.prepass.mipCount;
	parameters._padding0 = 0u;
	parameters._padding1 = 0u;
	parameters._padding2 = 0u;
	cortao_prepass(pixelCoords, parameters);
}
