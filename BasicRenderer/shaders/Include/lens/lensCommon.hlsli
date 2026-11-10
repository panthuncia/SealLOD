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
    float sensorZ; // focus node 0's: the frame's focused plane is LensLoadFocus's
    float rearPlaneZ;
    float frontPlaneZ;
    float stopScale;
    float maxSensorRadius;
    float distortionStep;
    float inverseNormalization; // focus node 0's: the frame's is LensInverseNormalization
    float designWavelength;
    uint surfaceOffset;
    uint mediumOffset;
    uint pupilNodeOffset;
    uint distortionOffset;
    uint selfTestOffset;
    uint revision; // low word
    uint pupilDiscCount;
    float coveredField; // radians, focus node 0's: the field LENS_FLAG_FIELD_REMAP fits the frame's diagonal to (LensCoveredField)
    uint2 polynomialKey; // LENS_FLAG_POLYNOMIAL: the generated code's hash, which the shader's must match
    uint polynomialDegree;
    float4 polynomialDomain; // sensor radius, lambda min, lambda max, rear half extent
    float baseSensorZ; // the infinity-focus sensor plane; a focus shift is added to it
    float shiftMin;    // focus node 0's shift (mm)
    float shiftMax;    // the last focus node's
    float nearFocusMm; // the focus table's last entry
    uint focusNodeCount;
    uint focusNodeOffset;
    uint focusTableOffset;
    uint focusTableCount;
    uint normalizationOffset;
    uint normalizationCount;
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
    const float4 row8 = data[LENS_ROW_HEADER8];
    const uint4 row9 = asuint(data[LENS_ROW_HEADER9]);
    const uint4 row10 = asuint(data[LENS_ROW_HEADER10]);
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
    header.revision = row5.y;
    header.pupilDiscCount = row5.z;
    header.coveredField = asfloat(row5.w);
    header.polynomialKey = row6.xy;
    header.polynomialDegree = row6.z;
    header.polynomialDomain = row7;
    header.baseSensorZ = row8.x;
    header.shiftMin = row8.y;
    header.shiftMax = row8.z;
    header.nearFocusMm = row8.w;
    header.focusNodeCount = max(row9.x, 1u);
    header.focusNodeOffset = row9.y;
    header.focusTableOffset = row9.z;
    header.focusTableCount = row9.w;
    header.normalizationOffset = row10.x;
    header.normalizationCount = row10.y;
    return header;
}

// The frame's focus (lensDataLayout.h, "Per-frame focus"): the focus shift, the sensor plane it puts the sensor at, and the two
// focus nodes the focus-dependent tables are interpolated between (node and node + 1, weight t on the second).
struct LensFocus
{
    float shift;
    float sensorZ;
    uint node;
    float t;
};

StructuredBuffer<float4> LensFocusData()
{
    StructuredBuffer<float4> data = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::RayTracing::LensFocus)];
    return data;
}

// A focus shift (mm), clamped to the lens's focus range, as the nodes to interpolate.
LensFocus LensFocusFromShift(LensHeader header, float shift)
{
    LensFocus focus;
    focus.shift = clamp(shift, header.shiftMin, header.shiftMax);
    focus.sensorZ = header.baseSensorZ + focus.shift;
    const float last = float(header.focusNodeCount - 1u);
    const float s = header.shiftMax > header.shiftMin ? (focus.shift - header.shiftMin) / (header.shiftMax - header.shiftMin) * last : 0.0f;
    focus.node = header.focusNodeCount > 1u ? min(uint(max(s, 0.0f)), header.focusNodeCount - 2u) : 0u;
    focus.t = header.focusNodeCount > 1u ? saturate(s - float(focus.node)) : 0.0f;
    return focus;
}

// This frame's focus, or the previous frame's (for motion vectors). Before LensFocusPass has run once, focus node 0.
LensFocus LensLoadFocus(LensHeader header, bool previous)
{
    StructuredBuffer<float4> state = LensFocusData();
    const bool initialized = asuint(state[LENS_FOCUS_ROW_STATE].x) == LENS_FOCUS_MAGIC;
    const float shift = state[previous ? LENS_FOCUS_ROW_PREVIOUS : LENS_FOCUS_ROW_CURRENT].x;
    return LensFocusFromShift(header, initialized ? shift : header.shiftMin);
}

// Whether the focus moved this frame (LensFocusPass): accumulated samples at the old focus are no longer the image.
bool LensFocusRestart()
{
    StructuredBuffer<float4> state = LensFocusData();
    return asuint(state[LENS_FOCUS_ROW_STATE].y) != 0u;
}

uint LensFocusNextNode(LensHeader header, LensFocus focus)
{
    return min(focus.node + 1u, header.focusNodeCount - 1u);
}

// A focus node's row: (sensorZ, 1/K, covered field, focus distance mm).
float4 LensFocusNodeRow(StructuredBuffer<float4> data, LensHeader header, uint node)
{
    return data[header.focusNodeOffset + node * LENS_FOCUS_NODE_ROWS];
}

// 1/K at the frame's focus: the normalization that makes the centre pixel's exposure a pinhole camera's, from the normalization
// table (lensDataLayout.h). K is close to the exit pupil's area over the square of its distance from the sensor, which grows
// linearly with the focus shift, so 1 / sqrt(K) is interpolated linearly.
float LensInverseNormalization(StructuredBuffer<float4> data, LensHeader header, LensFocus focus)
{
    const uint count = header.normalizationCount;
    if (count < 2u || header.focusNodeCount < 2u) return header.inverseNormalization;
    const float x = clamp((float(focus.node) + focus.t) / float(header.focusNodeCount - 1u), 0.0f, 1.0f) * float(count - 1u);
    const uint i = min(uint(x), count - 2u);
    const float a = sqrt(data[header.normalizationOffset + i / 4u][i % 4u]);
    const float b = sqrt(data[header.normalizationOffset + (i + 1u) / 4u][(i + 1u) % 4u]);
    const float root = lerp(a, b, x - float(i));
    return root * root;
}

// The covered half-field (radians) at the frame's focus: what LENS_FLAG_FIELD_REMAP fits the frame's diagonal to.
float LensCoveredField(StructuredBuffer<float4> data, LensHeader header, LensFocus focus)
{
    return lerp(LensFocusNodeRow(data, header, focus.node).z, LensFocusNodeRow(data, header, LensFocusNextNode(header, focus)).z, focus.t);
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

// Chief-ray image radius (mm) at a field angle, through a focus node's distortion table. Past its last entry it continues as
// the last entry's tan scaling, so a field the lens can't image still maps somewhere sensible (it vignettes to black anyway).
float LensImageRadiusAtNode(StructuredBuffer<float4> data, LensHeader header, uint focusNode, float angle)
{
    const uint count = header.distortionCount;
    if (count < 2u) return 0.0f;
    const float scaled = max(angle, 0.0f) / header.distortionStep;
    const float last = float(count - 1u);
    const uint base = header.distortionOffset + focusNode * ((count + 3u) / 4u);
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

// The same at the frame's focus. The tables are the same chief rays propagated to each node's plane, so this is exact.
float LensImageRadiusAt(StructuredBuffer<float4> data, LensHeader header, LensFocus focus, float angle)
{
    const float a = LensImageRadiusAtNode(data, header, focus.node, angle);
    if (focus.t <= 0.0f) return a;
    return lerp(a, LensImageRadiusAtNode(data, header, LensFocusNextNode(header, focus), angle), focus.t);
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

LensSensorFrame LensSensorHalfExtent(StructuredBuffer<float4> data, LensHeader header, LensFocus focus, float4x4 unjitteredProjection)
{
    const float tanHalfVertical = 1.0f / max(abs(unjitteredProjection[1][1]), 1e-6f);
    const float aspect = abs(unjitteredProjection[1][1]) / max(abs(unjitteredProjection[0][0]), 1e-6f);
    const float diagonal = sqrt(1.0f + aspect * aspect);
    LensSensorFrame frame;
    frame.fieldScale = 1.0f;
    const float coveredField = LensCoveredField(data, header, focus);
    const float tanCovered = tan(clamp(coveredField, 1e-3f, 1.5f));
    const float tanHalfDiagonal = tanHalfVertical * diagonal;
    if ((header.flags & LENS_FLAG_FIELD_REMAP) != 0u && coveredField > 0.0f && tanHalfDiagonal > tanCovered) {
        const float halfHeight = LensImageRadiusAt(data, header, focus, coveredField) / diagonal;
        frame.halfExtent = float2(halfHeight * aspect, halfHeight);
        frame.fieldScale = tanHalfDiagonal / tanCovered;
        return frame;
    }
    const float halfHeight = LensImageRadiusAt(data, header, focus, atan(tanHalfVertical));
    frame.halfExtent = float2(halfHeight * aspect, halfHeight);
    return frame;
}

// At this frame's focus (reads Builtin::RayTracing::LensFocus, which the calling pass must declare).
LensSensorFrame LensSensorHalfExtent(StructuredBuffer<float4> data, LensHeader header, float4x4 unjitteredProjection)
{
    return LensSensorHalfExtent(data, header, LensLoadFocus(header, false), unjitteredProjection);
}

// This frame's (or the previous frame's) focus with its sensor frame and 1/K, as LensFocusPass computed them once for the frame
// (lensDataLayout.h, focus rows 5 and 6); computed here when they are another lens revision's. unjitteredProjection is the
// frame's: the camera's, or its previous one.
struct LensFrame
{
    LensFocus focus;
    LensSensorFrame sensor;
    float inverseNormalization;
    bool cached; // LensFocusPass's rows are this lens revision's: the pupil at the focus is in them too (LensPupilRegionAtFrame)
};

LensFrame LensLoadFrame(StructuredBuffer<float4> data, LensHeader header, bool previous, float4x4 unjitteredProjection)
{
    LensFrame frame;
    frame.focus = LensLoadFocus(header, previous);
    StructuredBuffer<float4> state = LensFocusData();
    const float4 stored = state[previous ? LENS_FOCUS_ROW_PREVIOUS_FRAME : LENS_FOCUS_ROW_FRAME];
    const bool current = asuint(state[LENS_FOCUS_ROW_STATE].x) == LENS_FOCUS_MAGIC && asuint(state[LENS_FOCUS_ROW_PREVIOUS_FRAME].w) == header.revision;
    frame.cached = current && !previous;
    if (current) {
        frame.sensor.halfExtent = stored.xy;
        frame.sensor.fieldScale = stored.z;
        frame.inverseNormalization = previous ? LensInverseNormalization(data, header, frame.focus) : stored.w;
    }
    else {
        frame.sensor = LensSensorHalfExtent(data, header, frame.focus, unjitteredProjection);
        frame.inverseNormalization = LensInverseNormalization(data, header, frame.focus);
    }
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

// The NDC a view-space direction images at, through the chief-ray table (infinity focus at the focused sensor plane of
// `focus`), with the frame's field remap undone first. The lens inverts the image and the sensor-to-NDC mapping inverts it back,
// so the azimuth is kept.
float2 LensProjectViewDirection(StructuredBuffer<float4> data, LensHeader header, LensFocus focus, LensSensorFrame frame, float3 viewDirection)
{
    const float3 d = normalize(viewDirection);
    // tan(lens angle) = tan(view angle) / fieldScale; atan2 keeps directions past 90 degrees well defined.
    const float lateral = length(d.xy);
    const float angle = atan2(lateral, -d.z * frame.fieldScale);
    const float radius = LensImageRadiusAt(data, header, focus, angle);
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
