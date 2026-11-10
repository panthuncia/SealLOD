#ifndef LENS_CAMERA_HLSLI
#define LENS_CAMERA_HLSLI

// Primary camera rays through the physical lens: the sensor point, the rear-plane point from the exact pupil, then the
// transport through either the surfaces (lensExplicit.hlsli) or the polynomial (lensPolynomial.hlsli, when the lens data has
// LENS_FLAG_POLYNOMIAL and the shader was compiled with its code).

#include "include/lens/lensExplicit.hlsli"
#include "include/lens/lensPolynomial.hlsli"

// Twice the area of the disc (c, r) left of x, up to a constant: r^2 (u sqrt(1 - u^2) + asin u), u = (x - c) / r.
float LensDiscSliceIntegral(float c, float r, float x)
{
    const float u = clamp((x - c) / r, -1.0f, 1.0f);
    return r * r * (u * sqrt(max(1.0f - u * u, 0.0f)) + asin(u));
}

float LensHalfChord(float c, float r, float x)
{
    const float d = x - c;
    return sqrt(max(r * r - d * d, 0.0f));
}

void LensSortPair(inout float a, inout float b)
{
    const float lo = min(a, b);
    b = max(a, b);
    a = lo;
}

// A point uniform over the intersection of up to LENS_MAX_PUPIL_DISCS discs centred on the x axis (LensCore's
// SamplePupilRegion in float32): the x range splits where two discs' half chords are equal (h_i^2 - h_j^2 is linear in x)
// into intervals each bounded by one disc; an interval is picked by area, x inverts the area to its left (Newton, kept in
// a bracket), and y is uniform across the region at x. area is the region's (the pdf is 1 / area); it is 0 when the region
// is empty. Every array is indexed by constants after unrolling, so it stays in registers.
float2 LensSamplePupilRegion(uint count, float4 centre, float4 radius, float2 u, out float area)
{
    area = 0.0f;
    float minX = -1e30f, maxX = 1e30f;
    [unroll] for (uint i = 0u; i < LENS_MAX_PUPIL_DISCS; ++i) {
        if (i < count) {
            minX = max(minX, centre[i] - radius[i]);
            maxX = min(maxX, centre[i] + radius[i]);
        }
    }
    if (!(count > 0u && minX < maxX)) return 0.0f.xx;
    // Breakpoints: the 6 pairs' crossings inside (minX, maxX); the others sit at maxX and make empty intervals.
    float edge[8];
    edge[0] = minX;
    uint e = 1u;
    [unroll] for (uint a = 0u; a < LENS_MAX_PUPIL_DISCS; ++a) {
        [unroll] for (uint b = a + 1u; b < LENS_MAX_PUPIL_DISCS; ++b) {
            const float dc = centre[b] - centre[a];
            float x = maxX;
            if (b < count && dc != 0.0f) {
                x = (radius[a] * radius[a] - radius[b] * radius[b] + centre[b] * centre[b] - centre[a] * centre[a]) / (2.0f * dc);
                x = x > minX && x < maxX ? x : maxX;
            }
            edge[e] = x;
            ++e;
        }
    }
    edge[7] = maxX;
    // Sort edges 1..6 (a bubble network on constants).
    [unroll] for (uint pass = 0u; pass < 5u; ++pass) {
        [unroll] for (uint k = 1u; k + 1u < 7u - pass; ++k) LensSortPair(edge[k], edge[k + 1u]);
    }
    float intervalArea[7];
    float intervalCentre[7];
    float intervalRadius[7];
    float total = 0.0f;
    [unroll] for (uint k = 0u; k < 7u; ++k) {
        const float lo = edge[k], hi = edge[k + 1u];
        const float m = 0.5f * (lo + hi);
        float bestH2 = 1e30f, c = centre.x, r = radius.x;
        [unroll] for (uint i = 0u; i < LENS_MAX_PUPIL_DISCS; ++i) {
            const float h2 = radius[i] * radius[i] - (m - centre[i]) * (m - centre[i]);
            if (i < count && h2 < bestH2) {
                bestH2 = h2;
                c = centre[i];
                r = radius[i];
            }
        }
        intervalCentre[k] = c;
        intervalRadius[k] = r;
        intervalArea[k] = hi > lo ? max(LensDiscSliceIntegral(c, r, hi) - LensDiscSliceIntegral(c, r, lo), 0.0f) : 0.0f;
        total += intervalArea[k];
    }
    if (!(total > 0.0f)) return 0.0f.xx;
    area = total;
    // The interval at u.x of the total, and the area still to cover inside it.
    float target = u.x * total;
    float lo = edge[0], hi = edge[1], c = intervalCentre[0], r = intervalRadius[0], intervalTotal = intervalArea[0];
    bool found = false;
    [unroll] for (uint k = 0u; k < 7u; ++k) {
        if (!found && intervalArea[k] > 0.0f && (target <= intervalArea[k] || k == 6u)) {
            found = true;
            lo = edge[k];
            hi = edge[k + 1u];
            c = intervalCentre[k];
            r = intervalRadius[k];
            intervalTotal = intervalArea[k];
        }
        else if (!found) {
            target -= intervalArea[k];
        }
    }
    target = clamp(target, 0.0f, intervalTotal);
    const float base = LensDiscSliceIntegral(c, r, lo);
    float x = lo + (hi - lo) * (target / max(intervalTotal, 1e-30f));
    [loop] for (uint iteration = 0u; iteration < 10u; ++iteration) {
        const float f = LensDiscSliceIntegral(c, r, x) - base - target;
        if (f > 0.0f) hi = x;
        else lo = x;
        const float slope = 2.0f * LensHalfChord(c, r, x);
        float next = slope > 0.0f ? x - f / slope : 0.5f * (lo + hi);
        if (!(next > lo && next < hi)) next = 0.5f * (lo + hi);
        x = next;
    }
    return float2(x, (2.0f * u.y - 1.0f) * LensHalfChord(c, r, x));
}

// The exact pupil at a sensor radius: the two nearest nodes' discs and polynomial transmission (per band), interpolated.
// False past the last node or where a node has no light.
bool LensPupilRegionAt(StructuredBuffer<float4> data, LensHeader header, float sensorRadius, out float4 centre, out float4 radius,
    out float3 transmission)
{
    centre = 0.0f.xxxx;
    radius = 0.0f.xxxx;
    transmission = 0.0f.xxx;
    if (header.pupilNodeCount < 2u || header.pupilDiscCount == 0u) return false;
    const float scaled = sensorRadius / header.maxSensorRadius * float(header.pupilNodeCount - 1u);
    if (!(scaled <= float(header.pupilNodeCount - 1u))) return false;
    const uint node = min(uint(scaled), header.pupilNodeCount - 2u);
    const float t = scaled - float(node);
    const uint row = header.pupilNodeOffset + node * LENS_PUPIL_NODE_ROWS;
    const uint next = row + LENS_PUPIL_NODE_ROWS;
    const float4 a0 = data[row], a1 = data[row + 1u], b0 = data[next], b1 = data[next + 1u];
    if (!(a0.y > 0.0f) || !(b0.y > 0.0f)) return false;
    centre = lerp(float4(a0.xz, a1.xz), float4(b0.xz, b1.xz), t);
    radius = lerp(float4(a0.yw, a1.yw), float4(b0.yw, b1.yw), t);
    transmission = lerp(data[row + 2u].xyz, data[next + 2u].xyz, t);
    return true;
}

// A primary ray through the lens for a point on the image (pixelPosition in pixels). False when it doesn't clear the lens:
// the path then carries nothing. On success the ray is in world space and weight is its estimator weight (transmission,
// the sensor-to-rear-plane geometry over the pupil sample's pdf, the normalization that makes the centre pixel match a
// pinhole camera's exposure, and the wavelength's RGB response over its pdf).
bool LensCameraRay(Camera camera, float2 pixelPosition, float2 resolution, float2 pupilSample, float wavelengthSample, out RayDesc ray,
    out float3 weight)
{
    ray.Origin = camera.positionWorldSpace.xyz;
    ray.Direction = -camera.viewInverse[2].xyz;
    ray.TMin = 0.0f;
    ray.TMax = 1e20f;
    weight = 0.0f.xxx;

    StructuredBuffer<float4> data = LensData();
    const LensHeader header = LensLoadHeader(data);
    if (!header.valid) return false;

    // The pixel's NDC without the upscaler's jitter: the jittered pinhole direction, projected without it.
    const float2 uv = pixelPosition / resolution;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 viewPoint = mul(float4(ndc, 1.0f, 1.0f), camera.projectionInverse);
    viewPoint.xyz /= max(abs(viewPoint.w), 1e-6f);
    const float4 clip = mul(float4(viewPoint.xyz, 1.0f), camera.unjitteredProjection);
    const float2 unjittered = clip.xy / max(abs(clip.w), 1e-6f);

    // The image is inverted on the sensor: the top right of the frame is the sensor's bottom left.
    const LensSensorFrame frame = LensSensorHalfExtent(data, header, camera.unjitteredProjection);
    const float2 sensor = -unjittered * frame.halfExtent;
    const float sensorRadius = length(sensor);
    if (!(sensorRadius < header.maxSensorRadius)) return false;

    // The exact pupil, sampled uniformly: its discs are in the canonical frame, with the sensor point on +x, so the sample is
    // rotated to the point's azimuth. The pdf is 1 / area.
    float4 centre, radius;
    float3 polynomialTransmission;
    if (!LensPupilRegionAt(data, header, sensorRadius, centre, radius, polynomialTransmission)) return false;
    float area;
    const float2 canonical = LensSamplePupilRegion(header.pupilDiscCount, centre, radius, pupilSample, area);
    if (!(area > 0.0f)) return false;
    const float2 azimuth = sensorRadius > 0.0f ? sensor / sensorRadius : float2(1.0f, 0.0f);
    const float2 rear = float2(azimuth.x * canonical.x - azimuth.y * canonical.y, azimuth.y * canonical.x + azimuth.x * canonical.y);

    float3 origin = float3(sensor, header.sensorZ);
    const float3 toRear = float3(rear, header.rearPlaneZ) - origin;
    const float distance2 = dot(toRear, toRear);
    float3 direction = toRear * rsqrt(distance2);
    // Both the sensor and the rear plane face the optical axis: cos theta on each is |dz|.
    const float geometry = direction.z * direction.z / distance2;

    float3 spectral = 1.0f.xxx;
    const float lambdaNm = (header.flags & LENS_FLAG_MONOCHROMATIC) != 0u ? header.designWavelength
        : LensSampleWavelength(wavelengthSample, spectral);
    float transmission;
    if (LensPolynomialCompiledFor(header) && sensorRadius <= header.polynomialDomain.x) {
        // The polynomial starts at the rear plane: the ray's crossing there is the point just sampled. It can't tell a
        // blocked ray from a passing one, so every sample carries the region's mean transmission instead. Past the radius
        // it is used to (the image circle, LensManager: physical mode's dark corners) the surfaces are traced.
        if (!LensPolynomialTransport(header, rear, direction, lambdaNm, origin, transmission)) return false;
        // The bands' transmission, quadratic in the wavelength through the three band centres.
        const float t = (lambdaNm - LENS_POLYNOMIAL_BAND_NM) / LENS_POLYNOMIAL_BAND_STEP_NM;
        const float3 f = polynomialTransmission;
        transmission *= f.y + 0.5f * t * (f.z - f.x) + 0.5f * t * t * (f.x - 2.0f * f.y + f.z);
    }
    else if (LensTraceBackward(data, header, origin, direction, lambdaNm, transmission) != LENS_STATUS_OK) {
        return false;
    }

    // Lens space is view space in millimetres: +z points from the scene to the sensor, as view space's +z points behind the
    // camera, and the front vertex sits at the camera.
    // The field remap (lensCommon.hlsli's LensSensorFrame), for both evaluators alike: lens space scaled laterally by k, the
    // exit origin and direction both. Scaling the direction alone would also widen each pixel's ray bundle and pull the
    // focus plane in to focus / k; scaling the whole ray maps the bundle's meeting point to the remapped point at the same
    // depth, so focus is kept and the defocus blur scales with the frame.
    if (frame.fieldScale != 1.0f) LensRemapField(origin, direction, frame.fieldScale);
    const float3 viewOrigin = origin * 1.0e-3f;
    ray.Origin = mul(float4(viewOrigin, 1.0f), camera.viewInverse).xyz;
    ray.Direction = normalize(mul(float4(direction, 0.0f), camera.viewInverse).xyz);
    const float3 forward = -camera.viewInverse[2].xyz;
    ray.TMin = camera.zNear / max(dot(ray.Direction, forward), 1e-3f);
    weight = spectral * (transmission * geometry * area * header.inverseNormalization);
    return all(isfinite(weight));
}

#endif // LENS_CAMERA_HLSLI
