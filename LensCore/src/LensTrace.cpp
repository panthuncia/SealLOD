#include "LensCore/LensTrace.h"

#include <algorithm>
#include <limits>
#include <numbers>

namespace lens {

	namespace {
		constexpr double kHitEpsilon = 1.0e-7;

		double FresnelUncoated(double n1, double n2, double cosI, double cosT)
		{
			const double rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
			const double rp = (n2 * cosI - n1 * cosT) / (n2 * cosI + n1 * cosT);
			return 0.5 * (rs * rs + rp * rp);
		}

		// |r|^2 of a film between two interfaces with real amplitude coefficients r12 and r23 and
		// round-trip phase delta.
		double FilmReflectance(double r12, double r23, double cosDelta)
		{
			const double cross = 2.0 * r12 * r23 * cosDelta;
			return (r12 * r12 + r23 * r23 + cross) / (1.0 + r12 * r12 * r23 * r23 + cross);
		}
	} // namespace

	LensPath LensPath::Reversed() const
	{
		LensPath out;
		out.steps.assign(steps.rbegin(), steps.rend());
		return out;
	}

	uint32_t LensPath::ReflectionCount() const
	{
		return static_cast<uint32_t>(std::count_if(steps.begin(), steps.end(), [](const PathStep& s) { return s.event == Event::Reflect; }));
	}

	std::string LensPath::Code() const
	{
		std::string code;
		for (const PathStep& step : steps) {
			if (step.event == Event::Reflect) {
				code += "R" + std::to_string(step.surface);
			}
		}
		return code.empty() ? std::string("T") : code;
	}

	LensPath TransmissionPath(const LensPrescription& lens, bool forward)
	{
		LensPath path;
		const uint32_t count = lens.SurfaceCount();
		for (uint32_t i = 0; i < count; ++i) {
			path.steps.push_back({ static_cast<uint16_t>(forward ? i : count - 1 - i), Event::Transmit });
		}
		return path;
	}

	LensPath GhostPath(const LensPrescription& lens, uint32_t a, uint32_t b)
	{
		LensPath path;
		const uint32_t count = lens.SurfaceCount();
		if (b >= a || a >= count) {
			return path;
		}
		for (uint32_t i = 0; i < a; ++i) {
			path.steps.push_back({ static_cast<uint16_t>(i), Event::Transmit });
		}
		path.steps.push_back({ static_cast<uint16_t>(a), Event::Reflect });
		for (uint32_t i = a - 1; i > b; --i) {
			path.steps.push_back({ static_cast<uint16_t>(i), Event::Transmit });
		}
		path.steps.push_back({ static_cast<uint16_t>(b), Event::Reflect });
		for (uint32_t i = b + 1; i < count; ++i) {
			path.steps.push_back({ static_cast<uint16_t>(i), Event::Transmit });
		}
		return path;
	}

	std::vector<LensPath> EnumerateGhostPaths(const LensPrescription& lens)
	{
		std::vector<LensPath> paths;
		for (uint32_t a = 1; a < lens.SurfaceCount(); ++a) {
			if (!lens.IsInterface(a)) {
				continue;
			}
			for (uint32_t b = 0; b < a; ++b) {
				if (lens.IsInterface(b)) {
					paths.push_back(GhostPath(lens, a, b));
				}
			}
		}
		return paths;
	}

	const char* ToString(TraceStatus status)
	{
		switch (status) {
		case TraceStatus::Ok:
			return "ok";
		case TraceStatus::Missed:
			return "missed";
		case TraceStatus::Clipped:
			return "clipped";
		case TraceStatus::Stopped:
			return "stopped";
		case TraceStatus::TotalInternalReflection:
			return "tir";
		}
		return "?";
	}

	bool PropagateToPlane(Ray& ray, double planeZ)
	{
		if (std::abs(ray.dir.z) < 1.0e-15) {
			return false;
		}
		const double t = (planeZ - ray.origin.z) / ray.dir.z;
		if (t < -kHitEpsilon) {
			return false;
		}
		ray.origin = ray.origin + ray.dir * t;
		return true;
	}

	bool IntersectSurface(const Surface& surface, const Ray& ray, Vec3& hit, Vec3& normal)
	{
		const Vec3 p{ ray.origin.x, ray.origin.y, ray.origin.z - surface.vertexZ };
		const Vec3 d = ray.dir;
		const double c = surface.Curvature();
		double t = std::numeric_limits<double>::infinity();

		if (c == 0.0) {
			if (std::abs(d.z) < 1.0e-15) {
				return false;
			}
			t = -p.z / d.z;
		} else {
			// c (x^2 + y^2 + (1 + k) z^2) - 2 z = 0, on the sheet through the vertex.
			const double k1 = 1.0 + surface.conic;
			const double a = c * (d.x * d.x + d.y * d.y + k1 * d.z * d.z);
			const double b = 2.0 * (c * (p.x * d.x + p.y * d.y + k1 * p.z * d.z) - d.z);
			const double cc = c * (p.x * p.x + p.y * p.y + k1 * p.z * p.z) - 2.0 * p.z;
			double roots[2];
			int rootCount = 0;
			if (std::abs(a) < 1.0e-15) {
				if (std::abs(b) < 1.0e-15) {
					return false;
				}
				roots[rootCount++] = -cc / b;
			} else {
				const double disc = b * b - 4.0 * a * cc;
				if (disc < 0.0) {
					return false;
				}
				const double q = -0.5 * (b + std::copysign(std::sqrt(disc), b));
				roots[rootCount++] = q / a;
				if (q != 0.0) {
					roots[rootCount++] = cc / q;
				}
			}
			for (int i = 0; i < rootCount; ++i) {
				const double candidate = roots[i];
				if (candidate <= kHitEpsilon || candidate >= t) {
					continue;
				}
				const double z = p.z + candidate * d.z;
				if (k1 * c * z < 1.0 + 1.0e-12) {
					t = candidate;
				}
			}
		}
		if (!(t > kHitEpsilon) || !std::isfinite(t)) {
			if (!surface.HasAspheric()) {
				return false;
			}
			// Aspheric departure can put the hit where the base conic has none; start from the plane.
			if (std::abs(d.z) < 1.0e-15) {
				return false;
			}
			t = -p.z / d.z;
			if (!(t > kHitEpsilon)) {
				return false;
			}
		}

		if (surface.HasAspheric()) {
			// Newton on g(t) = z(t) - sag(r(t)).
			bool converged = false;
			for (int iteration = 0; iteration < 32; ++iteration) {
				const Vec3 q = p + d * t;
				const double r = std::sqrt(q.x * q.x + q.y * q.y);
				const double sag = surface.Sag(r);
				const double slope = surface.SagSlope(r);
				if (!std::isfinite(sag) || !std::isfinite(slope)) {
					return false;
				}
				const double g = q.z - sag;
				const double drdt = r > 0.0 ? (q.x * d.x + q.y * d.y) / r : 0.0;
				const double dg = d.z - slope * drdt;
				if (std::abs(dg) < 1.0e-15) {
					return false;
				}
				const double step = g / dg;
				t -= step;
				if (std::abs(step) < 1.0e-12) {
					converged = true;
					break;
				}
			}
			if (!converged || !(t > kHitEpsilon)) {
				return false;
			}
		}

		const Vec3 local = p + d * t;
		const double r = std::sqrt(local.x * local.x + local.y * local.y);
		if (c == 0.0 && !surface.HasAspheric()) {
			normal = { 0.0, 0.0, 1.0 };
		} else {
			const double slope = surface.SagSlope(r);
			if (!std::isfinite(slope)) {
				return false;
			}
			normal = r > 0.0 ? Normalize(Vec3{ -slope * local.x / r, -slope * local.y / r, 1.0 }) : Vec3{ 0.0, 0.0, 1.0 };
		}
		hit = { local.x, local.y, local.z + surface.vertexZ };
		return true;
	}

	double FresnelReflectance(double n1, double n2, double cosI, const Coating& coating, double lambdaNm)
	{
		if (n1 == n2) {
			return 0.0;
		}
		cosI = std::clamp(cosI, 0.0, 1.0);
		const double sinI2 = 1.0 - cosI * cosI;
		const double sinT2 = (n1 / n2) * (n1 / n2) * sinI2;
		if (sinT2 >= 1.0) {
			return 1.0;
		}
		const double cosT = std::sqrt(1.0 - sinT2);
		if (!coating.enabled) {
			return FresnelUncoated(n1, n2, cosI, cosT);
		}
		const double nc = coating.index;
		const double sinC2 = (n1 / nc) * (n1 / nc) * sinI2;
		if (sinC2 >= 1.0) {
			return FresnelUncoated(n1, n2, cosI, cosT);
		}
		const double cosC = std::sqrt(1.0 - sinC2);
		// Quarter-wave thickness lambda0 / (4 nc): round-trip phase pi (lambda0 / lambda) cos(theta_c).
		const double delta = std::numbers::pi * (coating.designWavelengthNm / lambdaNm) * cosC;
		const double cosDelta = std::cos(delta);
		const double r12s = (n1 * cosI - nc * cosC) / (n1 * cosI + nc * cosC);
		const double r23s = (nc * cosC - n2 * cosT) / (nc * cosC + n2 * cosT);
		const double r12p = (nc * cosI - n1 * cosC) / (nc * cosI + n1 * cosC);
		const double r23p = (n2 * cosC - nc * cosT) / (n2 * cosC + nc * cosT);
		return 0.5 * (FilmReflectance(r12s, r23s, cosDelta) + FilmReflectance(r12p, r23p, cosDelta));
	}

	TraceResult TraceSequential(const LensPrescription& lens, const Ray& ray, double lambdaNm, const LensPath& path, const TraceOptions& options,
		std::vector<Vec3>* hits)
	{
		TraceResult result;
		Ray current{ ray.origin, Normalize(ray.dir) };
		for (uint32_t stepIndex = 0; stepIndex < path.steps.size(); ++stepIndex) {
			const PathStep& step = path.steps[stepIndex];
			const Surface& surface = lens.surfaces[step.surface];
			Vec3 hit;
			Vec3 normal;
			if (!IntersectSurface(surface, current, hit, normal)) {
				result.status = TraceStatus::Missed;
				result.failedStep = stepIndex;
				return result;
			}
			if (hits) {
				hits->push_back(hit);
			}
			const double radius = std::sqrt(hit.x * hit.x + hit.y * hit.y);
			if (surface.isStop) {
				if (options.clipStop && radius > surface.semiAperture * options.stopScale) {
					result.status = TraceStatus::Stopped;
					result.failedStep = stepIndex;
					return result;
				}
				result.stopHit = hit;
				result.crossedStop = true;
				current.origin = hit;
				continue;
			}
			if (options.clipHousing && radius > surface.semiAperture) {
				result.status = TraceStatus::Clipped;
				result.failedStep = stepIndex;
				return result;
			}

			const bool travellingForward = current.dir.z > 0.0;
			const double nBefore = lens.IndexBefore(step.surface, lambdaNm);
			const double nAfter = lens.IndexAfter(step.surface, lambdaNm);
			const double n1 = travellingForward ? nBefore : nAfter;
			const double n2 = travellingForward ? nAfter : nBefore;
			if (Dot(normal, current.dir) > 0.0) {
				normal = -normal;
			}
			const double cosI = -Dot(normal, current.dir);
			const double reflectance = options.fresnel ? FresnelReflectance(n1, n2, cosI, surface.coating, lambdaNm) : 0.0;

			Vec3 next;
			if (step.event == Event::Transmit) {
				const double eta = n1 / n2;
				const double k = 1.0 - eta * eta * (1.0 - cosI * cosI);
				if (k < 0.0) {
					result.status = TraceStatus::TotalInternalReflection;
					result.failedStep = stepIndex;
					return result;
				}
				next = current.dir * eta + normal * (eta * cosI - std::sqrt(k));
				if (options.fresnel) {
					result.throughput *= 1.0 - reflectance;
				}
			} else {
				next = current.dir + normal * (2.0 * cosI);
				if (options.fresnel) {
					result.throughput *= reflectance;
				}
			}
			current.origin = hit;
			current.dir = Normalize(next);
		}
		result.exit = current;
		return result;
	}

} // namespace lens
