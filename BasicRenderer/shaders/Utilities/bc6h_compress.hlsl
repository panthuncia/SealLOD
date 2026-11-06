#include "Include/cbuffers.hlsli"

// BC6H_UF16 encoder for the single-region modes 11-14: one pair of endpoints a block, 4-bit indices.
//
// The format (as AMD Compressonator's reference decoder, cmp_compressonatorlib/bc6h/bc6h_decode.cpp, reads it):
// - Endpoints are unsigned integers of `prec` bits that stand for half-float bit patterns. They are unquantized to 16
//   bits (0 -> 0, the largest -> 0xFFFF, else ((q << 16) + 0x8000) >> prec), interpolated with the 4-bit weights as
//   (a * (64 - w) + b * w + 32) >> 6, and the result scaled by 31/64 is the texel's half.
// - Mode 11 stores both 10-bit endpoints. Modes 12, 13 and 14 store an 11, 12 or 16-bit endpoint A and B - A in 9, 8
//   or 4 signed bits: the closer the endpoints, the more precise they can be, which suits smooth HDR gradients.
// - Bits [0,5) hold the mode, then A's low 10 bits for red, green, blue, then per channel (10 bits each from bit 35)
//   the second field followed by A's bits above 10 from the highest down, then the indices from bit 65: 3 bits for
//   texel 0, whose top bit is implied zero, and 4 for each other texel.
//
// Errors are measured in half bits, roughly relative error, which is what the format interpolates in.
//
// Root constants: 0 source SRV (Texture2DArray<float4>, one slice), 1 destination raw UAV, 2 its byte offset,
// 3 its row pitch, 4/5 source width and height.

static const uint kBc6hWeights[16] = { 0u, 4u, 9u, 13u, 17u, 21u, 26u, 30u, 34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u };
static const uint kBc6hMaxHalf = 0x7BFFu; // The largest finite half.

struct Bc6hBlock
{
    uint4 words;
    uint bitPosition;
};

void Bc6hWriteBits(inout Bc6hBlock block, uint bitCount, uint value)
{
    for (uint bit = 0u; bit < bitCount; ++bit)
    {
        const uint absoluteBit = block.bitPosition + bit;
        if (((value >> bit) & 1u) != 0u)
        {
            block.words[absoluteBit >> 5u] |= 1u << (absoluteBit & 31u);
        }
    }
    block.bitPosition += bitCount;
}

uint3 Bc6hHalfBits(float3 value)
{
    // Unsigned: negatives and NaN to zero, infinities to the largest half.
    const float3 clean = float3(value.x > 0.0f ? value.x : 0.0f, value.y > 0.0f ? value.y : 0.0f, value.z > 0.0f ? value.z : 0.0f);
    return min(f32tof16(min(clean, 65504.0f.xxx)), kBc6hMaxHalf.xxx);
}

uint Bc6hUnquantize(uint q, uint prec)
{
    if (prec >= 15u) return q;
    if (q == 0u) return 0u;
    if (q == (1u << prec) - 1u) return 0xFFFFu;
    return ((q << 16u) + 0x8000u) >> prec;
}

// The texel's half bits for a palette entry of unquantized endpoints.
uint3 Bc6hPaletteHalf(uint3 a, uint3 b, uint weight)
{
    const uint3 interpolated = (a * (64u - weight) + b * weight + 32u) >> 6u;
    return (interpolated * 31u) >> 6u;
}

// The prec-bit endpoint whose unquantized value is nearest u (0..65535).
uint Bc6hQuantizeComponent(float u, uint prec)
{
    const uint maxQ = (1u << prec) - 1u;
    const int guess = (int)floor(saturate(u / 65535.0f) * (float)(maxQ + 1u));
    uint best = 0u;
    float bestError = 3.4e38f;
    [unroll]
    for (int offset = -1; offset <= 1; ++offset)
    {
        const uint q = (uint)clamp(guess + offset, 0, (int)maxQ);
        const float error = abs((float)Bc6hUnquantize(q, prec) - u);
        if (error < bestError)
        {
            bestError = error;
            best = q;
        }
    }
    return best;
}

uint3 Bc6hQuantize(float3 u, uint prec)
{
    return uint3(Bc6hQuantizeComponent(u.x, prec), Bc6hQuantizeComponent(u.y, prec), Bc6hQuantizeComponent(u.z, prec));
}

uint Bc6hSquaredError(uint3 a, uint3 b)
{
    const int3 difference = int3(a) - int3(b);
    return (uint)dot(difference, difference);
}

// Unquantized space (0..65535) of a half: what the endpoints are fitted in.
float3 Bc6hHalfToUnquantized(uint3 halfBits)
{
    return float3(halfBits) * (64.0f / 31.0f);
}

void Bc6hFitEndpoints(float3 points[16], out float3 endpointA, out float3 endpointB)
{
    float3 mean = 0.0f.xxx;
    float3 lo = points[0];
    float3 hi = points[0];
    [unroll]
    for (uint i = 0u; i < 16u; ++i)
    {
        mean += points[i];
        lo = min(lo, points[i]);
        hi = max(hi, points[i]);
    }
    mean /= 16.0f;

    // Principal axis by power iteration on the covariance.
    float3 c0 = 0.0f.xxx, c1 = 0.0f.xxx, c2 = 0.0f.xxx;
    [unroll]
    for (uint j = 0u; j < 16u; ++j)
    {
        const float3 d = points[j] - mean;
        c0 += d.x * d;
        c1 += d.y * d;
        c2 += d.z * d;
    }
    float3 axis = hi - lo;
    [unroll]
    for (uint iteration = 0u; iteration < 6u; ++iteration)
    {
        axis = float3(dot(c0, axis), dot(c1, axis), dot(c2, axis));
        const float len = length(axis);
        axis = len > 1e-6f ? axis / len : float3(0.57735f, 0.57735f, 0.57735f);
    }

    float tMin = 3.4e38f, tMax = -3.4e38f;
    [unroll]
    for (uint k = 0u; k < 16u; ++k)
    {
        const float t = dot(points[k] - mean, axis);
        tMin = min(tMin, t);
        tMax = max(tMax, t);
    }
    endpointA = clamp(mean + axis * tMin, 0.0f, 65535.0f);
    endpointB = clamp(mean + axis * tMax, 0.0f, 65535.0f);
}

// Least squares endpoints for fixed indices.
void Bc6hRefineEndpoints(float3 points[16], uint indices[16], inout float3 endpointA, inout float3 endpointB)
{
    float aa = 0.0f, ab = 0.0f, bb = 0.0f;
    float3 ax = 0.0f.xxx, bx = 0.0f.xxx;
    [unroll]
    for (uint i = 0u; i < 16u; ++i)
    {
        const float w = kBc6hWeights[indices[i]] / 64.0f;
        const float wa = 1.0f - w;
        aa += wa * wa;
        ab += wa * w;
        bb += w * w;
        ax += wa * points[i];
        bx += w * points[i];
    }
    const float determinant = aa * bb - ab * ab;
    if (abs(determinant) < 1e-6f) return;
    const float inverse = 1.0f / determinant;
    endpointA = clamp((ax * bb - bx * ab) * inverse, 0.0f, 65535.0f);
    endpointB = clamp((bx * aa - ax * ab) * inverse, 0.0f, 65535.0f);
}

struct Bc6hModeResult
{
    uint error;
    uint3 qA;
    uint3 qB; // Mode 11: endpoint B. Transformed modes: B - A, in tBits.
    uint indices[16];
};

// modeIndex 0..3 for modes 11..14.
Bc6hModeResult Bc6hEncodeMode(uint modeIndex, float3 points[16], uint3 halves[16], float3 fitA, float3 fitB)
{
    const uint prec = modeIndex == 0u ? 10u : modeIndex == 1u ? 11u : modeIndex == 2u ? 12u : 16u;
    const uint tBits = modeIndex == 0u ? 10u : modeIndex == 1u ? 9u : modeIndex == 2u ? 8u : 4u;
    const bool transformed = modeIndex != 0u;
    const int deltaMin = -(int)(1u << (tBits - 1u));
    const int deltaMax = (int)(1u << (tBits - 1u)) - 1;

    Bc6hModeResult result;
    result.error = 0xFFFFFFFFu;
    result.qA = 0u.xxx;
    result.qB = 0u.xxx;
    [unroll]
    for (uint init = 0u; init < 16u; ++init) result.indices[init] = 0u;

    float3 endpointA = fitA;
    float3 endpointB = fitB;
    uint3 qA = 0u.xxx;
    uint3 qB = 0u.xxx;
    uint indices[16];
    uint totalError = 0u;
    [loop]
    for (uint iteration = 0u; iteration < 3u; ++iteration)
    {
        qA = Bc6hQuantize(endpointA, prec);
        qB = Bc6hQuantize(endpointB, prec);
        if (transformed)
        {
            // B as close as the delta's range allows: between A and the fit, so it stays in [0, 2^prec).
            const int3 delta = clamp(int3(qB) - int3(qA), deltaMin.xxx, deltaMax.xxx);
            qB = uint3(int3(qA) + delta);
        }
        const uint3 unqA = uint3(Bc6hUnquantize(qA.x, prec), Bc6hUnquantize(qA.y, prec), Bc6hUnquantize(qA.z, prec));
        const uint3 unqB = uint3(Bc6hUnquantize(qB.x, prec), Bc6hUnquantize(qB.y, prec), Bc6hUnquantize(qB.z, prec));

        uint3 palette[16];
        [unroll]
        for (uint p = 0u; p < 16u; ++p) palette[p] = Bc6hPaletteHalf(unqA, unqB, kBc6hWeights[p]);

        totalError = 0u;
        [unroll]
        for (uint i = 0u; i < 16u; ++i)
        {
            uint bestIndex = 0u;
            uint bestError = 0xFFFFFFFFu;
            [unroll]
            for (uint candidate = 0u; candidate < 16u; ++candidate)
            {
                const uint error = Bc6hSquaredError(palette[candidate], halves[i]);
                if (error < bestError)
                {
                    bestError = error;
                    bestIndex = candidate;
                }
            }
            indices[i] = bestIndex;
            totalError += bestError;
        }
        if (iteration < 2u) Bc6hRefineEndpoints(points, indices, endpointA, endpointB);
    }

    // Texel 0's index must have its top bit clear: swap the endpoints and mirror the indices.
    if (indices[0] >= 8u)
    {
        const uint3 swapped = qA;
        qA = qB;
        qB = swapped;
        [unroll]
        for (uint i = 0u; i < 16u; ++i) indices[i] = 15u - indices[i];
    }
    if (transformed)
    {
        const int3 delta = int3(qB) - int3(qA);
        if (any(delta < deltaMin.xxx) || any(delta > deltaMax.xxx))
        {
            return result; // Only after a swap of a delta at the range's negative end: never chosen.
        }
        qB = uint3(delta) & ((1u << tBits) - 1u).xxx;
    }

    result.error = totalError;
    result.qA = qA;
    result.qB = qB;
    [unroll]
    for (uint i = 0u; i < 16u; ++i) result.indices[i] = indices[i];
    return result;
}

uint4 Bc6hPackBlock(uint modeIndex, Bc6hModeResult encoded)
{
    const uint prec = modeIndex == 0u ? 10u : modeIndex == 1u ? 11u : modeIndex == 2u ? 12u : 16u;
    const uint tBits = modeIndex == 0u ? 10u : modeIndex == 1u ? 9u : modeIndex == 2u ? 8u : 4u;
    const uint modeBits = modeIndex == 0u ? 0x03u : modeIndex == 1u ? 0x07u : modeIndex == 2u ? 0x0Bu : 0x0Fu;

    Bc6hBlock block = (Bc6hBlock)0;
    Bc6hWriteBits(block, 5u, modeBits);
    [unroll]
    for (uint c = 0u; c < 3u; ++c) Bc6hWriteBits(block, 10u, encoded.qA[c] & 0x3FFu);
    [unroll]
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        Bc6hWriteBits(block, tBits, encoded.qB[channel]);
        // A's bits above the low 10, highest first.
        for (uint k = 0u; k < 10u - tBits; ++k)
        {
            Bc6hWriteBits(block, 1u, (encoded.qA[channel] >> (prec - 1u - k)) & 1u);
        }
    }
    Bc6hWriteBits(block, 3u, encoded.indices[0]);
    [unroll]
    for (uint i = 1u; i < 16u; ++i) Bc6hWriteBits(block, 4u, encoded.indices[i]);
    return block.words;
}

[numthreads(8, 8, 1)]
void BC6HCompressCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2DArray<float4> srcTexture = ResourceDescriptorHeap[UintRootConstant0];
    RWByteAddressBuffer dstBuffer = ResourceDescriptorHeap[UintRootConstant1];

    const uint blocksX = (UintRootConstant4 + 3u) / 4u;
    const uint blocksY = (UintRootConstant5 + 3u) / 4u;
    if (dispatchThreadId.x >= blocksX || dispatchThreadId.y >= blocksY)
    {
        return;
    }

    const uint2 blockBase = dispatchThreadId.xy * 4u;
    const uint2 textureSize = uint2(UintRootConstant4, UintRootConstant5);
    uint3 halves[16];
    float3 points[16];
    [unroll]
    for (uint y = 0u; y < 4u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 4u; ++x)
        {
            const uint2 pixel = min(blockBase + uint2(x, y), textureSize - 1u.xx);
            const uint index = y * 4u + x;
            halves[index] = Bc6hHalfBits(srcTexture.Load(int4(pixel, 0, 0)).rgb);
            points[index] = Bc6hHalfToUnquantized(halves[index]);
        }
    }

    float3 fitA, fitB;
    Bc6hFitEndpoints(points, fitA, fitB);

    uint bestMode = 0u;
    Bc6hModeResult best = Bc6hEncodeMode(0u, points, halves, fitA, fitB);
    [loop]
    for (uint mode = 1u; mode < 4u; ++mode)
    {
        if (best.error == 0u) break;
        const Bc6hModeResult candidate = Bc6hEncodeMode(mode, points, halves, fitA, fitB);
        if (candidate.error < best.error)
        {
            best = candidate;
            bestMode = mode;
        }
    }

    const uint byteOffset = UintRootConstant2 + dispatchThreadId.y * UintRootConstant3 + dispatchThreadId.x * 16u;
    dstBuffer.Store4(byteOffset, Bc6hPackBlock(bestMode, best));
}
