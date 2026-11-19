#include "Core/Appearance.h"

#include <algorithm>
#include <cmath>

#include <DirectXMath.h>

namespace clodeval {
namespace {
	constexpr double kPi = 3.14159265358979323846;
	// Histogram layouts, shared so meshes' histograms add up. Eight bins an octave resolve a quantile to about 4%.
	constexpr uint32_t kBinsPerOctave = 8u;
	LogHistogram AngleHistogram() { return LogHistogram(1.0e-4, kPi, kBinsPerOctave); }
	LogHistogram ChordHistogram() { return LogHistogram(1.0e-4, 2.0, kBinsPerOctave); }
	LogHistogram DistanceHistogram() { return LogHistogram(1.0e-4, 1.0e4, kBinsPerOctave); }

	double Length(const Vec& value) { return std::sqrt(Dot(value, value)); }

	Vec Interpolate(const std::array<Vec, 3>& values, const std::array<double, 3>& weights)
	{
		return values[0] * weights[0] + values[1] * weights[1] + values[2] * weights[2];
	}

	float ChordSquared(const Vec& a, const Vec& b)
	{
		const Vec d = a - b;
		return static_cast<float>(std::min(Dot(d, d), 4.0));
	}
}

double TriangleArea(const ShadedTriangle& triangle)
{
	const auto& [a, b, c] = triangle.position;
	return 0.5 * Length(Cross(b - a, c - a));
}

Vec FaceNormal(const ShadedTriangle& triangle)
{
	const auto& [a, b, c] = triangle.position;
	const Vec cross = Cross(b - a, c - a);
	const double length = Length(cross);
	return length > 0.0 ? cross * (1.0 / length) : Vec{};
}

Vec ShadingNormal(const ShadedTriangle& triangle, const std::array<double, 3>& weights)
{
	const Vec normal = Interpolate(triangle.normal, weights);
	const double length = Length(normal);
	return length >= kZeroNormalLength ? normal * (1.0 / length) : Vec{};
}

double SurfaceSize(const std::vector<ShadedTriangle>& triangles)
{
	if (triangles.empty()) {
		return 0.0;
	}
	Vec low = triangles.front().position[0], high = low;
	for (const ShadedTriangle& triangle : triangles) {
		for (const Vec& p : triangle.position) {
			low = { std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z) };
			high = { std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z) };
		}
	}
	return Length(high - low);
}

namespace {
	std::vector<std::array<DirectX::XMFLOAT3, 3>> GridPositions(const std::vector<ShadedTriangle>& triangles)
	{
		std::vector<std::array<DirectX::XMFLOAT3, 3>> positions;
		positions.reserve(triangles.size());
		for (const ShadedTriangle& triangle : triangles) {
			std::array<DirectX::XMFLOAT3, 3> corners;
			for (int corner = 0; corner < 3; ++corner) {
				const Vec& p = triangle.position[corner];
				corners[corner] = DirectX::XMFLOAT3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
			}
			positions.push_back(corners);
		}
		return positions;
	}
}

SurfaceIndex::SurfaceIndex(std::vector<ShadedTriangle> triangles)
	: m_triangles(std::move(triangles))
	, m_grid(GridPositions(m_triangles))
{
	m_faceNormals.reserve(m_triangles.size());
	for (const ShadedTriangle& triangle : m_triangles) {
		m_faceNormals.push_back(FaceNormal(triangle));
	}
}

SurfaceMatch SurfaceIndex::Match(const Vec& point, const Vec& faceNormal, double facingReach) const
{
	SurfaceMatch match;
	const TriangleGrid::Hit nearest = m_grid.NearestIf(point, std::numeric_limits<double>::max(), [](uint32_t) { return true; });
	if (!nearest.Found()) {
		return match;
	}
	match.distance = std::sqrt(nearest.distanceSquared);
	match.normal = ShadingNormal(m_triangles[nearest.index], nearest.weights);
	if (Dot(m_faceNormals[nearest.index], faceNormal) > 0.0) {
		match.sameFacingDistance = match.distance;
		match.sameFacingNormal = match.normal;
		return match;
	}
	const TriangleGrid::Hit facing = m_grid.NearestIf(point, match.distance + facingReach, [&](uint32_t index) { return Dot(m_faceNormals[index], faceNormal) > 0.0; });
	if (facing.Found()) {
		match.sameFacingDistance = std::sqrt(facing.distanceSquared);
		match.sameFacingNormal = ShadingNormal(m_triangles[facing.index], facing.weights);
	}
	return match;
}

AppearanceSample ResolveSample(const SurfaceMatch& match, const Vec& ownNormal, bool ownIsCoarse, double weight)
{
	AppearanceSample sample;
	sample.weight = static_cast<float>(weight);
	sample.distance = static_cast<float>(match.distance);
	sample.sameFacingDistance = static_cast<float>(match.sameFacingDistance);
	const bool ownZero = Dot(ownNormal, ownNormal) == 0.0;
	const auto resolve = [&](const Vec& reference, float& chordSquared, uint8_t coarseZeroBit) {
		const bool referenceZero = Dot(reference, reference) == 0.0;
		const bool coarseZero = ownIsCoarse ? ownZero : referenceZero;
		const bool sourceZero = ownIsCoarse ? referenceZero : ownZero;
		if (coarseZero) {
			sample.flags |= coarseZeroBit;
			chordSquared = 4.0f;
		}
		else if (sourceZero) {
			chordSquared = -1.0f;
		}
		else {
			chordSquared = ChordSquared(ownNormal, reference);
		}
	};
	if (std::isfinite(match.distance)) {
		resolve(match.normal, sample.chordSquared, kCoarseZeroNearest);
	}
	if (std::isfinite(match.sameFacingDistance)) {
		resolve(match.sameFacingNormal, sample.sameFacingChordSquared, kCoarseZeroSameFacing);
	}
	return sample;
}

AppearanceTolerance MakeAppearanceTolerance(double allowedError, double meshSize)
{
	const double floor = std::max(meshSize * 1.0e-6, 1.0e-12);
	AppearanceTolerance tolerance;
	tolerance.coverage = std::max(std::isfinite(allowedError) ? allowedError : 0.0, floor);
	tolerance.unmatched = std::max(2.0 * tolerance.coverage, meshSize * 1.0e-3);
	tolerance.facingFloor = floor;
	tolerance.facingCap = std::max(meshSize * 0.02, floor);
	return tolerance;
}

AppearanceTotals::AppearanceTotals()
	: angle(AngleHistogram())
	, chord(ChordHistogram())
	, distanceOverTolerance(DistanceHistogram())
{
}

void AppearanceTotals::Add(const AppearanceSample& sample, const AppearanceTolerance& tolerance)
{
	const double weight = sample.weight;
	if (!(weight > 0.0)) {
		return;
	}
	++samples;
	area += weight;
	const double distance = sample.distance;
	if (distance > tolerance.coverage) {
		beyondArea += weight;
	}
	if (std::isfinite(distance)) {
		distanceOverTolerance.Add(distance / tolerance.coverage, weight);
		maxDistanceOverTolerance = std::max(maxDistanceOverTolerance, distance / tolerance.coverage);
		maxDistance = std::max(maxDistance, distance);
	}
	if (!(distance <= tolerance.unmatched)) {
		unmatchedArea += weight;
		++unmatchedSamples;
		return;
	}
	const double facingSlack = std::clamp(std::max(0.5 * tolerance.coverage, 0.1 * distance), tolerance.facingFloor, tolerance.facingCap);
	const bool sameFacing = static_cast<double>(sample.sameFacingDistance) <= distance + facingSlack;
	if (!sameFacing) {
		ambiguousArea += weight;
		++ambiguousSamples;
	}
	const float chordSquared = sameFacing ? sample.sameFacingChordSquared : sample.chordSquared;
	if ((sample.flags & (sameFacing ? kCoarseZeroSameFacing : kCoarseZeroNearest)) != 0u) {
		coarseZeroNormalArea += weight;
		++coarseZeroNormalSamples;
	}
	if (chordSquared < 0.0f) {
		sourceZeroNormalArea += weight;
		++sourceZeroNormalSamples;
		return;
	}
	const double chordLength = std::sqrt(static_cast<double>(chordSquared));
	const double angleValue = 2.0 * std::asin(std::min(chordLength * 0.5, 1.0));
	measuredArea += weight;
	normalQ += weight * chordSquared;
	angle.Add(angleValue, weight);
	chord.Add(chordLength, weight);
	maxAngle = std::max(maxAngle, angleValue);
	for (size_t threshold = 0; threshold < kAngleThresholdsDegrees.size(); ++threshold) {
		if (angleValue > kAngleThresholdsDegrees[threshold] * kPi / 180.0) {
			angleOverArea[threshold] += weight;
		}
	}
}

void AppearanceTotals::AddExact(double exactArea)
{
	if (!(exactArea > 0.0)) {
		return;
	}
	area += exactArea;
	measuredArea += exactArea;
	angle.Add(0.0, exactArea);
	chord.Add(0.0, exactArea);
	distanceOverTolerance.Add(0.0, exactArea);
}

void AppearanceTotals::Scale(double factor)
{
	for (double* value : { &area, &measuredArea, &normalQ, &beyondArea, &unmatchedArea, &ambiguousArea, &coarseZeroNormalArea, &sourceZeroNormalArea }) {
		*value *= factor;
	}
	for (double& value : angleOverArea) {
		value *= factor;
	}
	angle.Scale(factor);
	chord.Scale(factor);
	distanceOverTolerance.Scale(factor);
}

void AppearanceTotals::Merge(const AppearanceTotals& other)
{
	area += other.area;
	measuredArea += other.measuredArea;
	normalQ += other.normalQ;
	beyondArea += other.beyondArea;
	unmatchedArea += other.unmatchedArea;
	ambiguousArea += other.ambiguousArea;
	coarseZeroNormalArea += other.coarseZeroNormalArea;
	sourceZeroNormalArea += other.sourceZeroNormalArea;
	for (size_t threshold = 0; threshold < angleOverArea.size(); ++threshold) {
		angleOverArea[threshold] += other.angleOverArea[threshold];
	}
	maxAngle = std::max(maxAngle, other.maxAngle);
	maxDistanceOverTolerance = std::max(maxDistanceOverTolerance, other.maxDistanceOverTolerance);
	maxDistance = std::max(maxDistance, other.maxDistance);
	samples += other.samples;
	unmatchedSamples += other.unmatchedSamples;
	ambiguousSamples += other.ambiguousSamples;
	coarseZeroNormalSamples += other.coarseZeroNormalSamples;
	sourceZeroNormalSamples += other.sourceZeroNormalSamples;
	angle.Merge(other.angle);
	chord.Merge(other.chord);
	distanceOverTolerance.Merge(other.distanceOverTolerance);
}

SurfaceComparison CompareSurfaces(const std::vector<ShadedTriangle>& source, const std::vector<ShadedTriangle>& coarse, double allowedError)
{
	SurfaceComparison comparison;
	const double size = SurfaceSize(source);
	const AppearanceTolerance tolerance = MakeAppearanceTolerance(allowedError, size);
	const SurfaceIndex sourceIndex(source);
	const SurfaceIndex coarseIndex(coarse);
	if (sourceIndex.Empty() || coarseIndex.Empty()) {
		return comparison;
	}
	for (const ShadedTriangle& triangle : coarse) {
		const double area = TriangleArea(triangle);
		if (!(area > 0.0)) {
			continue;
		}
		const Vec face = FaceNormal(triangle);
		for (const auto& weights : kTriangleSamplePoints) {
			const Vec point = Interpolate(triangle.position, weights);
			const SurfaceMatch match = sourceIndex.Match(point, face, tolerance.FacingReach());
			comparison.coarseToSource.Add(ResolveSample(match, ShadingNormal(triangle, weights), true, area / kTriangleSamplePoints.size()), tolerance);
		}
	}
	for (const ShadedTriangle& triangle : source) {
		const double area = TriangleArea(triangle);
		if (!(area > 0.0)) {
			continue;
		}
		const std::array<double, 3> centroid{ 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 };
		const Vec point = Interpolate(triangle.position, centroid);
		const SurfaceMatch match = coarseIndex.Match(point, FaceNormal(triangle), tolerance.FacingReach());
		comparison.sourceToCoarse.Add(ResolveSample(match, ShadingNormal(triangle, centroid), false, area), tolerance);
	}
	return comparison;
}

}
