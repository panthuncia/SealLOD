#pragma once

// Lens prescriptions for the physical camera: surfaces, media and coatings, loaded from
// lenses/<name>.lens.json. CPU-only, double precision; this is the ground truth every faster lens
// representation (HLSL explicit tracing, the polynomial lens) is validated against.
//
// Coordinates: millimetres, optical axis +z, light from the scene travels +z. Surface 0 is the one
// nearest the scene and its vertex sits at z = 0. A positive radius puts the centre of curvature on
// the +z (sensor) side, the usual optics sign convention.

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lens {

	struct Vec3 {
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline Vec3 operator-(Vec3 a) { return { -a.x, -a.y, -a.z }; }
	inline Vec3 operator*(Vec3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
	inline Vec3 operator*(double s, Vec3 a) { return a * s; }
	inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
	inline Vec3 Normalize(Vec3 a) { return a * (1.0 / Length(a)); }

	// Fraunhofer lines, nm.
	inline constexpr double kLambdaD = 587.5618;
	inline constexpr double kLambdaF = 486.1327;
	inline constexpr double kLambdaC = 656.2725;

	struct Glass {
		enum class Model : uint8_t { Constant, Cauchy, Sellmeier };

		std::string name;
		Model model = Model::Constant;
		double nd = 1.0;
		double vd = 0.0;
		// Sellmeier: n^2 = 1 + sum B_i L^2 / (L^2 - C_i), L in micrometres, C in um^2.
		std::array<double, 3> sellmeierB{};
		std::array<double, 3> sellmeierC{};
		// Cauchy fitted through nd and nF - nC = (nd - 1) / vd: n = A + B / lambda_nm^2.
		double cauchyA = 1.0;
		double cauchyB = 0.0;

		double IndexAt(double lambdaNm) const;

		static Glass Constant(std::string name, double n);
		static Glass FromAbbe(std::string name, double nd, double vd);
		static Glass FromSellmeier(std::string name, std::array<double, 3> b, std::array<double, 3> c);
	};

	using GlassCatalog = std::unordered_map<std::string, Glass>;

	// Single-layer quarter-wave anti-reflection film at designWavelengthNm.
	struct Coating {
		bool enabled = false;
		double index = 1.38; // MgF2
		double designWavelengthNm = 550.0;
	};

	struct Surface {
		double radius = 0.0; // 0 = flat
		double conic = 0.0;
		std::vector<double> aspheric; // coefficients of r^4, r^6, r^8, ...
		double thickness = 0.0;      // to the next surface, or to the sensor for the last one
		double semiAperture = 0.0;   // clear radius; rays outside are blocked by the housing
		bool isStop = false;
		uint32_t mediumAfter = 0; // index into LensPrescription::media
		Coating coating;
		double vertexZ = 0.0; // filled by the loader

		double Curvature() const { return radius == 0.0 ? 0.0 : 1.0 / radius; }
		bool HasAspheric() const { return !aspheric.empty(); }
		// Sag z(r) relative to the vertex, or NaN outside the conic's domain.
		double Sag(double r) const;
		// dz/dr, or NaN outside the conic's domain.
		double SagSlope(double r) const;
	};

	struct LensPrescription {
		std::string name;
		std::string source;
		std::vector<Glass> media; // media[0] is air
		std::vector<Surface> surfaces;
		uint32_t stopIndex = 0;
		double sensorZ = 0.0;                  // image plane for infinity focus
		double imageCircleRadiusMm = 0.0;      // design half-diagonal, 0 if unknown
		double nominalFocalLengthMm = 0.0;     // as published, for validation only
		double nominalFNumber = 0.0;           // as published, for validation only
		uint64_t sourceHash = 0;               // FNV-1a over the prescription and glass sources

		uint32_t MediumBefore(uint32_t surface) const { return surface == 0 ? 0u : surfaces[surface - 1].mediumAfter; }
		double IndexBefore(uint32_t surface, double lambdaNm) const { return media[MediumBefore(surface)].IndexAt(lambdaNm); }
		double IndexAfter(uint32_t surface, double lambdaNm) const { return media[surfaces[surface].mediumAfter].IndexAt(lambdaNm); }
		uint32_t SurfaceCount() const { return static_cast<uint32_t>(surfaces.size()); }
		// A surface that can reflect: the media on its two sides differ.
		bool IsInterface(uint32_t surface) const;
		// Furthest +z point of the last surface, a plane every backward camera ray can start behind.
		double RearPlaneZ() const;
		// Furthest -z point of the first surface.
		double FrontPlaneZ() const;
	};

	struct LoadResult {
		std::optional<LensPrescription> lens;
		std::string error;
	};

	uint64_t Fnv1a64(std::string_view bytes, uint64_t seed = 0xcbf29ce484222325ull);

	std::optional<GlassCatalog> ParseGlassCatalog(std::string_view json, std::string* error);
	std::optional<GlassCatalog> LoadGlassCatalog(const std::filesystem::path& path, std::string* error);

	// The last surface's "thickness" may be the string "paraxial", which places the sensor at the
	// paraxial back focal distance for kLambdaD.
	LoadResult ParsePrescription(std::string_view json, const GlassCatalog& catalog, uint64_t catalogHash = 0);
	// Loads glass_catalog.json from the same directory when it exists.
	LoadResult LoadPrescription(const std::filesystem::path& path);

	// Fills vertexZ and validates; used by the loader and by code that builds lenses directly.
	bool FinalizePrescription(LensPrescription& lens, std::string* error);

} // namespace lens
