#ifndef LENS_GHOST_HLSLI
#define LENS_GHOST_HLSLI

// Ghost flares: forward rays (scene to sensor) through a two-reflection path of the lens, as LensCore's GhostPath and
// TraceSequential (the CPU reference; GhostFlareManager's self-test compares them). It reuses lensExplicit.hlsli's surface,
// intersection and Fresnel code; the camera rays' LensTraceBackward is untouched.
//
// Nothing is clipped: the trace records how far outside the stop and the clear apertures the ray went (as ratios), so the
// draw can cut the aperture's shape inside a grid cell (Hullin et al. 2011). Only a missed surface, total internal
// reflection or an exit that doesn't reach the sensor fails.

#include "include/lens/lensExplicit.hlsli"
#include "include/lens/lensGhostLayout.h"

struct LensGhostHeader
{
    bool valid;
    uint ghostCount;
    uint fieldNodes;
    uint lensRevision;
    float maxField;
    float fieldStep;
};

LensGhostHeader LensGhostLoadHeader(StructuredBuffer<float4> ghosts)
{
    const uint4 row0 = asuint(ghosts[0]);
    const float4 row1 = ghosts[1];
    LensGhostHeader header;
    header.valid = row0.x == LENS_GHOST_MAGIC && row0.y > 0u && row0.y <= LENS_GHOST_MAX_GHOSTS && row0.z >= 2u;
    header.ghostCount = row0.y;
    header.fieldNodes = row0.z;
    header.lensRevision = row0.w;
    header.maxField = row1.x;
    header.fieldStep = row1.y;
    return header;
}

// The ghost's reflecting surfaces (a then b < a) and its box on the front plane at a field angle, linearly between nodes.
// False when no ray of the ghost reaches the sensor at that angle.
bool LensGhostLoad(StructuredBuffer<float4> ghosts, LensGhostHeader header, uint ghost, float field, out uint a, out uint b, out float3 box)
{
    const uint row = LENS_GHOST_HEADER_ROWS + ghost * LENS_GHOST_ROWS_PER_GHOST(header.fieldNodes);
    const uint2 surfaces = asuint(ghosts[row].xy);
    a = surfaces.x;
    b = surfaces.y;
    const float scaled = clamp(field / max(header.fieldStep, 1e-6f), 0.0f, float(header.fieldNodes - 1u));
    const uint node = min(uint(scaled), header.fieldNodes - 2u);
    const float t = scaled - float(node);
    const float3 box0 = ghosts[row + 1u + node].xyz;
    const float3 box1 = ghosts[row + 2u + node].xyz;
    const bool empty0 = box0.x > box0.y;
    const bool empty1 = box1.x > box1.y;
    if (empty0 && empty1) return false;
    // A node without rays takes the other's box: the light fades by the ratios, not by the box.
    box = lerp(empty0 ? box1 : box0, empty1 ? box0 : box1, t);
    return box.x <= box.y && box.z > 0.0f;
}

// Step `index` of GhostPath(a, b): surfaces 0..a-1 forward, reflect at a, a-1..b+1 backward, reflect at b, b+1..N-1 forward.
// 2 (a - b) + N steps in all.
uint LensGhostStepSurface(uint index, uint a, uint b, out bool reflect)
{
    reflect = false;
    if (index < a) return index;
    if (index == a) {
        reflect = true;
        return a;
    }
    const uint back = index - a; // 1 .. a - b
    if (back < a - b) return a - back;
    if (back == a - b) {
        reflect = true;
        return b;
    }
    return b + (back - (a - b));
}

// Traces a forward ray from in front of the lens along GhostPath(a, b) at lambdaNm. On LENS_STATUS_OK, origin and direction
// are the ray leaving the last surface; throughput is the product of the Fresnel terms (T at transmissions, R at the two
// reflections, with the quarter-wave coatings), stopRatio the largest stop crossing radius over the scaled stop radius and
// rimRatio the largest hit radius over the clear radius at any other surface.
uint LensTraceGhost(StructuredBuffer<float4> data, LensHeader header, uint a, uint b, inout float3 origin, inout float3 direction, float lambdaNm,
    out float throughput, out float stopRatio, out float rimRatio)
{
    throughput = 1.0f;
    stopRatio = 0.0f;
    rimRatio = 0.0f;
    direction = normalize(direction);
    const uint steps = 2u * (a - b) + header.surfaceCount;
    for (uint step = 0u; step < steps; ++step) {
        bool reflect;
        const uint index = LensGhostStepSurface(step, a, b, reflect);
        const LensSurface surface = LensLoadSurface(data, header, index);
        float3 hit, normal;
        if (!LensIntersectSurface(surface, origin, direction, hit, normal)) return LENS_STATUS_MISSED;
        const float radius = length(hit.xy);
        if (surface.isStop) {
            stopRatio = max(stopRatio, radius / max(surface.semiAperture * header.stopScale, 1e-6f));
            origin = hit;
            continue;
        }
        rimRatio = max(rimRatio, radius / max(surface.semiAperture, 1e-6f));
        const bool forward = direction.z > 0.0f;
        const float nBefore = LensMediumIndex(data, header, surface.mediumBefore, lambdaNm);
        const float nAfter = LensMediumIndex(data, header, surface.mediumAfter, lambdaNm);
        const float n1 = forward ? nBefore : nAfter;
        const float n2 = forward ? nAfter : nBefore;
        if (dot(normal, direction) > 0.0f) normal = -normal;
        const float cosI = -dot(normal, direction);
        const float reflectance = LensFresnelReflectance(n1, n2, cosI, surface.coated, surface.coatingIndex, surface.coatingWavelength, lambdaNm);
        if (reflect) {
            throughput *= reflectance;
            direction = normalize(direction + normal * (2.0f * cosI));
        }
        else {
            const float eta = n1 / n2;
            const float k = 1.0f - eta * eta * (1.0f - cosI * cosI);
            if (k < 0.0f) return LENS_STATUS_TIR;
            throughput *= 1.0f - reflectance;
            direction = normalize(direction * eta + normal * (eta * cosI - sqrt(k)));
        }
        origin = hit;
    }
    return LENS_STATUS_OK;
}

#endif // LENS_GHOST_HLSLI
