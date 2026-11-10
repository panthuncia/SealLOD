#ifndef LENS_DATA_LAYOUT_H
#define LENS_DATA_LAYOUT_H

// The physical lens's GPU data (Builtin::RayTracing::LensData): one StructuredBuffer<float4>, built by LensManager
// (src/Camera/Lens) and read by lensCommon.hlsli. Included by both C++ and HLSL, so macros only. Rows are float4; "u" rows
// hold uints (asuint/asfloat). Lens units are millimetres, optical axis +z, surface 0 nearest the scene at z = 0.

#define LENS_DATA_MAGIC 0x534E454Cu // "LENS": a buffer without it (the fallback, or nothing yet) is a pinhole camera.

// Row 0 (u): magic, surfaceCount, stopIndex, flags.
// Row 1 (u): mediumCount, pupilNodeCount, distortionCount, selfTestRayCount.
// Row 2: sensorZ of focus node 0 (the far end of the focus range; the focused plane is per frame, LensLoadFocus), rearPlaneZ,
// frontPlaneZ, stopScale.
// Row 3: maxSensorRadius (the pupil nodes span [0, it]), distortionStep (radians between table entries), 1/K of focus node 0
// (per frame: LensInverseNormalization), design wavelength nm.
// Row 4 (u): row offsets of the surfaces, media, pupil nodes and distortion table.
// Row 5 (u): row offset of the self-test rays, revision low word, pupil disc count, asuint(covered field of focus node 0): the
// half-field (radians) the lens images with usable light, which LENS_FLAG_FIELD_REMAP fits the frame's diagonal to (per focus
// node in the focus node rows).
// Row 6 (u): with LENS_FLAG_POLYNOMIAL, the polynomial's key low and high words (the hash of the code LensManager generated
// for it; a shader compiled with other code traces the surfaces instead), its degree, 0.
// Row 7: with LENS_FLAG_POLYNOMIAL, its domain: the sensor radius it is used to (at most the image circle), lambda min, max (nm), rear half
// extent (mm).
// Row 8: the focus range: the infinity-focus sensor plane (the prescription's), the focus shifts of the first and last focus
// node (mm, added to it; the nodes are uniform in shift between them), the near focus limit (mm from the front vertex).
// Row 9 (u): focus node count, row offset of the focus nodes, row offset and entry count of the focus table.
// Row 10 (u): row offset and entry count of the normalization table, 0, 0.
#define LENS_ROW_HEADER0 0u
#define LENS_ROW_HEADER1 1u
#define LENS_ROW_HEADER2 2u
#define LENS_ROW_HEADER3 3u
#define LENS_ROW_HEADER4 4u
#define LENS_ROW_HEADER5 5u
#define LENS_ROW_HEADER6 6u
#define LENS_ROW_HEADER7 7u
#define LENS_ROW_HEADER8 8u
#define LENS_ROW_HEADER9 9u
#define LENS_ROW_HEADER10 10u
#define LENS_HEADER_ROWS 11u

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

// Focus nodes (per-frame focus, SARP's docs/lens_simulation.md "Focus and autofocus"): every focus-dependent table is built at
// LENS_FOCUS_NODES sensor planes uniform in the focus shift from shift min to shift max (header row 8), and the shader
// interpolates between the two nearest for the frame's focus (LensFocus). One row per node: (sensorZ, 1/K, covered field
// (radians), focus distance mm (0 at infinity)).
#define LENS_FOCUS_NODE_ROWS 1u

// Normalization table: 1/K at focus coordinates uniform from 0 to the last focus node (LENS_NORMALIZATION_PER_INTERVAL entries
// per focus interval), four to a row; 1 / sqrt(K) is interpolated linearly between entries. K bends between focus nodes (up to
// 0.2% off a line between 8 nodes for the double Gauss) and costs little to integrate, so it is sampled finer than the pupil.
#define LENS_NORMALIZATION_PER_INTERVAL 8u
#define LENS_MAX_FOCUS_NODES 32u

// Focus table (autofocus): the focus shift (mm) for object distances uniform in 1 / distance, from infinity (entry 0, shift
// 0) to the near focus limit (the last entry), four to a row. The autofocus pass reads it to turn a measured depth into a shift.

// Pupil nodes: the exact pupil (LensCore ComputePupilDiscs). Per node, the discs whose intersection is the region of the
// rear plane that camera rays from that sensor radius clear the lens through, in the canonical frame (sensor point on +x,
// so every centre is on the x axis). Nodes sit at sensor radii i * maxSensorRadius / (nodes - 1) and are interpolated
// linearly; one set of nodes per focus node, focus node major, interpolated bilinearly (the discs are widened so that the region
// holds the pupil between focus nodes too, LensCore ComputeFocusedPupilDiscs). LENS_PUPIL_NODE_ROWS rows per node: (centre0, radius0, centre1, radius1), (centre2, radius2, centre3,
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

// Distortion table: chief-ray image radius (mm) at field angles 0, step, 2 step, ..., four to a row; one table per focus node,
// each starting on a row (ceil(count / 4) rows apart). The chief rays don't depend on focus, and each table propagates them to
// its node's plane, so interpolating two nodes' tables is exact.

// Per-frame focus (Builtin::RayTracing::LensFocus): a RWStructuredBuffer<float4> LensFocusPass writes before the path tracer
// and LensAutofocusPass after it, which the camera rays read. It is never rebuilt, so a focus change is not a lens build.
// Row 0: this frame's focus: shift (mm), sensorZ, focus coordinate (0..nodes-1), the spring's latched target shift.
// Row 1: the previous frame's: shift, sensorZ, coordinate, 0 (for motion vectors).
// Row 2: autofocus: spring velocity (mm/s), 0, measured target distance (mm, 0: none), its shift.
// Row 3 (u): LENS_FOCUS_MAGIC once initialized, restart (1 when the focus moved this frame: accumulation restarts), epoch (focus
// moves so far), the lens identity it was initialized for.
// Row 4: the autofocus measurement: valid sample fraction, log2 depth spread of the samples used (octaves), samples used, 0.
// Row 5: this frame's sensor frame at its focus and the camera's unjittered projection (LensSensorHalfExtent): half extent
// x, y, field scale, and 1/K at the focus (LensInverseNormalization); row 6 (u in w): the previous frame's, at its focus and
// projection, w the lens revision's low word both rows were computed for. The camera rays and motion vectors read these
// instead of interpolating the tables per ray; a revision other than the lens data's (the first frame of a new lens) computes
// them in place.
#define LENS_FOCUS_ROW_CURRENT 0u
#define LENS_FOCUS_ROW_PREVIOUS 1u
#define LENS_FOCUS_ROW_SPRING 2u
#define LENS_FOCUS_ROW_STATE 3u
#define LENS_FOCUS_ROW_MEASURE 4u
#define LENS_FOCUS_ROW_FRAME 5u
#define LENS_FOCUS_ROW_PREVIOUS_FRAME 6u
// From row 7: the exact pupil's nodes interpolated to this frame's focus (LENS_PUPIL_NODE_ROWS each, as the lens data's), so a
// camera ray interpolates two radius nodes, not four; valid with rows 5 and 6.
#define LENS_FOCUS_ROW_PUPIL 7u
#define LENS_FOCUS_MAX_PUPIL_NODES 64u
#define LENS_FOCUS_ROWS (LENS_FOCUS_ROW_PUPIL + LENS_FOCUS_MAX_PUPIL_NODES * LENS_PUPIL_NODE_ROWS)
#define LENS_FOCUS_MAGIC 0x53434F46u // "FOCS"
#define LENS_FOCUS_MODE_MANUAL 0u
#define LENS_FOCUS_MODE_AUTO 1u

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
