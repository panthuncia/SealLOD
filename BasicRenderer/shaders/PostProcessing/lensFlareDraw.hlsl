// Ghost flares, stage 3 (GhostFlareManager): every lattice cell the trace wrote becomes a quad, drawn additively into the
// flare target. A cell carries the energy its entrance area intercepts (source irradiance times the mean Fresnel throughput
// and cosine) and spreads it over its sensor area, so its radiance is that energy over the sensor area, divided by the lens's
// K as the path tracer's camera weight is: the same units as the traced image (Hullin et al. 2011's intensity, through the
// Jacobian). Each vertex takes the radiance of the cells around it together (their energy over their area), interpolated
// across the cell, so the ghost is smooth rather than faceted. A cell smaller than a target pixel divides by the pixel
// instead (no fireflies where a ghost focuses). The stop and rim ratios interpolate across the cell and cut the aperture's
// shape inside it, with an edge one pixel wide.
#include "include/cbuffers.hlsli"
#include "include/lens/lensGhostLayout.h"

struct LensFlareVertexOutput
{
    float4 position : SV_Position;
    float3 radiance : TEXCOORD0;
    float2 ratios : TEXCOORD1; // stop, rim
};

static const uint2 kCorner[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1) };

struct LensFlareLatticeVertex
{
    bool valid;
    float2 sensor;
    float throughput; // times the cosine
    float2 ratios;
};

LensFlareLatticeVertex LensFlareLoadVertex(StructuredBuffer<float4> vertices, uint instanceRow, uint side, int2 at)
{
    LensFlareLatticeVertex vertex = (LensFlareLatticeVertex)0;
    if (any(at < 0) || any(at >= int(side))) return vertex;
    const float4 v = vertices[instanceRow + LENS_FLARE_INSTANCE_HEADER_ROWS + (uint(at.y) * side + uint(at.x)) * LENS_FLARE_VERTEX_ROWS];
    const uint packed = asuint(v.w);
    vertex.valid = packed != LENS_FLARE_VERTEX_FAILED;
    vertex.sensor = v.xy;
    vertex.throughput = v.z;
    vertex.ratios = float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16u));
    return vertex;
}

// A cell's sensor area (its corners 0, 1, 3, 2 go round it; the shoelace formula, so a folded cell counts its net area) and
// the energy its entrance area carries per unit irradiance. False when a corner failed.
bool LensFlareCell(LensFlareLatticeVertex c00, LensFlareLatticeVertex c10, LensFlareLatticeVertex c01, LensFlareLatticeVertex c11, float entranceArea,
    out float sensorArea, out float energy)
{
    sensorArea = 0.0f;
    energy = 0.0f;
    if (!(c00.valid && c10.valid && c01.valid && c11.valid)) return false;
    const float2 p0 = c00.sensor, p1 = c10.sensor, p2 = c11.sensor, p3 = c01.sensor;
    sensorArea = 0.5f * abs((p0.x * p1.y - p1.x * p0.y) + (p1.x * p2.y - p2.x * p1.y) + (p2.x * p3.y - p3.x * p2.y) + (p3.x * p0.y - p0.x * p3.y));
    energy = 0.25f * (c00.throughput + c10.throughput + c01.throughput + c11.throughput) * entranceArea;
    return true;
}

LensFlareVertexOutput LensFlareVS(uint vertexId : SV_VertexID, uint instance : SV_InstanceID)
{
    LensFlareVertexOutput output = (LensFlareVertexOutput)0;
    output.position = float4(2.0f, 2.0f, 2.0f, 1.0f); // outside the clip volume: the triangle is dropped

    StructuredBuffer<float4> vertices = ResourceDescriptorHeap[LENS_FLARE_RC_VERTICES];
    StructuredBuffer<float4> sources = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::LensFlare::Sources)];
    const uint grid = LENS_FLARE_RC_GRID;
    const uint side = grid + 1u;
    const uint instanceRow = instance * LENS_FLARE_INSTANCE_ROWS(grid);
    const float4 instanceHeader = vertices[instanceRow];
    if (asuint(instanceHeader.z) == 0u) return output;
    const uint cellIndex = vertexId / 6u;
    if (cellIndex >= grid * grid) return output;
    const int2 cell = int2(cellIndex % grid, cellIndex / grid);

    // The 3x3 lattice vertices around this triangle vertex: the four cells that share it.
    const int2 mine = int2(kCorner[vertexId % 6u]);
    const int2 at = cell + mine;
    LensFlareLatticeVertex v[3][3];
    [unroll]
    for (int y = 0; y < 3; ++y) {
        [unroll]
        for (int x = 0; x < 3; ++x) v[y][x] = LensFlareLoadVertex(vertices, instanceRow, side, at + int2(x - 1, y - 1));
    }
    // This triangle's own cell: dropped if a corner failed, or if every corner is outside an aperture (the lens blocks all of it).
    const int2 own = int2(1, 1) - mine; // the cell's lower corner in the 3x3 block
    const LensFlareLatticeVertex o00 = v[own.y][own.x], o10 = v[own.y][own.x + 1], o01 = v[own.y + 1][own.x], o11 = v[own.y + 1][own.x + 1];
    if (!(o00.valid && o10.valid && o01.valid && o11.valid)) return output;
    const float nearest = min(min(max(o00.ratios.x, o00.ratios.y), max(o10.ratios.x, o10.ratios.y)),
        min(max(o01.ratios.x, o01.ratios.y), max(o11.ratios.x, o11.ratios.y)));
    if (nearest > 1.0f) return output;

    // The vertex's radiance: the energy of the valid cells around it over their sensor area (each at least a pixel), so the
    // radiance is smooth across cells and still conserves energy cell by cell.
    const float4 frame0 = sources[LENS_FLARE_ROW_FRAME0];
    const float4 frame2 = sources[LENS_FLARE_ROW_FRAME2];
    float energy = 0.0f, area = 0.0f;
    [unroll]
    for (int cy = 0; cy < 2; ++cy) {
        [unroll]
        for (int cx = 0; cx < 2; ++cx) {
            float cellArea, cellEnergy;
            if (LensFlareCell(v[cy][cx], v[cy][cx + 1], v[cy + 1][cx], v[cy + 1][cx + 1], instanceHeader.x, cellArea, cellEnergy)) {
                energy += cellEnergy;
                area += max(cellArea, frame2.z);
            }
        }
    }
    const uint wavelengthSlots = max(LENS_FLARE_RC_WAVELENGTHS, 1u);
    const uint wavelength = instance % wavelengthSlots;
    const uint slot = instance / (wavelengthSlots * LENS_FLARE_RC_GHOSTS);
    const float3 sourceIrradiance = sources[LENS_FLARE_ROW_SOURCES + slot * LENS_FLARE_SOURCE_ROWS + 1u].rgb;
    const float3 weight = sources[LENS_FLARE_ROW_WAVELENGTHS + wavelength].xyz;
    output.radiance = sourceIrradiance * weight * (energy / max(area, frame2.z) * frame0.w);

    // The lens inverts the image; NDC is the sensor point over the half extent, negated (lensCamera.hlsli's inverse).
    output.position = float4(-v[1][1].sensor / frame0.xy, 0.0f, 1.0f);
    output.ratios = v[1][1].ratios;
    return output;
}

float4 LensFlarePS(LensFlareVertexOutput input) : SV_Target
{
    // 1 inside both apertures, falling to 0 over about a pixel at the edge.
    const float2 width = max(fwidth(input.ratios), 1e-4f.xx);
    const float2 inside = saturate((1.0f - input.ratios) / width + 0.5f);
    return float4(input.radiance * (inside.x * inside.y), 0.0f);
}
