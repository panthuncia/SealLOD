#ifndef LENS_DATA_LAYOUT_H
#define LENS_DATA_LAYOUT_H

// The physical lens's GPU data (Builtin::RayTracing::LensData): one StructuredBuffer<float4>, built by LensManager
// (src/Camera/Lens) and read by lensCommon.hlsli. Included by both C++ and HLSL, so macros only. Rows are float4; "u" rows
// hold uints (asuint/asfloat). Lens units are millimetres, optical axis +z, surface 0 nearest the scene at z = 0.

#define LENS_DATA_MAGIC 0x534E454Cu // "LENS": a buffer without it (the fallback, or nothing yet) is a pinhole camera.

// Row 0 (u): magic, surfaceCount, stopIndex, flags.
// Row 1 (u): mediumCount, pupilNodeCount, distortionCount, selfTestRayCount.
// Row 2: sensorZ (with the focus shift), rearPlaneZ, frontPlaneZ, stopScale.
// Row 3: maxSensorRadius (the pupil nodes span [0, it]), distortionStep (radians between table entries), 1/K, design
// wavelength nm.
// Row 4 (u): row offsets of the surfaces, media, pupil nodes and distortion table.
// Row 5 (u): row offset of the self-test rays, revision low word, pupil disc count, asuint(covered field): the half-field
// (radians) the lens images with usable light, which LENS_FLAG_FIELD_REMAP fits the frame's diagonal to.
// Row 6 (u): with LENS_FLAG_POLYNOMIAL, the polynomial's key low and high words (the hash of the code LensManager generated
// for it; a shader compiled with other code traces the surfaces instead), its degree, 0.
// Row 7: with LENS_FLAG_POLYNOMIAL, its domain: the sensor radius it is used to (at most the image circle), lambda min, max (nm), rear half
// extent (mm).
#define LENS_ROW_HEADER0 0u
#define LENS_ROW_HEADER1 1u
#define LENS_ROW_HEADER2 2u
#define LENS_ROW_HEADER3 3u
#define LENS_ROW_HEADER4 4u
#define LENS_ROW_HEADER5 5u
#define LENS_ROW_HEADER6 6u
#define LENS_ROW_HEADER7 7u
#define LENS_HEADER_ROWS 8u

#define LENS_FLAG_MONOCHROMATIC 1u
// Primary rays through the polynomial (lensPolynomial.hlsli) from the rear plane to the front plane, not the surfaces.
#define LENS_FLAG_POLYNOMIAL 2u
// lensFieldMode remap: a camera field wider than the covered field is fitted to it (the frame's half-diagonal is the covered
// image radius) and the exit rays' angle from the axis is widened, tan scaled by tan(camera half-diagonal) / tan(covered).
#define LENS_FLAG_FIELD_REMAP 4u

// Surface, 4 rows: (curvature, conic, vertexZ, semiAperture), u(isStop, mediumBefore, mediumAfter, coatingEnabled),
// (coatingIndex, coatingWavelengthNm, asuint(asphericCount), 0), aspheric coefficients of r^4, r^6, r^8, r^10.
#define LENS_SURFACE_ROWS 4u
#define LENS_MAX_SURFACES 32u
#define LENS_MAX_ASPHERIC 4u

// Medium, 3 rows: (asuint(model), nd, cauchyA, cauchyB), Sellmeier B, Sellmeier C (um^2).
#define LENS_MEDIUM_ROWS 3u
#define LENS_MEDIUM_CONSTANT 0u
#define LENS_MEDIUM_CAUCHY 1u
#define LENS_MEDIUM_SELLMEIER 2u

// Pupil nodes: the exact pupil (LensCore ComputePupilDiscs). Per node, the discs whose intersection is the region of the
// rear plane that camera rays from that sensor radius clear the lens through, in the canonical frame (sensor point on +x,
// so every centre is on the x axis). Nodes sit at sensor radii i * maxSensorRadius / (nodes - 1) and are interpolated
// linearly. LENS_PUPIL_NODE_ROWS rows per node: (centre0, radius0, centre1, radius1), (centre2, radius2, centre3,
// radius3), (polynomial transmission at LENS_POLYNOMIAL_BAND_NM - step, at it, at it + step, 0). A node past the lens's
// coverage has zero radii. The polynomial transmission is 1 for the explicit lens; for the polynomial one it is the region's
// energy that clears the lens exactly over the energy the polynomial carries (the discs hold the pupil but are not it: a few
// % of the region is blocked at a rim), which every polynomial sample is weighted by, since the polynomial can't tell a
// blocked ray from a passing one. It is per wavelength band (the middle of each third of 380-780 nm), interpolated
// quadratically; all three are the d line's when monochromatic.
#define LENS_MAX_PUPIL_DISCS 4u
#define LENS_PUPIL_NODE_ROWS 3u
#define LENS_POLYNOMIAL_BAND_NM 580.0f
#define LENS_POLYNOMIAL_BAND_STEP_NM 133.333333f

// Distortion table: chief-ray image radius (mm) at field angles 0, step, 2 step, ..., four to a row.

// Self-test ray, 2 rows: (origin xyz mm, wavelength nm), (direction xyz, 0). Output, 6 rows per ray: explicit
// u(status, 0, 0, 0), (exit origin xyz, throughput), (exit direction xyz, 0); then, with a polynomial lens whose code the
// shader was compiled with, the canonical inputs it evaluated (r, dx, dy, lambda), (px, py, throughput, u(1)), (dx, dy, dz,
// 0): the raw polynomial outputs with the direction normalized (u(0) in the second row: not evaluated).
#define LENS_SELFTEST_INPUT_ROWS 2u
#define LENS_SELFTEST_OUTPUT_ROWS 6u
#define LENS_MAX_SELFTEST_RAYS 1024u

// Trace status, as LensCore's TraceStatus.
#define LENS_STATUS_OK 0u
#define LENS_STATUS_MISSED 1u
#define LENS_STATUS_CLIPPED 2u
#define LENS_STATUS_STOPPED 3u
#define LENS_STATUS_TIR 4u

// Spectral sampling range (uniform pdf), nm.
#define LENS_LAMBDA_MIN 380.0f
#define LENS_LAMBDA_MAX 780.0f

#endif // LENS_DATA_LAYOUT_H
