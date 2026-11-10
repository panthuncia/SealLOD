#ifndef LENS_COMMON_HLSLI
#define LENS_COMMON_HLSLI

// The physical lens's data (lensDataLayout.h), wavelength sampling and the image-plane mapping. LensManager builds the
// buffer; a buffer without LENS_DATA_MAGIC is a pinhole camera.

#include "include/lens/lensDataLayout.h"

struct LensHeader
{
    bool valid;
    uint surfaceCount;
    uint stopIndex;
    uint flags;
    uint mediumCount;
    uint pupilNodeCount;
    uint distortionCount;
    uint selfTestRayCount;
    float sensorZ;
    float rearPlaneZ;
    float frontPlaneZ;
    float stopScale;
    float maxSensorRadius;
    float distortionStep;
    float inverseNormalization;
    float designWavelength;
    uint surfaceOffset;
    uint mediumOffset;
    uint pupilNodeOffset;
    uint distortionOffset;
    uint selfTestOffset;
    uint pupilDiscCount;
    float coveredField; // radians: the field LENS_FLAG_FIELD_REMAP fits the frame's diagonal to
    uint2 polynomialKey; // LENS_FLAG_POLYNOMIAL: the generated code's hash, which the shader's must match
    uint polynomialDegree;
    float4 polynomialDomain; // sensor radius, lambda min, lambda max, rear half extent
};

StructuredBuffer<float4> LensData()
{
    StructuredBuffer<float4> data = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::RayTracing::LensData)];
    return data;
}

LensHeader LensLoadHeader(StructuredBuffer<float4> data)
{
    const uint4 row0 = asuint(data[LENS_ROW_HEADER0]);
    const uint4 row1 = asuint(data[LENS_ROW_HEADER1]);
    const float4 row2 = data[LENS_ROW_HEADER2];
    const float4 row3 = data[LENS_ROW_HEADER3];
    const uint4 row4 = asuint(data[LENS_ROW_HEADER4]);
    const uint4 row5 = asuint(data[LENS_ROW_HEADER5]);
    const uint4 row6 = asuint(data[LENS_ROW_HEADER6]);
    const float4 row7 = data[LENS_ROW_HEADER7];
    LensHeader header;
    header.valid = row0.x == LENS_DATA_MAGIC && row0.y > 0u && row0.y <= LENS_MAX_SURFACES;
    header.surfaceCount = row0.y;
    header.stopIndex = row0.z;
    header.flags = row0.w;
    header.mediumCount = row1.x;
    header.pupilNodeCount = row1.y;
    header.distortionCount = row1.z;
    header.selfTestRayCount = row1.w;
    header.sensorZ = row2.x;
    header.rearPlaneZ = row2.y;
    header.frontPlaneZ = row2.z;
    header.stopScale = row2.w;
    header.maxSensorRadius = row3.x;
    header.distortionStep = row3.y;
    header.inverseNormalization = row3.z;
    header.designWavelength = row3.w;
    header.surfaceOffset = row4.x;
    header.mediumOffset = row4.y;
    header.pupilNodeOffset = row4.z;
    header.distortionOffset = row4.w;
    header.selfTestOffset = row5.x;
    header.pupilDiscCount = row5.z;
    header.coveredField = asfloat(row5.w);
    header.polynomialKey = row6.xy;
    header.polynomialDegree = row6.z;
    header.polynomialDomain = row7;
    return header;
}

// Index of refraction of a medium at a wavelength (nm), as LensCore's Glass::IndexAt.
float LensMediumIndex(StructuredBuffer<float4> data, LensHeader header, uint medium, float lambdaNm)
{
    const uint row = header.mediumOffset + medium * LENS_MEDIUM_ROWS;
    const float4 m0 = data[row];
    const uint model = asuint(m0.x);
    if (model == LENS_MEDIUM_CAUCHY) {
        return m0.z + m0.w / (lambdaNm * lambdaNm);
    }
    if (model == LENS_MEDIUM_SELLMEIER) {
        const float3 b = data[row + 1u].xyz;
        const float3 c = data[row + 2u].xyz;
        const float l2 = (lambdaNm * 1.0e-3f) * (lambdaNm * 1.0e-3f);
        const float3 terms = b * l2 / (l2 - c);
        return sqrt(1.0f + terms.x + terms.y + terms.z);
    }
    return m0.y;
}

// Chief-ray image radius (mm) at a field angle, through the distortion table. Past its last entry it continues as the
// last entry's tan scaling, so a field the lens can't image still maps somewhere sensible (it vignettes to black anyway).
float LensImageRadiusAt(StructuredBuffer<float4> data, LensHeader header, float angle)
{
    const uint count = header.distortionCount;
    if (count < 2u) return 0.0f;
    const float scaled = max(angle, 0.0f) / header.distortionStep;
    const float last = float(count - 1u);
    const uint base = header.distortionOffset;
    if (scaled >= last) {
        const float lastRadius = data[base + (count - 1u) / 4u][(count - 1u) % 4u];
        const float lastAngle = last * header.distortionStep;
        return lastRadius * tan(min(angle, 1.55f)) / max(tan(lastAngle), 1e-6f);
    }
    const uint index = uint(scaled);
    const float t = scaled - float(index);
    const float r0 = data[base + index / 4u][index % 4u];
    const float r1 = data[base + (index + 1u) / 4u][(index + 1u) % 4u];
    return lerp(r0, r1, t);
}

// The sensor frame for the camera's projection: the half extent (mm, x the half-width) and the field scale.
// Physical: the vertical half-field maps to the half-height through the chief-ray table, and a field wider than the lens
// covers vignettes to black. Remap (LENS_FLAG_FIELD_REMAP), when the camera's half-diagonal field is wider than the covered
// field: the frame's half-diagonal is the covered image radius, and the exit rays' tan from the axis is scaled by fieldScale,
// tan(camera half-diagonal) / tan(covered field), so the frame's corners see what a pinhole's would. fieldScale is 1 otherwise.
struct LensSensorFrame
{
    float2 halfExtent;
    float fieldScale;
};

LensSensorFrame LensSensorHalfExtent(StructuredBuffer<float4> data, LensHeader header, float4x4 unjitteredProjection)
{
    const float tanHalfVertical = 1.0f / max(abs(unjitteredProjection[1][1]), 1e-6f);
    const float aspect = abs(unjitteredProjection[1][1]) / max(abs(unjitteredProjection[0][0]), 1e-6f);
    const float diagonal = sqrt(1.0f + aspect * aspect);
    LensSensorFrame frame;
    frame.fieldScale = 1.0f;
    const float tanCovered = tan(clamp(header.coveredField, 1e-3f, 1.5f));
    const float tanHalfDiagonal = tanHalfVertical * diagonal;
    if ((header.flags & LENS_FLAG_FIELD_REMAP) != 0u && header.coveredField > 0.0f && tanHalfDiagonal > tanCovered) {
        const float halfHeight = LensImageRadiusAt(data, header, header.coveredField) / diagonal;
        frame.halfExtent = float2(halfHeight * aspect, halfHeight);
        frame.fieldScale = tanHalfDiagonal / tanCovered;
        return frame;
    }
    const float halfHeight = LensImageRadiusAt(data, header, atan(tanHalfVertical));
    frame.halfExtent = float2(halfHeight * aspect, halfHeight);
    return frame;
}

// An exit ray (lens space, leaving the front of the lens toward -z) remapped by the field scale k: lens space scaled by k
// across the axis, origin and direction both. A point's tan from the axis, seen from the front vertex, is scaled by k with
// its azimuth and depth kept (rectilinear to rectilinear), and rays that met still meet, so focus and depth of field carry
// over. The direction's angle alone goes to atan(k tan); LensProjectViewDirection inverts that with a scale by 1 / k.
void LensRemapField(inout float3 origin, inout float3 direction, float fieldScale)
{
    origin.xy *= fieldScale;
    direction = normalize(float3(direction.xy * fieldScale, direction.z));
}

// The NDC a view-space direction images at, through the chief-ray table (infinity focus at the focused sensor plane), with
// the frame's field remap undone first. The lens inverts the image and the sensor-to-NDC mapping inverts it back, so the
// azimuth is kept.
float2 LensProjectViewDirection(StructuredBuffer<float4> data, LensHeader header, LensSensorFrame frame, float3 viewDirection)
{
    const float3 d = normalize(viewDirection);
    // tan(lens angle) = tan(view angle) / fieldScale; atan2 keeps directions past 90 degrees well defined.
    const float lateral = length(d.xy);
    const float angle = atan2(lateral, -d.z * frame.fieldScale);
    const float radius = LensImageRadiusAt(data, header, angle);
    const float2 azimuth = lateral > 1e-8f ? d.xy / lateral : float2(1.0f, 0.0f);
    return radius * azimuth / max(frame.halfExtent, 1e-6f.xx);
}

// CIE 1931 2-degree observer, the multi-lobe fit of Wyman, Sloan and Shirley (JCGT 2013).
float LensLobe(float x, float mu, float sigmaLow, float sigmaHigh)
{
    const float t = (x - mu) / (x < mu ? sigmaLow : sigmaHigh);
    return exp(-0.5f * t * t);
}

float3 LensCieXyz(float lambdaNm)
{
    const float x = 1.056f * LensLobe(lambdaNm, 599.8f, 37.9f, 31.0f) + 0.362f * LensLobe(lambdaNm, 442.0f, 16.0f, 26.7f)
        - 0.065f * LensLobe(lambdaNm, 501.1f, 20.4f, 26.2f);
    const float y = 0.821f * LensLobe(lambdaNm, 568.8f, 46.9f, 40.5f) + 0.286f * LensLobe(lambdaNm, 530.9f, 16.3f, 31.1f);
    const float z = 1.217f * LensLobe(lambdaNm, 437.0f, 11.8f, 36.0f) + 0.681f * LensLobe(lambdaNm, 459.0f, 26.0f, 13.8f);
    return float3(x, y, z);
}

// Linear Rec.709 response to a unit spectral sample at lambdaNm, scaled per channel so that an equal-energy spectrum over
// [LENS_LAMBDA_MIN, LENS_LAMBDA_MAX] integrates to (1, 1, 1). The integrals of the fit through the XYZ to sRGB matrix are
// 128.361, 101.538 and 97.065 (computed offline by quadrature). Negative lobes are out-of-gamut spectral colours.
float3 LensRgbWeight(float lambdaNm)
{
    const float3 xyz = LensCieXyz(lambdaNm);
    const float3 rgb = float3(
        3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
        -0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
        0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z);
    return rgb * float3(1.0f / 128.36102f, 1.0f / 101.53808f, 1.0f / 97.06480f);
}

// A wavelength and its weight over the pdf: uniform over the range for now.
float LensSampleWavelength(float u, out float3 weight)
{
    const float range = LENS_LAMBDA_MAX - LENS_LAMBDA_MIN;
    const float lambdaNm = LENS_LAMBDA_MIN + u * range;
    weight = LensRgbWeight(lambdaNm) * range;
    return lambdaNm;
}

#endif // LENS_COMMON_HLSLI
