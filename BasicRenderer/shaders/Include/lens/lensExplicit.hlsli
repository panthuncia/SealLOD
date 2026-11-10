#ifndef LENS_EXPLICIT_HLSLI
#define LENS_EXPLICIT_HLSLI

// Explicit, surface-by-surface tracing of camera rays backward through the lens (sensor to scene), in float32: a port of
// LensCore's IntersectSurface, FresnelReflectance and TraceSequential on the transmission path. lens.selftest compares
// the two (lensSelfTest.hlsl).

#include "include/lens/lensCommon.hlsli"

#define LENS_HIT_EPSILON 1.0e-5f

struct LensSurface
{
    float curvature;
    float conic;
    float vertexZ;
    float semiAperture;
    bool isStop;
    uint mediumBefore;
    uint mediumAfter;
    bool coated;
    float coatingIndex;
    float coatingWavelength;
    uint asphericCount;
    float4 aspheric;
};

LensSurface LensLoadSurface(StructuredBuffer<float4> data, LensHeader header, uint index)
{
    const uint row = header.surfaceOffset + index * LENS_SURFACE_ROWS;
    const float4 s0 = data[row];
    const uint4 s1 = asuint(data[row + 1u]);
    const float4 s2 = data[row + 2u];
    LensSurface surface;
    surface.curvature = s0.x;
    surface.conic = s0.y;
    surface.vertexZ = s0.z;
    surface.semiAperture = s0.w;
    surface.isStop = s1.x != 0u;
    surface.mediumBefore = s1.y;
    surface.mediumAfter = s1.z;
    surface.coated = s1.w != 0u;
    surface.coatingIndex = s2.x;
    surface.coatingWavelength = s2.y;
    surface.asphericCount = asuint(s2.z);
    surface.aspheric = data[row + 3u];
    return surface;
}

// Sag and its slope dz/dr; NaN-free: valid is false outside the conic's domain.
float LensSag(LensSurface surface, float r, out bool valid)
{
    const float c = surface.curvature;
    const float r2 = r * r;
    const float root = 1.0f - (1.0f + surface.conic) * c * c * r2;
    valid = root >= 0.0f;
    float z = c * r2 / (1.0f + sqrt(max(root, 0.0f)));
    float power = r2 * r2;
    for (uint i = 0u; i < surface.asphericCount; ++i) {
        z += surface.aspheric[i] * power;
        power *= r2;
    }
    return z;
}

float LensSagSlope(LensSurface surface, float r, out bool valid)
{
    const float c = surface.curvature;
    const float r2 = r * r;
    const float root = 1.0f - (1.0f + surface.conic) * c * c * r2;
    valid = root > 0.0f;
    float slope = c * r / sqrt(max(root, 1e-30f));
    float power = r2 * r;
    float exponent = 4.0f;
    for (uint i = 0u; i < surface.asphericCount; ++i) {
        slope += exponent * surface.aspheric[i] * power;
        power *= r2;
        exponent += 2.0f;
    }
    return slope;
}

// The ray's hit on the surface and the unit normal oriented toward +z. False when there is no valid hit ahead.
bool LensIntersectSurface(LensSurface surface, float3 origin, float3 direction, out float3 hit, out float3 normal)
{
    hit = 0.0f.xxx;
    normal = float3(0.0f, 0.0f, 1.0f);
    const float3 p = float3(origin.xy, origin.z - surface.vertexZ);
    const float3 d = direction;
    const float c = surface.curvature;
    const bool hasAspheric = surface.asphericCount > 0u;
    float t = 1e30f;
    if (c == 0.0f) {
        if (abs(d.z) < 1e-12f) return false;
        t = -p.z / d.z;
    }
    else {
        // c (x^2 + y^2 + (1 + k) z^2) - 2 z = 0, on the sheet through the vertex.
        const float k1 = 1.0f + surface.conic;
        const float a = c * (d.x * d.x + d.y * d.y + k1 * d.z * d.z);
        const float b = 2.0f * (c * (p.x * d.x + p.y * d.y + k1 * p.z * d.z) - d.z);
        const float cc = c * (p.x * p.x + p.y * p.y + k1 * p.z * p.z) - 2.0f * p.z;
        float roots[2] = { -1.0f, -1.0f };
        uint rootCount = 0u;
        if (abs(a) < 1e-12f) {
            if (abs(b) < 1e-12f) return false;
            roots[0] = -cc / b;
            rootCount = 1u;
        }
        else {
            const float disc = b * b - 4.0f * a * cc;
            if (disc < 0.0f) return false;
            const float q = -0.5f * (b + (b >= 0.0f ? sqrt(disc) : -sqrt(disc)));
            roots[0] = q / a;
            rootCount = 1u;
            if (q != 0.0f) {
                roots[1] = cc / q;
                rootCount = 2u;
            }
        }
        for (uint i = 0u; i < rootCount; ++i) {
            const float candidate = roots[i];
            if (candidate <= LENS_HIT_EPSILON || candidate >= t) continue;
            const float z = p.z + candidate * d.z;
            if (k1 * c * z < 1.0f + 1e-6f) t = candidate;
        }
    }
    if (!(t > LENS_HIT_EPSILON) || t >= 1e30f) {
        if (!hasAspheric || abs(d.z) < 1e-12f) return false;
        // Aspheric departure can put the hit where the base conic has none; start from the plane.
        t = -p.z / d.z;
        if (!(t > LENS_HIT_EPSILON)) return false;
    }
    if (hasAspheric) {
        // Newton on g(t) = z(t) - sag(r(t)).
        bool converged = false;
        for (uint iteration = 0u; iteration < 16u; ++iteration) {
            const float3 q = p + d * t;
            const float r = length(q.xy);
            bool sagValid, slopeValid;
            const float sag = LensSag(surface, r, sagValid);
            const float slope = LensSagSlope(surface, r, slopeValid);
            if (!sagValid || !slopeValid) return false;
            const float g = q.z - sag;
            const float drdt = r > 0.0f ? dot(q.xy, d.xy) / r : 0.0f;
            const float dg = d.z - slope * drdt;
            if (abs(dg) < 1e-12f) return false;
            const float step = g / dg;
            t -= step;
            if (abs(step) < 1e-6f) {
                converged = true;
                break;
            }
        }
        if (!converged || !(t > LENS_HIT_EPSILON)) return false;
    }
    const float3 local = p + d * t;
    const float r = length(local.xy);
    if (c != 0.0f || hasAspheric) {
        bool slopeValid;
        const float slope = LensSagSlope(surface, r, slopeValid);
        if (!slopeValid) return false;
        normal = r > 0.0f ? normalize(float3(-slope * local.xy / r, 1.0f)) : float3(0.0f, 0.0f, 1.0f);
    }
    hit = float3(local.xy, local.z + surface.vertexZ);
    return true;
}

float LensFresnelUncoated(float n1, float n2, float cosI, float cosT)
{
    const float rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    const float rp = (n2 * cosI - n1 * cosT) / (n2 * cosI + n1 * cosT);
    return 0.5f * (rs * rs + rp * rp);
}

float LensFilmReflectance(float r12, float r23, float cosDelta)
{
    const float cross = 2.0f * r12 * r23 * cosDelta;
    return (r12 * r12 + r23 * r23 + cross) / (1.0f + r12 * r12 * r23 * r23 + cross);
}

// Unpolarized reflectance from n1 to n2, with an optional quarter-wave film (index, design wavelength).
float LensFresnelReflectance(float n1, float n2, float cosI, bool coated, float coatingIndex, float coatingWavelength, float lambdaNm)
{
    if (n1 == n2) return 0.0f;
    cosI = saturate(cosI);
    const float sinI2 = 1.0f - cosI * cosI;
    const float sinT2 = (n1 / n2) * (n1 / n2) * sinI2;
    if (sinT2 >= 1.0f) return 1.0f;
    const float cosT = sqrt(1.0f - sinT2);
    if (!coated) return LensFresnelUncoated(n1, n2, cosI, cosT);
    const float nc = coatingIndex;
    const float sinC2 = (n1 / nc) * (n1 / nc) * sinI2;
    if (sinC2 >= 1.0f) return LensFresnelUncoated(n1, n2, cosI, cosT);
    const float cosC = sqrt(1.0f - sinC2);
    const float delta = 3.14159265f * (coatingWavelength / lambdaNm) * cosC;
    const float cosDelta = cos(delta);
    const float r12s = (n1 * cosI - nc * cosC) / (n1 * cosI + nc * cosC);
    const float r23s = (nc * cosC - n2 * cosT) / (nc * cosC + n2 * cosT);
    const float r12p = (nc * cosI - n1 * cosC) / (nc * cosI + n1 * cosC);
    const float r23p = (n2 * cosC - nc * cosT) / (n2 * cosC + nc * cosT);
    return 0.5f * (LensFilmReflectance(r12s, r23s, cosDelta) + LensFilmReflectance(r12p, r23p, cosDelta));
}

// Traces a ray from the sensor side through surfaces N-1..0 (TransmissionPath(lens, false)), with Fresnel transmission,
// housing and stop clipping. On LENS_STATUS_OK, origin and direction are the ray leaving surface 0, in lens millimetres.
uint LensTraceBackward(StructuredBuffer<float4> data, LensHeader header, inout float3 origin, inout float3 direction, float lambdaNm,
    out float throughput)
{
    throughput = 1.0f;
    direction = normalize(direction);
    for (uint step = 0u; step < header.surfaceCount; ++step) {
        const uint index = header.surfaceCount - 1u - step;
        const LensSurface surface = LensLoadSurface(data, header, index);
        float3 hit, normal;
        if (!LensIntersectSurface(surface, origin, direction, hit, normal)) return LENS_STATUS_MISSED;
        const float radius = length(hit.xy);
        if (surface.isStop) {
            if (radius > surface.semiAperture * header.stopScale) return LENS_STATUS_STOPPED;
            origin = hit;
            continue;
        }
        if (radius > surface.semiAperture) return LENS_STATUS_CLIPPED;
        const bool forward = direction.z > 0.0f;
        const float nBefore = LensMediumIndex(data, header, surface.mediumBefore, lambdaNm);
        const float nAfter = LensMediumIndex(data, header, surface.mediumAfter, lambdaNm);
        const float n1 = forward ? nBefore : nAfter;
        const float n2 = forward ? nAfter : nBefore;
        if (dot(normal, direction) > 0.0f) normal = -normal;
        const float cosI = -dot(normal, direction);
        const float eta = n1 / n2;
        const float k = 1.0f - eta * eta * (1.0f - cosI * cosI);
        if (k < 0.0f) return LENS_STATUS_TIR;
        const float reflectance = LensFresnelReflectance(n1, n2, cosI, surface.coated, surface.coatingIndex, surface.coatingWavelength, lambdaNm);
        throughput *= 1.0f - reflectance;
        direction = normalize(direction * eta + normal * (eta * cosI - sqrt(k)));
        origin = hit;
    }
    return LENS_STATUS_OK;
}

#endif // LENS_EXPLICIT_HLSLI
