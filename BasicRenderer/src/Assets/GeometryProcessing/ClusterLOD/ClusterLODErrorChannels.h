#pragma once

// Error channels for CLod selection (stage 3, docs/clod-simplification-validation.md): every group's simplified output measured
// against the original surface after the build, as two channels kept apart from meshoptimizer's quadric error, made monotone
// through the DAG so the runtime cut stays consistent. The traversal hierarchy stores them in its nodes; the runtime combines them
// with the quadric error by view settings (clodLodGeometricChannel, clodLodAppearanceWeight).

#include <array>
#include <cstddef>
#include <cstdint>

#include <BasicRenderer/Assets/ClusterLODTextureColor.h>

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODTopology.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODUvRemapBuilder.h"

namespace clod_detail {

	struct ErrorChannelStats
	{
		uint32_t groups = 0;               // groups with a simplified output (not terminal)
		uint64_t simplifiedTriangles = 0;  // triangles of those outputs, all measured
		uint64_t samples = 0;              // appearance samples taken on them
		uint64_t samplesWithoutNormal = 0; // of which the original had no usable normal at the match (skipped)
		uint64_t sourceTriangles = 0;      // original triangles followed up the DAG
		uint32_t raisedOverQuadric = 0;    // groups whose monotone geometric channel exceeds their quadric error
		uint32_t envelopeRaisedGeometric = 0; // groups whose geometric channel the monotone envelope raised above their own measurement
		uint32_t envelopeRaisedAppearance = 0;
		// Stage 6 texture channel (ErrorChannelTextureInputs given): area of the simplified outputs measured, and of it the area
		// whose texture is displaced (mesh units squared, summed over groups), and groups with any displaced area.
		double textureMeasuredArea = 0.0;
		double textureDisplacedArea = 0.0;
		uint32_t textureGroupsDisplaced = 0;
		// Stage 6 step 1 (ErrorChannelTextureInputs::color): the displaced area weighted by colour difference, which is then Q_T.
		double textureWeightedArea = 0.0;
		uint64_t textureColorSamples[5] = {}; // weighed samples by dE: under 1, 3, the reference, at or over it, unmatched (count 1)
		// Stage 6 step 2 (ErrorChannelTextureInputs::remap): groups given UV remap blocks, what they hold, and those groups' Q_T and
		// binary displaced area before and after (through the remap).
		uint32_t remapGroups = 0;
		uint32_t remapClusters = 0;
		uint64_t remapTriangles = 0;
		uint64_t remapNodes = 0;
		uint64_t remapNodesUnmatched = 0;
		uint64_t remapNodesAlongNormal = 0;
		uint64_t remapBytes = 0;
		uint64_t remapTrianglesOverBudget = 0; // needed one but did not fit their page
		uint64_t remapSubTriangles = 0, remapSubTrianglesCharts = 0, remapSubTrianglesSeams = 0; // straddling charts / seams
		double remapDisplacedBefore = 0.0;
		double remapDisplacedAfter = 0.0;
		double remapQBefore = 0.0;
		double remapQAfter = 0.0;
		// Of the displaced areas: the slide test, the chart test, and triangles mixing charts (before, after).
		double remapSlideBefore = 0.0, remapSlideAfter = 0.0, remapChartBefore = 0.0, remapChartAfter = 0.0, remapMixedBefore = 0.0, remapMixedAfter = 0.0;
		double remapMs = 0.0;
		// Layout 2 (chart regions): triangles remapped as regions / on the grid, regions, two-region cells, region triangle area and
		// the share of it the cells' lines give another region (estimated), lattice points without a frontmost crossing.
		uint64_t remapTrianglesRegions = 0, remapTrianglesGrid = 0, remapRegions = 0, remapRegionCells = 0, remapPointsUnlabeled = 0;
		uint64_t remapRegionsFitted = 0, remapTrianglesRegionsRejected = 0, remapRegionsPlanarPieces = 0;
		std::array<uint64_t, 5> remapRegionGridCounts{}; // multi-region triangles by cell grid (2, 3, 5, 9, 17 nodes per edge)
		double remapRegionArea = 0.0, remapRegionMissArea = 0.0;
		// Stage 6 step 3 (normal spread): clusters given a block, their vertices and bytes, the simplified area sampled and the
		// area integral of |n_coarse - n_source|^2 over it (so the mean v is spreadQ / spreadArea), and vertices by stored byte.
		uint32_t spreadClusters = 0;
		uint64_t spreadVertices = 0;
		uint64_t spreadBytes = 0;
		double spreadArea = 0.0;
		double spreadQ = 0.0;
		uint64_t spreadVertexBands[4] = {}; // byte 0, 1-15 (v < 0.0035, under ~3.4 degrees), 16-63 (v < 0.06, ~14 degrees), 64-255
		double spreadMs = 0.0;
		double textureMs = 0.0;
		double measureMs = 0.0;
	};

	// Stage 6 step 1 (lodErrorChannelsTextureColor): the material's base colour, to weight each displaced sample by the colour
	// difference it draws: min(1, dE / referenceDeltaE), dE the CIE76 difference of factor times texture (linear light) at the
	// simplified texcoord and at the original's, each filtered at the mip where the group's error (the validator's radius) is one
	// texel of its own triangle's texcoord scale. Masked materials: a sample whose alpha falls on the other side of the cutoff
	// counts 1, and one invisible in both counts 0. Unmatched samples of triangles mixing charts count 1.
	struct ErrorChannelTextureColor
	{
		std::array<float, 4> baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
		const clod_texture::Texture* texture = nullptr; // null: the factor alone (uniform colour, so crossings cost nothing)
		bool alphaMasked = false;
		float alphaCutoff = 0.5f;
		float referenceDeltaE = 10.0f;
		float footprint = 1.0f; // the filter footprint in group errors (validator radii)
		bool everySample = false; // weigh every matched sample, not only displaced ones (TextureCheckOptions::weighEverySample)
		// The material's texture transform (ClusterLODMaterialAppearance::baseColorUvTransform), applied to both UVs of a sample
		// before the texture is read; off: the UVs as they are.
		bool uvTransformed = false;
		std::array<float, 6> uvTransform{ 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	};

	// Stage 6 (docs/clod-appearance-stage6.md): what the texture term of the appearance channel needs. The original's UV set 0 and
	// charts beside the validator's original surface, and the build vertices' UV set 0 and charts (indexed by the ids in
	// ClusterLODBuildState::groupVertexGlobalIdChunks, which must be held).
	// Stage 6 step 2 (lodErrorChannelsTextureRemap): which groups get UV remap blocks, and how they are built.
	// The visible damage of one displaced sample: min(1, dE / color.referenceDeltaE), each colour filtered at footprint mesh units
	// (ErrorChannelTextureColor). Also stage 6 step 4's colour-weighted chart test.
	float ColorSampleWeight(const ErrorChannelTextureColor& color, const TextureSampleQuery& query, float footprint);

	struct ErrorChannelTextureRemap
	{
		float minDisplacedShare = 0.0f; // a group whose output has at least this share of its area displaced gets remaps
		UvRemapSettings settings;
	};

	struct ErrorChannelTextureInputs
	{
		TextureCheckSource source;          // the original: positions, UV set 0, indices (as the validator's), charts
		const float* buildUvs = nullptr;    // 2 floats per build vertex, buildUvStride bytes apart
		size_t buildUvStride = 0;
		size_t buildVertexCount = 0;
		const std::vector<uint32_t>* buildCharts = nullptr; // per build vertex
		float weight = 0.0f;                // Q_T's weight against Q_N
		float slide = 8.0f;                 // slide tolerance in group errors
		const ErrorChannelTextureColor* color = nullptr; // stage 6 step 1: weight displaced samples by colour (null: each counts 1)
		const ErrorChannelTextureRemap* remap = nullptr; // stage 6 step 2: remap damaged groups (null: none)
	};

	// Fills state.errorChannels for every group from the raw per-group streams (state.groupVertexChunks and the meshlet chunks,
	// which must still be held). original is the untouched source; vertexStrideBytes the stride of the group vertex records (the
	// position at its start, the normal at MeshVertexLayout::NormalOffset when hasNormals).
	//
	// For group g, S(g) is every cluster whose refined group is g, in whichever group it was placed: the representation that
	// replaces g's clusters at a cut. Its channels:
	//  - geometric: the larger of the farthest S(g) sample (four area points and the corners per triangle) from the original
	//    surface, and the farthest original triangle owned by g (its centroid) from S(g). Ownership follows each original
	//    triangle up the DAG: it starts with the leaf group holding it and passes, at each group, to the group holding the
	//    nearest S(g) triangle. Sampled maxima, not certified bounds.
	//  - appearance: sqrt(Q_N), Q_N = sum over S(g)'s area samples of area * |n_coarse - n_source|^2, with decoded normals
	//    (octahedral snorm16, as the pages store them) interpolated and normalised, against the original's interpolated normal at
	//    the side-aware match. A coarse normal that cancels counts as chord^2 4. Mesh units: projected like a distance it is
	//    sqrt(p^2 Q_N), the root of the screen-integrated squared Lambertian shading error at p pixels per unit.
	// Then each channel becomes max(own, children's) in child-to-parent order, so a parent never selects below a child. A terminal
	// group (no S) gets geometric FLT_MAX, as its quadric error is.
	//
	// With texture (stage 6), the appearance channel is sqrt(Q_N + weight * Q_T), Q_T the area of S(g) whose UV set 0 texture is
	// displaced against the original (MeasureTextureDisplacement with the chart test and a slide tolerance of slide times g's error):
	// texture interpolated across charts, from another chart, or slid farther than the geometry explains. Like Q_N it is an
	// integrated squared intensity error (a displaced point counts as error 1), so it projects the same way.
	//
	// With texture->remap (stage 6 step 2), a group whose S(g) has at least minDisplacedShare of its measured area displaced gets a
	// UV remap block for each of its clusters with a damaged triangle (BuildClusterUvRemap), stored in state.meshletUvRemaps, and
	// its Q_T is measured again through the remap: what the remap cannot resolve (sub-triangles straddling a chart boundary, and
	// correspondence the matching gets wrong) is the damage left for selection.
	//
	// With normalSpread (stage 6 step 3, lodNormalSpread), every simplified cluster (a meshlet whose refined group is not -1) gets a
	// normal spread block (ClusterLODNormalSpread.h) in state.meshletNormalSpreads: each of its vertices takes the hat-weighted
	// area mean of |n_coarse - n_source|^2 over the area samples above of the triangles around it in its holding group's simplified
	// clusters, encoded as sqrt(min(v, 1)) in a byte. Each sample's |n_coarse - n_source|^2 is averaged over the source a pixel
	// covers where S(g) starts to draw: a disc of radius normalSpreadFootprint / 2 times g's error around the sample in the triangle's
	// plane (7-point disc rule; 0: the sample point alone), each point's |n_coarse - n_source|^2 first capped at normalSpreadSampleCap
	// (0: no cap), so a few normals far off (another sheet, a crease's other face) do not dominate what a single lobe can stand for.
	// Clusters whose every byte is 0 get none. measureChannels false (the spread
	// without lodErrorChannels) skips what only the channels need (the original's ownership up the DAG) and leaves
	// state.errorChannels empty, so nodes and metadata are as without channels.
	ErrorChannelStats MeasureClusterLODErrorChannels(
		ClusterLODBuildState& state, const SimplifyValidator& original, size_t vertexStrideBytes, bool hasNormals,
		const ErrorChannelTextureInputs* texture = nullptr, bool normalSpread = false, bool measureChannels = true,
		float normalSpreadFootprint = 1.0f, float normalSpreadSampleCap = 0.1f, uint32_t samplesPerTriangle = 4u);

}
