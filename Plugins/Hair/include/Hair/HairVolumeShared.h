#ifndef BR_HAIR_VOLUME_SHARED_H
#define BR_HAIR_VOLUME_SHARED_H

// The arithmetic of the hair density volumes (HairVolumeGPU, HairRuntimeGPUTypes.h), written once for C++ and HLSL like
// HairLodShared.h: what a segment deposits and where, and where a march samples. The voxelize pass (hairVolume.hlsl)
// and the shade pass's march (hairVolume.hlsli) include it, and HairCoreTests builds and marches a volume on the CPU
// with the same functions (a slab of known density). The loops over voxels are each side's own.

#ifdef __cplusplus
#include "Hair/HairRuntimeGPUTypes.h"

#include <cmath>

#ifndef HAIR_SHARED_INLINE
#define HAIR_SHARED_INLINE inline
#endif
#define HAIR_VOLUME_FLOOR(x) std::floor(x)
#define HAIR_VOLUME_CEIL(x) std::ceil(x)

namespace br::hair {
#else
#ifndef HAIR_SHARED_INLINE
#define HAIR_SHARED_INLINE
#endif
#define HAIR_VOLUME_FLOOR(x) floor(x)
#define HAIR_VOLUME_CEIL(x) ceil(x)
#endif

// PCG's output permutation (Jarzynski and Olano 2020).
HAIR_SHARED_INLINE uint HairVolumeHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A number in [0, 1) from a hash.
HAIR_SHARED_INLINE float HairVolumeUnit(uint hashed)
{
    return (float)(hashed >> 8u) * (1.0f / 16777216.0f);
}

// What a segment adds to the voxels it lies in, in the voxels' units, summed over them: its length times its diameter
// (metres) over the volume of a voxel. The diameter is the strand pool's, with the LOD's width scale in it: the strands
// a cluster draws have the summed width of all its strands (HairLodCoverageWidthScale), so the density of a groom does
// not change with the strands it is drawn with.
HAIR_SHARED_INLINE float HairVolumeSegmentDeposit(float length, float diameter, float voxelSize)
{
    return length * diameter / (voxelSize * voxelSize * voxelSize) * HAIR_VOLUME_DENSITY_UNIT;
}

// The points along a segment its deposit is made at: one per voxel of its length, evenly spaced.
HAIR_SHARED_INLINE uint HairVolumeDepositCount(float lengthInVoxels)
{
    const float count = HAIR_VOLUME_CEIL(lengthInVoxels);
    return count < 1.0f ? 1u : (count > (float)HAIR_VOLUME_MAX_DEPOSITS_PER_SEGMENT ? HAIR_VOLUME_MAX_DEPOSITS_PER_SEGMENT : (uint)count);
}

// A voxel takes whole units. An amount is rounded down or up at random in proportion to its fraction, with a number
// that is the deposit's own in every frame: the sum over the many segments of a voxel is then right, and still.
HAIR_SHARED_INLINE uint HairVolumeRound(float amount, uint hashed)
{
    const float whole = HAIR_VOLUME_FLOOR(amount);
    return (uint)whole + (amount - whole > HairVolumeUnit(hashed) ? 1u : 0u);
}

// The share of a deposit at fraction `f` (per axis, in [0, 1)) between voxel centres that the corner voxel `corner`
// (bit 0 x, bit 1 y, bit 2 z: set is the further one) takes.
HAIR_SHARED_INLINE float HairVolumeCornerWeight(float fx, float fy, float fz, uint corner)
{
    return ((corner & 1u) != 0u ? fx : 1.0f - fx) * ((corner & 2u) != 0u ? fy : 1.0f - fy) * ((corner & 4u) != 0u ? fz : 1.0f - fz);
}

// A march from `start` to `end` (metres along the ray) in `steps` samples: sample `index` is at this distance, with
// `offset` in [0, 1) the march's jitter, and stands for a stretch of HairVolumeMarchLength. The stretches grow linearly:
// short where the march starts, at the hair that shadows the vertex most, long toward the far side of the box. Together
// they cover the whole distance.
HAIR_SHARED_INLINE float HairVolumeMarchDistance(uint index, uint steps, float offset, float start, float end)
{
    const float u = ((float)index + offset) / (float)steps;
    return start + (end - start) * u * u;
}

HAIR_SHARED_INLINE float HairVolumeMarchLength(uint index, uint steps, float start, float end)
{
    const float u0 = (float)index / (float)steps;
    const float u1 = (float)(index + 1u) / (float)steps;
    return (end - start) * (u1 * u1 - u0 * u0);
}

// The fibres a ray is expected to cross over `length` metres of hair of density `units` (a voxel's density field, or
// several voxels' interpolated).
HAIR_SHARED_INLINE float HairVolumeFibresOfDensity(float units, float length)
{
    return units * (HAIR_VOLUME_ISOTROPIC_SINE / HAIR_VOLUME_DENSITY_UNIT) * length;
}

// The same for one voxel holding `value`.
HAIR_SHARED_INLINE float HairVolumeFibres(uint value, float length)
{
    return HairVolumeFibresOfDensity((float)(value & HAIR_VOLUME_DENSITY_MASK), length);
}

#ifdef __cplusplus
} // namespace br::hair
#endif

#endif // BR_HAIR_VOLUME_SHARED_H
