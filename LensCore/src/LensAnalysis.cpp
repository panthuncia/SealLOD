#include "LensCore/LensAnalysis.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <thread>

namespace lens {

	namespace {
		struct ParaxialTrace {
			double y = 0.0;
			double u = 0.0; // slope after the last surface, image space (air)
			double yAtStop = 0.0;
		};

		ParaxialTrace TraceParaxial(const LensPrescription& lens, double y, double nu, double lambdaNm)
		{
			ParaxialTrace out;
			const uint32_t count = lens.SurfaceCount();
			for (uint32_t i = 0; i < count; ++i) {
				const Surface& surface = lens.surfaces[i];
				if (i == lens.stopIndex) {
					out.yAtStop = y;
				}
				const double n1 = lens.IndexBefore(i, lambdaNm);
				const double n2 = lens.IndexAfter(i, lambdaNm);
				nu -= y * (n2 - n1) * surface.Curvature();
				if (i + 1 < count) {
					y += nu / n2 * surface.thickness;
				}
			}
			out.y = y;
			out.u = nu / lens.media[lens.surfaces.back().mediumAfter].IndexAt(lambdaNm);
			return out;
		}

		TraceOptions NoClipping()
		{
			TraceOptions options;
			options.fresnel = false;
			options.clipHousing = false;
			options.clipStop = false;
			return options;
		}
	} // namespace

	ParaxialData ComputeParaxial(const LensPrescription& lens, double lambdaNm)
	{
		ParaxialData data;
		const ParaxialTrace parallel = TraceParaxial(lens, 1.0, 0.0, lambdaNm);
		if (parallel.u == 0.0) {
			return data;
		}
		data.efl = -1.0 / parallel.u;
		data.bfl = -parallel.y / parallel.u;
		if (parallel.yAtStop != 0.0) {
			data.entrancePupilRadius = lens.surfaces[lens.stopIndex].semiAperture / std::abs(parallel.yAtStop);
			data.fNumber = data.efl / (2.0 * data.entrancePupilRadius);
		}
		return data;
	}

	double SensorShiftForFocus(const LensPrescription& lens, double objectDistanceMm, double lambdaNm)
	{
		if (!std::isfinite(objectDistanceMm) || objectDistanceMm <= 0.0) {
			return 0.0;
		}
		const ParaxialData infinity = ComputeParaxial(lens, lambdaNm);
		const ParaxialTrace finite = TraceParaxial(lens, 1.0, 1.0 / objectDistanceMm, lambdaNm);
		if (finite.u == 0.0) {
			return std::numeric_limits<double>::infinity();
		}
		return -finite.y / finite.u - infinity.bfl;
	}

	const ExitPupilBounds::Bin& ExitPupilBounds::BinFor(double sensorRadius) const
	{
		const double scaled = maxSensorRadius > 0.0 ? sensorRadius / maxSensorRadius * static_cast<double>(bins.size()) : 0.0;
		const size_t index = std::min(static_cast<size_t>(std::max(0.0, scaled)), bins.size() - 1);
		return bins[index];
	}

	namespace {
		// Runs body(index) for index in [0, count) on up to `threads` threads (0: hardware concurrency).
		template <typename Body>
		void ParallelFor(uint32_t count, uint32_t threads, const Body& body)
		{
			if (threads == 0) {
				threads = std::max(1u, std::thread::hardware_concurrency());
			}
			threads = std::min(threads, count);
			if (threads <= 1) {
				for (uint32_t index = 0; index < count; ++index) {
					body(index);
				}
				return;
			}
			std::atomic<uint32_t> next{ 0 };
			std::vector<std::thread> workers;
			workers.reserve(threads);
			for (uint32_t worker = 0; worker < threads; ++worker) {
				workers.emplace_back([&] {
					for (uint32_t index = next.fetch_add(1); index < count; index = next.fetch_add(1)) {
						body(index);
					}
				});
			}
			for (std::thread& worker : workers) {
				worker.join();
			}
		}
	}

	ExitPupilBounds ComputeExitPupilBounds(const LensPrescription& lens, double sensorZ, double maxSensorRadius, uint32_t binCount, uint32_t gridResolution,
		double lambdaNm, const TraceOptions& options, uint32_t threads)
	{
		ExitPupilBounds bounds;
		bounds.sensorZ = sensorZ;
		bounds.rearPlaneZ = lens.RearPlaneZ();
		bounds.maxSensorRadius = maxSensorRadius;
		bounds.bins.resize(binCount);

		const LensPath path = TransmissionPath(lens, false);
		const double extent = lens.surfaces.back().semiAperture * 1.25;
		const double cell = 2.0 * extent / static_cast<double>(gridResolution);

		ParallelFor(binCount, threads, [&](uint32_t binIndex) {
			std::vector<uint8_t> passMask(static_cast<size_t>(gridResolution) * gridResolution, uint8_t{ 0 });
			ExitPupilBounds::Bin& bin = bounds.bins[binIndex];
			double minX = std::numeric_limits<double>::infinity();
			double maxX = -minX;
			double minY = minX;
			double maxY = -minX;

			// Conservative over the bin: union of its two edges and its centre.
			for (const double fraction : { 0.0, 0.5, 1.0 }) {
				const double sensorRadius = (binIndex + fraction) / binCount * maxSensorRadius;
				const Vec3 origin{ sensorRadius, 0.0, sensorZ };
				for (uint32_t row = 0; row < gridResolution; ++row) {
					for (uint32_t column = 0; column < gridResolution; ++column) {
						const double x = -extent + (column + 0.5) * cell;
						const double y = -extent + (row + 0.5) * cell;
						const Ray ray{ origin, Normalize(Vec3{ x, y, bounds.rearPlaneZ } - origin) };
						if (TraceSequential(lens, ray, lambdaNm, path, options).status != TraceStatus::Ok) {
							continue;
						}
						passMask[static_cast<size_t>(row) * gridResolution + column] = 1;
						minX = std::min(minX, x);
						maxX = std::max(maxX, x);
						minY = std::min(minY, y);
						maxY = std::max(maxY, y);
					}
				}
			}
			if (!(minX <= maxX)) {
				return;
			}
			bin.minX = minX - cell;
			bin.maxX = maxX + cell;
			bin.minY = minY - cell;
			bin.maxY = maxY + cell;
			bin.valid = true;

			uint32_t inside = 0;
			uint32_t passing = 0;
			for (uint32_t row = 0; row < gridResolution; ++row) {
				for (uint32_t column = 0; column < gridResolution; ++column) {
					const double x = -extent + (column + 0.5) * cell;
					const double y = -extent + (row + 0.5) * cell;
					if (x < bin.minX || x > bin.maxX || y < bin.minY || y > bin.maxY) {
						continue;
					}
					++inside;
					passing += passMask[static_cast<size_t>(row) * gridResolution + column];
				}
			}
			bin.passFraction = inside > 0 ? static_cast<double>(passing) / inside : 0.0;
		});
		return bounds;
	}

	// ---- Exact pupil: intersections of discs centred on the canonical x axis ----

	bool PupilRegionContains(const PupilRegion& region, double x, double y)
	{
		if (region.count == 0) {
			return false;
		}
		for (uint32_t i = 0; i < region.count; ++i) {
			const double dx = x - region.centre[i];
			if (dx * dx + y * y > region.radius[i] * region.radius[i]) {
				return false;
			}
		}
		return true;
	}

	bool PupilRegionExtent(const PupilRegion& region, double& minX, double& maxX)
	{
		minX = -std::numeric_limits<double>::infinity();
		maxX = std::numeric_limits<double>::infinity();
		for (uint32_t i = 0; i < region.count; ++i) {
			if (!(region.radius[i] > 0.0)) {
				return false;
			}
			minX = std::max(minX, region.centre[i] - region.radius[i]);
			maxX = std::min(maxX, region.centre[i] + region.radius[i]);
		}
		return region.count > 0 && minX < maxX;
	}

	namespace {

		// Twice the area of disc (c, R) left of x, up to a constant: (x - c) h + R^2 asin((x - c) / R), h the half chord.
		double DiscSliceIntegral(double c, double radius, double x)
		{
			const double u = std::clamp((x - c) / radius, -1.0, 1.0);
			return radius * radius * (u * std::sqrt(std::max(1.0 - u * u, 0.0)) + std::asin(u));
		}

		double HalfChord(double c, double radius, double x)
		{
			return std::sqrt(std::max(radius * radius - (x - c) * (x - c), 0.0));
		}

		constexpr uint32_t kMaxRegionIntervals = kMaxPupilDiscs * (kMaxPupilDiscs - 1) / 2 + 1;

		// The intervals of [minX, maxX] over which one disc bounds the region: breakpoints where two discs' half chords are
		// equal (h_i^2 - h_j^2 is linear in x), the disc with the smallest half chord at each interval's midpoint.
		struct RegionIntervals {
			uint32_t count = 0;
			std::array<double, kMaxRegionIntervals + 1> edge{};
			std::array<uint32_t, kMaxRegionIntervals> disc{};
			std::array<double, kMaxRegionIntervals> area{};
			double total = 0.0;
		};

		bool BuildIntervals(const PupilRegion& region, RegionIntervals& out)
		{
			out = {};
			double minX = 0.0, maxX = 0.0;
			if (!PupilRegionExtent(region, minX, maxX)) {
				return false;
			}
			std::array<double, kMaxRegionIntervals + 1> edges{};
			uint32_t edgeCount = 0;
			edges[edgeCount++] = minX;
			for (uint32_t i = 0; i < region.count; ++i) {
				for (uint32_t j = i + 1; j < region.count; ++j) {
					const double dc = region.centre[j] - region.centre[i];
					if (dc == 0.0) {
						continue;
					}
					const double x = (region.radius[i] * region.radius[i] - region.radius[j] * region.radius[j] + region.centre[j] * region.centre[j]
						- region.centre[i] * region.centre[i]) / (2.0 * dc);
					if (x > minX && x < maxX) {
						edges[edgeCount++] = x;
					}
				}
			}
			edges[edgeCount++] = maxX;
			std::sort(edges.begin(), edges.begin() + edgeCount);
			out.edge[0] = edges[0];
			for (uint32_t e = 1; e < edgeCount; ++e) {
				const double a = out.edge[out.count], b = edges[e];
				if (!(b > a)) {
					continue;
				}
				const double m = 0.5 * (a + b);
				uint32_t best = 0;
				double bestH2 = std::numeric_limits<double>::infinity();
				for (uint32_t i = 0; i < region.count; ++i) {
					const double h2 = region.radius[i] * region.radius[i] - (m - region.centre[i]) * (m - region.centre[i]);
					if (h2 < bestH2) {
						bestH2 = h2;
						best = i;
					}
				}
				const double c = region.centre[best], radius = region.radius[best];
				out.disc[out.count] = best;
				out.area[out.count] = DiscSliceIntegral(c, radius, b) - DiscSliceIntegral(c, radius, a);
				out.total += out.area[out.count];
				++out.count;
				out.edge[out.count] = b;
			}
			return out.count > 0 && out.total > 0.0;
		}

	} // namespace

	double PupilRegionArea(const PupilRegion& region)
	{
		RegionIntervals intervals;
		return BuildIntervals(region, intervals) ? intervals.total : 0.0;
	}

	bool SamplePupilRegion(const PupilRegion& region, double u0, double u1, double& x, double& y, double& area)
	{
		RegionIntervals intervals;
		area = 0.0;
		if (!BuildIntervals(region, intervals)) {
			return false;
		}
		area = intervals.total;
		double target = std::clamp(u0, 0.0, 1.0) * intervals.total;
		uint32_t k = 0;
		while (k + 1 < intervals.count && target > intervals.area[k]) {
			target -= intervals.area[k];
			++k;
		}
		target = std::clamp(target, 0.0, intervals.area[k]);
		const double c = region.centre[intervals.disc[k]], radius = region.radius[intervals.disc[k]];
		double lo = intervals.edge[k], hi = intervals.edge[k + 1];
		const double base = DiscSliceIntegral(c, radius, lo);
		// Newton on F(x) - F(lo) = target (F' = 2 h), kept inside a shrinking bracket; bisection when a step leaves it.
		double t = lo + (hi - lo) * (intervals.area[k] > 0.0 ? target / intervals.area[k] : 0.5);
		for (int iteration = 0; iteration < 60; ++iteration) {
			const double f = DiscSliceIntegral(c, radius, t) - base - target;
			if (f > 0.0) {
				hi = t;
			} else {
				lo = t;
			}
			const double slope = 2.0 * HalfChord(c, radius, t);
			double next = slope > 0.0 ? t - f / slope : 0.5 * (lo + hi);
			if (!(next > lo && next < hi)) {
				next = 0.5 * (lo + hi);
			}
			const bool converged = std::abs(next - t) < 1e-13 * std::max(1.0, std::abs(t));
			t = next;
			if (converged) {
				break;
			}
		}
		x = t;
		y = (2.0 * std::clamp(u1, 0.0, 1.0) - 1.0) * HalfChord(c, radius, t);
		return true;
	}

	PupilRegion PupilDiscs::At(double sensorRadius) const
	{
		const uint32_t count = NodeCount();
		if (count < 2 || !(sensorRadius >= 0.0) || !(maxSensorRadius > 0.0)) {
			return {};
		}
		const double scaled = sensorRadius / maxSensorRadius * static_cast<double>(count - 1);
		if (scaled > static_cast<double>(count - 1)) {
			return {};
		}
		const uint32_t index = std::min(static_cast<uint32_t>(scaled), count - 2);
		const double t = scaled - static_cast<double>(index);
		const PupilRegion& a = nodes[index];
		const PupilRegion& b = nodes[index + 1];
		if (a.count == 0 || b.count == 0) {
			return {};
		}
		PupilRegion region;
		region.count = a.count;
		for (uint32_t i = 0; i < a.count; ++i) {
			region.centre[i] = a.centre[i] + (b.centre[i] - a.centre[i]) * t;
			region.radius[i] = a.radius[i] + (b.radius[i] - a.radius[i]) * t;
		}
		return region;
	}

	namespace {

		struct BoundaryPoint {
			double x = 0.0, y = 0.0;
			int label = -1; // the aperture (surface) that blocks just outside, or -1 (missed, TIR)
		};

		struct DiscFitter {
			const LensPrescription& lens;
			LensPath path; // backward transmission
			TraceOptions options;
			double sensorZ = 0.0;
			double rearZ = 0.0;
			double extent = 0.0;
			const PupilDiscOptions& settings;
			std::vector<LensPath> toAperture; // per surface: the backward path from the last surface down to it

			Ray RayTo(double sensorRadius, double x, double y) const
			{
				const Vec3 origin{ sensorRadius, 0.0, sensorZ };
				return { origin, Normalize(Vec3{ x, y, rearZ } - origin) };
			}

			// -2: clears the lens; otherwise the surface that blocks it, or -1 when it is not an aperture.
			int Trace(double sensorRadius, double x, double y, double lambdaNm) const
			{
				TraceOptions trace = options;
				trace.fresnel = false;
				const TraceResult result = TraceSequential(lens, RayTo(sensorRadius, x, y), lambdaNm, path, trace);
				if (result.status == TraceStatus::Ok) {
					return -2;
				}
				if (result.status == TraceStatus::Clipped || result.status == TraceStatus::Stopped) {
					return path.steps[result.failedStep].surface;
				}
				return -1;
			}

			// Whether the ray reaches surface `aperture` inside its limit, with nothing else clipping: 1 inside, 0 outside the
			// aperture, -1 failed before reaching it.
			int ThroughAperture(double sensorRadius, double x, double y, double lambdaNm, uint32_t aperture) const
			{
				TraceOptions trace;
				trace.fresnel = false;
				trace.clipHousing = false;
				trace.clipStop = false;
				std::vector<Vec3> hits;
				const TraceResult result = TraceSequential(lens, RayTo(sensorRadius, x, y), lambdaNm, toAperture[aperture], trace, &hits);
				if (result.status != TraceStatus::Ok || hits.empty()) {
					return -1;
				}
				const Surface& surface = lens.surfaces[aperture];
				const double limit = surface.semiAperture * (surface.isStop ? options.stopScale : 1.0);
				return std::hypot(hits.back().x, hits.back().y) <= limit ? 1 : 0;
			}

			// A point inside the pupil: the centroid of a grid's passing points if it passes, else the passing grid point
			// nearest to it.
			bool Interior(double sensorRadius, double lambdaNm, double& ox, double& oy) const
			{
				constexpr uint32_t kGrid = 40;
				const double cell = 2.0 * extent / kGrid;
				std::vector<std::pair<double, double>> passing;
				double sum = 0.0;
				for (uint32_t row = 0; row < kGrid; ++row) {
					for (uint32_t column = 0; column < kGrid; ++column) {
						const double x = -extent + (column + 0.5) * cell, y = -extent + (row + 0.5) * cell;
						if (Trace(sensorRadius, x, y, lambdaNm) == -2) {
							passing.push_back({ x, y });
							sum += x;
						}
					}
				}
				if (passing.empty()) {
					return false;
				}
				ox = sum / passing.size();
				oy = 0.0;
				if (Trace(sensorRadius, ox, oy, lambdaNm) == -2) {
					return true;
				}
				double best = std::numeric_limits<double>::infinity();
				const double cx = ox;
				for (const auto& [x, y] : passing) {
					const double d = std::hypot(x - cx, y);
					if (d < best) {
						best = d;
						ox = x;
						oy = y;
					}
				}
				return true;
			}

			// The boundary along each direction from (ox, oy), by doubling then bisection on `inside`: the last point
			// inside, labelled with what `inside` reported just outside.
			template <typename Inside>
			void Boundary(double ox, double oy, const Inside& inside, std::vector<BoundaryPoint>& out) const
			{
				for (uint32_t k = 0; k < settings.angles; ++k) {
					const double angle = 2.0 * std::numbers::pi * (k + 0.5) / settings.angles;
					const double ux = std::cos(angle), uy = std::sin(angle);
					double tIn = 0.0, tOut = 0.02 * extent;
					int label = 0;
					bool bounded = false;
					while (tOut < 6.0 * extent) {
						int l = 0;
						if (!inside(ox + tOut * ux, oy + tOut * uy, l)) {
							label = l;
							bounded = true;
							break;
						}
						tIn = tOut;
						tOut *= 2.0;
					}
					if (!bounded) {
						continue;
					}
					for (uint32_t b = 0; b < settings.bisections; ++b) {
						const double mid = 0.5 * (tIn + tOut);
						int l = 0;
						if (inside(ox + mid * ux, oy + mid * uy, l)) {
							tIn = mid;
						} else {
							tOut = mid;
							label = l;
						}
					}
					out.push_back({ ox + tIn * ux, oy + tIn * uy, label });
				}
			}

			// The true pupil's boundary at a sensor radius over the wavelengths (mirrored in y), and an interior point per
			// wavelength (x NaN where none passes).
			std::vector<BoundaryPoint> PupilBoundary(double sensorRadius, std::vector<std::array<double, 2>>* interiors = nullptr) const
			{
				std::vector<BoundaryPoint> points;
				for (const double lambdaNm : settings.wavelengths) {
					double ox = 0.0, oy = 0.0;
					const bool found = Interior(sensorRadius, lambdaNm, ox, oy);
					if (interiors) {
						interiors->push_back(found ? std::array<double, 2>{ ox, oy } : std::array<double, 2>{ std::nan(""), 0.0 });
					}
					if (!found) {
						continue;
					}
					const size_t first = points.size();
					Boundary(ox, oy, [&](double x, double y, int& label) {
						const int result = Trace(sensorRadius, x, y, lambdaNm);
						label = result;
						return result == -2;
					}, points);
					const size_t end = points.size();
					for (size_t i = first; i < end; ++i) {
						points.push_back({ points[i].x, -points[i].y, points[i].label });
					}
				}
				return points;
			}

			// The centre of a circle through an aperture's rim image (its own boundary, nothing else clipping), on the x
			// axis: least squares on x^2 + y^2 = 2 c x + k. False without enough rim points.
			bool FitRimCentre(double sensorRadius, const std::vector<std::array<double, 2>>& interiors, uint32_t aperture, double& centre) const
			{
				std::vector<BoundaryPoint> rim;
				for (size_t w = 0; w < settings.wavelengths.size() && w < interiors.size(); ++w) {
					if (std::isnan(interiors[w][0])) {
						continue;
					}
					const double lambdaNm = settings.wavelengths[w];
					const size_t first = rim.size();
					Boundary(interiors[w][0], interiors[w][1], [&](double x, double y, int& label) {
						const int result = ThroughAperture(sensorRadius, x, y, lambdaNm, aperture);
						label = result; // 0: outside the aperture (a rim point), -1: the trace failed first
						return result == 1;
					}, rim);
					rim.erase(std::remove_if(rim.begin() + first, rim.end(), [](const BoundaryPoint& p) { return p.label != 0; }), rim.end());
				}
				if (rim.size() < 8) {
					return false;
				}
				double sxx = 0.0, sx = 0.0, n = 0.0, sxs = 0.0, ss = 0.0;
				for (const auto& p : rim) {
					const double s = p.x * p.x + p.y * p.y;
					sxx += p.x * p.x;
					sx += p.x;
					n += 1.0;
					sxs += p.x * s;
					ss += s;
				}
				const double det = sxx * n - sx * sx;
				if (!(std::abs(det) > 1e-12 * std::max(1.0, sxx * n))) {
					return false;
				}
				centre = 0.5 * (sxs * n - sx * ss) / det;
				return std::isfinite(centre);
			}
		};

		double MaxDistance(const std::vector<BoundaryPoint>& points, double centre)
		{
			double result = 0.0;
			for (const auto& p : points) {
				result = std::max(result, std::hypot(p.x - centre, p.y));
			}
			return result;
		}

	} // namespace

	PupilDiscs ComputePupilDiscs(const LensPrescription& lens, double sensorZ, double maxSensorRadius, const TraceOptions& options,
		const PupilDiscOptions& discOptions)
	{
		FocusedPupilDiscs focused = ComputeFocusedPupilDiscs(lens, { sensorZ }, maxSensorRadius, options, discOptions, 0);
		PupilDiscs discs = std::move(focused.focus.front());
		discs.buildMs = focused.buildMs;
		return discs;
	}

	PupilDiscs FocusedPupilDiscs::AtFocus(double s) const
	{
		if (focus.empty()) {
			return {};
		}
		if (focus.size() == 1) {
			return focus.front();
		}
		const double last = static_cast<double>(focus.size() - 1);
		s = std::clamp(std::isfinite(s) ? s : 0.0, 0.0, last);
		const size_t index = std::min(static_cast<size_t>(s), focus.size() - 2);
		const double t = s - static_cast<double>(index);
		const PupilDiscs& a = focus[index];
		const PupilDiscs& b = focus[index + 1];
		PupilDiscs out = a;
		out.sensorZ = a.sensorZ + (b.sensorZ - a.sensorZ) * t;
		for (size_t node = 0; node < out.nodes.size() && node < b.nodes.size(); ++node) {
			PupilRegion& region = out.nodes[node];
			const PupilRegion& other = b.nodes[node];
			if (region.count == 0 || other.count == 0) {
				region = {};
				continue;
			}
			for (uint32_t d = 0; d < region.count; ++d) {
				region.centre[d] += (other.centre[d] - region.centre[d]) * t;
				region.radius[d] += (other.radius[d] - region.radius[d]) * t;
			}
		}
		return out;
	}

	double FocusedPupilDiscs::SensorZAt(double s) const
	{
		if (focus.empty()) {
			return 0.0;
		}
		if (focus.size() == 1) {
			return focus.front().sensorZ;
		}
		s = std::clamp(std::isfinite(s) ? s : 0.0, 0.0, static_cast<double>(focus.size() - 1));
		const size_t index = std::min(static_cast<size_t>(s), focus.size() - 2);
		const double t = s - static_cast<double>(index);
		return focus[index].sensorZ + (focus[index + 1].sensorZ - focus[index].sensorZ) * t;
	}

	PupilRegion FocusedPupilDiscs::At(double sensorRadius, double s) const
	{
		if (focus.empty()) {
			return {};
		}
		if (focus.size() == 1) {
			return focus.front().At(sensorRadius);
		}
		s = std::clamp(std::isfinite(s) ? s : 0.0, 0.0, static_cast<double>(focus.size() - 1));
		const size_t index = std::min(static_cast<size_t>(s), focus.size() - 2);
		const double t = s - static_cast<double>(index);
		const PupilRegion a = focus[index].At(sensorRadius);
		const PupilRegion b = focus[index + 1].At(sensorRadius);
		if (a.count == 0 || b.count == 0) {
			return {};
		}
		PupilRegion region = a;
		for (uint32_t d = 0; d < region.count; ++d) {
			region.centre[d] += (b.centre[d] - a.centre[d]) * t;
			region.radius[d] += (b.radius[d] - a.radius[d]) * t;
		}
		return region;
	}

	FocusedPupilDiscs ComputeFocusedPupilDiscs(const LensPrescription& lens, const std::vector<double>& sensorZs, double maxSensorRadius,
		const TraceOptions& options, const PupilDiscOptions& discOptions, uint32_t focusBetweenSamples)
	{
		const auto started = std::chrono::steady_clock::now();
		FocusedPupilDiscs result;
		const uint32_t focusCount = static_cast<uint32_t>(sensorZs.size());
		if (focusCount == 0) {
			return result;
		}
		PupilDiscOptions settings = discOptions;
		settings.nodes = std::max(settings.nodes, 2u);
		settings.angles = std::max(settings.angles, 8u);
		settings.maxDiscs = std::clamp(settings.maxDiscs, 1u, kMaxPupilDiscs);
		if (settings.wavelengths.empty()) {
			settings.wavelengths = { kLambdaD };
		}
		const uint32_t nodeCount = settings.nodes;
		const double rearZ = lens.RearPlaneZ();
		result.focus.assign(focusCount, PupilDiscs{});
		for (uint32_t f = 0; f < focusCount; ++f) {
			result.focus[f].sensorZ = sensorZs[f];
			result.focus[f].rearPlaneZ = rearZ;
			result.focus[f].maxSensorRadius = maxSensorRadius;
			result.focus[f].nodes.assign(nodeCount, PupilRegion{});
		}
		const auto finish = [&]() {
			result.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			for (auto& discs : result.focus) {
				discs.buildMs = result.buildMs;
			}
			return std::move(result);
		};

		DiscFitter base{ lens, TransmissionPath(lens, false), options, sensorZs[0], rearZ, lens.surfaces.back().semiAperture * 1.25, settings, {} };
		const uint32_t surfaceCount = lens.SurfaceCount();
		base.toAperture.resize(surfaceCount);
		for (uint32_t s = 0; s < surfaceCount; ++s) {
			base.toAperture[s].steps.assign(base.path.steps.begin(), base.path.steps.begin() + (surfaceCount - s));
		}
		std::vector<DiscFitter> fitters;
		fitters.reserve(focusCount);
		for (uint32_t f = 0; f < focusCount; ++f) {
			fitters.push_back(base);
			fitters.back().sensorZ = sensorZs[f];
		}
		const auto radiusAt = [&](double node) { return node / (nodeCount - 1) * maxSensorRadius; };
		const auto at = [nodeCount](uint32_t f, uint32_t node) { return static_cast<size_t>(f) * nodeCount + node; };

		// 1. The pupil's boundary at each focus and radius node, and which apertures bind it (at any of them).
		std::vector<std::vector<BoundaryPoint>> boundary(static_cast<size_t>(focusCount) * nodeCount);
		std::vector<std::vector<std::array<double, 2>>> interiors(boundary.size());
		ParallelFor(static_cast<uint32_t>(boundary.size()), settings.threads, [&](uint32_t job) {
			const uint32_t f = job / nodeCount, node = job % nodeCount;
			boundary[job] = fitters[f].PupilBoundary(radiusAt(node), &interiors[job]);
		});
		std::vector<uint32_t> binding;
		std::vector<uint32_t> lastValid(focusCount, 0);
		for (uint32_t f = 0; f < focusCount; ++f) {
			if (boundary[at(f, 0)].empty()) {
				return finish();
			}
			for (uint32_t node = 0; node < nodeCount; ++node) {
				if (boundary[at(f, node)].empty()) {
					break; // light ends here
				}
				lastValid[f] = node;
				for (const auto& p : boundary[at(f, node)]) {
					if (p.label >= 0 && std::find(binding.begin(), binding.end(), static_cast<uint32_t>(p.label)) == binding.end()) {
						binding.push_back(static_cast<uint32_t>(p.label));
					}
				}
			}
		}
		if (binding.empty()) {
			return finish();
		}
		std::sort(binding.begin(), binding.end());

		// 2. Each binding aperture's disc per node: the centre from its rim image, the radius the smallest that holds the
		// pupil's boundary points.
		std::vector<std::vector<double>> centres(boundary.size(), std::vector<double>(binding.size(), 0.0));
		std::vector<std::vector<double>> radii(boundary.size(), std::vector<double>(binding.size(), 0.0));
		ParallelFor(static_cast<uint32_t>(boundary.size()), settings.threads, [&](uint32_t job) {
			const uint32_t f = job / nodeCount, node = job % nodeCount;
			if (node > lastValid[f]) {
				return;
			}
			double meanX = 0.0;
			for (const auto& p : boundary[job]) {
				meanX += p.x;
			}
			meanX /= static_cast<double>(boundary[job].size());
			for (size_t d = 0; d < binding.size(); ++d) {
				double centre = meanX;
				if (!fitters[f].FitRimCentre(radiusAt(node), interiors[job], binding[d], centre)) {
					// No usable rim image: centre it on the pupil points this aperture blocks, else on all of them.
					double sum = 0.0;
					uint32_t count = 0;
					for (const auto& p : boundary[job]) {
						if (p.label == static_cast<int>(binding[d])) {
							sum += p.x;
							++count;
						}
					}
					centre = count ? sum / count : meanX;
				}
				centres[job][d] = centre;
				radii[job][d] = MaxDistance(boundary[job], centre);
			}
		});

		// 3. Past the last node with light, nodes extrapolated linearly from the last two, so the light between them is held: one
		// node with one focus, and with several, up to the last node any focus needs (the others' light, between focus nodes, is
		// checked in step 5).
		uint32_t commonLast = 0;
		for (uint32_t f = 0; f < focusCount; ++f) {
			commonLast = std::max(commonLast, lastValid[f] + 1 < nodeCount ? lastValid[f] + 1 : lastValid[f]);
		}
		for (uint32_t f = 0; f < focusCount; ++f) {
			for (uint32_t node = lastValid[f] + 1; node <= commonLast; ++node) {
				const uint32_t previous = node - 1;
				const uint32_t beforePrevious = previous > 0 ? previous - 1 : previous;
				for (size_t d = 0; d < binding.size(); ++d) {
					centres[at(f, node)][d] = 2.0 * centres[at(f, previous)][d] - centres[at(f, beforePrevious)][d];
					radii[at(f, node)][d] = std::max(0.0, 2.0 * radii[at(f, previous)][d] - radii[at(f, beforePrevious)][d]);
				}
			}
		}

		// 4. At most maxDiscs: drop the disc that cuts the least off the others' intersection at any node, while it cuts
		// nothing or there are too many. Measured on a grid over each node's pupil, so any number of discs compares.
		std::vector<size_t> kept(binding.size());
		for (size_t d = 0; d < kept.size(); ++d) {
			kept[d] = d;
		}
		const auto importance = [&](size_t d) {
			double value = 0.0;
			for (uint32_t f = 0; f < focusCount; ++f) {
				for (uint32_t node = 0; node <= lastValid[f]; ++node) {
					const auto& points = boundary[at(f, node)];
					const auto& c = centres[at(f, node)];
					const auto& r = radii[at(f, node)];
					double minX = std::numeric_limits<double>::infinity(), maxX = -minX, maxY = 0.0;
					for (const auto& p : points) {
						minX = std::min(minX, p.x);
						maxX = std::max(maxX, p.x);
						maxY = std::max(maxY, std::abs(p.y));
					}
					const double pad = 0.05 * (maxX - minX) + 1e-6;
					constexpr uint32_t kGrid = 96;
					uint32_t all = 0, others = 0;
					for (uint32_t row = 0; row < kGrid; ++row) {
						for (uint32_t column = 0; column < kGrid; ++column) {
							const double x = minX - pad + (maxX - minX + 2.0 * pad) * (column + 0.5) / kGrid;
							const double y = (maxY + pad) * (row + 0.5) / kGrid;
							bool inAll = true, inOthers = true;
							for (const size_t e : kept) {
								if (std::hypot(x - c[e], y) > r[e]) {
									inAll = false;
									inOthers = inOthers && e == d;
								}
							}
							all += inAll ? 1u : 0u;
							others += inOthers ? 1u : 0u;
						}
					}
					if (others > 0) {
						value = std::max(value, 1.0 - double(all) / double(others));
					}
				}
			}
			return value;
		};
		while (kept.size() > 1) {
			size_t weakest = 0;
			double weakestImportance = std::numeric_limits<double>::infinity();
			for (size_t k = 0; k < kept.size(); ++k) {
				const double value = importance(kept[k]);
				if (value < weakestImportance) {
					weakestImportance = value;
					weakest = k;
				}
			}
			if (kept.size() <= settings.maxDiscs && weakestImportance > 0.0) {
				break;
			}
			kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(weakest));
		}
		for (const size_t d : kept) {
			result.apertures.push_back(binding[d]);
		}

		// 5. Between nodes the interpolated discs must hold the boundary found there too: widen the nodes' radii by any
		// shortfall. Between radius nodes at each focus node (linear), and between focus nodes (bilinear: the corners grow).
		std::vector<std::vector<double>> widen(boundary.size(), std::vector<double>(kept.size(), 0.0));
		if (commonLast > 0) {
			// Radius intervals at each focus node.
			std::vector<std::vector<double>> shortfall(boundary.size(), std::vector<double>(kept.size(), 0.0)); // per (focus, interval)
			ParallelFor(focusCount * commonLast, settings.threads, [&](uint32_t job) {
				const uint32_t f = job / commonLast, interval = job % commonLast;
				auto& out = shortfall[at(f, interval)];
				for (uint32_t s = 1; s <= settings.betweenSamples; ++s) {
					const double t = double(s) / (settings.betweenSamples + 1);
					const std::vector<BoundaryPoint> points = fitters[f].PupilBoundary(radiusAt(interval + t));
					for (size_t k = 0; k < kept.size(); ++k) {
						const size_t d = kept[k];
						const auto& c0 = centres[at(f, interval)];
						const auto& c1 = centres[at(f, interval + 1)];
						const auto& r0 = radii[at(f, interval)];
						const auto& r1 = radii[at(f, interval + 1)];
						const double c = c0[d] + (c1[d] - c0[d]) * t;
						const double r = r0[d] + (r1[d] - r0[d]) * t;
						out[k] = std::max(out[k], MaxDistance(points, c) - r);
					}
				}
			});
			for (uint32_t f = 0; f < focusCount; ++f) {
				for (uint32_t interval = 0; interval < commonLast; ++interval) {
					for (size_t k = 0; k < kept.size(); ++k) {
						const double value = shortfall[at(f, interval)][k];
						widen[at(f, interval)][k] = std::max(widen[at(f, interval)][k], value);
						widen[at(f, interval + 1)][k] = std::max(widen[at(f, interval + 1)][k], value);
					}
				}
			}
		}
		if (focusCount > 1 && focusBetweenSamples > 0 && commonLast > 0) {
			// Between focus nodes: at each radius node and halfway to the next (radius sample 2n is node n, 2n + 1 halfway past it),
			// on focusBetweenSamples planes per focus interval.
			const uint32_t radiusSamples = 2u * commonLast + 1u;
			const uint32_t jobs = (focusCount - 1) * focusBetweenSamples * radiusSamples;
			const auto cornerOf = [&](uint32_t radiusSample, uint32_t& node, uint32_t& nextNode, double& tr) {
				node = std::min(radiusSample / 2u, commonLast - 1u);
				nextNode = node + 1u;
				tr = radiusSample == radiusSamples - 1u ? 1.0 : (radiusSample % 2u) * 0.5;
			};
			std::vector<std::vector<double>> shortfall(jobs, std::vector<double>(kept.size(), 0.0));
			ParallelFor(jobs, settings.threads, [&](uint32_t job) {
				const uint32_t interval = job / (focusBetweenSamples * radiusSamples);
				const uint32_t plane = (job / radiusSamples) % focusBetweenSamples;
				uint32_t node = 0, nextNode = 0;
				double tr = 0.0;
				cornerOf(job % radiusSamples, node, nextNode, tr);
				const double tf = double(plane + 1) / (focusBetweenSamples + 1);
				DiscFitter fitter = base;
				fitter.sensorZ = sensorZs[interval] + (sensorZs[interval + 1] - sensorZs[interval]) * tf;
				const std::vector<BoundaryPoint> points = fitter.PupilBoundary(radiusAt(node + tr));
				if (points.empty()) {
					return;
				}
				for (size_t k = 0; k < kept.size(); ++k) {
					const size_t d = kept[k];
					const auto bilinear = [&](const std::vector<std::vector<double>>& values) {
						const double a = values[at(interval, node)][d] + (values[at(interval, nextNode)][d] - values[at(interval, node)][d]) * tr;
						const double b = values[at(interval + 1, node)][d] + (values[at(interval + 1, nextNode)][d] - values[at(interval + 1, node)][d]) * tr;
						return a + (b - a) * tf;
					};
					shortfall[job][k] = std::max(0.0, MaxDistance(points, bilinear(centres)) - bilinear(radii));
				}
			});
			for (uint32_t job = 0; job < jobs; ++job) {
				const uint32_t interval = job / (focusBetweenSamples * radiusSamples);
				uint32_t node = 0, nextNode = 0;
				double tr = 0.0;
				cornerOf(job % radiusSamples, node, nextNode, tr);
				for (size_t k = 0; k < kept.size(); ++k) {
					const double value = shortfall[job][k];
					if (!(value > 0.0)) {
						continue;
					}
					for (const uint32_t f : { interval, interval + 1 }) {
						if (tr < 1.0) widen[at(f, node)][k] = std::max(widen[at(f, node)][k], value);
						if (tr > 0.0) widen[at(f, nextNode)][k] = std::max(widen[at(f, nextNode)][k], value);
					}
				}
			}
		}

		for (uint32_t f = 0; f < focusCount; ++f) {
			PupilDiscs& discs = result.focus[f];
			discs.apertures = result.apertures;
			for (uint32_t node = 0; node <= commonLast; ++node) {
				PupilRegion& region = discs.nodes[node];
				region.count = static_cast<uint32_t>(kept.size());
				for (size_t k = 0; k < kept.size(); ++k) {
					region.centre[k] = centres[at(f, node)][kept[k]];
					region.radius[k] = (radii[at(f, node)][kept[k]] + widen[at(f, node)][k]) * (1.0 + settings.relativeMargin) + settings.absoluteMarginMm;
				}
			}
		}
		return finish();
	}

	double FocusRange::Coordinate(double sensorShift) const
	{
		if (shift.size() < 2 || !(shiftMax > shiftMin)) {
			return 0.0;
		}
		const double t = (std::clamp(sensorShift, shiftMin, shiftMax) - shiftMin) / (shiftMax - shiftMin);
		return t * static_cast<double>(shift.size() - 1);
	}

	FocusRange MakeFocusRange(const LensPrescription& lens, double nearFocusMm, uint32_t count, double sensorZMin, double sensorZMax, double lambdaNm)
	{
		FocusRange range;
		range.baseSensorZ = lens.sensorZ;
		double nearShift = SensorShiftForFocus(lens, nearFocusMm, lambdaNm);
		if (!std::isfinite(nearShift) || nearShift < 0.0) {
			nearShift = 0.0;
		}
		range.shiftMin = std::clamp(0.0, sensorZMin - lens.sensorZ, sensorZMax - lens.sensorZ);
		range.shiftMax = std::clamp(nearShift, sensorZMin - lens.sensorZ, sensorZMax - lens.sensorZ);
		if (range.shiftMax < range.shiftMin) {
			range.shiftMax = range.shiftMin;
		}
		count = std::max(count, range.shiftMax > range.shiftMin ? 2u : 1u);
		for (uint32_t i = 0; i < count; ++i) {
			const double shift = count > 1 ? range.shiftMin + (range.shiftMax - range.shiftMin) * i / (count - 1) : range.shiftMin;
			range.shift.push_back(shift);
			range.sensorZ.push_back(lens.sensorZ + shift);
			range.distanceMm.push_back(FocusDistanceForShift(lens, shift, lambdaNm));
		}
		return range;
	}

	double FocusDistanceForShift(const LensPrescription& lens, double sensorShift, double lambdaNm)
	{
		if (!(sensorShift > 0.0)) {
			return std::numeric_limits<double>::infinity();
		}
		// SensorShiftForFocus grows with 1 / distance until the object reaches the front focal point: bracket in 1 / distance.
		double lo = 0.0, hi = 1.0e-3;
		for (int i = 0; i < 60; ++i) {
			const double shift = SensorShiftForFocus(lens, 1.0 / hi, lambdaNm);
			if (!std::isfinite(shift) || shift < 0.0 || shift >= sensorShift) {
				break;
			}
			lo = hi;
			hi *= 2.0;
		}
		for (int i = 0; i < 80; ++i) {
			const double mid = 0.5 * (lo + hi);
			const double shift = SensorShiftForFocus(lens, 1.0 / mid, lambdaNm);
			if (std::isfinite(shift) && shift >= 0.0 && shift < sensorShift) {
				lo = mid;
			} else {
				hi = mid;
			}
		}
		const double u = 0.5 * (lo + hi);
		return u > 0.0 ? 1.0 / u : std::numeric_limits<double>::infinity();
	}

	double CentreNormalization(const LensPrescription& lens, const PupilDiscs& discs, const TraceOptions& options, uint32_t grid, double lambdaNm)
	{
		const PupilRegion region = discs.At(0.0);
		double minX = 0.0, maxX = 0.0;
		if (!PupilRegionExtent(region, minX, maxX) || grid == 0) {
			return 0.0;
		}
		double halfY = std::numeric_limits<double>::infinity();
		for (uint32_t d = 0; d < region.count; ++d) {
			halfY = std::min(halfY, region.radius[d]);
		}
		const LensPath backward = TransmissionPath(lens, false);
		const double cellX = (maxX - minX) / grid, cellY = 2.0 * halfY / grid;
		const Vec3 origin{ 0.0, 0.0, discs.sensorZ };
		double normalization = 0.0;
		for (uint32_t row = 0; row < grid; ++row) {
			for (uint32_t column = 0; column < grid; ++column) {
				const Vec3 rear{ minX + (column + 0.5) * cellX, -halfY + (row + 0.5) * cellY, discs.rearPlaneZ };
				const Vec3 toRear = rear - origin;
				const double distance2 = Dot(toRear, toRear);
				const Ray ray{ origin, toRear * (1.0 / std::sqrt(distance2)) };
				const TraceResult result = TraceSequential(lens, ray, lambdaNm, backward, options);
				if (result.status != TraceStatus::Ok) {
					continue;
				}
				normalization += result.throughput * ray.dir.z * ray.dir.z / distance2 * cellX * cellY;
			}
		}
		return normalization;
	}

	PupilDiscStats MeasurePupilDiscs(const LensPrescription& lens, const PupilDiscs& discs, const TraceOptions& options, uint64_t raysPerInterval,
		double lambdaMin, double lambdaMax, double boxMargin, uint64_t seed, uint32_t threads)
	{
		PupilDiscStats stats;
		const uint32_t intervalCount = discs.NodeCount() > 1 ? discs.NodeCount() - 1 : 0;
		stats.intervals.resize(intervalCount);
		const LensPath path = TransmissionPath(lens, false);
		constexpr uint64_t kChunk = 1u << 15;
		const uint64_t chunks = std::max<uint64_t>(1, (raysPerInterval + kChunk - 1) / kChunk);
		std::vector<PupilDiscStats::Interval> partial(static_cast<size_t>(intervalCount * chunks));
		ParallelFor(static_cast<uint32_t>(partial.size()), threads, [&](uint32_t job) {
			const uint32_t interval = static_cast<uint32_t>(job / chunks);
			const uint64_t chunk = job % chunks;
			if (chunk * kChunk >= raysPerInterval) {
				return;
			}
			PupilDiscStats::Interval& out = partial[job];
			std::mt19937_64 random(seed * 0x9E3779B97F4A7C15ull + job);
			std::uniform_real_distribution<double> unit(0.0, 1.0);
			const uint64_t count = std::min(kChunk, raysPerInterval - chunk * kChunk);
			for (uint64_t index = 0; index < count; ++index) {
				const double sensorRadius = (interval + unit(random)) / intervalCount * discs.maxSensorRadius;
				const PupilRegion region = discs.At(sensorRadius);
				double minX = 0.0, maxX = 0.0;
				if (!PupilRegionExtent(region, minX, maxX)) {
					continue;
				}
				double halfY = std::numeric_limits<double>::infinity();
				uint32_t smallest = 0;
				for (uint32_t d = 0; d < region.count; ++d) {
					halfY = std::min(halfY, region.radius[d]);
					if (region.radius[d] < region.radius[smallest]) {
						smallest = d;
					}
				}
				const double width = maxX - minX, height = 2.0 * halfY;
				const double x0 = minX - boxMargin * width, x1 = maxX + boxMargin * width;
				const double y0 = -halfY - boxMargin * height, y1 = halfY + boxMargin * height;
				const double x = x0 + (x1 - x0) * unit(random);
				const double y = y0 + (y1 - y0) * unit(random);
				const double lambdaNm = lambdaMax > lambdaMin ? lambdaMin + (lambdaMax - lambdaMin) * unit(random) : lambdaMin;
				const bool inRegion = PupilRegionContains(region, x, y);
				const bool inSmallest = std::hypot(x - region.centre[smallest], y) <= region.radius[smallest];
				++out.rays;
				out.inRegion += inRegion ? 1u : 0u;
				out.inSmallest += inSmallest ? 1u : 0u;
				out.regionAreaSum += PupilRegionArea(region);
				out.smallestAreaSum += std::numbers::pi * region.radius[smallest] * region.radius[smallest];
				out.boxAreaSum += (x1 - x0) * (y1 - y0);
				const Vec3 origin{ sensorRadius, 0.0, discs.sensorZ };
				const Vec3 toRear = Vec3{ x, y, discs.rearPlaneZ } - origin;
				const double distance2 = Dot(toRear, toRear);
				const Ray ray{ origin, toRear * (1.0 / std::sqrt(distance2)) };
				const TraceResult result = TraceSequential(lens, ray, lambdaNm, path, options);
				if (result.status != TraceStatus::Ok) {
					out.stoppedInRegion += inRegion && result.status == TraceStatus::Stopped ? 1u : 0u;
					out.clippedInRegion += inRegion && result.status == TraceStatus::Clipped ? 1u : 0u;
					continue;
				}
				const double energy = result.throughput * ray.dir.z * ray.dir.z / distance2;
				out.passInSmallest += inSmallest ? 1u : 0u;
				if (inRegion) {
					++out.passInRegion;
					out.energyInRegion += energy;
				} else {
					++out.passOutside;
					out.energyOutside += energy;
				}
			}
		});
		for (uint32_t interval = 0; interval < intervalCount; ++interval) {
			PupilDiscStats::Interval& total = stats.intervals[interval];
			for (uint64_t chunk = 0; chunk < chunks; ++chunk) {
				const PupilDiscStats::Interval& part = partial[interval * chunks + chunk];
				total.rays += part.rays;
				total.inRegion += part.inRegion;
				total.inSmallest += part.inSmallest;
				total.passInRegion += part.passInRegion;
				total.passInSmallest += part.passInSmallest;
				total.passOutside += part.passOutside;
				total.stoppedInRegion += part.stoppedInRegion;
				total.clippedInRegion += part.clippedInRegion;
				total.energyInRegion += part.energyInRegion;
				total.energyOutside += part.energyOutside;
				total.regionAreaSum += part.regionAreaSum;
				total.smallestAreaSum += part.smallestAreaSum;
				total.boxAreaSum += part.boxAreaSum;
			}
		}
		return stats;
	}

	bool FindChiefRay(const LensPrescription& lens, double fieldAngle, double lambdaNm, Ray& chiefRay)
	{
		const Vec3 dir{ std::sin(fieldAngle), 0.0, std::cos(fieldAngle) };
		const double startZ = lens.FrontPlaneZ() - 1.0;
		const LensPath path = TransmissionPath(lens, true);
		const TraceOptions options = NoClipping();

		// Signed stop-plane height of the ray entering at height h; NaN if it cannot reach the stop.
		const LensPath toStop{ std::vector<PathStep>(path.steps.begin(), path.steps.begin() + lens.stopIndex + 1) };
		const auto stopHeight = [&](double h) {
			const TraceResult result = TraceSequential(lens, Ray{ { h, 0.0, startZ }, dir }, lambdaNm, toStop, options);
			return result.status == TraceStatus::Ok && result.crossedStop ? result.stopHit.x : std::numeric_limits<double>::quiet_NaN();
		};

		if (fieldAngle == 0.0) {
			chiefRay = { { 0.0, 0.0, startZ }, dir };
			return true;
		}
		const double range = lens.surfaces.front().semiAperture * 2.0;
		constexpr int kSamples = 129;
		double bestLo = 0.0;
		double bestHi = 0.0;
		double bestDistance = std::numeric_limits<double>::infinity();
		double previousH = -range;
		double previousValue = stopHeight(previousH);
		for (int i = 1; i < kSamples; ++i) {
			const double h = -range + 2.0 * range * i / (kSamples - 1);
			const double value = stopHeight(h);
			if (std::isfinite(value) && std::isfinite(previousValue) && (value == 0.0 || (value > 0.0) != (previousValue > 0.0))) {
				const double distance = std::abs(0.5 * (h + previousH));
				if (distance < bestDistance) {
					bestDistance = distance;
					bestLo = previousH;
					bestHi = h;
				}
			}
			previousH = h;
			previousValue = value;
		}
		if (!std::isfinite(bestDistance)) {
			return false;
		}
		double lo = bestLo;
		double hi = bestHi;
		double valueLo = stopHeight(lo);
		for (int iteration = 0; iteration < 100 && hi - lo > 1.0e-13; ++iteration) {
			const double mid = 0.5 * (lo + hi);
			const double value = stopHeight(mid);
			if (!std::isfinite(value)) {
				return false;
			}
			if ((value > 0.0) == (valueLo > 0.0)) {
				lo = mid;
				valueLo = value;
			} else {
				hi = mid;
			}
		}
		chiefRay = { { 0.5 * (lo + hi), 0.0, startZ }, dir };
		return true;
	}

	double DistortionTable::ImageRadiusAt(double angle) const
	{
		if (fieldAngle.size() < 2) {
			return 0.0;
		}
		const double step = fieldAngle[1] - fieldAngle[0];
		const double scaled = std::clamp(angle / step, 0.0, static_cast<double>(fieldAngle.size() - 1));
		const size_t index = std::min(static_cast<size_t>(scaled), fieldAngle.size() - 2);
		const double t = scaled - static_cast<double>(index);
		return imageRadius[index] + (imageRadius[index + 1] - imageRadius[index]) * t;
	}

	double DistortionTable::FieldAngleAt(double radius) const
	{
		if (imageRadius.size() < 2) {
			return 0.0;
		}
		const auto upper = std::upper_bound(imageRadius.begin(), imageRadius.end(), radius);
		if (upper == imageRadius.begin()) {
			return fieldAngle.front();
		}
		if (upper == imageRadius.end()) {
			return fieldAngle.back();
		}
		const size_t index = static_cast<size_t>(upper - imageRadius.begin()) - 1;
		const double t = (radius - imageRadius[index]) / (imageRadius[index + 1] - imageRadius[index]);
		return fieldAngle[index] + (fieldAngle[index + 1] - fieldAngle[index]) * t;
	}

	DistortionTable ComputeDistortionTable(const LensPrescription& lens, double sensorZ, double maxFieldAngle, uint32_t sampleCount, double lambdaNm)
	{
		DistortionTable table;
		table.sensorZ = sensorZ;
		const LensPath path = TransmissionPath(lens, true);
		const TraceOptions options = NoClipping();
		for (uint32_t i = 0; i < sampleCount; ++i) {
			const double angle = maxFieldAngle * i / (sampleCount - 1);
			Ray chief;
			if (!FindChiefRay(lens, angle, lambdaNm, chief)) {
				break;
			}
			const TraceResult result = TraceSequential(lens, chief, lambdaNm, path, options);
			Ray exit = result.exit;
			if (result.status != TraceStatus::Ok || !PropagateToPlane(exit, sensorZ)) {
				break;
			}
			// Light travelling toward +x lands at +x on the sensor (the inverted image of an object at -x).
			const double radius = exit.origin.x;
			if (!table.imageRadius.empty() && radius <= table.imageRadius.back()) {
				break;
			}
			table.fieldAngle.push_back(angle);
			table.imageRadius.push_back(radius);
		}
		table.maxFieldAngle = table.fieldAngle.empty() ? 0.0 : table.fieldAngle.back();
		return table;
	}

	std::vector<DistortionTable> ComputeDistortionTables(const LensPrescription& lens, const std::vector<double>& sensorZs, double maxFieldAngle,
		uint32_t sampleCount, double lambdaNm)
	{
		std::vector<DistortionTable> tables(sensorZs.size());
		for (size_t f = 0; f < sensorZs.size(); ++f) {
			tables[f].sensorZ = sensorZs[f];
		}
		const LensPath path = TransmissionPath(lens, true);
		const TraceOptions options = NoClipping();
		bool open = !tables.empty();
		for (uint32_t i = 0; i < sampleCount && open; ++i) {
			const double angle = maxFieldAngle * i / (sampleCount - 1);
			Ray chief;
			if (!FindChiefRay(lens, angle, lambdaNm, chief)) {
				break;
			}
			const TraceResult result = TraceSequential(lens, chief, lambdaNm, path, options);
			if (result.status != TraceStatus::Ok) {
				break;
			}
			// Every plane's entry, or none: the tables stay the same length.
			std::vector<double> radii(tables.size());
			for (size_t f = 0; f < tables.size() && open; ++f) {
				Ray exit = result.exit;
				if (!PropagateToPlane(exit, sensorZs[f]) || (!tables[f].imageRadius.empty() && exit.origin.x <= tables[f].imageRadius.back())) {
					open = false;
				}
				radii[f] = exit.origin.x;
			}
			if (!open) {
				break;
			}
			for (size_t f = 0; f < tables.size(); ++f) {
				tables[f].fieldAngle.push_back(angle);
				tables[f].imageRadius.push_back(radii[f]);
			}
		}
		for (auto& table : tables) {
			table.maxFieldAngle = table.fieldAngle.empty() ? 0.0 : table.fieldAngle.back();
		}
		return tables;
	}

	std::vector<RankedPath> RankGhostPaths(const LensPrescription& lens, const std::vector<double>& fieldAngles, uint32_t gridResolution, double lambdaNm,
		double sensorHalfExtentMm)
	{
		std::vector<RankedPath> ranked;
		for (LensPath& path : EnumerateGhostPaths(lens)) {
			ranked.push_back({ std::move(path), 0.0 });
		}
		const LensPath transmission = TransmissionPath(lens, true);
		const double startZ = lens.FrontPlaneZ() - 1.0;
		const double extent = lens.surfaces.front().semiAperture;
		const double cell = 2.0 * extent / gridResolution;
		double transmitted = 0.0;

		const auto landed = [&](const TraceResult& result) {
			if (result.status != TraceStatus::Ok) {
				return false;
			}
			Ray exit = result.exit;
			if (exit.dir.z <= 0.0 || !PropagateToPlane(exit, lens.sensorZ)) {
				return false;
			}
			return sensorHalfExtentMm <= 0.0 || std::sqrt(exit.origin.x * exit.origin.x + exit.origin.y * exit.origin.y) <= sensorHalfExtentMm;
		};

		for (const double angle : fieldAngles) {
			const Vec3 dir{ std::sin(angle), 0.0, std::cos(angle) };
			const double shift = -std::tan(angle) * 1.0;
			for (uint32_t row = 0; row < gridResolution; ++row) {
				for (uint32_t column = 0; column < gridResolution; ++column) {
					const double x = -extent + (column + 0.5) * cell;
					const double y = -extent + (row + 0.5) * cell;
					if (x * x + y * y > extent * extent) {
						continue;
					}
					const Ray ray{ { x + shift, y, startZ }, dir };
					const TraceResult main = TraceSequential(lens, ray, lambdaNm, transmission);
					if (landed(main)) {
						transmitted += main.throughput;
					}
					for (RankedPath& entry : ranked) {
						const TraceResult ghost = TraceSequential(lens, ray, lambdaNm, entry.path);
						if (landed(ghost)) {
							entry.energy += ghost.throughput;
						}
					}
				}
			}
		}
		for (RankedPath& entry : ranked) {
			entry.energy = transmitted > 0.0 ? entry.energy / transmitted : 0.0;
		}
		std::stable_sort(ranked.begin(), ranked.end(), [](const RankedPath& a, const RankedPath& b) { return a.energy > b.energy; });
		return ranked;
	}

} // namespace lens
