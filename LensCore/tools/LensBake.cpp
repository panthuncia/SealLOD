// LensBake: offline tool for physical camera lenses; see scripts/lens/README.md for the bake pipeline.
//
//   LensBake info <lens.json> [--field-deg <half field>] [--ghosts <n>]
//       paraxial data, distortion, pupil, ghosts
//   LensBake rank <lens.json> [--top <n>] [--grid <n>]
//       ghost paths by energy over field angles 0..1.1x the half field and lambda F, d, C
//   LensBake pupil <lens.json> [--focus-m M] [--stop S] [--rims 0|1] [--mono 0|1] [--nodes N] [--angles A] [--rays R] [--json <out.json>]
//       the exact pupil sampler's discs (as LensManager builds them), the light outside them and what it rejects
//   LensBake fitpoly <lens.json> --out <file.brlens> [--degrees 5,7] [--rows N] [--seed S] [--min-focus-mm D] [--manifest <MANIFEST.json>]
//       the polynomial lens: least squares on exact rays, its accuracy on fresh rays, written to a .brlens
//   LensBake eval <file.brlens> <lens.json> [--count N] [--seed S] [--json <out.json>]
//       a .brlens's polynomials against exact transport

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "LensCore/BrLens.h"
#include "LensCore/Lens.h"
#include "LensCore/LensAnalysis.h"
#include "LensCore/LensDataset.h"
#include "LensCore/LensPolynomial.h"
#include "LensCore/LensTrace.h"

namespace {
	using namespace lens;
	using nlohmann::json;

	constexpr double kDeg = std::numbers::pi / 180.0;
	constexpr double kArcmin = kDeg / 60.0;

	int Usage()
	{
		std::fprintf(stderr,
			"usage:\n"
			"  LensBake info <lens.json> [--field-deg <half field>] [--ghosts <n>]\n"
			"  LensBake rank <lens.json> [--top <n>] [--grid <n>]\n"
			"  LensBake pupil <lens.json> [--focus-m M] [--stop S] [--rims 0|1] [--mono 0|1] [--nodes N] [--angles A] [--rays R] [--json <out.json>]\n"
			"  LensBake fitpoly <lens.json> --out <file.brlens> [--degrees 5,7] [--rows N] [--seed S] [--threads K] [--min-focus-mm D]\n"
			"               [--eval-count N] [--eval-seed S] [--manifest <MANIFEST.json>]\n"
			"  LensBake eval <file.brlens> <lens.json> [--count N] [--seed S] [--threads K] [--json <out.json>]\n");
		return 2;
	}

	bool LoadLens(const char* path, LensPrescription& lens)
	{
		LoadResult loaded = LoadPrescription(path);
		if (!loaded.lens) {
			std::fprintf(stderr, "%s\n", loaded.error.c_str());
			return false;
		}
		lens = std::move(*loaded.lens);
		return true;
	}

	double MaxFieldAngle(const LensPrescription& lens)
	{
		const ParaxialData paraxial = ComputeParaxial(lens);
		const double imageCircle = lens.imageCircleRadiusMm > 0.0 ? lens.imageCircleRadiusMm : paraxial.efl * std::tan(20.0 * kDeg);
		return 1.1 * std::atan(imageCircle / paraxial.efl);
	}

	// Simple "--key value" parser; positional arguments are kept in order.
	struct Args {
		std::map<std::string, std::string> options;
		std::vector<std::string> positional;
		bool ok = true;

		Args(int argc, char** argv, const std::vector<std::string>& known)
		{
			for (int i = 0; i < argc; ++i) {
				const std::string a = argv[i];
				if (a.rfind("--", 0) == 0) {
					if (std::find(known.begin(), known.end(), a) == known.end() || i + 1 >= argc) {
						std::fprintf(stderr, "unknown or incomplete option %s\n", a.c_str());
						ok = false;
						return;
					}
					options[a] = argv[++i];
				} else {
					positional.push_back(a);
				}
			}
		}
		bool Has(const char* key) const { return options.count(key) != 0; }
		std::string Str(const char* key, const std::string& fallback = {}) const { return Has(key) ? options.at(key) : fallback; }
		double Num(const char* key, double fallback) const { return Has(key) ? std::atof(options.at(key).c_str()) : fallback; }
		uint64_t U64(const char* key, uint64_t fallback) const { return Has(key) ? std::strtoull(options.at(key).c_str(), nullptr, 10) : fallback; }
	};

	int Info(int argc, char** argv)
	{
		const Args args(argc, argv, { "--field-deg", "--ghosts" });
		if (!args.ok || args.positional.size() != 1) {
			return Usage();
		}
		double fieldDeg = args.Num("--field-deg", 0.0);
		const size_t ghostCount = static_cast<size_t>(args.U64("--ghosts", 12));
		LensPrescription lens;
		if (!LoadLens(args.positional[0].c_str(), lens)) {
			return 1;
		}
		const ParaxialData paraxial = ComputeParaxial(lens);
		std::printf("%s (hash %016llx)\n  %s\n", lens.name.c_str(), static_cast<unsigned long long>(lens.sourceHash), lens.source.c_str());
		std::printf("  surfaces %u, stop %u, media %zu, total track %.3f mm, sensor z %.3f mm\n", lens.SurfaceCount(), lens.stopIndex, lens.media.size(),
			lens.surfaces.back().vertexZ, lens.sensorZ);
		std::printf("  front plane z %.4f, rear plane z %.4f\n", lens.FrontPlaneZ(), lens.RearPlaneZ());
		std::printf("  EFL %.4f  BFL %.4f  entrance pupil radius %.4f  f/%.3f\n", paraxial.efl, paraxial.bfl, paraxial.entrancePupilRadius, paraxial.fNumber);
		std::printf("  EFL at F/d/C: %.4f %.4f %.4f\n", ComputeParaxial(lens, kLambdaF).efl, paraxial.efl, ComputeParaxial(lens, kLambdaC).efl);
		for (const double distance : { 10000.0, 3000.0, 1000.0, 500.0, 300.0 }) {
			std::printf("  focus %6.0f mm: sensor shift %+.4f mm\n", distance, SensorShiftForFocus(lens, distance));
		}

		if (fieldDeg <= 0.0) {
			fieldDeg = lens.imageCircleRadiusMm > 0.0 ? std::atan(lens.imageCircleRadiusMm / paraxial.efl) / kDeg : 20.0;
		}
		const DistortionTable table = ComputeDistortionTable(lens, lens.sensorZ, fieldDeg * kDeg, 9);
		std::printf("  distortion (chief ray, infinity focus):\n");
		for (size_t i = 0; i < table.fieldAngle.size(); ++i) {
			const double ideal = paraxial.efl * std::tan(table.fieldAngle[i]);
			std::printf("    %5.1f deg  image %.4f mm  f tan %.4f  %+.3f%%\n", table.fieldAngle[i] / kDeg, table.imageRadius[i], ideal,
				ideal > 0.0 ? 100.0 * (table.imageRadius[i] / ideal - 1.0) : 0.0);
		}

		const double maxRadius = lens.imageCircleRadiusMm > 0.0 ? lens.imageCircleRadiusMm : table.ImageRadiusAt(table.maxFieldAngle);
		const ExitPupilBounds pupil = ComputeExitPupilBounds(lens, lens.sensorZ, maxRadius, 8, 96);
		std::printf("  exit pupil boxes on rear plane z=%.3f (sensor radius bins to %.2f mm):\n", pupil.rearPlaneZ, maxRadius);
		for (size_t i = 0; i < pupil.bins.size(); ++i) {
			const auto& bin = pupil.bins[i];
			std::printf("    bin %zu: %s x [%.3f, %.3f] y [%.3f, %.3f] pass %.2f\n", i, bin.valid ? "  " : "--", bin.minX, bin.maxX, bin.minY, bin.maxY,
				bin.passFraction);
		}

		const std::vector<RankedPath> ranked = RankGhostPaths(lens, { 0.0, 0.5 * fieldDeg * kDeg }, 48, kLambdaD, maxRadius);
		std::printf("  ghosts (%zu two-bounce paths), energy relative to transmission:\n", ranked.size());
		for (size_t i = 0; i < std::min(ghostCount, ranked.size()); ++i) {
			std::printf("    %-10s %.3e\n", ranked[i].path.Code().c_str(), ranked[i].energy);
		}
		return 0;
	}

	int Rank(int argc, char** argv)
	{
		const Args args(argc, argv, { "--top", "--grid" });
		if (!args.ok || args.positional.size() != 1) {
			return Usage();
		}
		LensPrescription lens;
		if (!LoadLens(args.positional[0].c_str(), lens)) {
			return 1;
		}
		const size_t top = static_cast<size_t>(args.U64("--top", 12));
		const uint32_t grid = static_cast<uint32_t>(args.U64("--grid", 64));
		const double maxField = MaxFieldAngle(lens);
		const std::vector<double> fields{ 0.0, 0.25 * maxField, 0.5 * maxField, 0.75 * maxField, maxField };
		const double lambdas[] = { kLambdaF, kLambdaD, kLambdaC };
		std::map<std::string, std::array<double, 3>> energy;
		std::vector<std::string> order;
		for (int l = 0; l < 3; ++l) {
			for (const RankedPath& r : RankGhostPaths(lens, fields, grid, lambdas[l], lens.imageCircleRadiusMm)) {
				const std::string code = r.path.Code();
				if (!energy.count(code)) {
					order.push_back(code);
					energy[code] = {};
				}
				energy[code][l] = r.energy;
			}
		}
		std::vector<std::pair<double, std::string>> sorted;
		for (const std::string& code : order) {
			const auto& e = energy[code];
			sorted.push_back({ (e[0] + e[1] + e[2]) / 3.0, code });
		}
		std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::printf("%s: %zu two-bounce ghosts; energy on the image circle relative to transmission,\n", lens.name.c_str(), sorted.size());
		std::printf("field angles 0, 0.25, 0.5, 0.75, 1 x %.2f deg, grid %u over the front aperture\n", maxField / kDeg, grid);
		std::printf("rank  code        mean        F           d           C\n");
		for (size_t i = 0; i < std::min(top, sorted.size()); ++i) {
			const auto& e = energy[sorted[i].second];
			std::printf("%4zu  %-10s  %.3e   %.3e   %.3e   %.3e\n", i + 1, sorted[i].second.c_str(), sorted[i].first, e[0], e[1], e[2]);
		}
		return 0;
	}

	double Percentile(std::vector<double> v, double p)
	{
		if (v.empty()) {
			return 0.0;
		}
		const size_t k = std::min(v.size() - 1, static_cast<size_t>(p / 100.0 * (v.size() - 1) + 0.5));
		std::nth_element(v.begin(), v.begin() + k, v.end());
		return v[k];
	}

	double Mean(const std::vector<double>& v)
	{
		double s = 0.0;
		for (const double x : v) {
			s += x;
		}
		return v.empty() ? 0.0 : s / v.size();
	}

	// A polynomial against exact transport on fresh rays, uniform over the domain (the rays that clear the lens), through the
	// float32 evaluation the shader does: exit position, direction angle and throughput errors.
	json EvaluatePolynomial(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, const LensPolynomial& poly,
		uint64_t count, uint64_t seed, uint32_t threads)
	{
		const LabelledRays rays = SampleUniformRays(lens, path, domain, count, seed, threads);
		std::vector<double> position, angle, throughput;
		for (size_t index = 0; index < rays.inputs.size(); ++index) {
			if (!rays.labels[index]) {
				continue;
			}
			const auto& in = rays.inputs[index];
			const auto& exact = rays.outputs[index];
			const auto y = EvalTransport(poly, { float(in[0]), float(in[1]), float(in[2]), float(in[3]) });
			position.push_back(std::hypot(double(y[0]) - exact[0], double(y[1]) - exact[1]));
			const Vec3 a{ y[2], y[3], y[4] }, b{ exact[2], exact[3], exact[4] };
			const Vec3 cross{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
			angle.push_back(std::atan2(Length(cross), Dot(a, b)) / kArcmin);
			throughput.push_back(std::abs(double(y[5]) - exact[5]));
		}
		return { { "degree", poly.degree }, { "terms", poly.TermCount() }, { "valid_rays", position.size() }, { "angle_mean_arcmin", Mean(angle) },
			{ "angle_p99_arcmin", Percentile(angle, 99.0) }, { "angle_max_arcmin", position.empty() ? 0.0 : *std::max_element(angle.begin(), angle.end()) },
			{ "position_mean_mm", Mean(position) }, { "position_p99_mm", Percentile(position, 99.0) }, { "throughput_mean_abs", Mean(throughput) },
			{ "throughput_p99_abs", Percentile(throughput, 99.0) } };
	}

	void PrintAccuracy(const json& a)
	{
		std::printf("  P%u (%u terms): angle mean %.5f' p99 %.5f' max %.4f', position mean %.7f mm p99 %.7f mm, throughput mean %.2e p99 %.2e (%llu rays)\n",
			a["degree"].get<uint32_t>(), a["terms"].get<uint32_t>(), a["angle_mean_arcmin"].get<double>(), a["angle_p99_arcmin"].get<double>(),
			a["angle_max_arcmin"].get<double>(), a["position_mean_mm"].get<double>(), a["position_p99_mm"].get<double>(), a["throughput_mean_abs"].get<double>(),
			a["throughput_p99_abs"].get<double>(), static_cast<unsigned long long>(a["valid_rays"].get<uint64_t>()));
	}

	std::vector<uint32_t> ParseDegrees(const std::string& text)
	{
		std::vector<uint32_t> degrees;
		size_t at = 0;
		while (at < text.size()) {
			const size_t comma = text.find(',', at);
			const uint32_t degree = static_cast<uint32_t>(std::strtoul(text.substr(at, comma - at).c_str(), nullptr, 10));
			if (degree > 0) {
				degrees.push_back(degree);
			}
			at = comma == std::string::npos ? text.size() : comma + 1;
		}
		return degrees;
	}

	// The polynomial lens: exact rays uniform over the backward transmission domain, a least-squares polynomial per degree,
	// its accuracy on fresh rays, and the .brlens (plus, with --manifest, the lens's entry in MANIFEST.json).
	int FitPoly(int argc, char** argv)
	{
		const Args args(argc, argv, { "--out", "--degrees", "--rows", "--seed", "--threads", "--min-focus-mm", "--eval-count", "--eval-seed", "--manifest" });
		if (!args.ok || args.positional.size() != 1 || !args.Has("--out")) {
			return Usage();
		}
		LensPrescription lens;
		if (!LoadLens(args.positional[0].c_str(), lens)) {
			return 1;
		}
		const std::vector<uint32_t> degrees = ParseDegrees(args.Str("--degrees", "5,7"));
		const uint32_t threads = static_cast<uint32_t>(args.U64("--threads", 0));
		const LensPath path = TransmissionPath(lens, false);
		const SamplingDomain domain = DefaultSamplingDomain(lens, path, PathDirection::Backward, args.Num("--min-focus-mm", 300.0));
		DatasetOptions options;
		options.seed = args.U64("--seed", 101);
		options.threads = threads;
		options.rowCount = args.U64("--rows", 4000000);
		std::printf("%s: %llu exact rays over the backward domain (sensor disk %.3f mm, sensor z [%.4f, %.4f] mm, rear square %.3f mm)\n",
			lens.name.c_str(), static_cast<unsigned long long>(options.rowCount), domain.sensorRadius, domain.sensorZMin, domain.sensorZMax,
			domain.rearHalfExtent);
		const Dataset dataset = GenerateDataset(lens, path, domain, options);
		std::printf("  traced in %.1f s (uniform pass rate %.3f)\n", dataset.stats.seconds,
			dataset.stats.uniformDraws ? double(dataset.stats.uniformValid) / dataset.stats.uniformDraws : 0.0);
		if (!dataset.complete || dataset.RowCount() == 0) {
			std::fprintf(stderr, "the dataset is incomplete\n");
			return 1;
		}

		BrLensFile file;
		file.sourceHash = lens.sourceHash;
		file.lensName = lens.name;
		file.domain = domain;
		const uint64_t evalCount = args.U64("--eval-count", 400000);
		const uint64_t evalSeed = args.U64("--eval-seed", 9000001);
		json metadata;
		metadata["fit"] = { { "rows", dataset.RowCount() }, { "seed", options.seed }, { "method", "normal equations, equilibrated, Cholesky, one refinement step (double)" },
			{ "min_focus_mm", args.Num("--min-focus-mm", 300.0) } };
		metadata["eval"] = { { "count", evalCount }, { "seed", evalSeed } };
		metadata["domain"] = json::parse(SamplingDomainToJson(domain));
		json accuracy = json::array();
		for (const uint32_t degree : degrees) {
			LensPolynomial poly;
			std::string error;
			PolynomialFitReport report;
			if (!FitPolynomial(dataset.inputs, dataset.outputs, degree, poly, &error, threads, &report)) {
				std::fprintf(stderr, "degree %u: %s\n", degree, error.c_str());
				return 1;
			}
			std::printf("  degree %u: %u terms (%u even), fitted in %.2f s (refinement correction %.1e)\n", degree, poly.TermCount(), poly.evenTerms,
				report.seconds, report.maxRelativeResidual);
			json a = EvaluatePolynomial(lens, path, domain, poly, evalCount, evalSeed, threads);
			a["fit_seconds"] = report.seconds;
			PrintAccuracy(a);
			accuracy.push_back(a);
			file.polynomials.push_back(std::move(poly));
		}
		metadata["accuracy"] = accuracy;
		file.metadataJson = metadata.dump();
		std::string error;
		if (!WriteBrLens(args.Str("--out"), file, &error)) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		std::printf("wrote %s\n", args.Str("--out").c_str());

		if (args.Has("--manifest")) {
			const std::filesystem::path manifestPath = args.Str("--manifest");
			json manifest = { { "format", "brlens-manifest" }, { "version", 2 }, { "lenses", json::object() } };
			if (std::ifstream in(manifestPath); in) {
				try {
					json existing = json::parse(in);
					if (existing.value("version", 0) == 2) manifest = std::move(existing);
				} catch (const std::exception&) {
				}
			}
			const std::vector<uint8_t> bytes = EncodeBrLens(file);
			manifest["lenses"][lens.name] = { { "brlens", std::filesystem::path(args.Str("--out")).filename().string() }, { "brlens_bytes", bytes.size() },
				{ "brlens_crc32", Crc32(bytes.data(), bytes.size()) }, { "brlens_version", kBrLensVersion }, { "source_hash", HashHex(lens.sourceHash) },
				{ "fit", metadata["fit"] }, { "eval", metadata["eval"] }, { "accuracy", accuracy } };
			std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
			out << manifest.dump(2) << "\n";
			std::printf("updated %s\n", manifestPath.string().c_str());
		}
		return 0;
	}

	// A .brlens's polynomials against exact transport on fresh rays.
	int Eval(int argc, char** argv)
	{
		const Args args(argc, argv, { "--count", "--seed", "--threads", "--json" });
		if (!args.ok || args.positional.size() != 2) {
			return Usage();
		}
		LensPrescription lens;
		if (!LoadLens(args.positional[1].c_str(), lens)) {
			return 1;
		}
		std::string error;
		const std::optional<BrLensFile> file = ReadBrLens(args.positional[0], lens.sourceHash, &error);
		if (!file) {
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		const LensPath path = TransmissionPath(lens, false);
		const uint64_t count = args.U64("--count", 400000);
		const uint64_t seed = args.U64("--seed", 9000001);
		std::printf("%s: %zu polynomials, float32 evaluation against exact transport, %llu rays (seed %llu)\n", lens.name.c_str(), file->polynomials.size(),
			static_cast<unsigned long long>(count), static_cast<unsigned long long>(seed));
		json report = { { "lens", lens.name }, { "count", count }, { "seed", seed }, { "accuracy", json::array() } };
		for (const LensPolynomial& poly : file->polynomials) {
			const json a = EvaluatePolynomial(lens, path, file->domain, poly, count, seed, static_cast<uint32_t>(args.U64("--threads", 0)));
			PrintAccuracy(a);
			report["accuracy"].push_back(a);
		}
		if (args.Has("--json")) {
			std::ofstream out(args.Str("--json"));
			out << report.dump(1);
		}
		return 0;
	}

	// The exact pupil sampler's discs (ComputePupilDiscs, as LensManager builds them) and what it misses and rejects.
	int Pupil(int argc, char** argv)
	{
		const Args args(argc, argv, { "--focus-m", "--stop", "--rims", "--nodes", "--angles", "--rays", "--mono", "--margin", "--threads", "--json",
			"--rel-margin" });
		if (!args.ok || args.positional.size() != 1) {
			return Usage();
		}
		LensPrescription lens;
		if (!LoadLens(args.positional[0].c_str(), lens)) {
			return 1;
		}
		TraceOptions options;
		options.stopScale = std::clamp(args.Num("--stop", 1.0), 0.01, 1.0);
		options.clipHousing = args.U64("--rims", 1) != 0;
		const double focusMm = args.Num("--focus-m", 3.0) * 1000.0;
		double shift = focusMm > 0.0 ? SensorShiftForFocus(lens, focusMm) : 0.0;
		if (!std::isfinite(shift)) {
			shift = 0.0;
		}
		const double sensorZ = lens.sensorZ + shift;
		const ParaxialData paraxial = ComputeParaxial(lens);
		const double designField = lens.imageCircleRadiusMm > 0.0 ? std::atan(lens.imageCircleRadiusMm / paraxial.efl) : 25.0 * kDeg;
		const DistortionTable table = ComputeDistortionTable(lens, sensorZ, std::min(designField * 1.4, 85.0 * kDeg), 129);
		const double imageCircle = lens.imageCircleRadiusMm > 0.0 ? lens.imageCircleRadiusMm : table.imageRadius.back();
		const double maxSensorRadius = imageCircle * 1.25;
		const bool mono = args.U64("--mono", 0) != 0;
		PupilDiscOptions discOptions;
		discOptions.nodes = static_cast<uint32_t>(args.U64("--nodes", discOptions.nodes));
		discOptions.angles = static_cast<uint32_t>(args.U64("--angles", discOptions.angles));
		discOptions.relativeMargin = args.Num("--rel-margin", discOptions.relativeMargin);
		discOptions.threads = static_cast<uint32_t>(args.U64("--threads", 0));
		if (mono) {
			discOptions.wavelengths = { kLambdaD };
		}
		const PupilDiscs discs = ComputePupilDiscs(lens, sensorZ, maxSensorRadius, options, discOptions);
		const uint64_t rays = args.U64("--rays", 1000000);
		const double margin = args.Num("--margin", 0.1);
		const auto started = std::chrono::steady_clock::now();
		const PupilDiscStats stats = mono ? MeasurePupilDiscs(lens, discs, options, rays, kLambdaD, kLambdaD, margin, 1, discOptions.threads)
										  : MeasurePupilDiscs(lens, discs, options, rays, 380.0, 780.0, margin, 1, discOptions.threads);
		const double measureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		std::printf("%s: focus %.2f m, stop scale %.3f, rims %s, %s, %u nodes to %.3f mm, %u angles; discs built in %.0f ms, measured in %.0f ms\n",
			lens.name.c_str(), focusMm / 1000.0, options.stopScale, options.clipHousing ? "on" : "off", mono ? "d line" : "380-780 nm", discs.NodeCount(),
			maxSensorRadius, discOptions.angles, discs.buildMs, measureMs);
		std::printf("  apertures (surfaces):");
		for (const uint32_t a : discs.apertures) {
			std::printf(" %u%s", a, lens.surfaces[a].isStop ? " (stop)" : "");
		}
		std::printf("\n  int   r_mm   accept  smallest  region/smallest  leak_rays  leak_energy  energy\n");
		json report = json::object();
		report["lens"] = lens.name;
		report["rims"] = options.clipHousing;
		report["mono"] = mono;
		report["apertures"] = discs.apertures;
		report["build_ms"] = discs.buildMs;
		json intervals = json::array();
		uint64_t leak = 0, pass = 0;
		double leakEnergy = 0.0, passEnergy = 0.0, weightSum = 0.0, accept = 0.0, acceptSmallest = 0.0;
		const double centreEnergy = stats.intervals.empty() ? 0.0 : stats.intervals[0].Energy();
		for (uint32_t index = 0; index < stats.intervals.size(); ++index) {
			const auto& s = stats.intervals[index];
			const double r0 = double(index) / stats.intervals.size() * maxSensorRadius, r1 = (index + 1.0) / stats.intervals.size() * maxSensorRadius;
			leak += s.passOutside;
			pass += s.passInRegion + s.passOutside;
			leakEnergy += s.energyOutside;
			passEnergy += s.energyInRegion + s.energyOutside;
			if (r0 < imageCircle && s.rays > 0) {
				const double weight = std::min(r1, imageCircle) * std::min(r1, imageCircle) - r0 * r0;
				weightSum += weight;
				accept += weight * s.Acceptance();
				acceptSmallest += weight * s.SmallestDiscAcceptance();
			}
			const double pe = s.energyInRegion + s.energyOutside;
			std::printf("  %3u %6.2f   %.4f  %.4f    %.4f           %6llu     %.2e     %.4f\n", index, 0.5 * (r0 + r1), s.Acceptance(), s.SmallestDiscAcceptance(),
				s.smallestAreaSum > 0.0 ? s.regionAreaSum / s.smallestAreaSum : 0.0, static_cast<unsigned long long>(s.passOutside), pe > 0.0 ? s.energyOutside / pe : 0.0,
				centreEnergy > 0.0 ? s.Energy() / centreEnergy : 0.0);
			intervals.push_back({ { "radius_mm", 0.5 * (r0 + r1) }, { "acceptance", s.Acceptance() }, { "smallest_disc_acceptance", s.SmallestDiscAcceptance() },
				{ "leak_rays", s.passOutside }, { "leak_energy_fraction", pe > 0.0 ? s.energyOutside / pe : 0.0 },
				{ "relative_energy", centreEnergy > 0.0 ? s.Energy() / centreEnergy : 0.0 } });
		}
		std::printf("  over the image circle (area weighted): exact-region acceptance %.4f, smallest-disc acceptance %.4f\n", accept / weightSum,
			acceptSmallest / weightSum);
		uint64_t inRegion = 0, stopped = 0, clipped = 0;
		for (const auto& s : stats.intervals) {
			inRegion += s.inRegion;
			stopped += s.stoppedInRegion;
			clipped += s.clippedInRegion;
		}
		const double regionRays = double(std::max<uint64_t>(inRegion, 1));
		std::printf("  rays in the region blocked by the stop %.4f, by a rim %.4f (all radii, not area weighted)\n", stopped / regionRays, clipped / regionRays);
		report["blocked_in_region"] = { { "stopped", stopped / regionRays }, { "clipped", clipped / regionRays } };
		std::printf("  passing rays outside the region: %llu of %llu (energy %.3e)\n", static_cast<unsigned long long>(leak), static_cast<unsigned long long>(pass),
			passEnergy > 0.0 ? leakEnergy / passEnergy : 0.0);
		report["intervals"] = intervals;
		report["acceptance_image_circle"] = accept / weightSum;
		report["smallest_disc_acceptance_image_circle"] = acceptSmallest / weightSum;
		report["leak_rays"] = leak;
		report["pass_rays"] = pass;
		report["leak_energy_fraction"] = passEnergy > 0.0 ? leakEnergy / passEnergy : 0.0;
		json nodes = json::array();
		for (const auto& node : discs.nodes) {
			json discsJson = json::array();
			for (uint32_t d = 0; d < node.count; ++d) {
				discsJson.push_back({ node.centre[d], node.radius[d] });
			}
			nodes.push_back(discsJson);
		}
		report["nodes"] = nodes;
		if (args.Has("--json")) {
			std::ofstream out(args.Str("--json"));
			out << report.dump(1);
		}
		return 0;
	}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2) {
		return Usage();
	}
	const std::string command = argv[1];
	if (command == "info") {
		return Info(argc - 2, argv + 2);
	}
	if (command == "rank") {
		return Rank(argc - 2, argv + 2);
	}
	if (command == "eval") {
		return Eval(argc - 2, argv + 2);
	}
	if (command == "fitpoly") {
		return FitPoly(argc - 2, argv + 2);
	}
	if (command == "pupil") {
		return Pupil(argc - 2, argv + 2);
	}
	return Usage();
}
