#pragma once

// Exact rays for the polynomial lens transport (LensPolynomial.h): canonical rays through one optical path,
// traced exactly with TraceSequential. See scripts/lens/README.md for the bake pipeline.
//
// Reference planes. A backward (camera) path takes its input ray AT the rear plane
// z = RearPlaneZ() travelling -z and reports the exit ray propagated to the front plane
// z = FrontPlaneZ(); a forward (scene light, ghost) path is the mirror image, front plane in, rear
// plane out. The model never sees the sensor position, so refocusing needs no refit.
//
// Canonical inputs (r, dx, dy, lambda_nm): r is the input point's distance from the axis; the
// direction is rotated so that point lies on +x and mirrored so dy >= 0 (Canonicalize in
// LensAnalysis.h); dz follows from |d| = 1 and the travel direction. Outputs (px, py, dx, dy, dz,
// throughput) are in the same canonical frame. Inputs are rounded to float32 BEFORE tracing, so
// every stored output is the exact transport of the stored input: TraceCanonical on a stored row
// reproduces its outputs up to float32 output rounding.
//
// Wavelengths are importance-sampled over [lambdaMin, lambdaMax] from
//   p(lambda) = 0.8 * cie(lambda) / integral(cie) + 0.2 / (lambdaMax - lambdaMin),
//   cie = xbar + ybar + zbar from the multi-lobe Gaussian fits of Wyman, Sloan & Shirley (JCGT 2013),
// tabulated in 1 nm bins (piecewise constant). The uniform floor keeps the spectral tails fitted.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "LensCore/Lens.h"
#include "LensCore/LensPolynomial.h"
#include "LensCore/LensTrace.h"

namespace lens {

	enum class PathDirection : uint8_t {
		Backward = 0, // camera rays: rear plane -> front plane
		Forward = 1,  // scene light: front plane -> rear plane
	};

	const char* ToString(PathDirection direction);

	// Where fitting (and runtime) rays come from, uniformly (rejection sampling). Distances in mm, angles in radians.
	struct SamplingDomain {
		PathDirection direction = PathDirection::Backward;
		double inputPlaneZ = 0.0;
		double outputPlaneZ = 0.0;
		double lambdaMinNm = 380.0;
		double lambdaMaxNm = 780.0;
		// Backward: ray from a sensor point (uniform in a disk at a uniform z) to a rear-plane point
		// (uniform in a square).
		double sensorRadius = 0.0;
		double sensorZMin = 0.0;
		double sensorZMax = 0.0;
		double rearHalfExtent = 0.0;
		// Forward: entry point uniform in a disk on the front plane; direction (sx, sy, sqrt(1 - s^2))
		// with s uniform in a disk of radius sin(maxFieldAngle) (cosine-weighted solid angle).
		double entryRadius = 0.0;
		double maxFieldAngle = 0.0;
	};

	// The domain the runtime will query. Backward: sensor disk 1.1x the image circle, focus from
	// infinity to minFocusDistanceMm, rear square 1.05x the last semi-aperture. Forward: field angle up
	// to 1.1 atan(imageCircle / efl), entry disk 1.05x the first semi-aperture.
	SamplingDomain DefaultSamplingDomain(const LensPrescription& lens, const LensPath& path, PathDirection direction, double minFocusDistanceMm = 300.0);

	// Exact transport of canonical inputs along the path, from the domain's input plane to its output
	// plane. Returns false if the ray does not complete the path (clipped, stopped, TIR, missed, or
	// leaving in the wrong direction).
	bool TraceCanonical(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, const std::array<double, kTransportInputs>& inputs,
		std::array<double, kTransportOutputs>& outputs, const TraceOptions& options = {});

	// Wavelength importance sampling (see the header comment).
	class WavelengthSampler {
	public:
		WavelengthSampler(double minNm = 380.0, double maxNm = 780.0, double uniformFraction = 0.2);
		double Sample(double u) const;
		double Pdf(double lambdaNm) const;
		double MinNm() const { return minNm_; }
		double MaxNm() const { return maxNm_; }

		// x + y + z colour-matching sum, analytic fit.
		static double CieSum(double lambdaNm);

	private:
		double minNm_;
		double maxNm_;
		std::vector<double> pdf_; // per 1 nm bin
		std::vector<double> cdf_; // size bins + 1
	};

	// splitmix64-seeded xoshiro256**; portable, so datasets are reproducible across compilers.
	class LensRng {
	public:
		explicit LensRng(uint64_t seed);
		uint64_t Next();
		double Uniform(); // [0, 1)

	private:
		uint64_t s_[4];
	};

	struct DatasetOptions {
		uint64_t seed = 1;
		uint32_t threads = 0; // 0 = hardware concurrency; results do not depend on it
		uint64_t rowCount = 100000;              // rays that complete the path
		uint32_t chunkSize = 8192;               // samples per independently seeded work unit
		uint64_t maxAttemptsPerSample = 2000000; // a chunk gives up if the path is (nearly) never valid
		TraceOptions trace{};
		std::function<void(uint64_t done, uint64_t total)> progress; // called from worker threads
	};

	struct DatasetStats {
		uint64_t traces = 0;
		uint64_t uniformDraws = 0;
		uint64_t uniformValid = 0; // uniform pass rate = uniformValid / uniformDraws
		double seconds = 0.0;
	};

	// Rays that complete the path: canonical inputs and their exact outputs, what FitPolynomial fits.
	struct Dataset {
		std::vector<float> inputs;  // [N][4]
		std::vector<float> outputs; // [N][6]
		DatasetStats stats;
		bool complete = true; // false if some chunk ran out of attempts
		uint64_t RowCount() const { return outputs.size() / kTransportOutputs; }
	};

	Dataset GenerateDataset(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, const DatasetOptions& options);

	// Natural-distribution (unbalanced) labelled rays: uniform over the domain, valid or not. outputs is
	// filled for valid rows only (zeros otherwise). Used for held-out evaluation.
	struct LabelledRays {
		std::vector<std::array<double, kTransportInputs>> inputs;
		std::vector<std::array<double, kTransportOutputs>> outputs;
		std::vector<uint8_t> labels;
	};
	LabelledRays SampleUniformRays(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, uint64_t count, uint64_t seed,
		uint32_t threads = 0, const TraceOptions& options = {});

	std::string HashHex(uint64_t hash);
	bool ParseHashHex(const std::string& text, uint64_t& hash);
	bool ParsePathCode(const LensPrescription& lens, const std::string& code, PathDirection direction, LensPath& path);

	// The domain as JSON, for the .brlens metadata and MANIFEST.json.
	std::string SamplingDomainToJson(const SamplingDomain& domain);

} // namespace lens
