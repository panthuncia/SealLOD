#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "Core/Metrics.h"
#include "Core/TriangleGrid.h"

// Pointwise comparison of one triangle surface with another: shading-normal residual before any averaging, distance,
// coverage and how sure the correspondence is. quality.appearance measures simplified geometry against the leaves with
// it, and the tests measure hand-made surfaces whose answers are known. See docs/clod-evaluation.md, "Appearance".
namespace clodeval {

struct ShadedTriangle {
	std::array<Vec, 3> position;
	std::array<Vec, 3> normal;  // unit, as decoded and normalised
};

// An interpolated shading normal shorter than this (from unit corners) cancels: the shader's normalize has nothing to
// point with.
inline constexpr double kZeroNormalLength = 1.0e-2;

// Points measured on each triangle, as barycentric weights, each standing for a quarter of its area: the centroid and
// one point towards each corner. Corners are left out, since simplification keeps original vertices, which sit on the
// source by construction.
inline constexpr std::array<std::array<double, 3>, 4> kTriangleSamplePoints{ {
	{ 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 }, { 2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0 }, { 1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0 }, { 1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0 } } };

double TriangleArea(const ShadedTriangle& triangle);
// Unit face normal by winding, or zero for a degenerate triangle.
Vec FaceNormal(const ShadedTriangle& triangle);
// The interpolated shading normal at barycentric weights, normalised, or zero where it cancels (kZeroNormalLength).
Vec ShadingNormal(const ShadedTriangle& triangle, const std::array<double, 3>& weights);

// What a point finds on a surface: the nearest triangle whatever its facing, and the nearest one facing the point's
// own way (face normals with a positive dot product) a little farther out.
struct SurfaceMatch {
	double distance = std::numeric_limits<double>::infinity();
	Vec normal;  // shading normal at the nearest point: unit, or zero where it cancels
	double sameFacingDistance = std::numeric_limits<double>::infinity();
	Vec sameFacingNormal;
};

// A triangle surface to measure points against.
class SurfaceIndex {
public:
	explicit SurfaceIndex(std::vector<ShadedTriangle> triangles);
	bool Empty() const { return m_triangles.empty(); }
	size_t Size() const { return m_triangles.size(); }
	// facingReach: how much farther than the nearest surface to look for one facing faceNormal's way.
	SurfaceMatch Match(const Vec& point, const Vec& faceNormal, double facingReach) const;

private:
	std::vector<ShadedTriangle> m_triangles;
	std::vector<Vec> m_faceNormals;
	TriangleGrid m_grid;
};

// One measured point, resolved for one direction of comparison. Its chords are against the nearest and the nearest
// same-facing reference, so which one counts can be decided later for any tolerance.
struct AppearanceSample {
	float weight = 0.0f;  // area the point stands for
	float distance = std::numeric_limits<float>::infinity();
	float sameFacingDistance = std::numeric_limits<float>::infinity();
	// |n_coarse - n_source|^2 against each reference; negative where the source normal cancels, so there is nothing to
	// compare with. A coarse normal that cancels counts as the worst case, 4 (180 degrees).
	float chordSquared = -1.0f;
	float sameFacingChordSquared = -1.0f;
	uint8_t flags = 0;  // kCoarseZero* bits
};
inline constexpr uint8_t kCoarseZeroNearest = 1u;
inline constexpr uint8_t kCoarseZeroSameFacing = 2u;

// ownIsCoarse: the measured point is on the coarse surface and the reference is the source (coarse to source), or the
// other way round (source to coarse). It decides which of the two normals cancelling is a defect and which is a gap.
AppearanceSample ResolveSample(const SurfaceMatch& match, const Vec& ownNormal, bool ownIsCoarse, double weight);

// Tolerances for one allowed geometric error, in mesh units. See docs/clod-evaluation.md for why each is what it is.
struct AppearanceTolerance {
	double coverage = 0.0;   // farther than this from the other surface is spurious (coarse) or missing (source)
	double unmatched = 0.0;  // farther than this, nothing on the other surface corresponds
	double facingFloor = 0.0;
	double facingCap = 0.0;  // the same-facing reference may be up to max(0.5 coverage, 0.1 distance) farther, capped
	// How far past the nearest surface SurfaceIndex::Match must look for a same-facing one: facingCap.
	double FacingReach() const { return facingCap; }
};
AppearanceTolerance MakeAppearanceTolerance(double allowedError, double meshSize);

// Area-weighted sums for one surface measured against another. Fractions are of `area`, which includes area known to
// be exactly on the other surface (AddExact) as well as sampled points.
struct AppearanceTotals {
	AppearanceTotals();

	double area = 0.0;
	double measuredArea = 0.0;      // with a normal residual: matched or ambiguous, and the source normal does not cancel
	double normalQ = 0.0;           // integral of |n_coarse - n_source|^2 dA
	double beyondArea = 0.0;        // farther than the coverage tolerance
	double unmatchedArea = 0.0;     // farther than the unmatched tolerance: no residual
	double ambiguousArea = 0.0;     // the nearest surface faces away and no same-facing one is near enough
	double coarseZeroNormalArea = 0.0;
	double sourceZeroNormalArea = 0.0;
	std::array<double, 3> angleOverArea{};  // over kAngleThresholdsDegrees
	double maxAngle = 0.0;                  // sampled, radians
	double maxDistanceOverTolerance = 0.0;  // sampled
	double maxDistance = 0.0;               // sampled, mesh units
	uint64_t samples = 0, unmatchedSamples = 0, ambiguousSamples = 0, coarseZeroNormalSamples = 0, sourceZeroNormalSamples = 0;
	LogHistogram angle;                  // radians, area-weighted
	LogHistogram chord;                  // |n_coarse - n_source|, area-weighted
	LogHistogram distanceOverTolerance;  // distance / coverage tolerance, area-weighted

	static constexpr std::array<double, 3> kAngleThresholdsDegrees{ 10.0, 30.0, 90.0 };

	void Add(const AppearanceSample& sample, const AppearanceTolerance& tolerance);
	// Area exactly on the other surface with the same normals: a leaf drawn as itself.
	void AddExact(double exactArea);
	// Scales every area (and histogram) by factor, so sampled points stand for an exactly known area. Counts and
	// maxima are left alone.
	void Scale(double factor);
	void Merge(const AppearanceTotals& other);
};

// Measures coarse against source both ways at one allowed error, with the sample pattern quality.appearance uses:
// kTriangleSamplePoints on each coarse triangle, the centroid of each source triangle. For tests and one-off use.
struct SurfaceComparison {
	AppearanceTotals coarseToSource;
	AppearanceTotals sourceToCoarse;
};
SurfaceComparison CompareSurfaces(const std::vector<ShadedTriangle>& source, const std::vector<ShadedTriangle>& coarse, double allowedError);

// Diagonal of the triangles' bounding box.
double SurfaceSize(const std::vector<ShadedTriangle>& triangles);

}
