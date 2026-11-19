#pragma once

// Builds the UV remap blocks of stage 6 step 2 (docs/clod-appearance-stage6.md, ClusterLODUvRemap.h): for each damaged triangle of
// a coarse cluster, source UVs on a barycentric grid, each node matched to the original surface along the triangle's normal.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <BasicRenderer/Assets/ClusterLODUvRemap.h>

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODTopology.h"

namespace clod_detail {

	struct UvRemapSettings
	{
		float texel = 4.0f;          // grid spacing in group errors
		uint32_t maxNodes = 17u;     // nodes per edge at most
		float sourceTexelUv = 1.0f / 512.0f; // the spacing is never finer than this many UV units on the matched original
		float slide = 8.0f;          // a triangle is remapped where its texture slides farther than this many group errors (or crosses charts)
		// 1: the step 2 grid (nodes matched by the nearest crossing along the normal). 2: the refinement: chart regions with exact
		// affine maps and a line per grid cell between two of them, matched by the frontmost crossing (what a viewer sees), with the
		// grid only where the regions do not fit; the residual is measured against the frontmost crossing at each sample.
		uint32_t layout = 1u;
		float cellMiss = 0.01f; // layout 2: the coarsest cell grid missing at most this share of a triangle is taken
	};

	// One triangle of the cluster in its stored order (corners as the page stores them).
	struct UvRemapTriangle
	{
		float position[3][3] = {};
		float uv[3][2] = {};     // the coarse corners' UV set 0
		uint32_t chart[3] = {};  // their charts (numbered as TextureCheckSource::charts)
		bool valid = true;       // false: not measured (no remap)
	};

	// A point's correspondence on the original.
	struct UvRemapMatch
	{
		bool found = false;
		bool alongNormal = false; // by the line along the normal (else the nearest same-facing point)
		float uv[2] = {};
		uint32_t chart = 0;
		float scale = 0.0f;       // UV units per mesh unit of the matched original triangle
	};

	// The correspondence of a point of a coarse triangle (face: its unit normal): the nearest original triangle crossed by the line
	// through the point along the normal, among those facing the same way (within 60 degrees) and within reach, so a point stays
	// on the sheet it lies over where the nearest point would jump between close layers (thin, layered or curved geometry), as
	// cage-ray texture bakers do. Without such a crossing, the nearest same-facing point within twice the reach. reach should
	// cover how far the coarse surface lies from the original (its measured deviation). slots is scratch.
	UvRemapMatch MatchUvRemapPoint(const SimplifyValidator& original, const TextureCheckSource& source, const float point[3], const float face[3],
		float reach, std::vector<uint32_t>& slots);

	// Layout 2's correspondence: the frontmost original triangle crossed by the line along the normal within reach either way, facing
	// the same way within 60 degrees (SimplifyTriangleBvh::FrontmostCrossing): what a viewer looking at the coarse surface sees of
	// the original, so where a frame and a recessed panel both lie within reach the frame wins wherever it covers. Without such a
	// crossing, the nearest same-facing point within twice the reach.
	UvRemapMatch MatchUvRemapFrontmost(const SimplifyValidator& original, const TextureCheckSource& source, const float point[3], const float face[3],
		float reach);

	struct UvRemapCluster
	{
		std::vector<std::byte> block;                  // empty when no triangle needed a remap
		std::vector<std::vector<uint32_t>> nodeCharts; // per triangle, each node's chart (empty when not remapped)
		std::vector<UvRemapTriangle> triangles;        // the input, for references at samples
		float reach = 0.0f;
		uint32_t trianglesRemapped = 0;
		uint64_t nodes = 0;
		uint64_t nodesUnmatched = 0;   // filled from a matched neighbour
		uint64_t nodesAlongNormal = 0; // matched by the ray along the triangle's normal (the rest by the nearest facing point)
		uint32_t trianglesOverBudget = 0; // needed one but did not fit the byte budget
		uint64_t subTriangles = 0;
		uint64_t subTrianglesStraddlingCharts = 0; // nodes from more than one chart (extrapolated, not interpolated)
		uint64_t subTrianglesStraddlingSeams = 0;  // one chart, but UVs too far apart to interpolate (a seam inside the chart)
		// Layout 2.
		uint32_t layout = 1u;
		std::vector<std::vector<uint32_t>> regionCharts; // per triangle, each region's chart (empty unless a region triangle)
		uint32_t trianglesRegions = 0;     // remapped as region triangles
		uint32_t trianglesGrid = 0;        // remapped on the grid (regions did not fit)
		uint64_t regions = 0;              // regions over the region triangles
		uint64_t regionCells = 0;          // cells holding two regions
		double regionArea = 0.0;           // area of the region triangles
		double regionMissArea = 0.0;       // of it, where the cells' lines give another region than the correspondence (estimated)
		uint64_t pointsUnlabeled = 0;      // lattice points without a frontmost crossing (filled from neighbours)
		uint64_t regionsFitted = 0;        // regions holding a least-squares affine fit (their chart is not one planar mapping here)
		uint32_t trianglesRegionsRejected = 0; // regions did not fit (too many, or a chart no affine map holds within a texel): node grid
		uint64_t regionsPlanarPieces = 0;  // regions added by cutting a chart into planar pieces
		std::array<uint64_t, 5> regionGridCounts{}; // multi-region triangles by cell grid: 2, 3, 5, 9, 17 nodes per edge
	};

	// A triangle gets a remap when, at the texture term's sample points, its corners mix charts or its interpolated UV is in another
	// chart than its correspondence's or slides farther than settings.slide times radius (the group's error). Its grid spacing is
	// settings.texel times radius, never finer than a source texel, with 4 to settings.maxNodes nodes per edge. reach: the match reach
	// (MatchUvRemapPoint). byteBudget: the most the block may take (what the cluster's page can still hold); over it, grids get
	// coarser (down to 4 nodes per edge) and then the smallest triangles go without.
	UvRemapCluster BuildClusterUvRemap(
		const SimplifyValidator& original, const TextureCheckSource& source, const UvRemapSettings& settings,
		std::span<const UvRemapTriangle> triangles, float radius, float reach, uint64_t byteBudget);

	// A remapped sample for TextureCheckOptions::remap: the block evaluated at the triangle's barycentrics, its chart from the
	// cluster's node charts, its UV scale over the triangle's area, and the reference (the same correspondence at the sample).
	// False when the triangle has no remap.
	bool EvaluateClusterUvRemap(
		const UvRemapCluster& cluster, const SimplifyValidator& original, const TextureCheckSource& source, uint32_t triangle,
		const float weights[3], std::vector<uint32_t>& slots, TextureRemapSample& out);

}
