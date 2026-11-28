#ifndef HAIR_VOLUME_HLSLI
#define HAIR_VOLUME_HLSLI

// Reading the hair density volumes (HairVolumeGPU, HairRuntimeGPUTypes.h): where a world position is in an instance's
// volume, and the march from a point toward a light that counts the fibres in the way. For the shade pass
// (hairShade.hlsl), the voxelize pass (hairVolume.hlsl) and the density debug view (hairDebugOverlay.hlsl). The
// arithmetic the CPU's tests share is Hair/HairVolumeShared.h.

#include "HairShaders/hairStructs.hlsli"
#include "Hair/HairVolumeShared.h"

bool HairVolumeExists(HairVolumeGPU volume)
{
    return volume.resolution.x != 0u;
}

// Voxel coordinates of a world position: voxel (i, j, k) covers [i, i + 1) on each axis.
float3 HairVolumeGridPosition(HairVolumeGPU volume, float3 world)
{
    return float3(dot(volume.worldToGrid0.xyz, world) + volume.worldToGrid0.w, dot(volume.worldToGrid1.xyz, world) + volume.worldToGrid1.w,
        dot(volume.worldToGrid2.xyz, world) + volume.worldToGrid2.w);
}

// Voxels per metre along a world direction.
float3 HairVolumeGridDirection(HairVolumeGPU volume, float3 direction)
{
    return float3(dot(volume.worldToGrid0.xyz, direction), dot(volume.worldToGrid1.xyz, direction), dot(volume.worldToGrid2.xyz, direction));
}

bool HairVolumeContains(HairVolumeGPU volume, int3 voxel)
{
    return all(voxel >= 0) && all(voxel < (int3)volume.resolution);
}

uint HairVolumeVoxelIndex(HairVolumeGPU volume, uint3 voxel)
{
    return volume.voxelBase + voxel.x + volume.resolution.x * (voxel.y + volume.resolution.y * voxel.z);
}

// The stretch of a ray (metres from `gridOrigin` along `gridDirection`, both in voxel coordinates) inside the volume's
// box. `exit` is not above `enter` for a ray that misses it.
void HairVolumeRayRange(HairVolumeGPU volume, float3 gridOrigin, float3 gridDirection, out float enter, out float exit)
{
    // A ray along a face of the box: a tiny slope keeps the division finite and the test right.
    const float3 slope = float3(abs(gridDirection.x) > 1.0e-6f ? gridDirection.x : 1.0e-6f, abs(gridDirection.y) > 1.0e-6f ? gridDirection.y : 1.0e-6f,
        abs(gridDirection.z) > 1.0e-6f ? gridDirection.z : 1.0e-6f);
    const float3 toLow = (0.0f.xxx - gridOrigin) / slope;
    const float3 toHigh = ((float3)volume.resolution - gridOrigin) / slope;
    const float3 nearer = min(toLow, toHigh);
    const float3 further = max(toLow, toHigh);
    enter = max(max(nearer.x, nearer.y), nearer.z);
    exit = min(min(further.x, further.y), further.z);
}

// The volume at a point in voxel coordinates: the density (in a voxel's units) and how much of the point is in the scalp
// shell (0 to 1). Interpolated between the centres of the eight voxels around the point, or the one voxel the point is in.
void HairVolumeSample(HairVolumeGPU volume, StructuredBuffer<uint> voxels, float3 grid, bool interpolate, out float density, out float scalp)
{
    const int3 last = (int3)volume.resolution - 1;
    if (!interpolate)
    {
        const uint value = voxels[HairVolumeVoxelIndex(volume, (uint3)clamp((int3)floor(grid), 0, last))];
        density = (float)(value & HAIR_VOLUME_DENSITY_MASK);
        scalp = (value & HAIR_VOLUME_SCALP_BIT) != 0u ? 1.0f : 0.0f;
        return;
    }
    const float3 centred = grid - 0.5f;
    const float3 low = floor(centred);
    const float3 share = centred - low;
    density = 0.0f;
    scalp = 0.0f;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        const int3 voxel = clamp((int3)low + int3(corner & 1u, (corner >> 1u) & 1u, corner >> 2u), 0, last);
        const uint value = voxels[HairVolumeVoxelIndex(volume, (uint3)voxel)];
        const float weight = HairVolumeCornerWeight(share.x, share.y, share.z, corner);
        density += weight * (float)(value & HAIR_VOLUME_DENSITY_MASK);
        scalp += (value & HAIR_VOLUME_SCALP_BIT) != 0u ? weight : 0.0f;
    }
}

// What a march from a point toward a light found.
struct HairVolumeMarch
{
    float fibreCount; // the fibres a ray that way is expected to cross
    float open;       // the share of the way that is clear of the scalp shell: 0 when nothing comes from there
    uint steps;       // samples taken
};

// Counts the fibres between `position` and a light `maxDistance` away along `direction` (a unit vector; a large distance
// for a directional light), in `steps` samples of the volume from `bias` voxels along the ray to where it leaves the
// box. `offset` in [0, 1) is the march's jitter. The first HAIR_VOLUME_INTERPOLATED_SAMPLES samples are interpolated
// between voxels; the others read the voxel they are in (the deposits are already spread over neighbouring voxels,
// hairVolume.hlsl).
HairVolumeMarch HairMarchVolume(HairVolumeGPU volume, StructuredBuffer<uint> voxels, float3 position, float3 direction, float maxDistance,
    uint steps, float bias, float offset)
{
    HairVolumeMarch march;
    march.fibreCount = 0.0f;
    march.open = 1.0f;
    march.steps = 0u;
    const float3 gridOrigin = HairVolumeGridPosition(volume, position);
    const float3 gridDirection = HairVolumeGridDirection(volume, direction);
    float enter;
    float exit;
    HairVolumeRayRange(volume, gridOrigin, gridDirection, enter, exit);
    const float start = max(enter, 0.0f) + bias * volume.voxelSize;
    const float end = min(exit, maxDistance);
    if (!(end > start))
    {
        return march;
    }
    for (uint index = 0u; index < steps; ++index)
    {
        const float distance = HairVolumeMarchDistance(index, steps, offset, start, end);
        float density;
        float scalp;
        HairVolumeSample(volume, voxels, gridOrigin + gridDirection * distance, index < HAIR_VOLUME_INTERPOLATED_SAMPLES, density, scalp);
        ++march.steps;
        march.open *= 1.0f - scalp;
        if (march.open < 1.0e-3f)
        {
            march.open = 0.0f;
            break;
        }
        march.fibreCount += HairVolumeFibresOfDensity(density, HairVolumeMarchLength(index, steps, start, end));
    }
    return march;
}

#endif // HAIR_VOLUME_HLSLI
