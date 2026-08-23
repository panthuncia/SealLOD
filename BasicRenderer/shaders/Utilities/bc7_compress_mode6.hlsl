#include "Include/cbuffers.hlsli"

static const uint BC7Mode6Weights[16] = {
    0u, 4u, 9u, 13u,
    17u, 21u, 26u, 30u,
    34u, 38u, 43u, 47u,
    51u, 55u, 60u, 64u
};

struct BC7Mode6Block
{
    uint4 words;
    uint bitPosition;
};

struct QuantizedEndpoint
{
    uint4 q7;
    uint pbit;
    uint4 bytes;
};

uint FloatToByte(float value)
{
    return (uint)round(saturate(value) * 255.0f);
}

uint FloatByteToUInt(float value)
{
    return (uint)round(clamp(value, 0.0f, 255.0f));
}

float4 ClampByteRange(float4 value)
{
    return clamp(value, 0.0f, 255.0f);
}

void WriteBits(inout BC7Mode6Block block, uint bitCount, uint value)
{
    for (uint bit = 0; bit < bitCount; ++bit)
    {
        const uint absoluteBit = block.bitPosition + bit;
        const uint wordIndex = absoluteBit >> 5u;
        const uint bitIndex = absoluteBit & 31u;
        if (((value >> bit) & 1u) != 0u)
        {
            block.words[wordIndex] |= (1u << bitIndex);
        }
    }

    block.bitPosition += bitCount;
}

uint ReconstructEndpoint(uint q7, uint pbit)
{
    return min(255u, (q7 << 1u) | pbit);
}

QuantizedEndpoint QuantizeEndpoint(float4 endpointValue)
{
    uint endpointBytes[4] = {
        FloatByteToUInt(endpointValue.x),
        FloatByteToUInt(endpointValue.y),
        FloatByteToUInt(endpointValue.z),
        FloatByteToUInt(endpointValue.w)
    };

    uint bestPbit = 0u;
    uint bestError = 0xffffffffu;
    uint4 bestQ7 = uint4(0u, 0u, 0u, 0u);

    [unroll]
    for (uint pbit = 0u; pbit < 2u; ++pbit)
    {
        uint totalError = 0u;
        uint4 candidateQ7 = uint4(0u, 0u, 0u, 0u);

        [unroll]
        for (uint component = 0u; component < 4u; ++component)
        {
            const uint value = endpointBytes[component];
            const uint q7 = min(127u, (value + 1u - pbit) >> 1u);
            const uint reconstructed = ReconstructEndpoint(q7, pbit);
            const int delta = int(value) - int(reconstructed);
            totalError += uint(delta * delta);
            candidateQ7[component] = q7;
        }

        if (totalError < bestError)
        {
            bestError = totalError;
            bestPbit = pbit;
            bestQ7 = candidateQ7;
        }
    }

    QuantizedEndpoint result;
    result.q7 = bestQ7;
    result.pbit = bestPbit;
    result.bytes = uint4(
        ReconstructEndpoint(bestQ7.x, bestPbit),
        ReconstructEndpoint(bestQ7.y, bestPbit),
        ReconstructEndpoint(bestQ7.z, bestPbit),
        ReconstructEndpoint(bestQ7.w, bestPbit));
    return result;
}

float4 PaletteColor(float4 endpoint0, float4 endpoint1, uint index)
{
    const float weight = float(BC7Mode6Weights[index]) * (1.0f / 64.0f);
    return lerp(endpoint0, endpoint1, weight);
}

uint ChooseBestIndex(float4 sampleValue, float4 endpoint0, float4 endpoint1)
{
    uint bestIndex = 0u;
    float bestError = 3.402823466e+38F;

    [unroll]
    for (uint index = 0u; index < 16u; ++index)
    {
        const float4 palette = PaletteColor(endpoint0, endpoint1, index);
        const float4 delta = sampleValue - palette;
        const float error = dot(delta, delta);
        if (error < bestError)
        {
            bestError = error;
            bestIndex = index;
        }
    }

    return bestIndex;
}

void RefineEndpoints(float4 samples[16], uint indices[16], inout float4 endpoint0, inout float4 endpoint1)
{
    float sumA2 = 0.0f;
    float sumAB = 0.0f;
    float sumB2 = 0.0f;
    float4 rhsA = 0.0f.xxxx;
    float4 rhsB = 0.0f.xxxx;

    [unroll]
    for (uint index = 0u; index < 16u; ++index)
    {
        const float t = float(BC7Mode6Weights[indices[index]]) * (1.0f / 64.0f);
        const float a = 1.0f - t;
        const float b = t;
        sumA2 += a * a;
        sumAB += a * b;
        sumB2 += b * b;
        rhsA += samples[index] * a;
        rhsB += samples[index] * b;
    }

    const float det = sumA2 * sumB2 - sumAB * sumAB;
    if (abs(det) < 1.0e-6f)
    {
        return;
    }

    endpoint0 = ClampByteRange((rhsA * sumB2 - rhsB * sumAB) / det);
    endpoint1 = ClampByteRange((rhsB * sumA2 - rhsA * sumAB) / det);
}

void FitPrincipalAxisEndpoints(float4 samples[16], inout float4 endpoint0, inout float4 endpoint1)
{
    float4 mean = 0.0f.xxxx;
    [unroll]
    for (uint index = 0u; index < 16u; ++index)
    {
        mean += samples[index];
    }
    mean *= (1.0f / 16.0f);

    // Component-wise range is a useful non-zero seed, but it is not itself a
    // valid endpoint fit: mode 6 has one shared index for all RGBA channels.
    float4 axis = endpoint1 - endpoint0;
    float axisLengthSquared = dot(axis, axis);
    if (axisLengthSquared < 1.0e-8f)
    {
        endpoint0 = mean;
        endpoint1 = mean;
        return;
    }
    axis *= rsqrt(axisLengthSquared);

    // Power iteration over the 4D covariance matrix. Expressing C*v as a sum
    // of delta*dot(delta,v) avoids materializing a matrix and maps well to the
    // small fixed 4x4 block.
    [unroll]
    for (uint iteration = 0u; iteration < 8u; ++iteration)
    {
        float4 nextAxis = 0.0f.xxxx;
        [unroll]
        for (uint index = 0u; index < 16u; ++index)
        {
            const float4 delta = samples[index] - mean;
            nextAxis += delta * dot(delta, axis);
        }

        const float nextLengthSquared = dot(nextAxis, nextAxis);
        if (nextLengthSquared < 1.0e-8f)
        {
            break;
        }
        axis = nextAxis * rsqrt(nextLengthSquared);
    }

    float minProjection = 3.402823466e+38F;
    float maxProjection = -3.402823466e+38F;
    [unroll]
    for (uint index = 0u; index < 16u; ++index)
    {
        const float projection = dot(samples[index] - mean, axis);
        minProjection = min(minProjection, projection);
        maxProjection = max(maxProjection, projection);
    }

    endpoint0 = ClampByteRange(mean + axis * minProjection);
    endpoint1 = ClampByteRange(mean + axis * maxProjection);
}

BC7Mode6Block EncodeMode6(Texture2D<float4> srcTexture, uint2 blockBase, uint2 textureSize)
{
    float4 samples[16];
    float4 endpoint0 = float4(255.0f, 255.0f, 255.0f, 255.0f);
    float4 endpoint1 = float4(0.0f, 0.0f, 0.0f, 0.0f);

    [unroll]
    for (uint y = 0u; y < 4u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 4u; ++x)
        {
            const uint linearIndex = y * 4u + x;
            const uint2 pixel = min(blockBase + uint2(x, y), textureSize - 1u.xx);
            const float4 sampleBytes = ClampByteRange(srcTexture.Load(int3(pixel, 0)) * 255.0f);
            samples[linearIndex] = sampleBytes;
            endpoint0 = min(endpoint0, sampleBytes);
            endpoint1 = max(endpoint1, sampleBytes);
        }
    }

    FitPrincipalAxisEndpoints(samples, endpoint0, endpoint1);

    uint indices[16];

    QuantizedEndpoint endpoint0Quant;
    QuantizedEndpoint endpoint1Quant;
    [unroll]
    for (uint iteration = 0u; iteration < 3u; ++iteration)
    {
        endpoint0Quant = QuantizeEndpoint(endpoint0);
        endpoint1Quant = QuantizeEndpoint(endpoint1);

        [unroll]
        for (uint sampleIndex = 0u; sampleIndex < 16u; ++sampleIndex)
        {
            indices[sampleIndex] = ChooseBestIndex(
                samples[sampleIndex],
                float4(endpoint0Quant.bytes),
                float4(endpoint1Quant.bytes));
        }

        if (iteration < 2u)
        {
            RefineEndpoints(samples, indices, endpoint0, endpoint1);
        }
    }

    uint4 endpoint0Q7 = endpoint0Quant.q7;
    uint4 endpoint1Q7 = endpoint1Quant.q7;
    uint endpoint0Pbit = endpoint0Quant.pbit;
    uint endpoint1Pbit = endpoint1Quant.pbit;

    if (indices[0] >= 8u)
    {
        const uint4 swappedQ7 = endpoint0Q7;
        endpoint0Q7 = endpoint1Q7;
        endpoint1Q7 = swappedQ7;

        const uint swappedPbit = endpoint0Pbit;
        endpoint0Pbit = endpoint1Pbit;
        endpoint1Pbit = swappedPbit;

        [unroll]
        for (uint index = 0u; index < 16u; ++index)
        {
            indices[index] = 15u - indices[index];
        }
    }

    BC7Mode6Block block = (BC7Mode6Block)0;
    block.bitPosition = 0u;
    WriteBits(block, 6u, 0u);
    WriteBits(block, 1u, 1u);

    [unroll]
    for (uint component = 0u; component < 4u; ++component)
    {
        WriteBits(block, 7u, endpoint0Q7[component]);
        WriteBits(block, 7u, endpoint1Q7[component]);
    }

    WriteBits(block, 1u, endpoint0Pbit);
    WriteBits(block, 1u, endpoint1Pbit);
    WriteBits(block, 3u, indices[0]);
    [unroll]
    for (uint index = 1u; index < 16u; ++index)
    {
        WriteBits(block, 4u, indices[index]);
    }

    return block;
}

[numthreads(8, 8, 1)]
void BC7CompressMode6CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D<float4> srcTexture = ResourceDescriptorHeap[UintRootConstant0];
    RWByteAddressBuffer dstBuffer = ResourceDescriptorHeap[UintRootConstant1];

    const uint blocksX = (UintRootConstant4 + 3u) / 4u;
    const uint blocksY = (UintRootConstant5 + 3u) / 4u;
    if (dispatchThreadId.x >= blocksX || dispatchThreadId.y >= blocksY)
    {
        return;
    }

    const uint2 blockBase = dispatchThreadId.xy * 4u;
    const uint2 textureSize = uint2(UintRootConstant4, UintRootConstant5);
    BC7Mode6Block block = EncodeMode6(srcTexture, blockBase, textureSize);

    const uint byteOffset = UintRootConstant2 + dispatchThreadId.y * UintRootConstant3 + dispatchThreadId.x * 16u;
    dstBuffer.Store4(byteOffset, block.words);
}
