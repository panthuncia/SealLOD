#include "LensCore/Lens.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

#include "LensCore/LensAnalysis.h"

namespace lens {

	namespace {
		using nlohmann::json;

		bool ReadFile(const std::filesystem::path& path, std::string& out)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return false;
			}
			std::ostringstream stream;
			stream << file.rdbuf();
			out = stream.str();
			return true;
		}

		bool SameGlass(const Glass& a, const Glass& b)
		{
			return a.model == b.model && a.name == b.name && a.nd == b.nd && a.vd == b.vd && a.sellmeierB == b.sellmeierB && a.sellmeierC == b.sellmeierC;
		}

		uint32_t AddMedium(LensPrescription& lens, const Glass& glass)
		{
			for (uint32_t i = 0; i < lens.media.size(); ++i) {
				if (SameGlass(lens.media[i], glass)) {
					return i;
				}
			}
			lens.media.push_back(glass);
			return static_cast<uint32_t>(lens.media.size() - 1);
		}

		std::optional<Glass> ParseMedium(const json& value, const GlassCatalog& catalog, std::string& error)
		{
			if (value.is_string()) {
				const std::string name = value.get<std::string>();
				if (name == "air") {
					return Glass::Constant("air", 1.0);
				}
				if (const auto it = catalog.find(name); it != catalog.end()) {
					return it->second;
				}
				error = "unknown glass '" + name + "'";
				return std::nullopt;
			}
			if (!value.is_object()) {
				error = "medium must be a string or an object";
				return std::nullopt;
			}
			if (value.contains("glass")) {
				return ParseMedium(value["glass"], catalog, error);
			}
			if (value.contains("nd")) {
				const double nd = value["nd"].get<double>();
				const double vd = value.value("vd", 0.0);
				std::string name = value.value("name", std::string{});
				if (vd <= 0.0) {
					return Glass::Constant(std::move(name), nd);
				}
				return Glass::FromAbbe(std::move(name), nd, vd);
			}
			if (value.contains("n")) {
				return Glass::Constant(value.value("name", std::string{}), value["n"].get<double>());
			}
			error = "medium object needs 'glass', 'nd' or 'n'";
			return std::nullopt;
		}

		std::optional<Glass> ParseCatalogGlass(const std::string& name, const json& value, std::string& error)
		{
			if (value.contains("sellmeier")) {
				const json& s = value["sellmeier"];
				const auto b = s.at("B").get<std::vector<double>>();
				const auto c = s.at("C").get<std::vector<double>>();
				if (b.size() != 3 || c.size() != 3) {
					error = "glass '" + name + "': sellmeier needs three B and three C terms";
					return std::nullopt;
				}
				return Glass::FromSellmeier(name, { b[0], b[1], b[2] }, { c[0], c[1], c[2] });
			}
			if (value.contains("nd")) {
				return Glass::FromAbbe(name, value["nd"].get<double>(), value.at("vd").get<double>());
			}
			error = "glass '" + name + "' needs 'sellmeier' or 'nd'/'vd'";
			return std::nullopt;
		}
	} // namespace

	double Glass::IndexAt(double lambdaNm) const
	{
		switch (model) {
		case Model::Constant:
			return nd;
		case Model::Cauchy:
			return cauchyA + cauchyB / (lambdaNm * lambdaNm);
		case Model::Sellmeier: {
			const double l2 = (lambdaNm * 1.0e-3) * (lambdaNm * 1.0e-3);
			double n2 = 1.0;
			for (int i = 0; i < 3; ++i) {
				n2 += sellmeierB[i] * l2 / (l2 - sellmeierC[i]);
			}
			return std::sqrt(n2);
		}
		}
		return nd;
	}

	Glass Glass::Constant(std::string name, double n)
	{
		Glass glass;
		glass.name = std::move(name);
		glass.model = Model::Constant;
		glass.nd = n;
		return glass;
	}

	Glass Glass::FromAbbe(std::string name, double nd, double vd)
	{
		Glass glass;
		glass.name = std::move(name);
		glass.model = Model::Cauchy;
		glass.nd = nd;
		glass.vd = vd;
		const double dispersion = (nd - 1.0) / vd; // nF - nC
		glass.cauchyB = dispersion / (1.0 / (kLambdaF * kLambdaF) - 1.0 / (kLambdaC * kLambdaC));
		glass.cauchyA = nd - glass.cauchyB / (kLambdaD * kLambdaD);
		return glass;
	}

	Glass Glass::FromSellmeier(std::string name, std::array<double, 3> b, std::array<double, 3> c)
	{
		Glass glass;
		glass.name = std::move(name);
		glass.model = Model::Sellmeier;
		glass.sellmeierB = b;
		glass.sellmeierC = c;
		glass.nd = glass.IndexAt(kLambdaD);
		const double nF = glass.IndexAt(kLambdaF);
		const double nC = glass.IndexAt(kLambdaC);
		glass.vd = (glass.nd - 1.0) / (nF - nC);
		return glass;
	}

	double Surface::Sag(double r) const
	{
		const double c = Curvature();
		const double r2 = r * r;
		const double root = 1.0 - (1.0 + conic) * c * c * r2;
		if (root < 0.0) {
			return std::numeric_limits<double>::quiet_NaN();
		}
		double z = c * r2 / (1.0 + std::sqrt(root));
		double power = r2 * r2;
		for (const double a : aspheric) {
			z += a * power;
			power *= r2;
		}
		return z;
	}

	double Surface::SagSlope(double r) const
	{
		const double c = Curvature();
		const double r2 = r * r;
		const double root = 1.0 - (1.0 + conic) * c * c * r2;
		if (root <= 0.0) {
			return std::numeric_limits<double>::quiet_NaN();
		}
		double slope = c * r / std::sqrt(root);
		double power = r2 * r; // r^3
		double exponent = 4.0;
		for (const double a : aspheric) {
			slope += exponent * a * power;
			power *= r2;
			exponent += 2.0;
		}
		return slope;
	}

	bool LensPrescription::IsInterface(uint32_t surface) const
	{
		const uint32_t before = MediumBefore(surface);
		const uint32_t after = surfaces[surface].mediumAfter;
		return before != after && std::abs(media[before].nd - media[after].nd) > 1.0e-9;
	}

	double LensPrescription::RearPlaneZ() const
	{
		const Surface& last = surfaces.back();
		const double sag = last.Sag(last.semiAperture);
		return last.vertexZ + (std::isfinite(sag) ? std::max(0.0, sag) : 0.0);
	}

	double LensPrescription::FrontPlaneZ() const
	{
		const Surface& first = surfaces.front();
		const double sag = first.Sag(first.semiAperture);
		return first.vertexZ + (std::isfinite(sag) ? std::min(0.0, sag) : 0.0);
	}

	uint64_t Fnv1a64(std::string_view bytes, uint64_t seed)
	{
		uint64_t hash = seed;
		for (const char ch : bytes) {
			hash ^= static_cast<uint8_t>(ch);
			hash *= 0x100000001b3ull;
		}
		return hash;
	}

	std::optional<GlassCatalog> ParseGlassCatalog(std::string_view text, std::string* error)
	{
		std::string localError;
		try {
			const json root = json::parse(text);
			GlassCatalog catalog;
			for (const auto& [name, value] : root.at("glasses").items()) {
				auto glass = ParseCatalogGlass(name, value, localError);
				if (!glass) {
					break;
				}
				catalog.emplace(name, std::move(*glass));
			}
			if (localError.empty()) {
				return catalog;
			}
		} catch (const std::exception& e) {
			localError = e.what();
		}
		if (error) {
			*error = localError;
		}
		return std::nullopt;
	}

	std::optional<GlassCatalog> LoadGlassCatalog(const std::filesystem::path& path, std::string* error)
	{
		std::string text;
		if (!ReadFile(path, text)) {
			if (error) {
				*error = "cannot read " + path.string();
			}
			return std::nullopt;
		}
		return ParseGlassCatalog(text, error);
	}

	bool FinalizePrescription(LensPrescription& lens, std::string* error)
	{
		const auto fail = [&](std::string message) {
			if (error) {
				*error = std::move(message);
			}
			return false;
		};
		if (lens.surfaces.size() < 2) {
			return fail("a lens needs at least two surfaces");
		}
		if (lens.media.empty() || lens.media[0].nd != 1.0) {
			return fail("media[0] must be air");
		}
		uint32_t stops = 0;
		double z = 0.0;
		for (uint32_t i = 0; i < lens.surfaces.size(); ++i) {
			Surface& surface = lens.surfaces[i];
			surface.vertexZ = z;
			z += surface.thickness;
			if (surface.semiAperture <= 0.0) {
				return fail("surface " + std::to_string(i) + " has no semi-aperture");
			}
			if (surface.mediumAfter >= lens.media.size()) {
				return fail("surface " + std::to_string(i) + " names a missing medium");
			}
			if (surface.isStop) {
				lens.stopIndex = i;
				++stops;
				if (surface.radius != 0.0 || lens.MediumBefore(i) != surface.mediumAfter) {
					return fail("the stop must be flat with the same medium on both sides");
				}
			}
			if (i + 1 < lens.surfaces.size() && surface.thickness < 0.0) {
				return fail("surface " + std::to_string(i) + " has a negative thickness");
			}
		}
		if (stops != 1) {
			return fail("a lens needs exactly one aperture stop");
		}
		if (lens.media[lens.surfaces.back().mediumAfter].nd != 1.0) {
			return fail("the image space must be air");
		}
		lens.sensorZ = z;
		return true;
	}

	LoadResult ParsePrescription(std::string_view text, const GlassCatalog& catalog, uint64_t catalogHash)
	{
		LoadResult result;
		try {
			const json root = json::parse(text);
			LensPrescription lens;
			lens.name = root.value("name", std::string{});
			lens.source = root.value("source", std::string{});
			lens.nominalFocalLengthMm = root.value("nominalFocalLengthMm", 0.0);
			lens.nominalFNumber = root.value("nominalFNumber", 0.0);
			lens.imageCircleRadiusMm = root.value("imageCircleRadiusMm", 0.0);
			lens.media.push_back(Glass::Constant("air", 1.0));

			Coating defaultCoating;
			if (root.contains("defaultCoating")) {
				const json& c = root["defaultCoating"];
				defaultCoating.enabled = c.is_boolean() ? c.get<bool>() : true;
				if (c.is_object()) {
					defaultCoating.index = c.value("index", defaultCoating.index);
					defaultCoating.designWavelengthNm = c.value("designWavelengthNm", defaultCoating.designWavelengthNm);
				}
			}

			bool paraxialImage = false;
			const json& surfaces = root.at("surfaces");
			for (size_t i = 0; i < surfaces.size(); ++i) {
				const json& s = surfaces[i];
				Surface surface;
				surface.isStop = s.value("stop", false);
				surface.radius = s.value("radius", 0.0);
				surface.conic = s.value("conic", 0.0);
				if (s.contains("aspheric")) {
					surface.aspheric = s["aspheric"].get<std::vector<double>>();
				}
				surface.semiAperture = s.at("semiAperture").get<double>();
				const json& thickness = s.at("thickness");
				if (thickness.is_string()) {
					if (thickness.get<std::string>() != "paraxial" || i + 1 != surfaces.size()) {
						result.error = "only the last surface's thickness may be \"paraxial\"";
						return result;
					}
					paraxialImage = true;
				} else {
					surface.thickness = thickness.get<double>();
				}
				if (surface.isStop) {
					surface.mediumAfter = lens.surfaces.empty() ? 0u : lens.surfaces.back().mediumAfter;
				} else {
					std::string mediumError;
					const auto medium = ParseMedium(s.at("medium"), catalog, mediumError);
					if (!medium) {
						result.error = "surface " + std::to_string(i) + ": " + mediumError;
						return result;
					}
					surface.mediumAfter = AddMedium(lens, *medium);
				}
				lens.surfaces.push_back(std::move(surface));
			}

			// Coatings go on glass-air interfaces; cemented interfaces are left bare.
			for (uint32_t i = 0; i < lens.surfaces.size(); ++i) {
				Surface& surface = lens.surfaces[i];
				const json& s = surfaces[i];
				const bool glassAir = lens.media[lens.MediumBefore(i)].nd == 1.0 || lens.media[surface.mediumAfter].nd == 1.0;
				if (!surface.isStop && glassAir && lens.IsInterface(i)) {
					surface.coating = defaultCoating;
				}
				if (s.contains("coating")) {
					const json& c = s["coating"];
					surface.coating.enabled = c.is_boolean() ? c.get<bool>() : true;
					if (c.is_object()) {
						surface.coating.index = c.value("index", 1.38);
						surface.coating.designWavelengthNm = c.value("designWavelengthNm", 550.0);
					}
				}
			}

			std::string error;
			if (!FinalizePrescription(lens, &error)) {
				result.error = error;
				return result;
			}
			if (paraxialImage) {
				lens.surfaces.back().thickness = ComputeParaxial(lens, kLambdaD).bfl;
				if (!FinalizePrescription(lens, &error)) {
					result.error = error;
					return result;
				}
			}
			lens.sourceHash = Fnv1a64(text, catalogHash == 0 ? 0xcbf29ce484222325ull : catalogHash);
			result.lens = std::move(lens);
		} catch (const std::exception& e) {
			result.error = e.what();
		}
		return result;
	}

	LoadResult LoadPrescription(const std::filesystem::path& path)
	{
		LoadResult result;
		std::string text;
		if (!ReadFile(path, text)) {
			result.error = "cannot read " + path.string();
			return result;
		}
		GlassCatalog catalog;
		uint64_t catalogHash = 0;
		const std::filesystem::path catalogPath = path.parent_path() / "glass_catalog.json";
		if (std::filesystem::exists(catalogPath)) {
			std::string catalogText;
			std::string error;
			if (!ReadFile(catalogPath, catalogText)) {
				result.error = "cannot read " + catalogPath.string();
				return result;
			}
			auto parsed = ParseGlassCatalog(catalogText, &error);
			if (!parsed) {
				result.error = catalogPath.string() + ": " + error;
				return result;
			}
			catalog = std::move(*parsed);
			catalogHash = Fnv1a64(catalogText);
		}
		result = ParsePrescription(text, catalog, catalogHash);
		if (!result.lens && !result.error.empty()) {
			result.error = path.string() + ": " + result.error;
		}
		return result;
	}

} // namespace lens
