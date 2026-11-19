#pragma once

// Replacement vertices for CLod simplification (stage 2, docs/clod-simplification-validation.md): the builder-owned build
// vertex buffer that grows past the source vertices, and the source-based fit that produces its new vertices. clusterlod.h owns
// the policy (when an update candidate is tried, when its vertices are committed); this owns the records and the fit.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <BasicRenderer/Assets/MeshData.h>

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h"

struct clodMesh;
struct clodReplacementCandidate;
struct clodReplacementBatch;
struct clodValidateCandidate;

namespace clod_detail {

	// Where things are in a vertex record and in the simplification attribute stream.
	struct ReplacementVertexLayout
	{
		size_t vertexStride = 0;
		size_t normalOffset = 0;
		size_t tangentOffset = SIZE_MAX;   // record tangent (float4), or SIZE_MAX
		size_t texcoordOffset = SIZE_MAX;  // record texcoord (float2), or SIZE_MAX
		size_t attributeStride = 0;        // floats per vertex in the attribute stream (0: no stream)
		int normalAttribute = -1;          // first of the stream's 3 normal floats, or -1
		int tangentAttribute = -1;         // first of the stream's 4 tangent floats, or -1
		// UV streams solved by the update, 2 floats each from uvAttribute on: the record texcoord first when texcoordSolved,
		// then the first uvSetsSolved UV sets. Others keep the replaced vertex's values.
		uint32_t uvAttribute = 0;
		bool texcoordSolved = false;
		uint32_t uvSetsSolved = 0;
		// An unmoved vertex is replaced only when its fitted normal meets its current one below this cosine.
		float refitCos = 0.99619470f; // 5 degrees
		// Follow-up (simplifyUpdateUvTransfer): every candidate's moved vertices take each solved UV stream from the original at
		// their side-aware match, where it lies inside that stream's neighbourhood box grown by a quarter. Off: only candidates
		// asking for their texcoords (stage 5 topology candidates) transfer, and only the record texcoord.
		bool transferUvs = false;
		// Bits of the fit's sample points on each triangle (kFitSamples order; 31: all five).
		uint32_t fitSamples = 31u;
		// Build cost: an unmoved vertex whose original normal at its own position meets its current one at least at this cosine is
		// not fitted further (below -1: off, every vertex fitted).
		float refitCheckCos = -2.0f;
	};

	// The build vertex buffer: the source records, then the replacement vertices in commit order, with the per-vertex streams
	// that grow with it. Grows only in Commit, which clusterlod calls while no iteration task runs.
	struct ReplacementVertexStore
	{
		std::vector<std::byte> vertices;
		std::vector<MeshUvSetData> uvSets;
		std::vector<float>* attributes = nullptr;          // the builder's simplification attribute stream, or null
		std::vector<unsigned char>* vertexLock = nullptr;  // the builder's per-vertex lock flags, or null
		std::vector<uint32_t>* charts = nullptr;           // follow-up: per-vertex UV chart (inherited from the replaced vertex), or null
		std::vector<std::byte>* skinning = nullptr;         // the skinning stream (last-resort fit): the parent's row, with the fitted normal
		size_t skinningStride = 0;
		size_t skinningNormalOffset = 0;
		size_t sourceVertexCount = 0;
	};

	class ReplacementVertexFitter
	{
	public:
		ReplacementVertexFitter(const SimplifyValidator& original, const ReplacementVertexLayout& layout, ReplacementVertexStore& store);

		// Bytes per replacement vertex carried from Fit to Commit: the vertex record, every UV set's value, the attribute row.
		size_t RecordSize() const { return m_layout.vertexStride + m_store.uvSets.size() * sizeof(float) * 2u + m_layout.attributeStride * sizeof(float); }

		// clodFitReplacements. Thread-safe: reads the store and the original, writes only the candidate's outputs.
		void Fit(const clodMesh& mesh, clodReplacementCandidate& candidate) const;
		// clodCommitReplacements. Serial.
		void Commit(clodMesh& mesh, const clodReplacementBatch& batch);
		// clodMeasureSimplify: MeasureDeviation of the candidate (a private update candidate or one indexing the store).
		float Measure(const clodValidateCandidate& candidate) const;

		// The store as a surface (positions and shading normals) indexed by indices.
		SimplifySurfaceView View(const uint32_t* indices, size_t indexCount) const;

		struct Stats
		{
			uint64_t fitVertices = 0;      // vertices fitted (used, unlocked)
			uint64_t fitFallbacks = 0;     // of which no original normal could be sampled
			uint64_t refitReplaced = 0;    // unmoved vertices replaced because their fitted normal differed
			uint64_t uvClamped = 0;        // moved vertices' solved UV components clamped into their neighbourhood's range
			uint64_t uvNonFinite = 0;      // solved UV streams that were not finite (the replaced vertex's kept)
			uint64_t uvTransferred = 0;    // moved vertices' UV streams taken from the original (transferUvs)
			uint64_t uvTransferOutside = 0; // streams whose match lay outside the neighbourhood (the clamped solve kept)
			uint64_t fanChecks = 0;        // unmoved vertices whose fit inputs were looked up
			uint64_t fanHits = 0;          // of which an earlier fit had left them unchanged with the same inputs (not fitted again)
			uint64_t checkSkipped = 0;     // unmoved vertices the refit check took as unchanged (refitCheckCos)
			uint64_t fitNs = 0;
		};
		Stats GetStats() const;

	private:
		// Stage 5: the original's texcoord at point's side-aware match (faces: the area-weighted face direction there, any length).
		// False without texcoords, when nothing faces that way, or when the match is degenerate.
		bool OriginalTexcoordAt(const float point[3], const double faces[3], float uv[2]) const;
		// The original triangle and barycentric weights at point's side-aware match, matched as OriginalTexcoordAt does. False when
		// nothing faces that way or the match names a vertex past the source.
		bool OriginalMatchAt(const float point[3], const double faces[3], uint32_t& triangle, float barycentric[3], MatchFacingCache* cache = nullptr) const;

		const SimplifyValidator& m_original;
		ReplacementVertexLayout m_layout;
		ReplacementVertexStore& m_store;
		mutable std::atomic<uint64_t> m_fitVertices = 0;
		mutable std::atomic<uint64_t> m_fitFallbacks = 0;
		mutable std::atomic<uint64_t> m_refitReplaced = 0;
		mutable std::atomic<uint64_t> m_uvClamped = 0;
		mutable std::atomic<uint64_t> m_uvNonFinite = 0;
		mutable std::atomic<uint64_t> m_uvTransferred = 0;
		mutable std::atomic<uint64_t> m_uvTransferOutside = 0;
		mutable std::atomic<uint64_t> m_fanChecks = 0;
		mutable std::atomic<uint64_t> m_fanHits = 0;
		mutable std::atomic<uint64_t> m_checkSkipped = 0;
		// Per build vertex: the key of the fit inputs of its last fit that left it unchanged (first 0: none). Grows in Commit.
		struct FanKey
		{
			uint64_t first = 0;
			uint64_t second = 0;
		};
		mutable std::vector<FanKey> m_fanKeys;
		uint32_t m_fanMemo = 2u; // BASICRENDERER_CLOD_FIT_FAN_MEMO (diagnostics): 0 off, 1 count only, 2 skip
		mutable std::atomic<uint64_t> m_fitNs = 0;
	};

}
