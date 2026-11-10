// Ghost flares, stage 2 (GhostFlareManager): per source, ghost and wavelength (the dispatch's y), a regular lattice of
// (grid + 1)^2 rays over the ghost's box on the front plane, traced forward along the ghost's path to the focused sensor
// plane (lensGhost.hlsli). A parallel source's rays are parallel; a point source's diverge from it. Writes each vertex's
// sensor point, Fresnel throughput (times the cosine) and its stop and rim ratios, or that it failed (lensGhostLayout.h).
#include "include/cbuffers.hlsli"
#include "include/lens/lensGhost.hlsli"

[numthreads(LENS_FLARE_TRACE_GROUP, 1, 1)]
void LensFlareTraceCS(uint3 dispatchThread : SV_DispatchThreadID)
{
    RWStructuredBuffer<float4> vertices = ResourceDescriptorHeap[LENS_FLARE_RC_VERTICES];
    StructuredBuffer<float4> sources = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::LensFlare::Sources)];
    StructuredBuffer<float4> ghosts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::LensFlare::Ghosts)];
    StructuredBuffer<float4> data = LensData();

    const uint4 frame1 = asuint(sources[LENS_FLARE_ROW_FRAME1]);
    const uint wavelengths = frame1.y;
    const uint grid = LENS_FLARE_RC_GRID;
    const uint ghostSlots = LENS_FLARE_RC_GHOSTS;
    const uint instance = dispatchThread.y;
    const uint wavelength = instance % max(LENS_FLARE_RC_WAVELENGTHS, 1u);
    const uint ghost = (instance / max(LENS_FLARE_RC_WAVELENGTHS, 1u)) % ghostSlots;
    const uint slot = instance / (max(LENS_FLARE_RC_WAVELENGTHS, 1u) * ghostSlots);
    const uint instanceRow = instance * LENS_FLARE_INSTANCE_ROWS(grid);
    const uint side = grid + 1u;
    const uint vertex = dispatchThread.x;

    const LensHeader header = LensLoadHeader(data);
    const LensGhostHeader ghostHeader = LensGhostLoadHeader(ghosts);
    const uint4 sourceFlags = asuint(sources[LENS_FLARE_ROW_SOURCES + slot * LENS_FLARE_SOURCE_ROWS + 2u]);
    bool valid = header.valid && ghostHeader.valid && slot < frame1.x && ghost < ghostHeader.ghostCount && wavelength < wavelengths
        && sourceFlags.x != 0u;
    uint a = 0u, b = 0u;
    float3 box = 0.0f.xxx;
    const float4 sourceRow = sources[LENS_FLARE_ROW_SOURCES + slot * LENS_FLARE_SOURCE_ROWS];
    const float4 extra = asfloat(sourceFlags);
    const float2 azimuth = extra.yz;
    if (valid) valid = LensGhostLoad(ghosts, ghostHeader, ghost, extra.w, a, b, box);
    const float lambdaNm = sources[LENS_FLARE_ROW_WAVELENGTHS + wavelength].w;
    const float cell = 1.0f / float(grid);
    if (vertex == 0u) {
        const float area = (box.y - box.x) * (2.0f * box.z) * cell * cell;
        vertices[instanceRow] = float4(area, lambdaNm, asfloat(valid ? 1u : 0u), 0.0f);
    }
    if (!valid || vertex >= side * side) return;

    // The lattice point in the canonical frame (the light travels toward +x), rotated to the source's azimuth.
    const uint column = vertex % side;
    const uint row = vertex / side;
    const float2 canonical = float2(lerp(box.x, box.y, float(column) * cell), lerp(-box.z, box.z, float(row) * cell));
    const float2 onFront = float2(azimuth.x * canonical.x - azimuth.y * canonical.y, azimuth.y * canonical.x + azimuth.x * canonical.y);
    const float3 frontPoint = float3(onFront, header.frontPlaneZ);
    float3 direction = sourceRow.w == LENS_FLARE_SOURCE_POINT ? normalize(frontPoint - sourceRow.xyz) : normalize(sourceRow.xyz);
    // Start 1 mm in front of the front plane along the ray, as LensCore's RankGhostPaths.
    float3 origin = frontPoint - direction * (1.0f / max(direction.z, 1e-4f));
    const float cosine = direction.z;

    float throughput, stopRatio, rimRatio;
    uint status = LensTraceGhost(data, header, a, b, origin, direction, lambdaNm, throughput, stopRatio, rimRatio);
    const float sensorZ = sources[LENS_FLARE_ROW_FRAME0].z;
    float2 sensor = 0.0f.xx;
    if (status == LENS_STATUS_OK) {
        if (direction.z > 1e-6f && origin.z <= sensorZ) sensor = origin.xy + direction.xy * ((sensorZ - origin.z) / direction.z);
        else status = LENS_FLARE_STATUS_BEHIND;
    }
    const uint outRow = instanceRow + LENS_FLARE_INSTANCE_HEADER_ROWS + vertex * LENS_FLARE_VERTEX_ROWS;
    const uint ratios = f32tof16(min(stopRatio, 60000.0f)) | (f32tof16(min(rimRatio, 60000.0f)) << 16u);
    vertices[outRow] = status == LENS_STATUS_OK ? float4(sensor, throughput * cosine, asfloat(ratios)) : float4(0.0f, 0.0f, 0.0f, asfloat(LENS_FLARE_VERTEX_FAILED));
}
