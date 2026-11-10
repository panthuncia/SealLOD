#pragma once

// Exact sequential ray tracing through a LensPrescription along one optical path: a fixed sequence
// of transmissions and reflections. This is the reference that datasets are generated from and that
// the HLSL explicit tracer must match.

#include <cstdint>
#include <string>
#include <vector>

#include "LensCore/Lens.h"

namespace lens {

	struct Ray {
		Vec3 origin;
		Vec3 dir; // unit
	};

	enum class Event : uint8_t { Transmit, Reflect };

	struct PathStep {
		uint16_t surface = 0;
		Event event = Event::Transmit;
	};

	struct LensPath {
		std::vector<PathStep> steps;

		LensPath Reversed() const;
		uint32_t ReflectionCount() const;
		// "T" for the transmission path, "R<a>R<b>" for a ghost, with the reflecting surfaces in
		// the order the light meets them.
		std::string Code() const;
	};

	// All surfaces front to back (forward, scene to sensor) or back to front (backward).
	LensPath TransmissionPath(const LensPrescription& lens, bool forward);
	// Forward ghost: reflect at surface a, travel back to b < a, reflect again, exit to the sensor.
	LensPath GhostPath(const LensPrescription& lens, uint32_t a, uint32_t b);
	// Every two-reflection ghost whose reflecting surfaces are real interfaces.
	std::vector<LensPath> EnumerateGhostPaths(const LensPrescription& lens);

	enum class TraceStatus : uint8_t {
		Ok,
		Missed,                 // no intersection with the next surface
		Clipped,                // outside a surface's clear aperture
		Stopped,                // outside the aperture stop
		TotalInternalReflection,
	};
	const char* ToString(TraceStatus status);

	struct TraceOptions {
		bool fresnel = true;      // false: throughput stays 1
		bool clipHousing = true;  // semi-aperture tests on refracting surfaces
		bool clipStop = true;
		double stopScale = 1.0;   // multiplies the stop's semi-aperture (stopping down)
	};

	struct TraceResult {
		TraceStatus status = TraceStatus::Ok;
		uint32_t failedStep = 0;
		Ray exit;                  // ray leaving the last surface of the path
		double throughput = 1.0;   // product of Fresnel T or R terms
		Vec3 stopHit{};            // where the path last crossed the stop plane (0 if it didn't)
		bool crossedStop = false;
	};

	TraceResult TraceSequential(const LensPrescription& lens, const Ray& ray, double lambdaNm, const LensPath& path, const TraceOptions& options = {},
		std::vector<Vec3>* hits = nullptr);

	// Intersects the ray with the surface (vertex at surface.vertexZ). Returns false when there is
	// no valid hit ahead of the origin; otherwise the hit and the unit normal, oriented toward +z.
	bool IntersectSurface(const Surface& surface, const Ray& ray, Vec3& hit, Vec3& normal);

	// Unpolarized Fresnel reflectance from medium n1 to n2 at incidence cosine cosI, with an optional
	// single-layer film between them.
	double FresnelReflectance(double n1, double n2, double cosI, const Coating& coating, double lambdaNm);

	// Moves a ray to the plane z = planeZ along its direction. Returns false if parallel or behind.
	bool PropagateToPlane(Ray& ray, double planeZ);

} // namespace lens
