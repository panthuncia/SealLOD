#pragma once

// Controlled topology changes for CLod simplification (stage 5, docs/clod-simplification-validation.md). clusterlod.h owns the
// policy (when a group tries topology candidates, which seams they relax); this owns the texture check those candidates pass
// besides stage 1's validation, since crossing a seam can move texture without moving geometry or shading.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h"

namespace clod_detail {

	// The original's texcoords, beside the surface the validator indexed (the same vertices and triangles, in the same order).
	struct TextureCheckSource
	{
		const float* positions = nullptr;  // 3 floats per vertex, positionStride bytes apart
		size_t positionStride = 0;
		const float* uvs = nullptr;        // 2 floats per vertex, uvStride bytes apart
		size_t uvStride = 0;
		const uint32_t* indices = nullptr; // the original's triangles, as given to the validator
		size_t indexCount = 0;
		const uint32_t* charts = nullptr;  // follow-up: each original vertex's UV chart (ComputeTextureCharts), or null
	};

	// Stage 6 step 1: one displaced sample, for TextureCheckOptions::sampleWeight. UV scales are texcoord units per mesh unit (the
	// square root of the triangle's texcoord area over its area), so a footprint in mesh units times a scale is a footprint in UV.
	struct TextureSampleQuery
	{
		float outputUv[2] = {};       // the output's interpolated texcoord at the sample
		float outputUvScale = 0.0f;   // of the output triangle (0 when its texcoords are degenerate)
		bool matched = false;         // false: no original triangle with texcoord area within the radius (only for triangles mixing charts)
		float sourceUv[2] = {};       // the original's texcoord at the match
		float sourceUvScale = 0.0f;   // of the matched original triangle
	};

	// Stage 6 step 2: what a UV remap draws at one sample of a remapped output triangle (TextureCheckOptions::remap).
	struct TextureRemapSample
	{
		float uv[2] = {};        // the remapped texcoord
		uint32_t chart = 0;      // the chart it comes from (numbered as TextureCheckSource::charts)
		float uvScale = 0.0f;    // texcoord units per mesh unit of the remap there (0 when degenerate)
		// The correspondence the remap was built with, at the sample: what it should draw there. With it the sample is measured
		// against this rather than against MatchFacing's nearest point, so the remap is judged by what it cannot represent, not by
		// where two matchings disagree. hasReference false: the sample has no correspondence (not measured).
		bool hasReference = false;
		float referenceUv[2] = {};
		uint32_t referenceChart = 0;
		float referenceUvScale = 0.0f;
	};

	// Follow-ups to the slide test (each off by default), counted beside it.
	struct TextureCheckOptions
	{
		float slideTolerance = 0.0f;            // mesh units: the slide test (stage 5)
		float uvTolerance = 0.0f;               // UV units: a point whose texcoord differs from the original's by more is displaced (0: off)
		const uint32_t* outputCharts = nullptr; // each output vertex's chart (numbered as source.charts), or null: no chart test
		// Stage 6 step 1: when set, every displaced sample (every sample of a triangle mixing charts, matched or not) also adds its
		// area times this weight (0..1, the visible damage) to TextureDisplacement::weightedDisplacedArea. Nothing else changes.
		const std::function<float(const TextureSampleQuery&)>* sampleWeight = nullptr;
		// With sampleWeight: weigh every matched sample, displaced or not (unmatched samples of triangles mixing charts still count 1),
		// so weightedDisplacedArea is the colour damage of the whole output rather than of its displaced part.
		bool weighEverySample = false;
		// Stage 6 step 2: a UV remap of some output triangles (triangle = its position in output.indices / 3; weights barycentric).
		// When it returns true for a triangle's first sample, every sample of that triangle reads the remapped texcoord, and is
		// measured against the hook's reference (TextureRemapSample::hasReference) with the remapped sample's chart in the chart test
		// (the triangle no longer mixes charts). Null: none.
		const std::function<bool(size_t triangle, const float weights[3], TextureRemapSample& out)>* remap = nullptr;
		// The traversals of the matches, made by the caller at the same points (GatherNearJoint with maxDistance radius and the
		// validator's match slack): gather 4 * triangle + sample, sample in this function's order. Null: each sample matches itself.
		// A gather whose first is SimplifyTriangleBvh::kNotGathered was not kept: that sample matches itself.
		const SimplifyTriangleBvh::NearGather* matchGathers = nullptr;
		const SimplifyTriangleBvh::NearCandidate* matchPool = nullptr;
		size_t matchGatherCount = 0;
		// Build cost: area samples per triangle (AreaSampleTaken; 4 takes all). matchGathers keep their four slots per triangle.
		uint32_t samplesPerTriangle = 4u;
	};

	// Area totals from one MeasureTextureDisplacement call (mesh units squared).
	struct TextureDisplacement
	{
		double area = 0.0;          // the candidate's area
		double measuredArea = 0.0;  // of which matched to an original triangle with texture area
		double displacedArea = 0.0; // of which the texture slides farther than the tolerance
		double slideOverArea[4] = {}; // of which it slides farther than 1, 2, 4 and 8 times the radius (calibration)
		// Follow-ups (TextureCheckOptions), measured only when one is enabled; displacedArea is then the area any test displaces.
		double slideDisplacedArea = 0.0; // the slide test alone
		double uvDisplacedArea = 0.0;    // the UV-space (texel) test alone
		double uvOverArea[4] = {};       // of which the texcoord differs by more than 1, 4, 16 and 64 times uvTolerance (calibration)
		double chartArea = 0.0;          // the chart test alone: in a triangle mixing charts, or matched to another chart
		double mixedArea = 0.0;          // of which in triangles whose corners belong to different charts (matched or not)
		double weightedDisplacedArea = 0.0; // stage 6 step 1 (TextureCheckOptions::sampleWeight): displaced area times each sample's weight
	};

	// Samples output's triangles at four points each (the centroid and the points at weights (2/3, 1/6, 1/6) and permutations, a
	// quarter of the area each), matches each point to the original side-aware (SimplifyTriangleBvh::MatchFacing), and measures how
	// far the texture slides along the surface there: the difference between output's interpolated texcoord (outputUvs, 2 floats
	// per vertex output indexes) and the original's at the match, divided by the matched triangle's texcoord scale (the square
	// root of its texcoord area over its area). A point sliding farther than tolerance is displaced. Points without a match within
	// radius, or matched to a triangle without texcoord area, are not measured. Matching alone moves a point along the surface by
	// up to about its distance from the original (more over creases), so a tolerance of a few radii separates that from texture
	// actually crossing to another chart, which slides by the charts' separation. Sampled, not certified. Thread-safe.
	TextureDisplacement MeasureTextureDisplacement(
		const SimplifyValidator& original, const TextureCheckSource& source, const SimplifySurfaceView& output, const float* outputUvs, float radius,
		float tolerance);

	// The same, with the follow-up tests. A point also counts as displaced when options.uvTolerance > 0 and its texcoord differs
	// from the original's at the match by more than that (a texel-relative tolerance: texels over the texture's resolution), or
	// when options.outputCharts and source.charts are set and its triangle mixes charts or its chart differs from the matched
	// original triangle's (texture from another chart, however near it lies in mesh units). A triangle mixing charts counts
	// whole, matched or not. With the options at their defaults it measures exactly what the overload above does.
	TextureDisplacement MeasureTextureDisplacement(
		const SimplifyValidator& original, const TextureCheckSource& source, const SimplifySurfaceView& output, const float* outputUvs, float radius,
		const TextureCheckOptions& options);

	// UV charts: the connected components of triangles sharing a vertex, where vertices at the same position whose texcoords differ
	// by at most 1/1024 (relative above 1, stage 5's UV-seam rule) are the same vertex, so normal hard edges do not cut charts and UV
	// seams do. Each vertex gets the lowest vertex index of its chart. Deterministic.
	std::vector<uint32_t> ComputeTextureCharts(
		const float* positions, size_t positionStride, const float* uvs, size_t uvStride, size_t vertexCount, const uint32_t* indices, size_t indexCount);

}
