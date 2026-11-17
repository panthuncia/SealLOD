#ifndef CORTAO_COMMON_HLSLI
#define CORTAO_COMMON_HLSLI

// Shared by the three CORTAO entry files (CORTAOPrepass.hlsl, CORTAOMain.hlsl, CORTAOFilter.hlsl). Each includes one
// vendored pass header, and those headers can't be included together, so the constants are mirrored here with their own
// names and copied into the vendored parameter structs by each entry point.

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"

// The linear depth map is cleared to FLT_MAX where nothing was drawn.
static const float CORTAO_SKY_LINEAR_DEPTH = 3.0e38f;

// Mirrors CORTAOConstants in ShaderBuffers.h: CORTAO's three parameter structs back to back, at 0, 32 and 112 bytes.
struct CORTAOPrepassConstants {
	uint packedResolution;
	float depthScale;
	float depthOffset;
	uint randomSeed;

	uint mipCount;
	uint3 _padding;
};

struct CORTAOMainPassConstants {
	uint packedResolution;
	uint randomSeed;
	float normalBias;
	float radius;

	float4 unprojectParams;
	float4 projectionParams;

	float maxApparentCircleRadiusCoefficient;
	float mipBias;
	float inverseMaxSlopeCompensation;
	float backfaceThreshold;

	float backfaceBoost;
	uint3 _padding;
};

struct CORTAOFilterPassConstants {
	uint packedResolution;
	float depthSensitivity;
	float normalSensitivity;
	uint _padding;
};

struct CORTAOConstants {
	CORTAOPrepassConstants prepass;
	CORTAOMainPassConstants mainPass;
	CORTAOFilterPassConstants filter;
};

Camera CortaoMainCamera() {
	ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
	StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
	return cameras[perFrameBuffer.mainCameraIndex];
}

// The canonical surface's world-space normal in view space. The renderer's view space is right-handed and looks down -Z,
// which is CORTAO's convention, so unlike XeGTAO no axis is flipped.
float3 CortaoLoadViewSpaceNormal(int2 pixelCoords, uint normalsDescriptorIndex) {
	Texture2D<float4> normals = ResourceDescriptorHeap[normalsDescriptorIndex];
	const float3 normalWS = normals.Load(int3(pixelCoords, 0)).xyz;
	const float3 normalVS = mul(normalWS, (float3x3)CortaoMainCamera().view);
	const float lengthSquared = dot(normalVS, normalVS);
	return lengthSquared > 1.0e-12f ? normalVS * rsqrt(lengthSquared) : float3(0.0f, 0.0f, 1.0f);
}

#endif // CORTAO_COMMON_HLSLI
