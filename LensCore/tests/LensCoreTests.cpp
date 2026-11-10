// Checks the CPU reference lens tracer against closed-form optics and against the published
// constants of the lenses in lenses/. No GPU.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numbers>

#include "LensCore/BrLens.h"
#include "LensCore/Lens.h"
#include "LensCore/LensAnalysis.h"
#include "LensCore/LensDataset.h"
#include "LensCore/LensPolynomial.h"
#include "LensCore/LensTrace.h"

namespace {
	using namespace lens;

	int g_failures = 0;

#define CHECK(condition)                                                                    \
	do {                                                                                    \
		if (!(condition)) {                                                                 \
			std::fprintf(stderr, "%s:%d FAILED: %s\n", __FILE__, __LINE__, #condition);     \
			++g_failures;                                                                   \
		}                                                                                   \
	} while (false)

#define CHECK_NEAR(a, b, tolerance)                                                                                                \
	do {                                                                                                                           \
		const double _a = (a);                                                                                                     \
		const double _b = (b);                                                                                                     \
		if (!(std::abs(_a - _b) <= (tolerance))) {                                                                                 \
			std::fprintf(stderr, "%s:%d FAILED: %s = %.9g, expected %.9g +- %.3g\n", __FILE__, __LINE__, #a, _a, _b, (double)(tolerance)); \
			++g_failures;                                                                                                          \
		}                                                                                                                          \
	} while (false)

	std::filesystem::path LensDir() { return std::filesystem::path(LENS_CORE_LENS_DIR); }

	LensPrescription Load(const char* name)
	{
		LoadResult result = LoadPrescription(LensDir() / name);
		if (!result.lens) {
			std::fprintf(stderr, "cannot load %s: %s\n", name, result.error.c_str());
			++g_failures;
			return {};
		}
		return std::move(*result.lens);
	}

	// Biconvex N-BK7 singlet with a stop at the first vertex.
	LensPrescription MakeSinglet(double r1, double r2, double thickness, double n)
	{
		LensPrescription lens;
		lens.media = { Glass::Constant("air", 1.0), Glass::Constant("glass", n) };
		Surface stop;
		stop.isStop = true;
		stop.semiAperture = 5.0;
		stop.thickness = 0.5;
		Surface front;
		front.radius = r1;
		front.thickness = thickness;
		front.semiAperture = 10.0;
		front.mediumAfter = 1;
		Surface back;
		back.radius = r2;
		back.thickness = 90.0;
		back.semiAperture = 10.0;
		back.mediumAfter = 0;
		lens.surfaces = { stop, front, back };
		std::string error;
		if (!FinalizePrescription(lens, &error)) {
			std::fprintf(stderr, "singlet: %s\n", error.c_str());
			++g_failures;
		}
		return lens;
	}

	void TestGlass()
	{
		std::string error;
		const auto catalog = LoadGlassCatalog(LensDir() / "glass_catalog.json", &error);
		CHECK(catalog.has_value());
		if (!catalog) {
			return;
		}
		const Glass& bk7 = catalog->at("N-BK7");
		CHECK_NEAR(bk7.IndexAt(kLambdaD), 1.5168, 2.0e-4);
		CHECK_NEAR(bk7.vd, 64.17, 0.1);
		const Glass& sf5 = catalog->at("N-SF5");
		CHECK_NEAR(sf5.IndexAt(kLambdaD), 1.6727, 5.0e-4);

		const Glass abbe = Glass::FromAbbe("x", 1.67, 47.1);
		CHECK_NEAR(abbe.IndexAt(kLambdaD), 1.67, 1.0e-12);
		CHECK_NEAR((abbe.IndexAt(kLambdaD) - 1.0) / (abbe.IndexAt(kLambdaF) - abbe.IndexAt(kLambdaC)), 47.1, 1.0e-9);
		CHECK(abbe.IndexAt(450.0) > abbe.IndexAt(650.0));
	}

	void TestFresnel()
	{
		const Coating bare;
		// Normal incidence: ((n1 - n2) / (n1 + n2))^2, symmetric in direction.
		CHECK_NEAR(FresnelReflectance(1.0, 1.5, 1.0, bare, 550.0), 0.04, 1.0e-12);
		CHECK_NEAR(FresnelReflectance(1.5, 1.0, 1.0, bare, 550.0), 0.04, 1.0e-12);
		// Beyond the critical angle.
		CHECK_NEAR(FresnelReflectance(1.5, 1.0, std::cos(60.0 * std::numbers::pi / 180.0), bare, 550.0), 1.0, 0.0);
		// Grazing incidence reflects everything.
		CHECK(FresnelReflectance(1.0, 1.5, 1.0e-6, bare, 550.0) > 0.99);

		// Quarter-wave film at its design wavelength: ((n1 n2 - nc^2) / (n1 n2 + nc^2))^2.
		Coating film;
		film.enabled = true;
		film.index = 1.38;
		film.designWavelengthNm = 550.0;
		const double expected = std::pow((1.0 * 1.52 - 1.38 * 1.38) / (1.0 * 1.52 + 1.38 * 1.38), 2.0);
		CHECK_NEAR(FresnelReflectance(1.0, 1.52, 1.0, film, 550.0), expected, 1.0e-12);
		CHECK_NEAR(FresnelReflectance(1.52, 1.0, 1.0, film, 550.0), expected, 1.0e-12);
		// Away from the design wavelength the film reflects more, but less than bare glass.
		const double off = FresnelReflectance(1.0, 1.52, 1.0, film, 420.0);
		CHECK(off > expected);
		CHECK(off < FresnelReflectance(1.0, 1.52, 1.0, bare, 420.0));
	}

	void TestSinglet()
	{
		const double r1 = 50.0;
		const double r2 = -50.0;
		const double d = 5.0;
		const double n = 1.5;
		const LensPrescription singlet = MakeSinglet(r1, r2, d, n);
		const ParaxialData paraxial = ComputeParaxial(singlet);
		// Thick lensmaker's equation.
		const double power = (n - 1.0) * (1.0 / r1 - 1.0 / r2 + (n - 1.0) * d / (n * r1 * r2));
		CHECK_NEAR(paraxial.efl, 1.0 / power, 1.0e-9);
		// Back focal distance: f (1 - (n - 1) d / (n r1)).
		CHECK_NEAR(paraxial.bfl, (1.0 / power) * (1.0 - (n - 1.0) * d / (n * r1)), 1.0e-9);
		CHECK_NEAR(paraxial.entrancePupilRadius, 5.0, 1.0e-12);

		// A real ray near the axis focuses at the paraxial focus.
		const Ray ray{ { 1.0e-3, 0.0, -1.0 }, { 0.0, 0.0, 1.0 } };
		const TraceResult result = TraceSequential(singlet, ray, kLambdaD, TransmissionPath(singlet, true));
		CHECK(result.status == TraceStatus::Ok);
		const double crossing = result.exit.origin.z - result.exit.origin.x / result.exit.dir.x * result.exit.dir.z;
		CHECK_NEAR(crossing, singlet.surfaces.back().vertexZ + paraxial.bfl, 1.0e-4);

		// Spherical aberration: marginal rays focus short of the paraxial focus for a positive singlet.
		const Ray marginal{ { 4.5, 0.0, -1.0 }, { 0.0, 0.0, 1.0 } };
		const TraceResult marginalResult = TraceSequential(singlet, marginal, kLambdaD, TransmissionPath(singlet, true));
		CHECK(marginalResult.status == TraceStatus::Ok);
		const double marginalCrossing =
			marginalResult.exit.origin.z - marginalResult.exit.origin.x / marginalResult.exit.dir.x * marginalResult.exit.dir.z;
		CHECK(marginalCrossing < crossing);

		// Thin-lens focusing: 1/s' = 1/f - 1/s, so the sensor moves back.
		const double shift = SensorShiftForFocus(singlet, 1000.0);
		CHECK(shift > 0.0);
		CHECK_NEAR(shift, 1.0 / (power - 1.0 / 1000.0) - 1.0 / power, 0.1);

		// Clipping by the stop and by the housing.
		const Ray outside{ { 5.5, 0.0, -1.0 }, { 0.0, 0.0, 1.0 } };
		CHECK(TraceSequential(singlet, outside, kLambdaD, TransmissionPath(singlet, true)).status == TraceStatus::Stopped);
		TraceOptions noStop;
		noStop.clipStop = false;
		const Ray beyondHousing{ { 10.5, 0.0, -1.0 }, { 0.0, 0.0, 1.0 } };
		CHECK(TraceSequential(singlet, beyondHousing, kLambdaD, TransmissionPath(singlet, true), noStop).status == TraceStatus::Clipped);
		TraceOptions stoppedDown;
		stoppedDown.stopScale = 0.5;
		const Ray mid{ { 3.0, 0.0, -1.0 }, { 0.0, 0.0, 1.0 } };
		CHECK(TraceSequential(singlet, mid, kLambdaD, TransmissionPath(singlet, true), stoppedDown).status == TraceStatus::Stopped);
	}

	void TestPublishedLenses()
	{
		struct Expectation {
			const char* file;
			double efl;
			double eflTolerance;
			double fNumber;
			double fNumberTolerance;
		};
		const Expectation expectations[] = {
			{ "doublet_100mm.lens.json", 100.0, 1.0, 0.0, 0.0 },
			{ "double_gauss_50mm.lens.json", 50.0, 1.5, 2.0, 0.3 },
			{ "wide_22mm.lens.json", 22.0, 1.0, 0.0, 0.0 },
		};
		for (const Expectation& expectation : expectations) {
			const LensPrescription lens = Load(expectation.file);
			if (lens.surfaces.empty()) {
				continue;
			}
			const ParaxialData paraxial = ComputeParaxial(lens);
			std::printf("%-28s efl %.4f bfl %.4f EP radius %.4f f/%.3f sensorZ %.4f\n", expectation.file, paraxial.efl, paraxial.bfl,
				paraxial.entrancePupilRadius, paraxial.fNumber, lens.sensorZ);
			CHECK_NEAR(paraxial.efl, expectation.efl, expectation.eflTolerance);
			if (expectation.fNumber > 0.0) {
				CHECK_NEAR(paraxial.fNumber, expectation.fNumber, expectation.fNumberTolerance);
			}
			CHECK_NEAR(lens.sensorZ, lens.surfaces.back().vertexZ + paraxial.bfl, 1.0e-9);

			// The on-axis ray stays on the axis and reaches the sensor straight.
			const LensPath forward = TransmissionPath(lens, true);
			const TraceResult axial = TraceSequential(lens, Ray{ { 0.0, 0.0, lens.FrontPlaneZ() - 1.0 }, { 0.0, 0.0, 1.0 } }, kLambdaD, forward);
			CHECK(axial.status == TraceStatus::Ok);
			CHECK_NEAR(axial.exit.dir.z, 1.0, 1.0e-12);
			CHECK_NEAR(axial.exit.origin.x, 0.0, 1.0e-12);
			CHECK(axial.throughput > 0.9 && axial.throughput < 1.0);

			// Rotation equivariance: rotating the input about the axis rotates the output.
			const Ray base{ { 2.0, 0.5, lens.FrontPlaneZ() - 1.0 }, Normalize({ 0.05, -0.02, 1.0 }) };
			const double angle = 1.1;
			const auto rotate = [&](Vec3 v) { return Vec3{ std::cos(angle) * v.x - std::sin(angle) * v.y, std::sin(angle) * v.x + std::cos(angle) * v.y, v.z }; };
			const TraceResult a = TraceSequential(lens, base, kLambdaD, forward);
			const TraceResult b = TraceSequential(lens, Ray{ rotate(base.origin), rotate(base.dir) }, kLambdaD, forward);
			CHECK(a.status == TraceStatus::Ok && b.status == TraceStatus::Ok);
			const Vec3 rotatedExit = rotate(a.exit.origin);
			CHECK_NEAR(rotatedExit.x, b.exit.origin.x, 1.0e-9);
			CHECK_NEAR(rotatedExit.y, b.exit.origin.y, 1.0e-9);
			CHECK_NEAR(Dot(rotate(a.exit.dir), b.exit.dir), 1.0, 1.0e-12);
			CHECK_NEAR(a.throughput, b.throughput, 1.0e-12);

			// Reversibility: send the exit ray back along the backward path.
			const TraceResult back = TraceSequential(lens, Ray{ a.exit.origin + a.exit.dir * 5.0, -a.exit.dir }, kLambdaD, TransmissionPath(lens, false));
			CHECK(back.status == TraceStatus::Ok);
			Ray returned = back.exit;
			CHECK(PropagateToPlane(returned, base.origin.z));
			CHECK_NEAR(returned.origin.x, base.origin.x, 1.0e-8);
			CHECK_NEAR(returned.origin.y, base.origin.y, 1.0e-8);
			CHECK_NEAR(Dot(returned.dir, -base.dir), 1.0, 1.0e-12);
			CHECK_NEAR(back.throughput, a.throughput, 1.0e-9);

			// Canonical frame round trip.
			CanonicalFrame frame;
			const Vec3 position{ -3.0, -1.5, 0.0 };
			const Vec3 dir = Normalize({ 0.1, 0.2, -1.0 });
			const CanonicalRay canonical = Canonicalize(position, dir, frame);
			CHECK(canonical.dy >= 0.0);
			const Vec3 restored = Uncanonicalize({ canonical.dx, canonical.dy, canonical.dz }, frame);
			CHECK_NEAR(restored.x, dir.x, 1.0e-12);
			CHECK_NEAR(restored.y, dir.y, 1.0e-12);
			const Vec3 restoredPosition = Uncanonicalize({ canonical.r, 0.0, 0.0 }, frame);
			CHECK_NEAR(restoredPosition.x, position.x, 1.0e-12);
			CHECK_NEAR(restoredPosition.y, position.y, 1.0e-12);

			// Distortion table: paraxial at small angles, f tan(theta).
			const double maxField = std::atan(lens.imageCircleRadiusMm / paraxial.efl);
			const DistortionTable table = ComputeDistortionTable(lens, lens.sensorZ, maxField, 64);
			CHECK(table.fieldAngle.size() >= 32);
			const double small = 0.01;
			CHECK_NEAR(table.ImageRadiusAt(small) / (paraxial.efl * std::tan(small)), 1.0, 5.0e-3);
			CHECK_NEAR(table.FieldAngleAt(table.ImageRadiusAt(0.2 * maxField)), 0.2 * maxField, 1.0e-6);
			std::printf("    chief ray image radius at %.1f deg: %.4f (f tan = %.4f), table to %.1f deg\n", maxField * 180.0 / std::numbers::pi,
				table.ImageRadiusAt(table.maxFieldAngle), paraxial.efl * std::tan(table.maxFieldAngle), table.maxFieldAngle * 180.0 / std::numbers::pi);

			// Exit pupil bounds: every backward ray from the on-axis bin's centre that clears the lens
			// starts inside the box, and some do.
			const ExitPupilBounds pupil = ComputeExitPupilBounds(lens, lens.sensorZ, lens.imageCircleRadiusMm, 16, 64);
			CHECK(pupil.bins[0].valid);
			CHECK(pupil.bins[0].passFraction > 0.5);
			std::printf("    exit pupil: centre bin box x [%.3f, %.3f] pass %.2f, edge bin valid %d pass %.2f\n", pupil.bins[0].minX, pupil.bins[0].maxX,
				pupil.bins[0].passFraction, pupil.bins.back().valid ? 1 : 0, pupil.bins.back().passFraction);
		}
	}

	// The exact pupil: discs that hold every ray that clears the lens, sampled uniformly over their intersection.
	void TestPupilDiscs()
	{
		// The region sampler against brute force on a three-disc lens shape: every sample inside, the area right, and the
		// samples uniform (the fraction left of the middle matches the area there).
		PupilRegion region;
		region.count = 3;
		region.centre = { 0.0, 3.0, 1.0, 0.0 };
		region.radius = { 4.0, 3.5, 5.0, 0.0 };
		double gridArea = 0.0;
		constexpr int kGrid = 2000;
		uint64_t gridLeft = 0, gridInside = 0;
		for (int row = 0; row < kGrid; ++row) {
			for (int column = 0; column < kGrid; ++column) {
				const double x = -6.0 + 12.0 * (column + 0.5) / kGrid, y = -6.0 + 12.0 * (row + 0.5) / kGrid;
				if (PupilRegionContains(region, x, y)) {
					++gridInside;
					gridLeft += x < 0.5 ? 1u : 0u;
				}
			}
		}
		gridArea = gridInside * (12.0 / kGrid) * (12.0 / kGrid);
		CHECK_NEAR(PupilRegionArea(region), gridArea, 2e-3 * gridArea);
		LensRng rng(5);
		uint64_t outside = 0, left = 0;
		constexpr int kSamples = 400000;
		for (int i = 0; i < kSamples; ++i) {
			double x = 0.0, y = 0.0, area = 0.0;
			CHECK(SamplePupilRegion(region, rng.Uniform(), rng.Uniform(), x, y, area));
			const double shrink = 1.0 - 1e-9;
			outside += PupilRegionContains(region, x * shrink, y * shrink) || PupilRegionContains(region, x, y) ? 0u : 1u;
			left += x < 0.5 ? 1u : 0u;
		}
		CHECK(outside == 0);
		CHECK_NEAR(double(left) / kSamples, double(gridLeft) / gridInside, 4e-3);

		for (const char* name : { "double_gauss_50mm.lens.json", "wide_22mm.lens.json" }) {
			const LensPrescription lens = Load(name);
			if (lens.surfaces.empty()) {
				continue;
			}
			TraceOptions options;
			const double sensorZ = lens.sensorZ + SensorShiftForFocus(lens, 3000.0);
			PupilDiscOptions discOptions;
			discOptions.nodes = 13;
			const PupilDiscs discs = ComputePupilDiscs(lens, sensorZ, 1.25 * lens.imageCircleRadiusMm, options, discOptions);
			CHECK(discs.NodeCount() == 13 && discs.nodes[0].count > 0 && !discs.apertures.empty());
			const PupilDiscStats stats = MeasurePupilDiscs(lens, discs, options, 40000, 380.0, 780.0, 0.1, 7);
			uint64_t leaked = 0, passed = 0, inRegion = 0, passInRegion = 0;
			for (const auto& interval : stats.intervals) {
				leaked += interval.passOutside;
				passed += interval.passInRegion + interval.passOutside;
				inRegion += interval.inRegion;
				passInRegion += interval.passInRegion;
			}
			CHECK(passed > 100000);
			CHECK(leaked == 0);
			const double acceptance = double(passInRegion) / double(inRegion);
			CHECK(acceptance > 0.9);
			std::printf("%s exact pupil (%zu discs, 13 nodes): %llu of %llu passing rays outside the region; acceptance %.3f\n", name,
				discs.apertures.size(), static_cast<unsigned long long>(leaked), static_cast<unsigned long long>(passed), acceptance);
		}
	}

	// The polynomial lens: the term set, float against double evaluation, and a degree-5 fit of a real lens against exact
	// transport on fresh rays.
	void TestPolynomialFit()
	{
		std::vector<std::array<uint8_t, kTransportInputs>> exponents;
		uint32_t even = 0;
		PolynomialExponents(5, exponents, even);
		CHECK(exponents.size() == 126 && even == 80);
		PolynomialExponents(7, exponents, even);
		CHECK(exponents.size() == 330 && even == 200);
		for (uint32_t k = 0; k < exponents.size(); ++k) {
			CHECK((exponents[k][2] % 2 == 1) == (k >= even));
		}

		const LensPrescription lens = Load("double_gauss_50mm.lens.json");
		if (lens.surfaces.empty()) {
			return;
		}
		const LensPath path = TransmissionPath(lens, false);
		const SamplingDomain domain = DefaultSamplingDomain(lens, path, PathDirection::Backward);
		DatasetOptions options;
		options.seed = 3;
		options.rowCount = 200000;
		const Dataset dataset = GenerateDataset(lens, path, domain, options);
		LensPolynomial poly;
		std::string error;
		CHECK(FitPolynomial(dataset.inputs, dataset.outputs, 5, poly, &error));
		CHECK(poly.IsValid() && poly.TermCount() == 126);
		const LabelledRays rays = SampleUniformRays(lens, path, domain, 20000, 77);
		double angleSum = 0.0, positionSum = 0.0, floatVsDouble = 0.0;
		uint64_t valid = 0;
		for (size_t i = 0; i < rays.inputs.size(); ++i) {
			if (!rays.labels[i]) {
				continue;
			}
			const auto& in = rays.inputs[i];
			const auto& exact = rays.outputs[i];
			const auto y = EvalTransport(poly, { float(in[0]), float(in[1]), float(in[2]), float(in[3]) });
			const auto yd = EvalPolynomial(poly, in);
			floatVsDouble = std::max(floatVsDouble, std::abs(double(EvalPolynomial(poly, std::array<float, kTransportInputs>{ float(in[0]), float(in[1]), float(in[2]), float(in[3]) })[0]) - yd[0]));
			const double dot = std::clamp(double(y[2]) * exact[2] + double(y[3]) * exact[3] + double(y[4]) * exact[4], -1.0, 1.0);
			angleSum += std::acos(dot) * 180.0 * 60.0 / std::numbers::pi;
			positionSum += std::hypot(double(y[0]) - exact[0], double(y[1]) - exact[1]);
			++valid;
		}
		CHECK(valid > 5000);
		const double angle = angleSum / valid, position = positionSum / valid;
		CHECK(angle < 0.3);          // arc-minutes; the shipped P5 is about 0.09'
		CHECK(position < 0.01);      // mm
		CHECK(floatVsDouble < 1e-3); // mm, the float32 evaluation of the same polynomial
		std::printf("polynomial P5 fit on 200k rays: angle mean %.4f', position mean %.5f mm, float vs double %.1e mm\n", angle, position, floatVsDouble);

		// .brlens: round trip, and the old versions and other prescriptions rejected with a message.
		BrLensFile file;
		file.sourceHash = lens.sourceHash;
		file.lensName = lens.name;
		file.domain = domain;
		file.polynomials.push_back(poly);
		file.metadataJson = "{}";
		const std::vector<uint8_t> bytes = EncodeBrLens(file);
		std::string decodeError;
		const auto decoded = DecodeBrLens(bytes, lens.sourceHash, &decodeError);
		CHECK(decoded.has_value());
		if (decoded) {
			CHECK(decoded->Find(5) != nullptr && decoded->Find(7) == nullptr);
			CHECK(decoded->Find(5)->coefficients == poly.coefficients);
			CHECK_NEAR(decoded->domain.sensorZMax, domain.sensorZMax, 0.0);
		}
		CHECK(!DecodeBrLens(bytes, lens.sourceHash ^ 1u, &decodeError).has_value());
		std::vector<uint8_t> old = bytes;
		const uint32_t version2 = 2;
		std::memcpy(old.data() + 8, &version2, 4);
		CHECK(!DecodeBrLens(old, lens.sourceHash, &decodeError).has_value() && decodeError.find("fitpoly") != std::string::npos);
		std::vector<uint8_t> corrupt = bytes;
		corrupt[corrupt.size() / 2] ^= 0x40u;
		CHECK(!DecodeBrLens(corrupt, lens.sourceHash, &decodeError).has_value());
		// The shipped files are version 3 and match their prescriptions.
		for (const char* shipped : { "double_gauss_50mm", "wide_22mm" }) {
			const LensPrescription prescription = Load((std::string(shipped) + ".lens.json").c_str());
			std::string readError;
			const auto shippedFile = ReadBrLens(std::filesystem::path(LENS_CORE_LENS_DIR) / (std::string(shipped) + ".brlens"), prescription.sourceHash, &readError);
			CHECK(shippedFile.has_value() && shippedFile->Find(5) && shippedFile->Find(7));
			if (!shippedFile) {
				std::fprintf(stderr, "  %s\n", readError.c_str());
			}
		}
	}

	void TestAchromat()
	{
		const LensPrescription doublet = Load("doublet_100mm.lens.json");
		if (doublet.surfaces.empty()) {
			return;
		}
		const double shiftFC = ComputeParaxial(doublet, kLambdaF).efl - ComputeParaxial(doublet, kLambdaC).efl;
		// An N-BK7 singlet of the same focal length has nF - nC = 0.0081 / 0.5168 * 100 mm = ~1.56 mm.
		CHECK(std::abs(shiftFC) < 0.2);
		std::printf("doublet F-C focal shift %.4f mm\n", shiftFC);
	}

	void TestGhosts()
	{
		const LensPrescription lens = Load("double_gauss_50mm.lens.json");
		if (lens.surfaces.empty()) {
			return;
		}
		uint32_t interfaces = 0;
		for (uint32_t i = 0; i < lens.SurfaceCount(); ++i) {
			interfaces += lens.IsInterface(i) ? 1u : 0u;
		}
		const std::vector<LensPath> ghosts = EnumerateGhostPaths(lens);
		CHECK(ghosts.size() == interfaces * (interfaces - 1) / 2);
		for (const LensPath& ghost : ghosts) {
			CHECK(ghost.ReflectionCount() == 2);
		}
		const std::vector<RankedPath> ranked = RankGhostPaths(lens, { 0.0, 0.25 }, 32, kLambdaD, lens.imageCircleRadiusMm);
		CHECK(!ranked.empty());
		CHECK(ranked.front().energy > 0.0);
		// Coated two-bounce ghosts carry well under 1% of the transmitted energy each.
		CHECK(ranked.front().energy < 0.01);
		std::printf("double gauss: %zu ghosts, top:", ranked.size());
		for (size_t i = 0; i < std::min<size_t>(5, ranked.size()); ++i) {
			std::printf(" %s=%.2e", ranked[i].path.Code().c_str(), ranked[i].energy);
		}
		std::printf("\n");

		// A ghost traced backward retraces the forward ghost.
		const LensPath forward = ranked.front().path;
		const Ray ray{ { 1.0, 0.3, lens.FrontPlaneZ() - 1.0 }, Normalize({ 0.02, 0.0, 1.0 }) };
		const TraceResult a = TraceSequential(lens, ray, kLambdaD, forward);
		if (a.status == TraceStatus::Ok) {
			const TraceResult b = TraceSequential(lens, Ray{ a.exit.origin + a.exit.dir * 2.0, -a.exit.dir }, kLambdaD, forward.Reversed());
			CHECK(b.status == TraceStatus::Ok);
			CHECK_NEAR(b.throughput, a.throughput, 1.0e-9);
		}
	}

	void TestLoaderErrors()
	{
		const GlassCatalog empty;
		CHECK(!ParsePrescription(R"({"surfaces":[{"radius":10,"thickness":1,"semiAperture":1,"medium":"unobtainium"}]})", empty).lens);
		// No stop.
		CHECK(!ParsePrescription(
			R"({"surfaces":[{"radius":10,"thickness":1,"semiAperture":1,"medium":{"n":1.5}},{"radius":-10,"thickness":5,"semiAperture":1,"medium":"air"}]})", empty)
				   .lens);
		// The hash covers the text.
		const char* a = R"({"surfaces":[{"stop":true,"thickness":1,"semiAperture":1},{"radius":10,"thickness":1,"semiAperture":2,"medium":{"n":1.5}},{"radius":-10,"thickness":5,"semiAperture":2,"medium":"air"}]})";
		const char* b = R"({"surfaces":[{"stop":true,"thickness":1,"semiAperture":1},{"radius":11,"thickness":1,"semiAperture":2,"medium":{"n":1.5}},{"radius":-10,"thickness":5,"semiAperture":2,"medium":"air"}]})";
		const LoadResult la = ParsePrescription(a, empty);
		const LoadResult lb = ParsePrescription(b, empty);
		CHECK(la.lens && lb.lens);
		if (la.lens && lb.lens) {
			CHECK(la.lens->sourceHash != lb.lens->sourceHash);
		}
	}

	void TestDataset()
	{
		const LensPrescription lens = Load("double_gauss_50mm.lens.json");
		if (lens.surfaces.empty()) {
			return;
		}
		uint32_t pathIndex = 0;
		for (const auto& [code, direction] : { std::pair<const char*, PathDirection>{ "T", PathDirection::Backward }, { "R8R6", PathDirection::Forward } }) {
			LensPath path;
			CHECK(ParsePathCode(lens, code, direction, path));
			const SamplingDomain domain = DefaultSamplingDomain(lens, path, direction);
			DatasetOptions options;
			options.seed = 77;
			options.rowCount = 3000;
			options.chunkSize = 512;
			options.threads = 4;
			const Dataset a = GenerateDataset(lens, path, domain, options);
			options.threads = 1;
			const Dataset b = GenerateDataset(lens, path, domain, options);
			options.seed = 78;
			const Dataset c = GenerateDataset(lens, path, domain, options);
			CHECK(a.complete && a.RowCount() == 3000);
			// Same seed: identical, whatever the thread count. Different seed: different.
			CHECK(a.inputs == b.inputs && a.outputs == b.outputs);
			CHECK(a.inputs != c.inputs);

			// Every row is the exact transport of its stored (float32) inputs; canonical dy >= 0.
			double worst = 0.0;
			uint32_t failures = 0;
			for (uint64_t i = 0; i < a.RowCount(); ++i) {
				const float* in = a.inputs.data() + i * 4;
				const float* out = a.outputs.data() + i * 6;
				CHECK(in[2] >= 0.0f && in[0] >= 0.0f && in[3] >= 380.0f && in[3] <= 780.0f);
				std::array<double, kTransportOutputs> traced;
				if (!TraceCanonical(lens, path, domain, { in[0], in[1], in[2], in[3] }, traced)) {
					++failures;
					continue;
				}
				for (int k = 0; k < 6; ++k) {
					worst = std::max(worst, std::abs(traced[k] - out[k]) / std::max(1.0, std::abs(traced[k])));
				}
			}
			CHECK(failures == 0);
			CHECK(worst < 1.0e-6);
			std::printf("dataset %s: re-trace worst relative diff %.2e, uniform pass rate %.3f\n", code, worst,
				static_cast<double>(a.stats.uniformValid) / std::max<uint64_t>(1, a.stats.uniformDraws));

			// The canonical frame really is canonical: a world-frame trace of the same ray, rotated,
			// gives the rotated outputs.
			if (a.RowCount() > 0) {
				const float* in = a.inputs.data();
				const float* out = a.outputs.data();
				const double phi = 1.1 + pathIndex;
				const double c0 = std::cos(phi);
				const double s0 = std::sin(phi);
				const bool backward = direction == PathDirection::Backward;
				const Vec3 d{ in[1], in[2], (backward ? -1.0 : 1.0) * std::sqrt(1.0 - double(in[1]) * in[1] - double(in[2]) * in[2]) };
				const Vec3 dw{ c0 * d.x - s0 * d.y, s0 * d.x + c0 * d.y, d.z };
				const Vec3 pw{ c0 * in[0], s0 * in[0], domain.inputPlaneZ };
				const TraceResult r = TraceSequential(lens, Ray{ pw - dw * 1.0, dw }, in[3], path);
				Ray exit = r.exit;
				CHECK(r.status == TraceStatus::Ok && PropagateToPlane(exit, domain.outputPlaneZ));
				CHECK_NEAR(exit.origin.x, c0 * out[0] - s0 * out[1], 1.0e-4);
				CHECK_NEAR(exit.origin.y, s0 * out[0] + c0 * out[1], 1.0e-4);
				CHECK_NEAR(exit.dir.z, out[4], 1.0e-6);
			}
			++pathIndex;
		}

		// Wavelength pdf integrates to 1 and favours the visible peak over the extremes.
		const WavelengthSampler sampler;
		double integral = 0.0;
		for (double l = 380.5; l < 780.0; l += 1.0) {
			integral += sampler.Pdf(l);
		}
		CHECK_NEAR(integral, 1.0, 1.0e-9);
		CHECK(sampler.Pdf(560.0) > 3.0 * sampler.Pdf(760.0));
		CHECK_NEAR(sampler.Sample(0.0), 380.0, 1.0e-9);
		CHECK(sampler.Sample(0.5) > 450.0 && sampler.Sample(0.5) < 620.0);

		// Path codes: forward ghosts list the later surface first, backward ones the reverse.
		LensPath p;
		CHECK(ParsePathCode(lens, "R8R6", PathDirection::Forward, p) && p.Code() == "R8R6");
		CHECK(ParsePathCode(lens, "R6R8", PathDirection::Backward, p) && p.Code() == "R6R8");
		CHECK(!ParsePathCode(lens, "R6R8", PathDirection::Forward, p));
		CHECK(!ParsePathCode(lens, "R99R1", PathDirection::Forward, p));
		uint64_t hash = 0;
		CHECK(ParseHashHex(HashHex(lens.sourceHash), hash) && hash == lens.sourceHash);
	}
} // namespace

int main()
{
	TestGlass();
	TestFresnel();
	TestSinglet();
	TestPublishedLenses();
	TestPupilDiscs();
	TestAchromat();
	TestGhosts();
	TestLoaderErrors();
	TestPolynomialFit();
	TestDataset();
	if (g_failures != 0) {
		std::fprintf(stderr, "LensCoreTests: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("LensCoreTests: all passed\n");
	return 0;
}
