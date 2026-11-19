#pragma once

// Validation of CLod simplification results against the original surface (docs/clod-simplification-validation.md).
// clusterlod.h owns the policy (which candidates to try, retry, terminal); this owns the measurements.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace clod_detail {

	// A triangle surface. Corners index positions and unit shading normals (strides in bytes; normals may be null, which turns
	// the shading test off). The builder points both the original and every candidate at the source vertex buffer; a builder
	// with private replacement vertices points a candidate at its own buffers instead.
	struct SimplifySurfaceView
	{
		const float* positions = nullptr;
		size_t positionStride = 0;
		const float* normals = nullptr;
		size_t normalStride = 0;
		const uint32_t* indices = nullptr;
		size_t indexCount = 0;
	};

	// Experimental thresholds (ClusterLODBuilderSettings::simplifyValidation*), not production constants.
	struct SimplifyValidationThresholds
	{
		float foldCos = 0.0f;               // a sample is folded when its face and the matched original face meet below this cosine
		float foldAreaFraction = 0.01f;     // new folded area allowed, as a fraction of the candidate's area
		float shadingCos = 0.2588f;         // a sample shades severely wrong at or below this cosine (75 degrees)
		float shadingAreaFraction = 0.02f;  // new severe-shading area allowed, as a fraction of the candidate's area
		float radiusErrorScale = 1.0f;      // match and coverage radius, as a multiple of the error the group would report
		float coverageAreaFraction = 0.01f; // uncovered input area and unmatched candidate area allowed, each as a fraction
		// The side-aware match against the original (SimplifyTriangleBvh::SetMatchShell): reach as a multiple of the nearest
		// distance, and the nearest candidates kept (0: every one within the reach, the exact match).
		float matchReachScale = 1.1f;
		uint32_t matchMaxCandidates = 0u;
		uint32_t matchFacingGrid = 0u; // facing bins: the octahedral grid (8 or 16), 0 off
		// Area samples per triangle of the candidate and coverage measurements (AreaSampleTaken; 4, the default, takes all four).
		uint32_t samplesPerTriangle = 4u;
	};

	// Build cost: which of the four area samples per triangle (the centroid, then towards each corner) a measurement takes when it
	// takes count of them. 4 (or more) takes all. 2 and 1 take two opposite ones, or one, rotating with the triangle's index, so a
	// surface's triangles still cover all four positions; each sample then stands for the triangle's area over count.
	inline bool AreaSampleTaken(size_t triangle, size_t sample, uint32_t count)
	{
		if (count >= 4u)
		{
			return true;
		}
		const size_t first = triangle % 4u;
		return count == 2u ? (sample + 4u - first) % 2u == 0u : sample == first;
	}
	inline uint32_t AreaSampleCount(uint32_t requested)
	{
		return requested == 1u || requested == 2u ? requested : 4u;
	}

	// Area totals from one Validate call (mesh units squared). The input-side fold/shading totals are measured only when the
	// candidate's own totals exceed what is allowed without a baseline.
	struct SimplifyValidationMeasurement
	{
		double outputArea = 0.0;
		double foldArea = 0.0;       // candidate samples facing away from the matched original surface
		double shadingArea = 0.0;    // candidate samples whose interpolated normal is severely off the original's
		double unmatchedArea = 0.0;  // candidate samples with no original surface within the radius
		double inputArea = 0.0;
		double inputFoldArea = 0.0;
		double inputShadingArea = 0.0;
		double uncoveredArea = 0.0;  // input samples with no candidate surface within the radius facing their way
		double sourceArea = 0.0;     // original surface the group owns (estimated from a stride sample when large)
		double sourceUncoveredArea = 0.0; // of which no candidate surface lies within the radius facing its way
		bool inputMeasured = false;  // the input-side totals are set (from the baseline)
		bool stoppedEarly = false;   // fold and shading were certain to fail before every candidate triangle was sampled
		float radius = 0.0f;
	};

	// Bits of a Validate result; equal to clusterlod's clodValidateReject.
	enum SimplifyValidationReject : uint32_t
	{
		SimplifyValidationReject_Fold = 1u << 0,
		SimplifyValidationReject_Shading = 1u << 1,
		SimplifyValidationReject_Coverage = 1u << 2,
		SimplifyValidationReject_Spurious = 1u << 3,
		SimplifyValidationReject_SourceCoverage = 1u << 4,
	};

	// Diagnostics (BASICRENDERER_CLOD_QUERY_STATS=1): counts of SimplifyTriangleBvh::GatherNearJoint traversals by the site that made
	// them (QueryStatsScope), logged by LogQueryStats. Process-wide; off costs one branch per traversal.
	enum class QuerySite : uint32_t
	{
		Other,
		Fit,
		Channels,
		Texture,
		Count,
	};
	class QueryStatsScope
	{
	public:
		explicit QueryStatsScope(QuerySite site);
		~QueryStatsScope();
		QueryStatsScope(const QueryStatsScope&) = delete;
		QueryStatsScope& operator=(const QueryStatsScope&) = delete;

	private:
		QuerySite m_previous;
	};
	bool QueryStatsEnabled();
	void LogQueryStats(const char* label);

	// A bounding volume hierarchy over a triangle soup (positions copied in), for nearest-surface queries. Immutable after
	// construction; queries are thread-safe.
	class SimplifyTriangleBvh
	{
	public:
		struct Match
		{
			uint32_t triangle = UINT32_MAX; // index in the order the triangles were given
			float barycentric[3] = {};      // closest point on that triangle
			float distanceSquared = 0.0f;
			float facing = 0.0f;            // its unit face normal dotted with the query's
		};

		SimplifyTriangleBvh() = default;
		void Build(const SimplifySurfaceView& surface);
		bool Empty() const { return m_nodes.empty(); }
		// Distinct for every Build in the process (MatchFacingCache keeps traversals of one build only).
		uint64_t BuildId() const { return m_buildId; }

		// The side-aware match's shell (MatchFacing, GatherNear): candidates lie within reachScale times the nearest distance
		// plus the slack (1.1 by default). maxCandidates (at most 32; 0, the default: no limit) keeps only that many of the nearest
		// of them, in traversal order among equal distances, and the traversal stops looking past the farthest kept. With
		// overlapping or dense source (a sample far above triangles much smaller than its distance) the exact shell holds thousands.
		// facingGrid (instead of maxCandidates; 8 or 16, 0 off) keeps the exact match while its 32-entry buffer can hold the
		// candidates; a gather that would overflow it keeps from then on the nearest candidate of each facing direction (the cells
		// of a facingGrid x facingGrid octahedral map), and skips subtrees that hold no direction it lacks or holds farther than the
		// subtree's box. Every face then selects among at most facingGrid^2.
		void SetMatchShell(float reachScale, uint32_t maxCandidates, uint32_t facingGrid = 0u);

		// The original-surface match for a sample: among the triangles about as near as the nearest (within 1.1x its distance
		// plus a small absolute slack; SetMatchShell), the one whose face agrees best with faceNormal, so coincident opposite
		// sheets resolve to the side the sample faces. False when nothing lies within maxDistance.
		bool MatchFacing(const float point[3], const float faceNormal[3], float maxDistance, float slack, Match& out) const;

		// Stage 6 step 2 (UV remap correspondence): the triangle crossed nearest to point by the line through it along direction (a
		// unit vector), within maxDistance either way, among those whose face meets direction at least minFacing; barycentric is the
		// crossing, distanceSquared its squared distance. Ties go to the lowest index. False when none.
		bool NearestCrossing(const float point[3], const float direction[3], float maxDistance, float minFacing, Match& out) const;

		// Stage 6 step 2 refinement: as NearestCrossing, but the crossing farthest along direction (the frontmost, what a viewer
		// looking against direction sees first) within maxDistance either way; distanceSquared is its signed offset squared and
		// facing its signed offset along direction. Ties go to the lowest index. False when none.
		bool FrontmostCrossing(const float point[3], const float direction[3], float maxDistance, float minFacing, Match& out) const;

		// Whether a triangle whose face meets faceNormal above minFacing lies within radius of point.
		bool AnyFacingWithin(const float point[3], const float faceNormal[3], float radius, float minFacing) const;

		// Squared distance to the nearest triangle, or maxDistanceSquared when none is nearer. nearest (optional) receives that
		// triangle's index in the order the triangles were given, or UINT32_MAX.
		float NearestDistanceSquared(const float point[3], float maxDistanceSquared, uint32_t* nearest = nullptr) const;

		// MatchFacing in two parts, for callers that query one point with several face normals (MatchFacingCache), or one point
		// with several reaches. GatherNear is the traversal, which does not depend on the face: it appends to pool every candidate
		// MatchFacing's buffer would take, in order, and describes them in out. SelectFacing picks MatchFacing's result from them
		// for a face. Together they return exactly what MatchFacing returns. When more equally near candidates turn up than
		// MatchFacing's buffer holds (out.overflow), the buffer's choice among them depends on the face, so SelectFacing replays it.
		struct NearCandidate
		{
			float distanceSquared;
			float weights[3];
			float normal[3];
			float reachSquared; // the reach once this candidate's distance was taken into account
			uint32_t triangle;
		};
		// NearGather::first of a traversal a caller did not keep (TextureCheckOptions::matchGathers: match that sample itself).
		static constexpr uint32_t kNotGathered = UINT32_MAX;
		struct NearGather
		{
			uint32_t first = 0;  // in pool
			uint32_t count = 0;
			float reachSquared = 0.0f; // final
			bool overflow = false; // every candidate taken is kept (for the replay), not only those within the final reach
			bool none = true;    // nothing within maxDistance (MatchFacing returns false)
		};
		struct NearRequest
		{
			float maxDistance;
			float slack;
		};
		void GatherNear(const float point[3], float maxDistance, float slack, std::vector<NearCandidate>& pool, NearGather& out) const;
		// Up to kMaxJointRequests GatherNear requests at one point and, when nearestDistanceSquared is given,
		// NearestDistanceSquared(point, *nearestDistanceSquared) (the result replaces it), in one traversal. Each part sees exactly
		// the nodes and triangles, in the same order, its own traversal would: a node is visited when any part would visit it.
		static constexpr uint32_t kMaxJointRequests = 3u;
		void GatherNearJoint(const float point[3], const NearRequest* requests, uint32_t requestCount, std::vector<NearCandidate>& pool, NearGather* out,
			float* nearestDistanceSquared = nullptr) const;
		static bool SelectFacing(const NearGather& gather, const NearCandidate* pool, const float faceNormal[3], Match& out);

		struct Triangle
		{
			float a[3], b[3], c[3];
			float normal[3]; // unit, or zero for degenerate triangles
			uint32_t source; // index in the order the triangles were given
		};
		// Appends the slots of the triangles in leaves whose bounds overlap the box; read them with TriangleAt.
		void CollectInBox(const float boundsMin[3], const float boundsMax[3], std::vector<uint32_t>& slots) const;
		const Triangle& TriangleAt(uint32_t slot) const { return m_triangles[slot]; }

	private:
		struct Node
		{
			float boundsMin[3];
			float boundsMax[3];
			uint32_t first; // leaf: first triangle slot; inner: right child (left child is the next node)
			uint32_t count; // leaf: triangle count (low 3 bits) and its LeafBlock (above); inner: 0
		};
		static uint32_t LeafCount(const Node& node) { return node.count & 7u; }

		// A leaf's triangles (at most 4) as lanes, for the 4-wide closest-point test; unused lanes repeat the last triangle.
		struct alignas(16) LeafBlock
		{
			float ax[4], ay[4], az[4], bx[4], by[4], bz[4], cx[4], cy[4], cz[4];
			float nx[4], ny[4], nz[4];
			uint32_t source[4];
		};

		uint32_t BuildNode(std::vector<uint32_t>& order, std::vector<float>& centroids, uint32_t begin, uint32_t end);
		// BuildNode into a node list (indices relative to it); large ranges build their subtrees as parallel tasks.
		uint32_t BuildNodeInto(std::vector<Node>& nodes, std::vector<uint32_t>& order, std::vector<float>& centroids, uint32_t begin, uint32_t end) const;
		template <uint32_t RequestCount, bool Nearest>
		void GatherNearJointImpl(const float point[3], const NearRequest* requests, std::vector<NearCandidate>& pool, NearGather* out,
			float* nearestDistanceSquared) const;

		std::vector<Node> m_nodes;
		std::vector<Triangle> m_triangles;
		std::vector<LeafBlock> m_blocks;
		float m_reachScale = 1.1f;
		uint32_t m_maxCandidates = 0u;
		uint64_t m_buildId = 0u;
		struct BinMask
		{
			uint64_t words[4];
		};
		uint32_t m_facingGrid = 0u;
		std::vector<BinMask> m_nodeBins; // with facing bins: per node, the bins of its triangles' normals
		std::vector<std::array<uint8_t, 4>> m_blockBins; // and per leaf block, each lane's bin
	};

	// Memoizes MatchFacing's traversal by exact query point (SimplifyTriangleBvh::GatherNear) for one thread and one
	// (maxDistance, slack): a point queried again, with any face normal, costs only the selection. Results are exactly
	// MatchFacing's. Reset between uses; the table and pool keep their memory, but the pool is bounded: past kPoolCapacity
	// candidates (overlapping source gathers thousands per point) the memo starts over, and Reset drops a pool grown far beyond it.
	class MatchFacingCache
	{
	public:
		void Reset(const SimplifyTriangleBvh* bvh, float maxDistance, float slack, size_t expectedPoints);
		// Reset, but a cache already covering the same build and reach keeps its points (a point's traversal does not depend on
		// who asks), up to kMaxPoints.
		void Continue(const SimplifyTriangleBvh* bvh, float maxDistance, float slack, size_t expectedPoints);
		bool Match(const float point[3], const float faceNormal[3], SimplifyTriangleBvh::Match& out);
		bool Covers(const SimplifyTriangleBvh* bvh, float maxDistance, float slack) const
		{
			return bvh == m_bvh && bvh != nullptr && bvh->BuildId() == m_buildId && maxDistance == m_maxDistance && slack == m_slack;
		}

		static constexpr size_t kPoolCapacity = size_t(1) << 18; // candidates (9 MiB)
		static constexpr size_t kMaxPoints = size_t(1) << 16;

	private:
		struct Entry
		{
			uint32_t key[3];
			uint32_t generation;
			SimplifyTriangleBvh::NearGather gather;
		};
		void Grow();
		void Forget();

		const SimplifyTriangleBvh* m_bvh = nullptr;
		uint64_t m_buildId = 0u;
		float m_maxDistance = 0.0f;
		float m_slack = 0.0f;
		std::vector<Entry> m_table;
		std::vector<SimplifyTriangleBvh::NearCandidate> m_pool;
		size_t m_used = 0;
		uint32_t m_generation = 0;
	};

	// The fold and severe-shading share an input already has against the original, which a candidate is not blamed for. One
	// group's candidates share their input, so a caller may keep this across them: Validate fills it on first need.
	struct SimplifyInputBaseline
	{
		bool measured = false;
		double area = 0.0;
		double foldArea = 0.0;
		double shadingArea = 0.0;

		// The original itself (a depth-0 group's input): nothing to blame it for.
		static SimplifyInputBaseline Original()
		{
			SimplifyInputBaseline baseline;
			baseline.measured = true;
			return baseline;
		}
	};

	// Validates simplification candidates against an original surface indexed once per mesh. Thread-safe after construction.
	class SimplifyValidator
	{
	public:
		SimplifyValidator(const SimplifySurfaceView& original, const SimplifyValidationThresholds& thresholds);

		// 0 to accept output as the simplification of input, otherwise SimplifyValidationReject bits. groupError is the error
		// the group would report with this candidate; the match and coverage radius scale with it. baseline (optional) is the
		// input's own failure share: used when measured, otherwise measured into it when the candidate's totals need it. With a
		// measured baseline the candidate's fold/shading sampling stops once both are certain to fail.
		// group, when not UINT32_MAX, names the group's triangles in the surface last given to SetLevel: the original surface
		// whose nearest level surface is that group's (the original the group stands for, however far earlier levels drifted)
		// must then have candidate surface within the radius (SimplifyValidationReject_SourceCoverage).
		uint32_t Validate(
			const SimplifySurfaceView& input, const SimplifySurfaceView& output, float groupError,
			SimplifyInputBaseline* baseline = nullptr, SimplifyValidationMeasurement* measurement = nullptr,
			uint32_t group = UINT32_MAX) const;

		// The current level's whole surface, and per triangle the group whose input it is. Not thread-safe: call between levels,
		// while no Validate runs.
		void SetLevel(const SimplifySurfaceView& level, std::vector<uint32_t> triangleGroups);

		// Building blocks, for builders that combine them differently.
		float RadiusForError(float groupError) const;
		// Samples surface's triangles against the original: total, folded, severe-shading and unmatched area. Stops early once
		// foldArea exceeds foldStop and shadingArea exceeds shadingStop (area then covers only the triangles sampled).
		void MeasureAgainstOriginal(
			const SimplifySurfaceView& surface, float radius,
			double& area, double& foldArea, double& shadingArea, double& unmatchedArea,
			double foldStop = -1.0, double shadingStop = -1.0) const;
		// Measures surface (a group input) into baseline.
		void MeasureBaseline(const SimplifySurfaceView& surface, SimplifyInputBaseline& baseline) const;
		// Samples covered's triangles against cover (indexed in coverBvh): total area and the area with no cover surface within
		// radius facing its way.
		void MeasureCoverage(
			const SimplifySurfaceView& covered, const SimplifyTriangleBvh& coverBvh, float radius,
			double& area, double& uncoveredArea) const;
		// Samples the original triangles owned by group (see Validate) near input's bounds against coverBvh.
		void MeasureSourceCoverage(
			const SimplifySurfaceView& input, const SimplifyTriangleBvh& coverBvh, uint32_t group, float radius,
			double& area, double& uncoveredArea) const;

		// The original's unit shading normal at the point matched for a sample (MatchFacing, any distance): the interpolated
		// normal of the matched original triangle, and how its face meets faceNormal. False when the original has no normals,
		// nothing matches, or the interpolated normal cancels out.
		bool OriginalNormalAt(const float point[3], const float faceNormal[3], float normal[3], float& facing) const;
		// The same through a cache of the match (prepare it with ResetOriginalMatchCache).
		bool OriginalNormalAt(const float point[3], const float faceNormal[3], float normal[3], float& facing, MatchFacingCache& cache) const;
		void ResetOriginalMatchCache(MatchFacingCache& cache, size_t expectedPoints) const;
		void ContinueOriginalMatchCache(MatchFacingCache& cache, size_t expectedPoints) const;
		// The same from a traversal made with OriginalMatchRequest (SimplifyTriangleBvh::GatherNearJoint, so one traversal can
		// serve several queries at a point). False without original normals, as OriginalNormalAt.
		bool OriginalNormalAt(const SimplifyTriangleBvh::NearGather& gather, const SimplifyTriangleBvh::NearCandidate* pool, const float faceNormal[3],
			float normal[3], float& facing) const;
		SimplifyTriangleBvh::NearRequest OriginalMatchRequest() const
		{
			return { std::max(m_diagonal, 1.0e-20f) * 2.0f, MatchSlack() };
		}
		bool HasOriginalNormals() const { return m_original.normals != nullptr; }
		// The side-aware match's slack (SimplifyTriangleBvh::MatchFacing) used by every query against the original.
		float MatchSlack() const { return m_diagonal * 1.0e-5f; }
		// The unit face normal of the original triangle matched for a sample (MatchFacing, any distance). False when nothing
		// matches or the matched triangle is degenerate. Stage 4's source-sheet reference.
		bool OriginalFaceNormalAt(const float point[3], const float faceNormal[3], float normal[3]) const;

		// An independent estimate of how far output deviates, in mesh units: the largest distance from an output sample (the
		// four per triangle and its corners) to the original surface, and from an input sample to the output surface. Sampled,
		// not a certified bound. With the input's own deviation from the original (its error), the second term bounds how far
		// the original lies from output.
		float MeasureDeviation(const SimplifySurfaceView& input, const SimplifySurfaceView& output) const;

		// The original surface's index, for nearest-surface queries (stage 3's error channels).
		const SimplifyTriangleBvh& OriginalBvh() const { return m_originalBvh; }
		// The original surface as given (stage 5's texture transfer reads its triangles).
		const SimplifySurfaceView& Original() const { return m_original; }

		const SimplifyValidationThresholds& Thresholds() const { return m_thresholds; }
		float Diagonal() const { return m_diagonal; }

		// Time spent per phase across all calls (nanoseconds, summed over threads), for cost reports.
		struct PhaseTimes
		{
			uint64_t candidate = 0; // candidates against the original
			uint64_t coverage = 0;  // inputs against candidates
			uint64_t baseline = 0;  // inputs against the original
			uint64_t source = 0;    // owned original surface against candidates
			uint64_t deviation = 0; // MeasureDeviation
		};
		PhaseTimes Times() const { return { m_candidateNs.load(), m_coverageNs.load(), m_baselineNs.load(), m_sourceNs.load(), m_deviationNs.load() }; }

	private:
		mutable std::atomic<uint64_t> m_candidateNs = 0;
		mutable std::atomic<uint64_t> m_coverageNs = 0;
		mutable std::atomic<uint64_t> m_baselineNs = 0;
		mutable std::atomic<uint64_t> m_sourceNs = 0;
		mutable std::atomic<uint64_t> m_deviationNs = 0;
		SimplifyTriangleBvh m_levelBvh;
		std::vector<uint32_t> m_levelGroups;
		SimplifySurfaceView m_original;
		SimplifyValidationThresholds m_thresholds;
		SimplifyTriangleBvh m_originalBvh;
		float m_diagonal = 0.0f;
	};

}
