#pragma once

// Optimal-placement simplification for CLod (stage 4, docs/clod-simplification-validation.md): an edge-collapse loop derived
// from meshoptimizer 1.3's (simplifier.cpp, meshopt_simplifyEdge) that solves each candidate collapse's replacement position and
// wedge attributes while ranking, costs the candidate at that position, and validates its neighbourhood against the solved
// position, including a persistent orientation check against the source sheet. clusterlod.h owns the policy (it offers this as
// an update candidate before stage 2's); stage 2 (ClusterLODReplacementVertices) fits the normals and commits the vertices.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h"

struct clodMesh;
struct clodOptimalRequest;

namespace clod_detail {

	// Placements a collapse may offer besides its endpoints (OptimalSimplifyParams::moves).
	enum OptimalMove : unsigned int
	{
		OptimalMove_BoundaryLine = 1u, // a border or seam pair along its own edge
		OptimalMove_InteriorLine = 2u, // a manifold or complex target along the edge
		OptimalMove_InteriorFree = 4u, // a manifold or complex target at its free solve (within clampScale of the edge)
	};

	// Experimental values (ClusterLODBuilderSettings::simplifyOptimal*), not production constants.
	struct OptimalSimplifyParams
	{
		float flipCos = 0.25f;     // a surviving face rotated by one collapse to or below this cosine rejects it (meshopt's ~75.5 degrees)
		float sheetCos = 0.5f;     // a surviving face meeting its source-sheet reference at or below this cosine rejects the collapse,
		                           // unless the collapse leaves it no worse than it was (60 degrees)
		float clampScale = 0.5f;   // a free solved position farther from its edge than this many edge lengths is not offered
		bool preserveFolds = false; // meshopt_SimplifyPreserveFolds' fold quadrics (comparison only; not a fold prohibition)
		unsigned int moves = 7;    // placements offered besides the endpoints (OptimalMove bits)
	};

	// Counters from SimplifyOptimal, summed per call.
	struct OptimalSimplifyStats
	{
		uint64_t collapses = 0;         // collapses performed
		uint64_t movedFree = 0;         // of which to a solved position off the edge's line
		uint64_t movedLine = 0;         // of which to a solved position strictly inside the edge (on its line)
		uint64_t fallbacks = 0;         // of which the second-ranked placement, after the first failed validation
		uint64_t rejectedFlip = 0;      // placements rejected: a face rotated beyond flipCos in one collapse (or degenerate)
		uint64_t rejectedSheet = 0;     // placements rejected: a face turned beyond sheetCos from its source-sheet reference
		uint64_t solveFailed = 0;       // free solves that were singular or not finite
		uint64_t solveClamped = 0;      // free solves too far from their edge
		uint64_t rejectedVanish = 0;    // moved placements rejected: the collapse deletes what is left of a component (endpoints only)
	};

	// The collapse loop. Private, dense buffers: positions (3 floats per vertex) and attributes (attributesStride floats per vertex,
	// the first attributeCount weighted by weights) are updated in place where a vertex moved; indices are replaced by the result,
	// whose count is returned. locks are meshopt_SimplifyVertex_* flags. triangleReference (optional) holds, per input triangle, an
	// area-weighted orientation of the source surface under it (3 floats); without it the input's own faces are the reference.
	// options are meshopt_Simplify* (Permissive, Regularize, ErrorAbsolute, PreserveFolds honoured). The error is absolute when
	// meshopt_SimplifyErrorAbsolute is set, as meshoptimizer reports it.
	size_t SimplifyOptimal(
		unsigned int* indices, size_t indexCount, float* positions, size_t vertexCount,
		float* attributes, size_t attributesStride, const float* weights, size_t attributeCount,
		const unsigned char* locks, const float* triangleReference, size_t targetIndexCount, unsigned int options,
		const OptimalSimplifyParams& params, float* outError, OptimalSimplifyStats* stats);

	// Builder glue (clodSimplifyOptimal): measures each input triangle's source-sheet reference against the original surface, then
	// runs SimplifyOptimal. Thread-safe.
	class OptimalSimplifier
	{
	public:
		OptimalSimplifier(const SimplifyValidator& original, const OptimalSimplifyParams& params);

		size_t Simplify(const clodMesh& mesh, clodOptimalRequest& request) const;

		struct Stats
		{
			OptimalSimplifyStats loop;
			uint64_t calls = 0;
			uint64_t referenceSamples = 0;
			uint64_t referenceUnmatched = 0; // reference samples with no original match (the input face stands in)
			uint64_t referenceNs = 0;
			uint64_t loopNs = 0;
		};
		Stats GetStats() const;

	private:
		const SimplifyValidator& m_original;
		OptimalSimplifyParams m_params;
		mutable std::atomic<uint64_t> m_counters[9] = {};
		mutable std::atomic<uint64_t> m_calls = 0;
		mutable std::atomic<uint64_t> m_referenceSamples = 0;
		mutable std::atomic<uint64_t> m_referenceUnmatched = 0;
		mutable std::atomic<uint64_t> m_referenceNs = 0;
		mutable std::atomic<uint64_t> m_loopNs = 0;
	};

}
