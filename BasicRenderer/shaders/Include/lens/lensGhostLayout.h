#ifndef LENS_GHOST_LAYOUT_H
#define LENS_GHOST_LAYOUT_H

// Ghost lens flares (GhostFlareManager, src/Camera/Lens; SARP's docs/lens_simulation.md "Ghost flares"). Included by C++ and
// HLSL, so macros only. Every buffer is a StructuredBuffer<float4>; "u" rows hold uints (asuint/asfloat). Lens units are
// millimetres, as lensDataLayout.h's.

// ---- Ghost data ("Builtin::LensFlare::Ghosts"), built by GhostFlareManager for one lens revision.
// Row 0 (u): magic, ghost count, field nodes, the lens revision's low word (lensDataLayout.h header row 5 y).
// Row 1: max field (radians: sources up to this angle from the axis, in lens space, make ghosts), field step (radians), 0, 0.
// Per ghost, LENS_GHOST_ROWS_PER_GHOST(fieldNodes) rows: u(a, b, 0, 0) (reflect at surface a, then at b < a), then per field
// node i (angle i * step) the box on the front plane that holds the rays this ghost carries to the sensor, in the canonical
// frame (the source's light travelling toward +x): (x min, x max, y max, 0), mirrored in y. A node no ray passes has x min >
// x max.
#define LENS_GHOST_MAGIC 0x54534847u // "GHST": without it (the fallback) no ghost is traced.
#define LENS_GHOST_HEADER_ROWS 2u
#define LENS_GHOST_ROWS_PER_GHOST(fieldNodes) (1u + (fieldNodes))
#define LENS_GHOST_MAX_GHOSTS 32u
#define LENS_GHOST_FIELD_NODES 17u

// ---- Per-frame sources ("Builtin::LensFlare::Sources"), written by lensFlareSources.hlsl, read by the trace and the draw.
// Row 0: sensor half extent x, y (mm), sensor z (the focused plane, mm), 1/K.
// Row 1 (u): source count (slots used, valid or not; 0 when inactive), wavelength count (slots), grid resolution (cells per
// side), flags.
// Row 2: field scale k, the artistic intensity, the smallest sensor area a cell may divide its energy by (mm^2), the edge
// softness (in the stop and rim ratios).
// Row 3 (u): the self-test sequence (0 when not testing), the lens revision's low word, the ghost data's lens revision, the
// ghost slots (instances are laid out by them).
// Rows 4.. LENS_FLARE_MAX_WAVELENGTHS: per wavelength (rgb weight, lambda nm). The weights of all wavelengths sum to (1, 1, 1).
// Then per source slot LENS_FLARE_SOURCE_ROWS rows:
//   (lens-space travel direction xyz of a parallel source, or the lens-space position (mm) of a point source; w 0 parallel,
//   1 point), (irradiance rgb at the camera on a plane facing the source, times visibility, times the intensity; visibility),
//   u(valid, light index, 0, 0) with asfloat (cos, sin of the canonical azimuth, the lens-space field angle) in yzw.
#define LENS_FLARE_ROW_FRAME0 0u
#define LENS_FLARE_ROW_FRAME1 1u
#define LENS_FLARE_ROW_FRAME2 2u
#define LENS_FLARE_ROW_FRAME3 3u
#define LENS_FLARE_ROW_WAVELENGTHS 4u
#define LENS_FLARE_MAX_WAVELENGTHS 8u
#define LENS_FLARE_ROW_SOURCES (LENS_FLARE_ROW_WAVELENGTHS + LENS_FLARE_MAX_WAVELENGTHS)
#define LENS_FLARE_SOURCE_ROWS 3u
#define LENS_FLARE_MAX_SOURCES 8u // the sun and up to 7 local lights
#define LENS_FLARE_SOURCES_ROWS (LENS_FLARE_ROW_SOURCES + LENS_FLARE_MAX_SOURCES * LENS_FLARE_SOURCE_ROWS)

#define LENS_FLARE_SOURCE_PARALLEL 0.0f
#define LENS_FLARE_SOURCE_POINT 1.0f

// Row 1 w flags.
#define LENS_FLARE_FLAG_SELF_TEST 1u // source 0 is the self-test's: parallel, unit irradiance, always visible

// ---- Grid vertices ("Builtin::LensFlare::Vertices"), written by the trace, read by the draw. Per instance (source slot s,
// ghost g, wavelength w; index (s * ghosts + g) * wavelengths + w) one header row and one row per vertex of the
// (grid + 1)^2 lattice, row-major in the canonical frame's (x, y):
//   header: (entrance area of one cell on the front plane (mm^2), lambda nm, u(valid), 0)
//   vertex: (sensor x, y (mm), throughput (the Fresnel product) times the cosine of the ray to the axis in front of the
//   lens, u(f16 stop ratio | f16 rim ratio << 16)), or u(LENS_FLARE_VERTEX_FAILED) in w when the trace failed.
// The stop ratio is the largest radius at which the ray crossed the stop over the stop's (scaled) radius, the rim ratio the
// largest radius at a refracting or reflecting surface over its clear radius: above 1 the lens blocks the ray. The trace
// doesn't clip, so these interpolate across a cell and the draw cuts the aperture's shape inside it.
#define LENS_FLARE_INSTANCE_HEADER_ROWS 1u
#define LENS_FLARE_VERTEX_ROWS 1u
#define LENS_FLARE_INSTANCE_ROWS(grid) (LENS_FLARE_INSTANCE_HEADER_ROWS + ((grid) + 1u) * ((grid) + 1u) * LENS_FLARE_VERTEX_ROWS)
#define LENS_FLARE_VERTEX_FAILED 0xFFFFFFFFu

// Trace status, as lensDataLayout.h's (LENS_STATUS_*); the ghost never reports CLIPPED or STOPPED (it records ratios).
#define LENS_FLARE_STATUS_BEHIND 8u // the exit ray doesn't reach the sensor plane

#define LENS_FLARE_TRACE_GROUP 64u
#define LENS_FLARE_SOURCES_GROUP 64u
#define LENS_FLARE_VISIBILITY_RAYS 16u

// Root constants. Sources: the RT slots (TLAS 0, CLAS info 1, instance rows 13) as clodRayTracingSetupRootConstants.h's
// secondary rays, then these.
#define LENS_FLARE_RC_SOURCES_UAV UintRootConstant2
#define LENS_FLARE_RC_DEPTH_SRV UintRootConstant3
#define LENS_FLARE_RC_MAX_LIGHTS UintRootConstant4
#define LENS_FLARE_RC_WAVELENGTHS UintRootConstant5
#define LENS_FLARE_RC_GRID UintRootConstant6
#define LENS_FLARE_RC_INTENSITY UintRootConstant7 // asfloat
#define LENS_FLARE_RC_SELF_TEST UintRootConstant8 // sequence, 0 none
#define LENS_FLARE_RC_TARGET_WIDTH UintRootConstant9
#define LENS_FLARE_RC_TARGET_HEIGHT UintRootConstant10
#define LENS_FLARE_RC_TEST_SOURCE UintRootConstant11 // 1: the pipe's test source replaces the scene's
#define LENS_FLARE_RC_TEST_X UintRootConstant12 // asfloat: view-space direction toward it
#define LENS_FLARE_RC_TEST_Y UintRootConstant14
#define LENS_FLARE_RC_TEST_Z UintRootConstant15
#define LENS_FLARE_RC_TEST_DISTANCE UintRootConstant16 // asfloat metres, 0 at infinity
#define LENS_FLARE_RC_TEST_R UintRootConstant17
#define LENS_FLARE_RC_TEST_G UintRootConstant18
#define LENS_FLARE_RC_TEST_B UintRootConstant19
#define LENS_FLARE_RC_GHOSTS_SLOTS UintRootConstant20 // the ghost slots the trace and draw lay instances out by
// Trace and draw.
#define LENS_FLARE_RC_VERTICES UintRootConstant2 // UAV (trace) or SRV (draw)
#define LENS_FLARE_RC_SOURCE_SLOTS UintRootConstant3
#define LENS_FLARE_RC_GHOSTS UintRootConstant4
// Composite.
#define LENS_FLARE_RC_FLARE_SRV UintRootConstant2

#endif // LENS_GHOST_LAYOUT_H
