#pragma once

// Derived lens data: paraxial constants, focusing, the canonical frame the polynomial lens uses, the exact pupil,
// exit-pupil bounds for importance-sampling camera rays, the radial distortion table used for FOV
// matching and motion vectors, and ghost ranking.

#include <cstdint>
#include <vector>

#include "LensCore/Lens.h"
#include "LensCore/LensTrace.h"

namespace lens {

	struct ParaxialData {
		double efl = 0.0;            // effective focal length
		double bfl = 0.0;            // last vertex to rear focal point
		double entrancePupilRadius = 0.0;
		double fNumber = 0.0;        // efl / entrance pupil diameter, infinity focus
	};

	ParaxialData ComputeParaxial(const LensPrescription& lens, double lambdaNm = kLambdaD);

	// Image distance change (mm, +z) needed to focus an object at objectDistanceMm in front of the
	// first vertex, relative to infinity focus. Paraxial, at lambdaNm.
	double SensorShiftForFocus(const LensPrescription& lens, double objectDistanceMm, double lambdaNm = kLambdaD);

	// Rotational and mirror symmetry: a ray at (x, y) with direction d maps to (r, 0) with the
	// direction rotated by the same angle and mirrored so dy >= 0.
	struct CanonicalFrame {
		double cosPhi = 1.0;
		double sinPhi = 0.0;
		bool mirrored = false;
	};

	struct CanonicalRay {
		double r = 0.0;
		double dx = 0.0;
		double dy = 0.0; // >= 0
		double dz = 0.0;
	};

	inline CanonicalRay Canonicalize(Vec3 position, Vec3 dir, CanonicalFrame& frame)
	{
		const double r = std::sqrt(position.x * position.x + position.y * position.y);
		frame = {};
		if (r > 0.0) {
			frame.cosPhi = position.x / r;
			frame.sinPhi = position.y / r;
		}
		CanonicalRay out;
		out.r = r;
		out.dx = frame.cosPhi * dir.x + frame.sinPhi * dir.y;
		out.dy = -frame.sinPhi * dir.x + frame.cosPhi * dir.y;
		out.dz = dir.z;
		if (out.dy < 0.0) {
			out.dy = -out.dy;
			frame.mirrored = true;
		}
		return out;
	}

	// Inverse of Canonicalize for a vector or a point's xy, z unchanged.
	inline Vec3 Uncanonicalize(Vec3 v, const CanonicalFrame& frame)
	{
		if (frame.mirrored) {
			v.y = -v.y;
		}
		return { frame.cosPhi * v.x - frame.sinPhi * v.y, frame.sinPhi * v.x + frame.cosPhi * v.y, v.z };
	}

	// For each sensor-radius bin, the box on the rear plane (z = RearPlaneZ) that backward camera rays
	// must pass through to clear the lens. Built in the canonical frame (sensor point on +x).
	struct ExitPupilBounds {
		double sensorZ = 0.0;
		double rearPlaneZ = 0.0;
		double maxSensorRadius = 0.0;
		struct Bin {
			double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
			double passFraction = 0.0; // passing samples / samples inside the box
			bool valid = false;
		};
		std::vector<Bin> bins;

		const Bin& BinFor(double sensorRadius) const;
	};

	// The bins are computed in parallel on up to `threads` threads (0: hardware concurrency).
	ExitPupilBounds ComputeExitPupilBounds(const LensPrescription& lens, double sensorZ, double maxSensorRadius, uint32_t binCount = 64,
		uint32_t gridResolution = 128, double lambdaNm = kLambdaD, const TraceOptions& options = {}, uint32_t threads = 0);

	// The exact pupil: for a sensor point, the rear-plane points whose backward rays clear the lens are, closely, the
	// intersection of a few discs (Hanika & Dachsbacher 2014): the images of the aperture stop and of the element rims that
	// bind. In the canonical frame (sensor point on +x) every disc is centred on the x axis. A PupilRegion is such an
	// intersection; it is sampled uniformly and exactly (SamplePupilRegion), so its pdf is 1 / area.
	inline constexpr uint32_t kMaxPupilDiscs = 4;

	struct PupilRegion {
		uint32_t count = 0;                          // 0: empty
		std::array<double, kMaxPupilDiscs> centre{}; // x of each disc's centre
		std::array<double, kMaxPupilDiscs> radius{};
	};

	bool PupilRegionContains(const PupilRegion& region, double x, double y);
	// The x range of the intersection; false when it is empty.
	bool PupilRegionExtent(const PupilRegion& region, double& minX, double& maxX);
	double PupilRegionArea(const PupilRegion& region);
	// A point uniform over the region from two uniform numbers: x by inverting the area to its left (piecewise, over the
	// intervals where one disc bounds the region), then y uniform across the region at that x. False when it is empty.
	// The HLSL sampler (lensCamera.hlsli LensSamplePupilRegion) is the same algorithm in float32.
	bool SamplePupilRegion(const PupilRegion& region, double u0, double u1, double& x, double& y, double& area);

	// Per sensor radius, the discs of the region: nodes at radii i * maxSensorRadius / (nodes - 1), linearly interpolated
	// between them (each disc's centre and radius). Each disc is one aperture's image (`apertures`, surface indices), with
	// its centre fitted to that aperture's rim image and its radius the smallest that holds every boundary point of the
	// true pupil found at the node, at the given wavelengths, then widened so that the interpolation between nodes holds
	// the boundary found between them too, and by a relative margin. MeasurePupilDiscs checks with random rays that no
	// light passes outside.
	struct PupilDiscOptions {
		uint32_t nodes = 49;
		uint32_t angles = 256;         // boundary directions per node and wavelength (96 left ~1e-4 of the light out at cat's-eye tips)
		uint32_t bisections = 26;
		uint32_t betweenSamples = 3;   // radii between two nodes the interpolation is checked (and widened) at
		std::vector<double> wavelengths{ 380.0, 480.0, kLambdaD, 680.0, 780.0 };
		double relativeMargin = 0.002; // each disc's radius grows by this fraction, plus absoluteMarginMm
		double absoluteMarginMm = 1.0e-4;
		uint32_t maxDiscs = kMaxPupilDiscs;
		uint32_t threads = 0;          // 0: hardware concurrency
	};

	struct PupilDiscs {
		double sensorZ = 0.0;
		double rearPlaneZ = 0.0;
		double maxSensorRadius = 0.0;
		std::vector<uint32_t> apertures; // surface per disc
		std::vector<PupilRegion> nodes;  // count == apertures.size(), or 0 past the lens's coverage
		double buildMs = 0.0;

		uint32_t NodeCount() const { return static_cast<uint32_t>(nodes.size()); }
		// The interpolated region at a sensor radius (empty past the last node, or where both neighbours are).
		PupilRegion At(double sensorRadius) const;
	};

	PupilDiscs ComputePupilDiscs(const LensPrescription& lens, double sensorZ, double maxSensorRadius, const TraceOptions& options = {},
		const PupilDiscOptions& discOptions = {});

	// Random camera rays per node interval: sensor radius uniform between the two nodes, rear point uniform over the
	// region's bounding box at that radius widened by boxMargin of its size on each side, wavelength uniform in
	// [lambdaMin, lambdaMax] (lambdaMin alone when equal), traced exactly. What passes, inside and outside the region; the
	// region's acceptance (the exact sampler's: passing / inside) and the smallest disc's (sampling that disc and
	// rejecting outside the others). Energies are throughput * cos^2 / d^2 per ray.
	struct PupilDiscStats {
		struct Interval {
			uint64_t rays = 0;
			uint64_t inRegion = 0, inSmallest = 0;
			uint64_t passInRegion = 0, passInSmallest = 0, passOutside = 0;
			uint64_t stoppedInRegion = 0, clippedInRegion = 0; // rays in the region the stop, or a rim, blocks
			double energyInRegion = 0.0, energyOutside = 0.0;
			double regionAreaSum = 0.0, smallestAreaSum = 0.0, boxAreaSum = 0.0; // per ray, for the mean areas
			double Acceptance() const { return inRegion ? double(passInRegion) / double(inRegion) : 0.0; }
			double SmallestDiscAcceptance() const { return inSmallest ? double(passInSmallest) / double(inSmallest) : 0.0; }
			// The integral of the energy over the rear plane at this interval's radii: mean energy times the box area.
			double Energy() const { return rays ? (energyInRegion + energyOutside) / double(rays) * (boxAreaSum / double(rays)) : 0.0; }
		};
		std::vector<Interval> intervals; // nodes - 1
	};

	PupilDiscStats MeasurePupilDiscs(const LensPrescription& lens, const PupilDiscs& discs, const TraceOptions& options, uint64_t raysPerInterval,
		double lambdaMin, double lambdaMax, double boxMargin = 0.1, uint64_t seed = 1, uint32_t threads = 0);

	// Image height of the chief ray (through the stop centre) as a function of the field angle of
	// an object at infinity. Monotonic up to maxFieldAngle.
	struct DistortionTable {
		double sensorZ = 0.0;
		double maxFieldAngle = 0.0; // radians, last valid sample
		std::vector<double> fieldAngle;   // radians, increasing
		std::vector<double> imageRadius;  // mm, increasing

		double ImageRadiusAt(double angle) const;
		double FieldAngleAt(double radius) const;
	};

	// Returns false if no chief ray can be found even near the axis.
	bool FindChiefRay(const LensPrescription& lens, double fieldAngle, double lambdaNm, Ray& chiefRay);
	DistortionTable ComputeDistortionTable(const LensPrescription& lens, double sensorZ, double maxFieldAngle, uint32_t sampleCount = 128,
		double lambdaNm = kLambdaD);

	struct RankedPath {
		LensPath path;
		double energy = 0.0; // relative to the transmission path's energy for the same source rays
	};

	// Forward rays from distant point sources at the given field angles, on a grid over the front
	// plane. Ghosts sorted by descending energy; transmission is not included.
	std::vector<RankedPath> RankGhostPaths(const LensPrescription& lens, const std::vector<double>& fieldAngles, uint32_t gridResolution = 64,
		double lambdaNm = kLambdaD, double sensorHalfExtentMm = 0.0);

} // namespace lens
