#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/constants.hlsli"

// Thread-group size: 16x16 threads per face, dispatch Z=6 for the 6 faces
[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
	uint x = DTid.x;
	uint y = DTid.y;
	uint face = DTid.z;

    uint faceSize = UintRootConstant0;
    uint samplerIndex = UintRootConstant1;
    uint environmentIndex = UintRootConstant2;
    // The source cubemap's view, from the pass: the environment's own entry may not be uploaded yet.
    uint cubemapDescriptorIndex = UintRootConstant3;

    RWStructuredBuffer<EnvironmentInfo> environments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Environment::InfoBuffer)];
    TextureCube<float4> g_envMap = ResourceDescriptorHeap[cubemapDescriptorIndex];
	
    // out-of-bounds guard
        if (x >= faceSize || y >= faceSize)
            return;

    // Compute a unit-direction from (face, u, v)
	float u = (x + 0.5f) / faceSize * 2.0f - 1.0f;
	float v = (y + 0.5f) / faceSize * 2.0f - 1.0f;
	float3 dir;
	switch (face) {
		case 0:
			dir = float3(+1, -v, -u);
			break; // +X
		case 1:
			dir = float3(-1, -v, +u);
			break; // +X
		case 2:
			dir = float3(+u, +1, +v);
			break; // +Y
		case 3:
			dir = float3(+u, -1, -v);
			break; // +Y
		case 4:
			dir = float3(+u, -v, +1);
			break; // +Z
		default:
			dir = float3(-u, -v, -1); // -Z
	}
	dir = normalize(dir);

    SamplerState g_sampler = SamplerDescriptorHeap[samplerIndex];
	float3 L = g_envMap.SampleLevel(g_sampler, dir, 0).rgb;

	const float c0 = 0.28209479;
	const float c1 = 0.48860251;
	const float c2 = 1.09254843;
	const float c3 = 0.31539157;
	const float c4 = 0.54627422;

	float sh[9];
	sh[0] = c0;
	sh[1] = c1 * dir.y;
	sh[2] = c1 * dir.z;
	sh[3] = c1 * dir.x;

	sh[4] = c2 * dir.x * dir.y;
	sh[5] = c2 * dir.y * dir.z;
	sh[6] = c3 * (3 * dir.z * dir.z - 1);
	sh[7] = c2 * dir.z * dir.x;
	sh[8] = c4 * (dir.x * dir.x - dir.y * dir.y);

	for (uint i = 0; i < 9; ++i) {
		float3 contrib = L * sh[i] /** weight*/;
        uint flatIndex = i * 3;
        InterlockedAdd(environments[environmentIndex].sphericalHarmonics[flatIndex], (int) (contrib.x * SH_FLOAT_SCALE));
        InterlockedAdd(environments[environmentIndex].sphericalHarmonics[flatIndex + 1], (int) (contrib.y * SH_FLOAT_SCALE));
        InterlockedAdd(environments[environmentIndex].sphericalHarmonics[flatIndex + 2], (int) (contrib.z * SH_FLOAT_SCALE));
    }
}
