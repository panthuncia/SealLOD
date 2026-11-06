#ifndef CLOD_OPACITY_MICROMAP_HLSLI
#define CLOD_OPACITY_MICROMAP_HLSLI

// Opacity micromap classification, for the preprocessor's bake (ClusterLOD/opacityMicromapBake.hlsl). One thread group
// of 64 classifies one triangle. A micro-triangle is known only when bilinear filtering can give no alpha on the other
// side of the cutoff anywhere in it: the exact range of the filtered alpha over the micro-triangle itself, per texel
// cell it crosses, widened for the hardware's filtering precision. So a known state is always the answer the path
// tracer's any-hit would give (clodRayTracingAlpha.hlsli); the rest are unknown, opaque or transparent as the test at
// their centre answers.
//
// Needs clodStructs.hlsli, clodPageAccess.hlsli and clodClusterFormat.hlsli for the page decode.

#include "include/clodAlphaCutoff.hlsli"

#define OMM_STATE_TRANSPARENT 0u
#define OMM_STATE_OPAQUE 1u
#define OMM_STATE_UNKNOWN_TRANSPARENT 2u
#define OMM_STATE_UNKNOWN_OPAQUE 3u

#define OMM_SPECIAL_FULLY_TRANSPARENT 0xFFFFFFFFu        // -1
#define OMM_SPECIAL_FULLY_OPAQUE 0xFFFFFFFEu             // -2
#define OMM_SPECIAL_FULLY_UNKNOWN_TRANSPARENT 0xFFFFFFFDu // -3
#define OMM_SPECIAL_FULLY_UNKNOWN_OPAQUE 0xFFFFFFFCu      // -4

// Texels one micro-triangle may read per texture before it is left to any-hit.
#define OMM_TEXEL_BUDGET 1024
// The highest subdivision level classified: g_ommStates holds its 4,096 micro-triangles.
#define CLOD_OMM_MAX_LEVEL 6u
// Margins for filtering precision: hardware texel coordinates have finite subtexel precision, and filtered values can
// round slightly past the texels they blend.
#define OMM_TEXEL_MARGIN (1.0f / 64.0f)
#define OMM_ALPHA_MARGIN (1.0f / 512.0f)

// From the Vulkan specification's opacity micromap reference code: barycentrics to the micro-triangle's index along
// the space-filling curve.
uint CLodRtOmmBarycentricsToIndex(float u, float v, uint level)
{
    u = saturate(u);
    v = saturate(v);
    const uint size = 1u << level;
    const float fu = u * float(size);
    const float fv = v * float(size);
    uint iu = uint(fu);
    uint iv = uint(fv);
    const float uf = fu - float(iu);
    const float vf = fv - float(iv);
    if (iu >= size) iu = size - 1u;
    if (iv >= size) iv = size - 1u;
    const uint iuv = iu + iv;
    if (iuv >= size) iu -= iuv - size + 1u;
    uint iw = ~(iu + iv);
    if (uf + vf >= 1.0f && iuv < size - 1u) --iw;

    uint b0 = ~(iu ^ iw);
    b0 &= size - 1u;
    const uint t = (iu ^ iv) & b0;
    uint f = t;
    f ^= f >> 1u;
    f ^= f >> 2u;
    f ^= f >> 4u;
    f ^= f >> 8u;
    uint b1 = ((f ^ iu) & ~b0) | t;

    b0 = (b0 | (b0 << 8u)) & 0x00ff00ffu;
    b0 = (b0 | (b0 << 4u)) & 0x0f0f0f0fu;
    b0 = (b0 | (b0 << 2u)) & 0x33333333u;
    b0 = (b0 | (b0 << 1u)) & 0x55555555u;
    b1 = (b1 | (b1 << 8u)) & 0x00ff00ffu;
    b1 = (b1 | (b1 << 4u)) & 0x0f0f0f0fu;
    b1 = (b1 | (b1 << 2u)) & 0x33333333u;
    b1 = (b1 | (b1 << 1u)) & 0x55555555u;
    return b0 | (b1 << 1u);
}

// Micro-triangleIndex k of a level's uniform grid, in rows of increasing v: upright and inverted alternate along a row.
// Returns its three corners as (u, v) barycentrics.
void CLodRtOmmMicroTriangle(uint k, uint level, out float2 a, out float2 b, out float2 c)
{
    const uint size = 1u << level;
    uint row = 0u;
    uint rowStart = 0u;
    while (row + 1u < size && k >= rowStart + 2u * (size - row) - 1u) {
        rowStart += 2u * (size - row) - 1u;
        ++row;
    }
    const uint r = k - rowStart;
    const uint column = r >> 1u;
    const float scale = 1.0f / float(size);
    if ((r & 1u) == 0u) {
        a = float2(column, row) * scale;
        b = float2(column + 1u, row) * scale;
        c = float2(column, row + 1u) * scale;
    }
    else {
        a = float2(column + 1u, row) * scale;
        b = float2(column, row + 1u) * scale;
        c = float2(column + 1u, row + 1u) * scale;
    }
}

struct CLodRtOmmTexture
{
    bool present;
    bool atTestMip;
    uint descriptor;
    uint samplerIndex;
    uint mip;
    uint width;
    uint height;
};

uint CLodRtOmmWrap(int x, uint size)
{
    return uint((x % int(size) + int(size)) % int(size));
}

// Bilinear alpha over one texel cell, in cell-local coordinates [0,1]^2 from its corner texels (x0y0, x1y0, x0y1, x1y1):
// f = k.x + k.y x + k.z y + k.w x y.
float4 CLodRtOmmBilinear(float4 corners)
{
    return float4(corners.x, corners.y - corners.x, corners.z - corners.x, corners.x - corners.y - corners.z + corners.w);
}

float CLodRtOmmBilinearAt(float4 k, float2 p)
{
    return k.x + k.y * p.x + k.z * p.y + k.w * p.x * p.y;
}

// The range of a bilinear function over a convex polygon: a bilinear function has no extremum inside a region (it's a
// saddle or linear), so its extremes are at the polygon's vertices or inside its edges, where it is quadratic.
void CLodRtOmmBilinearRange(float4 k, float2 polygon[8], uint count, inout float lo, inout float hi)
{
    for (uint i = 0u; i < count; ++i) {
        const float2 a = polygon[i];
        const float2 d = polygon[(i + 1u) % count] - a;
        const float fa = CLodRtOmmBilinearAt(k, a);
        lo = min(lo, fa);
        hi = max(hi, fa);
        // f(a + t d) = fa + t (k.y d.x + k.z d.y + k.w (a.x d.y + a.y d.x)) + t^2 k.w d.x d.y
        const float quadratic = k.w * d.x * d.y;
        if (abs(quadratic) > 1e-12f) {
            const float firstOrder = k.y * d.x + k.z * d.y + k.w * (a.x * d.y + a.y * d.x);
            const float t = -firstOrder / (2.0f * quadratic);
            if (t > 0.0f && t < 1.0f) {
                const float ft = CLodRtOmmBilinearAt(k, a + d * t);
                lo = min(lo, ft);
                hi = max(hi, ft);
            }
        }
    }
}

// Clips a convex polygon to the half-plane where (axis ? p.y : p.x) * sign <= limit * sign.
uint CLodRtOmmClip(inout float2 polygon[8], uint count, bool axis, float limit, float sign)
{
    float2 result[8];
    uint written = 0u;
    for (uint i = 0u; i < count; ++i) {
        const float2 a = polygon[i];
        const float2 b = polygon[(i + 1u) % count];
        const float da = ((axis ? a.y : a.x) - limit) * sign;
        const float db = ((axis ? b.y : b.x) - limit) * sign;
        if (da <= 0.0f && written < 8u) {
            result[written++] = a;
        }
        if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
            if (written < 8u) {
                result[written++] = a + (b - a) * (da / (da - db));
            }
        }
    }
    for (uint j = 0u; j < written; ++j) {
        polygon[j] = result[j];
    }
    return written;
}

// The range of alpha bilinear filtering can produce anywhere in the micro-triangle whose UVs are uvA, uvB, uvC, whatever
// the sampler's address mode: texels one past an edge are read both wrapped and clamped (mirroring reads the clamped
// one), and anything further out is left unknown. Hardware filtering places a sample within a fraction of a texel of
// where it's asked (subtexel weights are fixed point), so the range is widened by OMM_TEXEL_MARGIN times the steepest
// slope of any cell within that distance; it's also at most the range of every texel those cells blend.
void CLodRtOmmAlphaRange(CLodRtOmmTexture texture, float2 uvA, float2 uvB, float2 uvC, out float lo, out float hi)
{
    lo = 1.0f;
    hi = 1.0f;
    if (!texture.present) {
        return;
    }
    lo = 0.0f;
    const float2 size = float2(texture.width, texture.height);
    // Texel space: texel centres at integers, so cell (i, j) spans [i, i + 1] x [j, j + 1].
    const float2 pA = uvA * size - 0.5f;
    const float2 pB = uvB * size - 0.5f;
    const float2 pC = uvC * size - 0.5f;
    const float2 pMin = min(pA, min(pB, pC));
    const float2 pMax = max(pA, max(pB, pC));
    const int2 first = int2(floor(pMin - OMM_TEXEL_MARGIN));
    const int2 last = int2(floor(pMax + OMM_TEXEL_MARGIN)) + 1;
    if (any(first < -1) || last.x > int(texture.width) || last.y > int(texture.height)
        || (last.x - first.x + 1) * (last.y - first.y + 1) > OMM_TEXEL_BUDGET) {
        return;
    }
    Texture2D<float4> image = ResourceDescriptorHeap[NonUniformResourceIndex(texture.descriptor)];
    float exactLo = 1.0f;
    float exactHi = 0.0f;
    float boundLo = 1.0f;
    float boundHi = 0.0f;
    float slope = 0.0f;
    // Cells first..last - 1 on each axis; each reads texels (x, y) to (x + 1, y + 1).
    for (int y = first.y; y < last.y; ++y) {
        for (int x = first.x; x < last.x; ++x) {
            float4 wrapped, clamped;
            [unroll]
            for (uint corner = 0u; corner < 4u; ++corner) {
                const int cx = x + int(corner & 1u);
                const int cy = y + int(corner >> 1u);
                const uint2 w = uint2(CLodRtOmmWrap(cx, texture.width), CLodRtOmmWrap(cy, texture.height));
                const uint2 c = uint2(clamp(cx, 0, int(texture.width) - 1), clamp(cy, 0, int(texture.height) - 1));
                wrapped[corner] = image.Load(int3(w, texture.mip)).a;
                clamped[corner] = (all(w == c)) ? wrapped[corner] : image.Load(int3(c, texture.mip)).a;
            }
            boundLo = min(boundLo, min(min(wrapped.x, wrapped.y), min(wrapped.z, wrapped.w)));
            boundHi = max(boundHi, max(max(wrapped.x, wrapped.y), max(wrapped.z, wrapped.w)));
            boundLo = min(boundLo, min(min(clamped.x, clamped.y), min(clamped.z, clamped.w)));
            boundHi = max(boundHi, max(max(clamped.x, clamped.y), max(clamped.z, clamped.w)));
            const float4 kw = CLodRtOmmBilinear(wrapped);
            const float4 kc = CLodRtOmmBilinear(clamped);
            slope = max(slope, abs(kw.y) + abs(kw.z) + 2.0f * abs(kw.w));
            slope = max(slope, abs(kc.y) + abs(kc.z) + 2.0f * abs(kc.w));

            // The micro-triangle's part of this cell.
            float2 polygon[8];
            const float2 origin = float2(x, y);
            polygon[0] = pA - origin;
            polygon[1] = pB - origin;
            polygon[2] = pC - origin;
            uint count = 3u;
            count = CLodRtOmmClip(polygon, count, false, 0.0f, -1.0f);
            count = count >= 3u ? CLodRtOmmClip(polygon, count, false, 1.0f, 1.0f) : 0u;
            count = count >= 3u ? CLodRtOmmClip(polygon, count, true, 0.0f, -1.0f) : 0u;
            count = count >= 3u ? CLodRtOmmClip(polygon, count, true, 1.0f, 1.0f) : 0u;
            if (count >= 3u) {
                CLodRtOmmBilinearRange(kw, polygon, count, exactLo, exactHi);
                CLodRtOmmBilinearRange(kc, polygon, count, exactLo, exactHi);
            }
        }
    }
    // A sliver of a micro-triangle that no clip kept: fall back to the bound.
    if (exactLo > exactHi) {
        lo = boundLo;
        hi = boundHi;
        return;
    }
    const float widen = OMM_TEXEL_MARGIN * slope;
    lo = max(exactLo - widen, boundLo);
    hi = min(exactHi + widen, boundHi);
}

float CLodRtOmmSampleAlpha(CLodRtOmmTexture texture, float2 uv)
{
    if (!texture.present) {
        return 1.0f;
    }
    Texture2D<float4> image = ResourceDescriptorHeap[NonUniformResourceIndex(texture.descriptor)];
    SamplerState imageSampler = SamplerDescriptorHeap[NonUniformResourceIndex(texture.samplerIndex)];
    return image.SampleLevel(imageSampler, uv, float(texture.mip)).a;
}

groupshared uint g_ommStates[256]; // Up to CLOD_OMM_MAX_LEVEL: 4,096 micro-triangles at 2 bits.
groupshared uint g_ommSeen;       // One bit per state that occurs.
groupshared uint g_ommSlot;

// A triangle's corner UVs (set 0), decoded as any-hit decodes them: corner order is the index order the CLAS is built from.
void CLodOmmLoadTriangleUvs(uint slabDescriptor, uint pageByteOffset, CLodPageHeader header, uint localMeshlet,
    CLodMeshletDescriptor descriptor, uint triangleIndex, out float2 uv0, out float2 uv1, out float2 uv2)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptor)];
    const uint triangleAddress = pageByteOffset + header.triangleStreamOffset + descriptor.triangleByteOffset + triangleIndex * 3u;
    const uint word = slab.Load(triangleAddress & ~3u);
    const uint nextWord = slab.Load((triangleAddress & ~3u) + 4u);
    const uint shift = (triangleAddress & 3u) * 8u;
    const uint packedIndices = shift == 0u ? word : ((word >> shift) | (nextWord << (32u - shift)));
    const uint3 vertexIndices = uint3(packedIndices & 0xFFu, (packedIndices >> 8u) & 0xFFu, (packedIndices >> 16u) & 0xFFu);
    const CLodUvStream uvStream = CLodLoadUvStream(slabDescriptor, pageByteOffset, header.uvSetCount,
        header.uvDescriptorOffset, header.uvBitstreamDirectoryOffset, localMeshlet, 0u);
    uv0 = CLodDecodeUv(slab, uvStream, vertexIndices.x);
    uv1 = CLodDecodeUv(slab, uvStream, vertexIndices.y);
    uv2 = CLodDecodeUv(slab, uvStream, vertexIndices.z);
}

// Classifies one triangle's micro-triangles at level into g_ommStates (2 bits each, in bird-curve order) and the states
// that occur into g_ommSeen. Every lane of the group must call it. Returns this lane's transparent, opaque and unknown
// counts; g_ommStates and g_ommSeen are complete when it returns.
uint3 CLodOmmClassifyTriangle(uint lane, float2 uv0, float2 uv1, float2 uv2, uint level, CLodRtOmmTexture baseTexture,
    CLodRtOmmTexture opacityTexture, float factor, float cutoff)
{
    const uint microCount = 1u << (2u * level);
    const uint stateWords = max(microCount / 16u, 1u);
    for (uint i = lane; i < stateWords; i += 64u) {
        g_ommStates[i] = 0u;
    }
    if (lane == 0u) {
        g_ommSeen = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint seen = 0u;
    uint3 stateCounts = uint3(0u, 0u, 0u); // Transparent, opaque, unknown.
    for (uint k = lane; k < microCount; k += 64u) {
        float2 a, b, c;
        CLodRtOmmMicroTriangle(k, level, a, b, c);
        // Barycentrics (u, v) weight vertices 1 and 2, as hit barycentrics do.
        const float2 uvA = uv0 * (1.0f - a.x - a.y) + uv1 * a.x + uv2 * a.y;
        const float2 uvB = uv0 * (1.0f - b.x - b.y) + uv1 * b.x + uv2 * b.y;
        const float2 uvC = uv0 * (1.0f - c.x - c.y) + uv1 * c.x + uv2 * c.y;
        const float2 microCenter = (a + b + c) / 3.0f;
        float baseLo, baseHi, opacityLo, opacityHi;
        CLodRtOmmAlphaRange(baseTexture, uvA, uvB, uvC, baseLo, baseHi);
        CLodRtOmmAlphaRange(opacityTexture, uvA, uvB, uvC, opacityLo, opacityHi);
        const float alphaLo = factor * baseLo * opacityLo;
        const float alphaHi = factor * baseHi * opacityHi;

        uint state;
        if (alphaLo >= cutoff + OMM_ALPHA_MARGIN) {
            state = OMM_STATE_OPAQUE;
        }
        else if (alphaHi < cutoff - OMM_ALPHA_MARGIN) {
            state = OMM_STATE_TRANSPARENT;
        }
        else {
            // What 2-state mode answers: the test at the micro-triangle's center.
            const float2 uvCenter = uv0 * (1.0f - microCenter.x - microCenter.y) + uv1 * microCenter.x + uv2 * microCenter.y;
            const float alpha = factor * CLodRtOmmSampleAlpha(baseTexture, uvCenter) * CLodRtOmmSampleAlpha(opacityTexture, uvCenter);
            state = CLodRtAlphaIsOpaque(alpha, cutoff) ? OMM_STATE_UNKNOWN_OPAQUE : OMM_STATE_UNKNOWN_TRANSPARENT;
        }
        seen |= 1u << state;
        stateCounts += uint3(state == OMM_STATE_TRANSPARENT, state == OMM_STATE_OPAQUE, state >= OMM_STATE_UNKNOWN_TRANSPARENT);
        const uint index = CLodRtOmmBarycentricsToIndex(microCenter.x, microCenter.y, level);
        InterlockedOr(g_ommStates[index >> 4u], state << ((index & 15u) * 2u));
    }
    InterlockedOr(g_ommSeen, seen);
    GroupMemoryBarrierWithGroupSync();
    return stateCounts;
}

#endif // CLOD_OPACITY_MICROMAP_HLSLI
