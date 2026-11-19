#ifndef CLOD_UV_REMAP_HLSLI
#define CLOD_UV_REMAP_HLSLI

// UV remap blocks (stage 6 step 2, docs/clod-appearance-stage6.md): a coarse cluster whose triangles cross UV charts carries, after
// its page's triangle stream, source UVs on a barycentric grid of each damaged triangle; its UV set 0 descriptor's reserved0 is the
// block's page-relative byte offset (0: none). Material evaluation reads UV set 0 through it, so the material's own textures are
// sampled where the original surface had them. The layout and the reference evaluation are ClusterLODUvRemap.h's; this follows it.
// One block layout (clod_uv_remap::kVersion) holds two kinds of triangle entry: grid triangles, and region triangles (entry bit 31,
// written by remap layout 2 builds): per cell of the grid a line between two chart regions, each with an exact affine map from the
// stored corners' barycentrics to source UV. The kind is per entry, so nothing here depends on the block's version.

#ifndef CLOD_UV_REMAP
#define CLOD_UV_REMAP 1
#endif

static const uint CLOD_UV_REMAP_HEADER_WORDS = 8u;
static const uint CLOD_UV_REMAP_UNKNOWN_TRIANGLE = 0xFFFFFFFFu;
static const uint CLOD_UV_REMAP_SWAPPED_CORNERS = 0x80000000u;
static const uint CLOD_UV_REMAP_REGION_ENTRY = 0x80000000u;
static const uint CLOD_UV_REMAP_REGION_WORDS = 6u;
static const uint CLOD_UV_REMAP_REGION_HALF_CELLS = 0x100u;
static const float CLOD_UV_REMAP_CELL_OFFSET_RANGE = 0.75f;

// The triangle material evaluation shades, set by the resolve before it evaluates: its meshlet-local index, with
// CLOD_UV_REMAP_SWAPPED_CORNERS when the resolve swapped corners 1 and 2 (a back face), so the barycentrics are in swapped order.
// CLOD_UV_REMAP_UNKNOWN_TRIANGLE: no remap (paths that do not set it).
static uint g_clodUvRemapTriangle = CLOD_UV_REMAP_UNKNOWN_TRIANGLE;

uint CLodUvRemapNodeCount(uint n)
{
    return n * (n + 1u) / 2u;
}

uint CLodUvRemapNodeIndex(uint n, uint i, uint j)
{
    return j * n - (j * (j - 1u)) / 2u + i; // j = 0: 0 * (0 - 1) wraps to 0
}

uint CLodUvRemapSubTriangleIndex(uint n, uint i, uint j, bool upper)
{
    return j * (2u * n - 2u - j) + 2u * i + (upper ? 1u : 0u);
}

uint CLodUvRemapMaskWords(uint n)
{
    return ((n - 1u) * (n - 1u) + 31u) / 32u;
}

float2 CLodUvRemapLoadNode(ByteAddressBuffer slab, uint first, uint index, float2 uvMin, float2 uvScale)
{
    const uint word = slab.Load(first + index * 4u);
    return uvMin + float2(word & 0xFFFFu, word >> 16u) * uvScale;
}

// A region triangle's cell: region A (6 bits) | B (6 bits) << 6 | angle (8 bits) << 12 | offset (12 bits) << 20; B on the positive
// side of the cell's line at (fx, fy), its own grid units (clod_uv_remap::CellRegion).
uint CLodUvRemapCellRegion(uint cell, float fx, float fy)
{
    const uint a = cell & 63u;
    const uint b = (cell >> 6u) & 63u;
    const float theta = float((cell >> 12u) & 255u) * (6.28318530718f / 256.0f);
    const float d = (float(cell >> 20u) * (2.0f / 4095.0f) - 1.0f) * CLOD_UV_REMAP_CELL_OFFSET_RANGE;
    float s, c;
    sincos(theta, s, c);
    const float distance = (fx - 0.5f) * c + (fy - 0.5f) * s - d;
    return (a == b || !(distance > 0.0f)) ? a : b;
}

struct CLodUvRemapSample
{
    bool remapped;
    float2 uv;
    float2 dUvDb1; // d(uv) / d(b1) and d(uv) / d(b2) of the stored corners' barycentrics
    float2 dUvDb2;
};

// blockAddress: the block's byte address in the slab. b1, b2: barycentrics of the triangle's stored corners 1 and 2.
CLodUvRemapSample CLodEvaluateUvRemap(ByteAddressBuffer slab, uint blockAddress, uint triangleIndex, float b1, float b2)
{
    CLodUvRemapSample result = (CLodUvRemapSample)0;
    const uint4 header0 = slab.Load4(blockAddress);        // magic, triangle count | version << 16, uvMin
    const uint4 header1 = slab.Load4(blockAddress + 16u);  // uvScale, uvStep, byte size
    if (triangleIndex >= (header0.y & 0xFFFFu))
    {
        return result;
    }
    const uint entry = slab.Load(blockAddress + (CLOD_UV_REMAP_HEADER_WORDS + triangleIndex) * 4u);
    const bool regionTriangle = (entry & CLOD_UV_REMAP_REGION_ENTRY) != 0u;
    const uint n = (entry >> 24u) & 0x7Fu;
    if (n < 2u)
    {
        return result;
    }
    const uint first = blockAddress + (entry & 0xFFFFFFu) * 4u;
    const float2 uvMin = asfloat(header0.zw);
    const float2 uvScale = asfloat(header1.xy);
    const float uvStep = asfloat(header1.z);

    const float span = float(n - 1u);
    const float x = saturate(b1) * span;
    const float y = saturate(b2) * span;
    const int j = clamp(int(floor(y)), 0, int(n) - 2);
    const int i = clamp(int(floor(x)), 0, int(n) - 2 - j);
    const float fx = x - float(i);
    const float fy = y - float(j);
    const bool upper = (i + j <= int(n) - 3) && (fx + fy > 1.0f);
    const uint ui = uint(i);
    const uint uj = uint(j);
    uint a, b, c;
    float wa, wb, wc;
    uint2 gridA, gridB, gridC; // the nodes' (i, j)
    if (!upper)
    {
        a = CLodUvRemapNodeIndex(n, ui, uj);
        b = CLodUvRemapNodeIndex(n, ui + 1u, uj);
        c = CLodUvRemapNodeIndex(n, ui, uj + 1u);
        gridA = uint2(ui, uj);
        gridB = uint2(ui + 1u, uj);
        gridC = uint2(ui, uj + 1u);
        wa = 1.0f - fx - fy;
        wb = fx;
        wc = fy;
    }
    else
    {
        a = CLodUvRemapNodeIndex(n, ui + 1u, uj + 1u);
        b = CLodUvRemapNodeIndex(n, ui, uj + 1u);
        c = CLodUvRemapNodeIndex(n, ui + 1u, uj);
        gridA = uint2(ui + 1u, uj + 1u);
        gridB = uint2(ui, uj + 1u);
        gridC = uint2(ui + 1u, uj);
        wa = fx + fy - 1.0f;
        wb = 1.0f - fx;
        wc = 1.0f - fy;
    }
    const uint subTriangle = CLodUvRemapSubTriangleIndex(n, ui, uj, upper);
    if (regionTriangle)
    {
        // The cell's region, then its exact map at the unclamped barycentrics: two to four loads.
        const uint word0 = slab.Load(first);
        const uint regions = word0 & 0xFFu;
        uint region = 0u;
        if (regions > 1u)
        {
            const uint cells = first + (1u + regions * CLOD_UV_REMAP_REGION_WORDS) * 4u;
            uint cell;
            if ((word0 & CLOD_UV_REMAP_REGION_HALF_CELLS) != 0u)
            {
                // Half cells (ClusterLODUvRemap.h ExpandHalfCell): A | B << 4 | angle / 16 << 8 | offset / 273 << 12, two per word.
                const uint half = (slab.Load(cells + (subTriangle / 2u) * 4u) >> (16u * (subTriangle & 1u))) & 0xFFFFu;
                cell = (half & 15u) | (((half >> 4u) & 15u) << 6u) | ((((half >> 8u) & 15u) * 16u) << 12u) | ((((half >> 12u) & 15u) * 273u) << 20u);
            }
            else
            {
                cell = slab.Load(cells + subTriangle * 4u);
            }
            region = CLodUvRemapCellRegion(cell, fx, fy);
        }
        const uint record = first + (1u + region * CLOD_UV_REMAP_REGION_WORDS) * 4u;
        const float4 map0 = asfloat(slab.Load4(record));       // origin, dUv/db1
        const float2 map1 = asfloat(slab.Load2(record + 16u)); // dUv/db2
        result.remapped = true;
        result.dUvDb1 = map0.zw;
        result.dUvDb2 = map1;
        result.uv = map0.xy + b1 * map0.zw + b2 * map1;
        return result;
    }
    const bool interpolate = ((slab.Load(first + (CLodUvRemapNodeCount(n) + subTriangle / 32u) * 4u) >> (subTriangle & 31u)) & 1u) != 0u;

    const float2 uvA = CLodUvRemapLoadNode(slab, first, a, uvMin, uvScale);
    const float2 uvB = CLodUvRemapLoadNode(slab, first, b, uvMin, uvScale);
    const float2 uvC = CLodUvRemapLoadNode(slab, first, c, uvMin, uvScale);

    result.remapped = true;
    if (interpolate)
    {
        result.uv = wa * uvA + wb * uvB + wc * uvC;
        // Grid steps along +i and +j: lower (b - a, c - a); upper (a - b, a - c).
        result.dUvDb1 = (upper ? uvA - uvB : uvB - uvA) * span;
        result.dUvDb2 = (upper ? uvA - uvC : uvC - uvA) * span;
    }
    else
    {
        // The sub-triangle straddles a chart boundary: from the node of largest weight along its own chart's grid steps (its 4-bit step
        // code, after the mask); an axis without one keeps the node's UV along it, with a footprint of one grid step.
        const bool nearA = wa >= wb && wa >= wc;
        const bool nearB = !nearA && wb >= wc;
        const uint nearest = nearA ? a : (nearB ? b : c);
        const uint2 grid = nearA ? gridA : (nearB ? gridB : gridC);
        const float2 uvNearest = nearA ? uvA : (nearB ? uvB : uvC);
        const uint codeWord = slab.Load(first + (CLodUvRemapNodeCount(n) + CLodUvRemapMaskWords(n) + nearest / 8u) * 4u);
        const uint code = (codeWord >> (4u * (nearest & 7u))) & 0xFu;
        float2 gradientI = 0.0f.xx;
        float2 gradientJ = 0.0f.xx;
        const uint stepI = code & 3u;
        const uint stepJ = (code >> 2u) & 3u;
        const int signI = stepI == 1u ? 1 : -1;
        const int signJ = stepJ == 1u ? 1 : -1;
        const int2 neighbourI = int2(grid) + int2(signI, 0);
        const int2 neighbourJ = int2(grid) + int2(0, signJ);
        const bool hasI = (stepI == 1u || stepI == 2u) && neighbourI.x >= 0 && neighbourI.x + neighbourI.y <= int(n) - 1;
        const bool hasJ = (stepJ == 1u || stepJ == 2u) && neighbourJ.y >= 0 && neighbourJ.x + neighbourJ.y <= int(n) - 1;
        if (hasI)
        {
            gradientI = (CLodUvRemapLoadNode(slab, first, CLodUvRemapNodeIndex(n, uint(neighbourI.x), uint(neighbourI.y)), uvMin, uvScale) - uvNearest) * float(signI);
        }
        if (hasJ)
        {
            gradientJ = (CLodUvRemapLoadNode(slab, first, CLodUvRemapNodeIndex(n, uint(neighbourJ.x), uint(neighbourJ.y)), uvMin, uvScale) - uvNearest) * float(signJ);
        }
        const float2 offset = float2(x, y) - float2(grid);
        result.uv = uvNearest + gradientI * offset.x + gradientJ * offset.y;
        result.dUvDb1 = hasI ? gradientI * span : float2(uvStep * span, 0.0f);
        result.dUvDb2 = hasJ ? gradientJ * span : float2(0.0f, uvStep * span);
    }
    return result;
}

#endif // CLOD_UV_REMAP_HLSLI
