#ifndef __PATH_TRACING_RANDOM_HLSLI__
#define __PATH_TRACING_RANDOM_HLSLI__

// PCG with an output permutation (Jarzynski and Olano 2020, "Hash Functions for GPU Rendering").
// Each seed also picks its own LCG increment: on a single 32-bit cycle, the streams of different
// pixels and samples become shifted copies of each other once enough samples have accumulated.
struct PathRng
{
    uint state;
    uint increment; // Always odd, which is what gives the LCG a full period.
};

uint PathRngHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint PathRngNextUint(inout PathRng rng)
{
    rng.state = rng.state * 747796405u + rng.increment;
    const uint word = ((rng.state >> ((rng.state >> 28u) + 4u)) ^ rng.state) * 277803737u;
    return (word >> 22u) ^ word;
}

PathRng PathRngCreate(uint2 pixel, uint frameIndex, uint sampleIndex)
{
    const uint seed = PathRngHash(pixel.x + PathRngHash(pixel.y + PathRngHash(frameIndex + PathRngHash(sampleIndex))));
    PathRng rng;
    rng.state = seed;
    rng.increment = PathRngHash(seed ^ 0x9E3779B9u) | 1u;
    PathRngNextUint(rng);
    return rng;
}

// In [0, 1). Keeping 24 bits means the conversion is exact and can never round up to 1.
float PathRngNext1(inout PathRng rng)
{
    return float(PathRngNextUint(rng) >> 8u) * (1.0f / 16777216.0f);
}

float2 PathRngNext2(inout PathRng rng)
{
    const float x = PathRngNext1(rng);
    const float y = PathRngNext1(rng);
    return float2(x, y);
}

#endif // __PATH_TRACING_RANDOM_HLSLI__
