#include "LensCore/LensDataset.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <thread>

#include <nlohmann/json.hpp>

#include "LensCore/LensAnalysis.h"

namespace lens {

	namespace {
		// The trace starts this far (mm, along the ray) before the input plane, so a ray whose input
		// point sits on a surface vertex still finds that surface ahead of it.
		constexpr double kTraceBackoffMm = 1.0;

		uint64_t SplitMix64(uint64_t& x)
		{
			uint64_t z = (x += 0x9E3779B97F4A7C15ull);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}

		uint64_t StreamSeed(uint64_t seed, uint64_t stream, uint64_t chunk)
		{
			uint64_t x = seed;
			uint64_t h = SplitMix64(x);
			x = h ^ (stream * 0xD1B54A32D192ED03ull);
			h = SplitMix64(x);
			x = h ^ (chunk * 0x8CB92BA72F3D8DD7ull);
			return SplitMix64(x);
		}

		uint64_t Rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

		double Gauss(double x, double mu, double s1, double s2)
		{
			const double t = (x - mu) / (x < mu ? s1 : s2);
			return std::exp(-0.5 * t * t);
		}

		uint32_t ResolveThreads(uint32_t threads)
		{
			if (threads == 0) {
				threads = std::max(1u, std::thread::hardware_concurrency());
			}
			return threads;
		}

		// Runs work(i) for i in [0, count) on a pool; order of completion does not matter to callers.
		template <typename F>
		void ParallelFor(uint64_t count, uint32_t threads, F&& work)
		{
			std::atomic<uint64_t> next{ 0 };
			const auto worker = [&] {
				for (;;) {
					const uint64_t i = next.fetch_add(1);
					if (i >= count) {
						return;
					}
					work(i);
				}
			};
			const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(threads, std::max<uint64_t>(count, 1)));
			std::vector<std::thread> pool;
			for (uint32_t t = 1; t < n; ++t) {
				pool.emplace_back(worker);
			}
			worker();
			for (std::thread& thread : pool) {
				thread.join();
			}
		}

		// A point of the sampling domain, before canonicalization. Backward: (sensor x, y, z, rear x,
		// rear y, lambda). Forward: (entry x, y, direction sx, sy, lambda, unused).
		using State = std::array<double, 6>;

		struct Sample {
			std::array<float, kTransportInputs> inputs{};
			std::array<double, kTransportOutputs> outputs{};
			bool valid = false;
		};

		class DomainSampler {
		public:
			DomainSampler(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, const TraceOptions& options)
				: lens_(lens), path_(path), domain_(domain), options_(options), lambda_(domain.lambdaMinNm, domain.lambdaMaxNm)
			{
				backward_ = domain.direction == PathDirection::Backward;
				sinMax_ = std::sin(domain.maxFieldAngle);
			}

			State Uniform(LensRng& rng) const
			{
				State s{};
				if (backward_) {
					const double rr = domain_.sensorRadius * std::sqrt(rng.Uniform());
					const double phi = 2.0 * std::numbers::pi * rng.Uniform();
					s[0] = rr * std::cos(phi);
					s[1] = rr * std::sin(phi);
					s[2] = domain_.sensorZMin + (domain_.sensorZMax - domain_.sensorZMin) * rng.Uniform();
					s[3] = domain_.rearHalfExtent * (2.0 * rng.Uniform() - 1.0);
					s[4] = domain_.rearHalfExtent * (2.0 * rng.Uniform() - 1.0);
					s[5] = lambda_.Sample(rng.Uniform());
				} else {
					const double re = domain_.entryRadius * std::sqrt(rng.Uniform());
					const double phiE = 2.0 * std::numbers::pi * rng.Uniform();
					const double rs = sinMax_ * std::sqrt(rng.Uniform());
					const double phiS = 2.0 * std::numbers::pi * rng.Uniform();
					s[0] = re * std::cos(phiE);
					s[1] = re * std::sin(phiE);
					s[2] = rs * std::cos(phiS);
					s[3] = rs * std::sin(phiS);
					s[4] = lambda_.Sample(rng.Uniform());
				}
				return s;
			}

			// Canonical float32 inputs of a state, then the exact transport of those inputs.
			Sample Evaluate(const State& s) const
			{
				Sample out;
				Vec3 point;
				Vec3 dir;
				double lambda;
				if (backward_) {
					point = { s[3], s[4], domain_.inputPlaneZ };
					dir = Normalize(point - Vec3{ s[0], s[1], s[2] });
					lambda = s[5];
				} else {
					point = { s[0], s[1], domain_.inputPlaneZ };
					dir = { s[2], s[3], std::sqrt(std::max(0.0, 1.0 - s[2] * s[2] - s[3] * s[3])) };
					lambda = s[4];
				}
				CanonicalFrame frame;
				const CanonicalRay canonical = Canonicalize(point, dir, frame);
				out.inputs = { static_cast<float>(canonical.r), static_cast<float>(canonical.dx), static_cast<float>(canonical.dy), static_cast<float>(lambda) };
				const std::array<double, kTransportInputs> in{ out.inputs[0], out.inputs[1], out.inputs[2], out.inputs[3] };
				out.valid = TraceCanonical(lens_, path_, domain_, in, out.outputs, options_);
				return out;
			}

		private:
			const LensPrescription& lens_;
			const LensPath& path_;
			const SamplingDomain& domain_;
			TraceOptions options_;
			WavelengthSampler lambda_;
			bool backward_ = true;
			double sinMax_ = 0.0;
		};

		struct ChunkResult {
			std::vector<float> inputs;
			std::vector<float> outputs;
			DatasetStats stats;
			bool complete = true;
		};

		// count rays that complete the path, by rejection sampling over the domain.
		ChunkResult RowChunk(const DomainSampler& sampler, const DatasetOptions& options, uint64_t count, LensRng& rng)
		{
			ChunkResult out;
			out.inputs.reserve(count * kTransportInputs);
			out.outputs.reserve(count * kTransportOutputs);
			uint64_t rows = 0;
			uint64_t attempts = 0;
			while (rows < count) {
				if (++attempts > options.maxAttemptsPerSample * std::max<uint64_t>(count, 1)) {
					out.complete = false;
					break;
				}
				const Sample s = sampler.Evaluate(sampler.Uniform(rng));
				++out.stats.traces;
				++out.stats.uniformDraws;
				if (s.valid) {
					++out.stats.uniformValid;
					out.inputs.insert(out.inputs.end(), s.inputs.begin(), s.inputs.end());
					for (const double o : s.outputs) {
						out.outputs.push_back(static_cast<float>(o));
					}
					++rows;
				}
			}
			return out;
		}
	} // namespace

	SamplingDomain DefaultSamplingDomain(const LensPrescription& lens, const LensPath& path, PathDirection direction, double minFocusDistanceMm)
	{
		SamplingDomain domain;
		domain.direction = direction;
		const ParaxialData paraxial = ComputeParaxial(lens);
		const double imageCircle = lens.imageCircleRadiusMm > 0.0 ? lens.imageCircleRadiusMm : paraxial.efl * std::tan(20.0 * std::numbers::pi / 180.0);
		if (direction == PathDirection::Backward) {
			domain.inputPlaneZ = lens.RearPlaneZ();
			domain.outputPlaneZ = lens.FrontPlaneZ();
			domain.sensorRadius = 1.1 * imageCircle;
			domain.sensorZMin = lens.sensorZ;
			const double shift = SensorShiftForFocus(lens, minFocusDistanceMm);
			domain.sensorZMax = lens.sensorZ + (std::isfinite(shift) ? std::max(0.0, shift) : 0.0);
			domain.rearHalfExtent = 1.05 * lens.surfaces.back().semiAperture;
		} else {
			domain.inputPlaneZ = lens.FrontPlaneZ();
			domain.outputPlaneZ = lens.RearPlaneZ();
			domain.entryRadius = 1.05 * lens.surfaces.front().semiAperture;
			domain.maxFieldAngle = std::min(1.1 * std::atan(imageCircle / paraxial.efl), 85.0 * std::numbers::pi / 180.0);
		}
		return domain;
	}

	bool TraceCanonical(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, const std::array<double, kTransportInputs>& inputs,
		std::array<double, kTransportOutputs>& outputs, const TraceOptions& options)
	{
		const double r = inputs[0];
		const double dx = inputs[1];
		const double dy = inputs[2];
		const double s = 1.0 - dx * dx - dy * dy;
		if (!(s > 0.0) || !(r >= 0.0)) {
			return false;
		}
		const bool backward = domain.direction == PathDirection::Backward;
		const Vec3 dir{ dx, dy, backward ? -std::sqrt(s) : std::sqrt(s) };
		const Vec3 point{ r, 0.0, domain.inputPlaneZ };
		const Ray ray{ point - dir * kTraceBackoffMm, dir };
		const TraceResult result = TraceSequential(lens, ray, inputs[3], path, options);
		if (result.status != TraceStatus::Ok) {
			return false;
		}
		Ray exit = result.exit;
		if (backward ? exit.dir.z >= 0.0 : exit.dir.z <= 0.0) {
			return false;
		}
		if (!PropagateToPlane(exit, domain.outputPlaneZ)) {
			return false;
		}
		outputs = { exit.origin.x, exit.origin.y, exit.dir.x, exit.dir.y, exit.dir.z, result.throughput };
		for (const double o : outputs) {
			if (!std::isfinite(o)) {
				return false;
			}
		}
		return true;
	}

	double WavelengthSampler::CieSum(double l)
	{
		const double x = 1.056 * Gauss(l, 599.8, 37.9, 31.0) + 0.362 * Gauss(l, 442.0, 16.0, 26.7) - 0.065 * Gauss(l, 501.1, 20.4, 26.2);
		const double y = 0.821 * Gauss(l, 568.8, 46.9, 40.5) + 0.286 * Gauss(l, 530.9, 16.3, 31.1);
		const double z = 1.217 * Gauss(l, 437.0, 11.8, 36.0) + 0.681 * Gauss(l, 459.0, 26.0, 13.8);
		return std::max(0.0, x + y + z);
	}

	WavelengthSampler::WavelengthSampler(double minNm, double maxNm, double uniformFraction) : minNm_(minNm), maxNm_(maxNm)
	{
		const size_t bins = std::max<size_t>(1, static_cast<size_t>(std::llround(maxNm - minNm)));
		const double width = (maxNm - minNm) / bins;
		pdf_.resize(bins);
		double total = 0.0;
		for (size_t i = 0; i < bins; ++i) {
			pdf_[i] = CieSum(minNm + (i + 0.5) * width);
			total += pdf_[i] * width;
		}
		cdf_.assign(bins + 1, 0.0);
		for (size_t i = 0; i < bins; ++i) {
			pdf_[i] = (1.0 - uniformFraction) * (total > 0.0 ? pdf_[i] / total : 1.0 / (maxNm - minNm)) + uniformFraction / (maxNm - minNm);
			cdf_[i + 1] = cdf_[i] + pdf_[i] * width;
		}
		for (double& c : cdf_) {
			c /= cdf_.back();
		}
	}

	double WavelengthSampler::Sample(double u) const
	{
		const auto upper = std::upper_bound(cdf_.begin(), cdf_.end(), u);
		const size_t index = std::min(static_cast<size_t>(std::max<std::ptrdiff_t>(upper - cdf_.begin() - 1, 0)), pdf_.size() - 1);
		const double width = (maxNm_ - minNm_) / pdf_.size();
		const double span = cdf_[index + 1] - cdf_[index];
		const double t = span > 0.0 ? (u - cdf_[index]) / span : 0.5;
		return minNm_ + (index + std::clamp(t, 0.0, 1.0)) * width;
	}

	double WavelengthSampler::Pdf(double lambdaNm) const
	{
		if (!(lambdaNm >= minNm_ && lambdaNm <= maxNm_)) {
			return 0.0;
		}
		const double scaled = (lambdaNm - minNm_) / (maxNm_ - minNm_) * pdf_.size();
		return pdf_[std::min(static_cast<size_t>(scaled), pdf_.size() - 1)];
	}

	LensRng::LensRng(uint64_t seed)
	{
		uint64_t x = seed;
		for (uint64_t& s : s_) {
			s = SplitMix64(x);
		}
	}

	uint64_t LensRng::Next()
	{
		const uint64_t result = Rotl(s_[1] * 5, 7) * 9;
		const uint64_t t = s_[1] << 17;
		s_[2] ^= s_[0];
		s_[3] ^= s_[1];
		s_[1] ^= s_[2];
		s_[0] ^= s_[3];
		s_[2] ^= t;
		s_[3] = Rotl(s_[3], 45);
		return result;
	}

	double LensRng::Uniform() { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }

	Dataset GenerateDataset(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, const DatasetOptions& options)
	{
		const auto start = std::chrono::steady_clock::now();
		const DomainSampler sampler(lens, path, domain, options.trace);
		const uint64_t chunk = std::max<uint32_t>(options.chunkSize, 2);
		const uint64_t chunks = (options.rowCount + chunk - 1) / chunk;
		std::vector<ChunkResult> results(chunks);
		std::atomic<uint64_t> done{ 0 };

		ParallelFor(chunks, ResolveThreads(options.threads), [&](uint64_t i) {
			const uint64_t count = std::min(chunk, options.rowCount - i * chunk);
			LensRng rng(StreamSeed(options.seed, 1, i));
			results[i] = RowChunk(sampler, options, count, rng);
			const uint64_t now = done.fetch_add(count) + count;
			if (options.progress) {
				options.progress(now, options.rowCount);
			}
		});

		Dataset dataset;
		dataset.inputs.reserve(options.rowCount * kTransportInputs);
		dataset.outputs.reserve(options.rowCount * kTransportOutputs);
		for (ChunkResult& r : results) {
			dataset.complete = dataset.complete && r.complete;
			dataset.stats.traces += r.stats.traces;
			dataset.stats.uniformDraws += r.stats.uniformDraws;
			dataset.stats.uniformValid += r.stats.uniformValid;
			dataset.inputs.insert(dataset.inputs.end(), r.inputs.begin(), r.inputs.end());
			dataset.outputs.insert(dataset.outputs.end(), r.outputs.begin(), r.outputs.end());
			r = {};
		}
		dataset.stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		return dataset;
	}

	LabelledRays SampleUniformRays(const LensPrescription& lens, const LensPath& path, const SamplingDomain& domain, uint64_t count, uint64_t seed,
		uint32_t threads, const TraceOptions& options)
	{
		const DomainSampler sampler(lens, path, domain, options);
		constexpr uint64_t kChunk = 8192;
		LabelledRays rays;
		rays.inputs.resize(count);
		rays.outputs.resize(count);
		rays.labels.resize(count);
		ParallelFor((count + kChunk - 1) / kChunk, ResolveThreads(threads), [&](uint64_t c) {
			LensRng rng(StreamSeed(seed, 3, c));
			for (uint64_t i = c * kChunk; i < std::min(count, (c + 1) * kChunk); ++i) {
				const Sample s = sampler.Evaluate(sampler.Uniform(rng));
				rays.inputs[i] = { s.inputs[0], s.inputs[1], s.inputs[2], s.inputs[3] };
				rays.outputs[i] = s.valid ? s.outputs : std::array<double, kTransportOutputs>{};
				rays.labels[i] = s.valid ? 1 : 0;
			}
		});
		return rays;
	}

	std::string HashHex(uint64_t hash)
	{
		char buffer[17];
		std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
		return buffer;
	}

	bool ParseHashHex(const std::string& text, uint64_t& hash)
	{
		if (text.empty() || text.size() > 16) {
			return false;
		}
		uint64_t value = 0;
		for (const char c : text) {
			value <<= 4;
			if (c >= '0' && c <= '9') {
				value |= static_cast<uint64_t>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				value |= static_cast<uint64_t>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				value |= static_cast<uint64_t>(c - 'A' + 10);
			} else {
				return false;
			}
		}
		hash = value;
		return true;
	}

	bool ParsePathCode(const LensPrescription& lens, const std::string& code, PathDirection direction, LensPath& path)
	{
		const bool forward = direction == PathDirection::Forward;
		if (code == "T") {
			path = TransmissionPath(lens, forward);
			return true;
		}
		unsigned first = 0;
		unsigned second = 0;
		char tail = 0;
		if (std::sscanf(code.c_str(), "R%uR%u%c", &first, &second, &tail) != 2) {
			return false;
		}
		// Forward ghosts meet the later surface first; backward ones are the reversed forward path.
		const LensPath candidate = forward ? GhostPath(lens, first, second) : GhostPath(lens, second, first).Reversed();
		if (candidate.steps.empty() || candidate.Code() != code) {
			return false;
		}
		path = candidate;
		return true;
	}

	std::string SamplingDomainToJson(const SamplingDomain& d)
	{
		nlohmann::json j;
		j["direction"] = ToString(d.direction);
		j["input_plane_z"] = d.inputPlaneZ;
		j["output_plane_z"] = d.outputPlaneZ;
		j["lambda_min_nm"] = d.lambdaMinNm;
		j["lambda_max_nm"] = d.lambdaMaxNm;
		j["sensor_radius"] = d.sensorRadius;
		j["sensor_z_min"] = d.sensorZMin;
		j["sensor_z_max"] = d.sensorZMax;
		j["rear_half_extent"] = d.rearHalfExtent;
		j["entry_radius"] = d.entryRadius;
		j["max_field_angle_rad"] = d.maxFieldAngle;
		return j.dump();
	}

	const char* ToString(PathDirection direction) { return direction == PathDirection::Forward ? "forward" : "backward"; }

} // namespace lens
