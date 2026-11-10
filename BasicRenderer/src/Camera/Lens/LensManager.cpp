#include "Camera/Lens/LensManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <Windows.h>
#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "LensCore/BrLens.h"
#include "LensCore/Lens.h"
#include "LensCore/LensAnalysis.h"
#include "LensCore/LensDataset.h"
#include "LensCore/LensPolynomial.h"
#include "LensCore/LensTrace.h"

#include "../../../generated/BuiltinResources.h"
#include "../shaders/Include/lens/lensDataLayout.h"
#include "Pipeline/ShaderCompilation/RendererShaderStore.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/Runtime/IReadbackService.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ReadbackRequest.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Utilities/Utilities.h"
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>

namespace br::lens {

namespace {

using Row = std::array<float, 4>;
constexpr double kDegrees = std::numbers::pi / 180.0;
// The lens buffer's state-graph address: its own primary id, so it can't meet another family's variant.
constexpr uint64_t kLensBufferPrimaryId = 0x4C454E53ull;
constexpr uint32_t kPupilBins = 48u;
constexpr uint32_t kPupilGrid = 128u;
constexpr uint32_t kNormalizationGrid = 384u;
// Rays per pupil node interval for the sampling statistics lens.get reports (pupil_sampling), at each place it measures (focus
// node 0 and halfway between each pair of focus nodes).
constexpr uint64_t kPupilStatsRaysPerBin = 4096u;
// Per-frame focus: the focus-dependent tables are built at this many focus nodes, uniform in the focus shift from infinity to
// kNearFocusMm (the polynomial's fitted range; explicit uses the same). 8 nodes hold the pupil as well as 16 (LensBake
// focuspupil: the same acceptance and no light outside, for half the build); SARP_LENS_FOCUS_NODES overrides it (2-32).
constexpr uint32_t kDefaultFocusNodes = 8u;
constexpr double kNearFocusMm = 300.0;
// The autofocus pass's distance-to-shift table: entries uniform in 1 / distance from infinity to the near limit.
constexpr uint32_t kFocusTableEntries = 64u;
// Frames between readbacks of the focus state for lens.get and the menu.
constexpr uint64_t kFocusReadbackInterval = 4u;
// The autofocus dead band: a quarter of the depth of focus's half-width (f-number times the circle of confusion, the frame
// diagonal on the sensor over 1500).
constexpr double kDeadBandDepthOfFocus = 0.25;
constexpr double kCircleOfConfusionDivisor = 1500.0;
constexpr uint32_t kDistortionSamples = 129u;
// The exact pupil (LensCore ComputePupilDiscs): nodes at the pupil bins' edges (kPupilBins + 1), boundary directions per node
// and wavelength. 256 directions hold every passing ray (96 left about 1e-4 of the light outside, at the cat's-eye tips).
constexpr uint32_t kPupilDiscAngles = 256u;

// Field remap's covered field ends where a pupil bin's throughput (pass fraction times box area) falls below this fraction
// of the centre's, if that comes before the prescription's image circle.
constexpr double kRemapMinRelativeThroughput = 0.1;
constexpr size_t kSelfTestOutputRows = 1u + LENS_MAX_SELFTEST_RAYS * LENS_SELFTEST_OUTPUT_ROWS;
// A lens named "name" is the prescription lenses/name.lens.json.
constexpr std::string_view kPrescriptionSuffix = ".lens.json";

float AsFloat(uint32_t value) {
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

uint32_t AsUint(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

Row UintRow(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return { AsFloat(x), AsFloat(y), AsFloat(z), AsFloat(w) }; }

uint64_t Mix(uint64_t hash, uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash = (hash ^ ((value >> (byte * 8)) & 0xFFu)) * 1099511628211ull;
    }
    return hash;
}

uint64_t MixDouble(uint64_t hash, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return Mix(hash, bits);
}

const char* ModeName(uint32_t mode) {
    switch (mode) {
    case 0: return "pinhole";
    case 1: return "explicit";
    case 2: return "polynomial";
    }
    return "unknown";
}

uint32_t ParseMode(const nlohmann::json& value) {
    if (value.is_number_unsigned() || value.is_number_integer()) {
        const auto mode = value.get<int64_t>();
        if (mode < 0 || mode > 2) throw std::runtime_error("lens mode must be 0 (pinhole), 1 (explicit) or 2 (polynomial)");
        return static_cast<uint32_t>(mode);
    }
    const auto name = value.get<std::string>();
    if (name == "pinhole") return 0u;
    if (name == "explicit") return 1u;
    if (name == "polynomial") return 2u;
    throw std::runtime_error("lens mode must be 'pinhole', 'explicit' or 'polynomial'");
}

uint32_t ParseDegree(const nlohmann::json& value) {
    const auto degree = value.get<int64_t>();
    if (degree != 5 && degree != 7) throw std::runtime_error("polynomial_degree must be 5 or 7");
    return static_cast<uint32_t>(degree);
}

const char* FieldModeName(uint32_t mode) { return mode == static_cast<uint32_t>(LensFieldMode::Physical) ? "physical" : "remap"; }

uint32_t ParseFieldMode(const nlohmann::json& value) {
    if (value.is_number_unsigned() || value.is_number_integer()) {
        const auto mode = value.get<int64_t>();
        if (mode < 0 || mode > 1) throw std::runtime_error("lens field_mode must be 0 (remap) or 1 (physical)");
        return static_cast<uint32_t>(mode);
    }
    const auto name = value.get<std::string>();
    if (name == "remap") return static_cast<uint32_t>(LensFieldMode::Remap);
    if (name == "physical") return static_cast<uint32_t>(LensFieldMode::Physical);
    throw std::runtime_error("lens field_mode must be 'remap' or 'physical'");
}

std::filesystem::path ResolveLensPath(const std::string& file) {
    std::filesystem::path path(file);
    if (path.extension() == ".json") {
        return path.is_absolute() ? path : LensManager::LensDirectory() / path;
    }
    return LensManager::LensDirectory() / (file + std::string(kPrescriptionSuffix));
}

// The .brlens beside a prescription: name.lens.json -> name.brlens.
std::filesystem::path PolynomialPathFor(const std::filesystem::path& prescription) {
    const std::string name = prescription.filename().string();
    if (name.size() > kPrescriptionSuffix.size() && name.ends_with(kPrescriptionSuffix)) {
        return prescription.parent_path() / (name.substr(0, name.size() - kPrescriptionSuffix.size()) + ".brlens");
    }
    auto path = prescription;
    return path.replace_extension(".brlens");
}

// A float literal HLSL reads back to the same float.
std::string FloatLiteral(float value) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    std::string text = buffer;
    if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
    return text + "f";
}

// lensPolynomial.hlsli's LensPolynomialEvaluate for one polynomial, its coefficients as literals, in LensCore's EvalPolynomial
// order: per variable its scaled value and powers, then per term the monomial ((p0 * p1) * p2) * p3 (powers of 0 left out,
// as multiplying by 1 is exact) added to its block's outputs. key receives the FNV-1a 64 of the code, which the text's
// LENS_POLYNOMIAL_KEY_LO / _HI carry too.
std::string GeneratePolynomialHlsl(const ::lens::LensPolynomial& poly, const std::string& lensName, const std::string& file, uint64_t& key) {
    std::string body;
    body.reserve(64u * 1024u);
    const char* inputs[4] = { "inputs.x", "inputs.y", "inputs.z", "(w * w)" };
    std::vector<std::array<bool, 17>> used(4);
    for (const auto& e : poly.exponents) {
        for (uint32_t i = 0; i < 4; ++i) used[i][e[i]] = true;
    }
    body += "void LensPolynomialEvaluate(float4 inputs, out float y[6])\n{\n    const float w = 1000.0f / inputs.w;\n";
    for (uint32_t i = 0; i < 4; ++i) {
        body += "    const float u" + std::to_string(i) + " = (" + inputs[i] + " - " + FloatLiteral(poly.center[i]) + ") * " + FloatLiteral(poly.invScale[i]) + ";\n";
        uint32_t highest = 0;
        for (uint32_t e = 1; e <= poly.degree; ++e) highest = used[i][e] ? e : highest;
        for (uint32_t e = 1; e <= highest; ++e) {
            const std::string name = "p" + std::to_string(i) + "_" + std::to_string(e);
            body += "    const float " + name + " = " + (e == 1 ? "u" + std::to_string(i) : "p" + std::to_string(i) + "_" + std::to_string(e - 1) + " * u" + std::to_string(i)) + ";\n";
        }
    }
    body += "    float px = 0.0f, py = 0.0f, dx = 0.0f, dy = 0.0f, dz = 0.0f, tp = 0.0f;\n";
    const uint32_t even = poly.evenTerms, odd = poly.OddTerms();
    const float* c = poly.coefficients.data();
    const float* cpx = c;
    const float* cpy = cpx + even;
    const float* cdx = cpy + odd;
    const float* cdy = cdx + even;
    const float* cdz = cdy + odd;
    const float* ctp = cdz + even;
    for (uint32_t k = 0; k < poly.TermCount(); ++k) {
        const auto& e = poly.exponents[k];
        std::string monomial;
        for (uint32_t i = 0; i < 4; ++i) {
            if (e[i] == 0) continue;
            const std::string factor = "p" + std::to_string(i) + "_" + std::to_string(e[i]);
            monomial = monomial.empty() ? factor : "(" + monomial + " * " + factor + ")";
        }
        if (monomial.empty()) monomial = "1.0f";
        body += "    { const float m = " + monomial + "; ";
        if (k < even) {
            body += "px += " + FloatLiteral(cpx[k]) + " * m; dx += " + FloatLiteral(cdx[k]) + " * m; dz += " + FloatLiteral(cdz[k]) + " * m; tp += "
                + FloatLiteral(ctp[k]) + " * m; }\n";
        } else {
            body += "py += " + FloatLiteral(cpy[k - even]) + " * m; dy += " + FloatLiteral(cdy[k - even]) + " * m; }\n";
        }
    }
    body += "    y[0] = px;\n    y[1] = py;\n    y[2] = dx;\n    y[3] = dy;\n    y[4] = dz;\n    y[5] = tp;\n}\n";
    key = 14695981039346656037ull;
    for (const char ch : body) key = (key ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    char keyLines[160];
    std::snprintf(keyLines, sizeof(keyLines), "#define LENS_POLYNOMIAL_KEY_LO 0x%08Xu\n#define LENS_POLYNOMIAL_KEY_HI 0x%08Xu\n",
        static_cast<uint32_t>(key), static_cast<uint32_t>(key >> 32u));
    return "// Generated by LensManager from " + file + ": the polynomial lens of '" + lensName + "', degree " + std::to_string(poly.degree) + " ("
        + std::to_string(poly.TermCount()) + " terms). It lives in memory only (lensPolynomial.hlsli).\n" + keyLines + body;
}

uint32_t FocusNodeCount() {
    if (const char* value = std::getenv("SARP_LENS_FOCUS_NODES")) {
        return std::clamp<uint32_t>(static_cast<uint32_t>(std::atoi(value)), 2u, LENS_MAX_FOCUS_NODES);
    }
    return kDefaultFocusNodes;
}

// Per pupil node, focus node and wavelength band: the energy that clears the lens exactly over the energy the polynomial carries,
// both over the exact pupil at sensor radii in the node's tent and focus coordinates in the focus node's tent, as the shader
// interpolates them (16384 samples from an R7 low-discrepancy sequence: two dimensions for the region, one for the wavelength in
// the band, two for the radius, two for the focus). A grid of cell centres never reached the region's outer band, where the
// blocked rays are, and left the polynomial +0.45% bright. The discs hold the pupil but are a little larger than it, and the
// polynomial can't tell a blocked ray from a passing one, so the polynomial lens weights every sample by this to keep each
// sensor radius's exposure; per band, because how much of the region the lens blocks changes with the wavelength (one
// factor for all left the spectral image 0.6% blue-dark and 0.3% red-bright). Three bands over 380-780 nm (centres
// LENS_POLYNOMIAL_BAND_*), interpolated quadratically in the shader; one band (the d line) when monochromatic. 1 where
// there is no region or the polynomial's domain ends.
std::vector<std::array<float, 3>> PolynomialTransmission(const ::lens::LensPrescription& prescription, const ::lens::LensPath& backward,
    const ::lens::FocusedPupilDiscs& focused, uint32_t focusNode, const ::lens::LensPolynomial& poly, const ::lens::SamplingDomain& domain,
    const ::lens::TraceOptions& options, bool monochromatic, uint32_t threads, nlohmann::json& stats) {
    constexpr uint32_t kSamples = 16384;
    const ::lens::PupilDiscs& discs = focused.focus[focusNode];
    const uint32_t nodes = discs.NodeCount();
    const double lastFocus = double(focused.FocusCount() - 1u);
    const uint32_t bands = monochromatic ? 1u : 3u;
    const double bandWidth = (double(LENS_LAMBDA_MAX) - double(LENS_LAMBDA_MIN)) / 3.0;
    std::vector<std::array<float, 3>> factors(nodes, { 1.0f, 1.0f, 1.0f });
    const double spacing = discs.maxSensorRadius / double(nodes - 1u);
    // R7 (Roberts 2018): alpha_d = phi^-(d + 1), phi the root of x^8 = x + 1.
    double phi = 1.2;
    for (int iteration = 0; iteration < 32; ++iteration) phi -= (std::pow(phi, 8.0) - phi - 1.0) / (8.0 * std::pow(phi, 7.0) - 1.0);
    double alpha[7];
    for (uint32_t d = 0; d < 7; ++d) alpha[d] = std::pow(phi, -double(d + 1));
    std::vector<double> blocked(nodes, -1.0);
    std::atomic<uint32_t> next{ 0 };
    const auto work = [&] {
        for (uint32_t job = next.fetch_add(1); job < nodes * bands; job = next.fetch_add(1)) {
            const uint32_t node = job / bands, band = job % bands;
            const double radius = node / double(nodes - 1u) * discs.maxSensorRadius;
            // A node past the fitted radius still weights the radii inside it that interpolate toward it.
            if (discs.nodes[node].count == 0 || radius - spacing > domain.sensorRadius) continue;
            double exact = 0.0, polynomial = 0.0;
            uint64_t samples = 0, passing = 0;
            for (uint32_t n = 0; n < kSamples; ++n) {
                double u[7];
                for (uint32_t d = 0; d < 7; ++d) u[d] = std::fmod(0.5 + n * alpha[d], 1.0);
                // The sensor radius from the tent around the node and the focus from the tent around the focus node (the shader
                // interpolates both linearly), the region as the shader interpolates it: the discs are widened most at the
                // nodes, so a node's own region alone overstates how much is blocked between them (P7 came out 0.15% dark).
                double sensorRadius = radius + (u[3] - u[4]) * spacing;
                sensorRadius = std::abs(sensorRadius);
                if (sensorRadius > domain.sensorRadius || sensorRadius > discs.maxSensorRadius) continue;
                double focus = double(focusNode) + (u[5] - u[6]);
                if (focus < 0.0) focus = -focus;
                if (focus > lastFocus) focus = 2.0 * lastFocus - focus;
                const ::lens::PupilRegion region = focused.At(sensorRadius, focus);
                if (region.count == 0) continue;
                const ::lens::Vec3 origin{ sensorRadius, 0.0, focused.SensorZAt(focus) };
                const double lambdaNm = monochromatic ? ::lens::kLambdaD : double(LENS_LAMBDA_MIN) + (band + u[2]) * bandWidth;
                double x = 0.0, y = 0.0, area = 0.0;
                if (!::lens::SamplePupilRegion(region, u[0], u[1], x, y, area)) continue;
                // Weighted by the region's area: the shader's estimate carries it (pdf 1 / area).
                const double weight = area;
                const ::lens::Vec3 rear{ x, y, discs.rearPlaneZ };
                const ::lens::Vec3 toRear = rear - origin;
                const double distance2 = ::lens::Dot(toRear, toRear);
                const ::lens::Ray ray{ origin, toRear * (1.0 / std::sqrt(distance2)) };
                const double geometry = ray.dir.z * ray.dir.z / distance2;
                ++samples;
                const ::lens::TraceResult result = ::lens::TraceSequential(prescription, ray, lambdaNm, backward, options);
                if (result.status == ::lens::TraceStatus::Ok) {
                    exact += weight * result.throughput * geometry;
                    ++passing;
                }
                if (std::abs(x) > domain.rearHalfExtent || std::abs(y) > domain.rearHalfExtent) continue;
                ::lens::CanonicalFrame frame;
                const ::lens::CanonicalRay canonical = ::lens::Canonicalize(rear, ray.dir, frame);
                const auto out = ::lens::EvalTransport(poly, { float(canonical.r), float(canonical.dx), float(canonical.dy),
                    float(std::clamp(lambdaNm, domain.lambdaMinNm, domain.lambdaMaxNm)) });
                if (out[4] < 0.0f && std::isfinite(out[5])) polynomial += weight * std::max(double(out[5]), 0.0) * geometry;
            }
            const float factor = polynomial > 0.0 && exact > 0.0 ? static_cast<float>(exact / polynomial) : 1.0f;
            if (monochromatic) factors[node] = { factor, factor, factor };
            else factors[node][band] = factor;
            if (samples && band == bands / 2u) blocked[node] = 1.0 - double(passing) / double(samples);
        }
    };
    {
        std::vector<std::thread> pool;
        for (uint32_t t = 1; t < std::max(1u, threads); ++t) pool.emplace_back(work);
        work();
        for (auto& thread : pool) thread.join();
    }
    double minimum = 1e30, maximum = 0.0, blockedMax = 0.0;
    nlohmann::json perNode = nlohmann::json::array();
    for (uint32_t node = 0; node < nodes; ++node) {
        perNode.push_back(factors[node]);
        if (blocked[node] < 0.0) continue;
        for (const float f : factors[node]) {
            minimum = std::min<double>(minimum, f);
            maximum = std::max<double>(maximum, f);
        }
        blockedMax = std::max(blockedMax, blocked[node]);
    }
    stats = { { "transmission_min", minimum }, { "transmission_max", maximum }, { "transmission_per_node", perNode },
        { "region_blocked_centre", blocked.empty() ? 0.0 : blocked[0] }, { "region_blocked_max", blockedMax }, { "samples", kSamples },
        { "bands", bands } };
    return factors;
}

// The polynomial of `degree` in the .brlens at `file`, which must have been fitted to this very prescription. Anything wrong
// with it is logged and gives none, and the lens is traced explicitly instead.
std::optional<::lens::LensPolynomial> LoadPolynomial(const std::filesystem::path& file, const ::lens::LensPrescription& prescription, uint32_t degree,
    ::lens::SamplingDomain& domain, std::string& fitJson, std::string& error) {
    std::optional<::lens::LensPolynomial> polynomial;
    std::string readError;
    const auto brlens = ::lens::ReadBrLens(file, prescription.sourceHash, &readError);
    if (!brlens) {
        error = readError.empty() ? "unreadable" : readError;
    } else if (const ::lens::LensPolynomial* found = brlens->Find(degree); !found) {
        error = "it has no polynomial of degree " + std::to_string(degree);
    } else if (std::abs(brlens->domain.inputPlaneZ - prescription.RearPlaneZ()) > 1e-6 || std::abs(brlens->domain.outputPlaneZ - prescription.FrontPlaneZ()) > 1e-6) {
        error = "its reference planes are not this prescription's rear and front planes";
    } else {
        polynomial = *found;
        domain = brlens->domain;
        fitJson = brlens->metadataJson;
    }
    if (!polynomial) spdlog::error("LensManager: the polynomial lens '{}' can't be used ({}); tracing the explicit lens instead", file.string(), error);
    return polynomial;
}

// The image radius field remap (LENS_FLAG_FIELD_REMAP) fits the frame's half-diagonal to, out to which the lens images with
// usable light: the prescription's image circle (its design coverage), cut short where a pupil bin's throughput (pass
// fraction times box area) falls below kRemapMinRelativeThroughput of the centre's.
double RemapRadius(const ::lens::ExitPupilBounds& pupil, double imageCircle, double maxSensorRadius) {
    const auto throughput = [&pupil](size_t bin) {
        const auto& box = pupil.bins[bin];
        return box.valid ? box.passFraction * (box.maxX - box.minX) * (box.maxY - box.minY) : 0.0;
    };
    const double centre = throughput(0);
    for (size_t bin = 1; bin < pupil.bins.size(); ++bin) {
        if (throughput(bin) < kRemapMinRelativeThroughput * centre) {
            return std::min(imageCircle, static_cast<double>(bin) / kPupilBins * maxSensorRadius);
        }
    }
    return imageCircle;
}

// lens.get's pupil_sampling: the exact pupil's apertures and build time, and, from random exact traces at focus node 0 and halfway
// between each pair of focus nodes (where the interpolated region is furthest from a fitted one), its acceptance (area weighted
// out to remapRadius, and at the centre) and how many passing rays fell outside it.
nlohmann::json MeasurePupilSampling(const ::lens::LensPrescription& prescription, const ::lens::FocusedPupilDiscs& focused, const ::lens::TraceOptions& options,
    bool monochromatic, double maxSensorRadius, double remapRadius, uint32_t threads) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<double> places{ 0.0 };
    for (uint32_t f = 0; f + 1u < focused.FocusCount(); ++f) places.push_back(f + 0.5);
    uint64_t leaked = 0;
    nlohmann::json perFocus = nlohmann::json::array();
    nlohmann::json intervals = nlohmann::json::array();
    double acceptanceDisk = 0.0, acceptanceCentre = 0.0, acceptanceMin = 1.0;
    for (size_t place = 0; place < places.size(); ++place) {
        const ::lens::PupilDiscs discs = focused.AtFocus(places[place]);
        const uint64_t seed = 3u + place * 7u;
        const auto stats = monochromatic
            ? ::lens::MeasurePupilDiscs(prescription, discs, options, kPupilStatsRaysPerBin, ::lens::kLambdaD, ::lens::kLambdaD, 0.1, seed, threads)
            : ::lens::MeasurePupilDiscs(prescription, discs, options, kPupilStatsRaysPerBin, LENS_LAMBDA_MIN, LENS_LAMBDA_MAX, 0.1, seed + 1u, threads);
        double weightSum = 0.0, acceptance = 0.0;
        uint64_t placeLeaked = 0;
        for (uint32_t index = 0; index < stats.intervals.size(); ++index) {
            const auto& s = stats.intervals[index];
            placeLeaked += s.passOutside;
            if (place == 0u) intervals.push_back(s.Acceptance());
            const double r0 = double(index) / kPupilBins * maxSensorRadius, r1 = std::min((index + 1.0) / kPupilBins * maxSensorRadius, remapRadius);
            if (r1 <= r0 || s.rays == 0) continue;
            weightSum += r1 * r1 - r0 * r0;
            acceptance += (r1 * r1 - r0 * r0) * s.Acceptance();
        }
        acceptance = weightSum > 0.0 ? acceptance / weightSum : 0.0;
        leaked += placeLeaked;
        acceptanceDisk += acceptance / double(places.size());
        acceptanceCentre += (stats.intervals.empty() ? 0.0 : stats.intervals[0].Acceptance()) / double(places.size());
        acceptanceMin = std::min(acceptanceMin, acceptance);
        perFocus.push_back({ { "focus_coordinate", places[place] }, { "acceptance_disk", acceptance }, { "passing_outside", placeLeaked } });
    }
    nlohmann::json apertures = nlohmann::json::array();
    for (const uint32_t aperture : focused.apertures) apertures.push_back(aperture);
    return {
        { "apertures", apertures }, { "build_ms", focused.buildMs }, { "focus_nodes", focused.FocusCount() }, { "acceptance_disk", acceptanceDisk },
        { "acceptance_disk_min", acceptanceMin }, { "acceptance_centre", acceptanceCentre }, { "passing_outside", leaked },
        { "rays_per_interval", kPupilStatsRaysPerBin }, { "per_focus", perFocus }, { "acceptance_per_interval_focus0", intervals },
        { "stats_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() },
    };
}

// Generates the polynomial's shader code and registers it as the in-memory include `generated/lensPolynomial_<key>.hlsli`.
// key receives the code's hash and include the quoted name LENS_POLYNOMIAL_INCLUDE is defined as. Returns lens.get's
// description of it, with the fit accuracy the .brlens recorded for this degree.
nlohmann::json RegisterPolynomialInclude(const ::lens::LensPolynomial& polynomial, const std::string& lensName, const std::string& fileName,
    const std::string& fitJson, uint64_t& key, std::wstring& include) {
    const std::string code = GeneratePolynomialHlsl(polynomial, lensName, fileName, key);
    char includeName[64];
    std::snprintf(includeName, sizeof(includeName), "lensPolynomial_%016llx.hlsli", static_cast<unsigned long long>(key));
    br::shaderstore::SetGeneratedSource(includeName, code);
    include = L"\"generated/" + std::wstring(includeName, includeName + std::strlen(includeName)) + L"\"";
    nlohmann::json description = { { "degree", polynomial.degree }, { "terms", polynomial.TermCount() }, { "code_bytes", code.size() },
        { "include", std::string("generated/") + includeName } };
    if (!fitJson.empty()) {
        try {
            const auto fit = nlohmann::json::parse(fitJson);
            for (const auto& entry : fit.value("accuracy", nlohmann::json::array())) {
                if (entry.value("degree", 0u) == polynomial.degree) description["fit_accuracy"] = entry;
            }
        } catch (const std::exception&) {
            // A .brlens whose metadata isn't JSON still has a usable polynomial; lens.get just lacks its fit accuracy.
        }
    }
    return description;
}

// The self-test's rays: sensor points across the field, rear-plane points in (and a little past) their bins' boxes, each
// with what LensCore's TraceSequential makes of it.
std::vector<LensSelfTestRay> MakeSelfTestRays(const ::lens::LensPrescription& prescription, const ::lens::LensPath& backward,
    const ::lens::TraceOptions& options, const ::lens::ExitPupilBounds& pupil, double maxSensorRadius, double sensorZ) {
    std::vector<LensSelfTestRay> rays;
    std::mt19937 random(0x4C454E53u);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double radii[] = { 0.0, 0.2, 0.4, 0.6, 0.8, 0.95 };
    const double azimuths[] = { 0.0, 0.9, 2.2, 3.7, 5.1 };
    const double wavelengths[] = { ::lens::kLambdaD, 450.0, 650.0 };
    uint32_t index = 0;
    for (const double fraction : radii) {
        for (const double azimuth : azimuths) {
            const double radius = fraction * maxSensorRadius;
            const auto& box = pupil.BinFor(radius);
            const double cx = 0.5 * (box.minX + box.maxX), cy = 0.5 * (box.minY + box.maxY);
            const double hx = box.valid ? 0.6 * (box.maxX - box.minX) : 5.0, hy = box.valid ? 0.6 * (box.maxY - box.minY) : 5.0;
            for (int sample = 0; sample < 12; ++sample, ++index) {
                const double u = cx + (unit(random) * 2.0 - 1.0) * hx;
                const double v = cy + (unit(random) * 2.0 - 1.0) * hy;
                const double c = std::cos(azimuth), s = std::sin(azimuth);
                const ::lens::Vec3 origin{ radius * c, radius * s, sensorZ };
                const ::lens::Vec3 rear{ c * u - s * v, s * u + c * v, pupil.rearPlaneZ };
                const ::lens::Ray ray{ origin, ::lens::Normalize(rear - origin) };
                LensSelfTestRay test;
                test.origin = { origin.x, origin.y, origin.z };
                test.direction = { ray.dir.x, ray.dir.y, ray.dir.z };
                test.lambdaNm = wavelengths[index % 3];
                std::vector<::lens::Vec3> hits;
                const ::lens::TraceResult result = ::lens::TraceSequential(prescription, ray, test.lambdaNm, backward, options, &hits);
                test.status = static_cast<uint32_t>(result.status);
                test.edgeMarginMm = 1e30;
                for (size_t step = 0; step < hits.size(); ++step) {
                    const auto& surface = prescription.surfaces[backward.steps[step].surface];
                    const double limit = surface.semiAperture * (surface.isStop ? options.stopScale : 1.0);
                    test.edgeMarginMm = std::min(test.edgeMarginMm, std::abs(std::sqrt(hits[step].x * hits[step].x + hits[step].y * hits[step].y) - limit));
                }
                test.exitOrigin = { result.exit.origin.x, result.exit.origin.y, result.exit.origin.z };
                test.exitDirection = { result.exit.dir.x, result.exit.dir.y, result.exit.dir.z };
                test.throughput = result.throughput;
                rays.push_back(test);
            }
        }
    }
    return rays;
}

} // namespace

struct LensPolynomialReference {
    ::lens::LensPrescription prescription;
    ::lens::LensPath path; // the backward transmission path
    ::lens::LensPolynomial polynomial;
    ::lens::SamplingDomain domain;
};

struct LensManager::Shared {
    std::atomic<std::shared_ptr<const LensPublishedState>> published;
    std::shared_ptr<org::Buffer> fallback;
    std::shared_ptr<br::render::VersionedBufferFamily> family;
    std::mutex publishMutex;

    std::mutex statusMutex;
    std::string lastError;
    uint64_t requestedRevision = 0; // the newest build posted to the state graph

    // The focus state the GPU last reported (OnFrame's readback of Builtin::RayTracing::LensFocus).
    std::mutex focusMutex;
    LensFocusReport focusReport;
    std::atomic<bool> focusReading{ false };

    std::atomic<uint64_t> selfTestRequested{ 0 };
    std::atomic<uint64_t> selfTestCompleted{ 0 };
    std::atomic<bool> selfTestReading{ false };
    std::mutex reportMutex;
    nlohmann::json selfTestReport = nlohmann::json::object();

    void Publish(std::shared_ptr<const LensPublishedState> state) {
        std::scoped_lock lock(publishMutex);
        const auto current = published.load(std::memory_order_acquire);
        if (current && current->revision > state->revision) return;
        published.store(std::move(state), std::memory_order_release);
    }
};

namespace {

// Builtin::RayTracing::LensData: the published lens buffer, or the fallback (no LENS_DATA_MAGIC: a pinhole camera).
class LensDataResolver final : public org::ClonableResolver<LensDataResolver> {
public:
    explicit LensDataResolver(std::shared_ptr<LensManager::Shared> shared) : m_shared(std::move(shared)) {}

    std::vector<std::shared_ptr<org::Resource>> Resolve() const override { return { Current() }; }

    uint64_t DeclarationVersionHint() const noexcept override {
        const auto published = m_shared->published.load(std::memory_order_acquire);
        return ((published ? published->revision : 0u) << 1u) | 1u;
    }

    std::shared_ptr<const org::ResolverDeclarationState> CaptureDeclarationState() const override {
        const auto published = m_shared->published.load(std::memory_order_acquire);
        const uint64_t revision = published ? published->revision : 0u;
        if (auto cached = m_cache->state.load(std::memory_order_acquire); cached && m_cache->revision.load() == revision) return cached;
        auto state = std::make_shared<org::ResolverDeclarationState>();
        auto resource = published && published->buffer ? published->buffer : std::static_pointer_cast<org::Resource>(m_shared->fallback);
        state->dependencyIdentity = m_shared;
        state->resourceSetIdentity = { resource ? resource->GetGlobalResourceID() : 0u, 0x6C656E7364617461ull };
        state->contentRevision = revision + 1u;
        state->resources = std::make_shared<const org::ResolverResourceList>(org::ResolverResourceList{ resource });
        state->waits = std::make_shared<const std::vector<org::ExternalTimelinePoint>>();
        state->tracked = true;
        m_cache->state.store(state, std::memory_order_release);
        m_cache->revision.store(revision);
        return state;
    }

private:
    std::shared_ptr<org::Resource> Current() const {
        const auto published = m_shared->published.load(std::memory_order_acquire);
        if (published && published->buffer) return published->buffer;
        return m_shared->fallback;
    }
    struct Cache {
        std::atomic<std::shared_ptr<const org::ResolverDeclarationState>> state;
        std::atomic<uint64_t> revision{ ~0ull };
    };
    std::shared_ptr<LensManager::Shared> m_shared;
    std::shared_ptr<Cache> m_cache = std::make_shared<Cache>();
};

} // namespace

namespace {

// One focus node's chief-ray table at an angle, as lensCommon.hlsli's LensImageRadiusAtNode.
double TableRadiusAt(const std::vector<double>& radius, double step, double angle) {
    if (radius.size() < 2 || step <= 0.0) return 0.0;
    const double scaled = std::max(angle, 0.0) / step;
    const double last = static_cast<double>(radius.size() - 1);
    if (scaled >= last) {
        return radius.back() * std::tan(std::min(angle, 1.55)) / std::tan(last * step);
    }
    const size_t index = static_cast<size_t>(scaled);
    const double t = scaled - static_cast<double>(index);
    return radius[index] + (radius[index + 1] - radius[index]) * t;
}

// The two focus nodes and the weight on the second, for a focus coordinate.
void FocusNodesAt(size_t count, double s, size_t& node, size_t& next, double& t) {
    node = next = 0;
    t = 0.0;
    if (count < 2) return;
    s = std::clamp(s, 0.0, double(count - 1));
    node = std::min(static_cast<size_t>(s), count - 2);
    next = node + 1;
    t = s - double(node);
}

std::atomic<std::shared_ptr<const LensStatus>> g_status{ std::make_shared<const LensStatus>() };

// lensFocusDistance's focus shift for the published lens, clamped to its focus range.
double ManualShift(const LensPublishedState& published, double focusM) {
    if (!published.prescription || !published.info) return 0.0;
    double shift = focusM > 0.0 ? ::lens::SensorShiftForFocus(*published.prescription, focusM * 1000.0) : 0.0;
    if (!std::isfinite(shift)) shift = published.info->shiftMaxMm;
    return std::clamp(shift, published.info->shiftMinMm, published.info->shiftMaxMm);
}

uint32_t ParseFocusMode(const nlohmann::json& value) {
    if (value.is_number_unsigned() || value.is_number_integer()) {
        const auto mode = value.get<int64_t>();
        if (mode < 0 || mode > 1) throw std::runtime_error("lens focus_mode must be 0 (manual) or 1 (autofocus)");
        return static_cast<uint32_t>(mode);
    }
    const auto name = value.get<std::string>();
    if (name == "manual") return static_cast<uint32_t>(LensFocusMode::Manual);
    if (name == "auto" || name == "autofocus" || name == "af") return static_cast<uint32_t>(LensFocusMode::Auto);
    throw std::runtime_error("lens focus_mode must be 'manual' or 'auto'");
}

const char* FocusModeName(uint32_t mode) { return mode == static_cast<uint32_t>(LensFocusMode::Auto) ? "auto" : "manual"; }

// Approximate depth of field (thin lens of the paraxial EFL, distances from the front vertex): the near and far limits (m,
// infinity past the hyperfocal distance) for focus at distanceM, f-number N and circle of confusion c (mm).
nlohmann::json DepthOfField(double eflMm, double fNumber, double cocMm, double distanceM) {
    nlohmann::json result = { { "approximate", true }, { "circle_of_confusion_mm", cocMm } };
    if (!(eflMm > 0.0 && fNumber > 0.0 && cocMm > 0.0)) return result;
    const double hyperfocal = eflMm * eflMm / (fNumber * cocMm) + eflMm;
    result["hyperfocal_m"] = hyperfocal / 1000.0;
    if (!std::isfinite(distanceM) || distanceM <= 0.0) {
        result["near_m"] = hyperfocal / 1000.0;
        result["far_m"] = nullptr; // infinity
        return result;
    }
    const double s = distanceM * 1000.0;
    result["near_m"] = s * (hyperfocal - eflMm) / (hyperfocal + s - 2.0 * eflMm) / 1000.0;
    if (s < hyperfocal) result["far_m"] = s * (hyperfocal - eflMm) / (hyperfocal - s) / 1000.0;
    else result["far_m"] = nullptr;
    return result;
}

nlohmann::json DistanceJson(double metres) {
    return std::isfinite(metres) ? nlohmann::json(metres) : nlohmann::json("infinity");
}

} // namespace

double LensInfo::FocusCoordinate(double shiftMm) const {
    if (focusNodes.size() < 2 || !(shiftMaxMm > shiftMinMm)) return 0.0;
    return (std::clamp(shiftMm, shiftMinMm, shiftMaxMm) - shiftMinMm) / (shiftMaxMm - shiftMinMm) * double(focusNodes.size() - 1);
}

double LensInfo::ImageRadiusAt(double angle, double focusCoordinate) const {
    if (distortionRadius.empty()) return 0.0;
    size_t node, next;
    double t;
    FocusNodesAt(distortionRadius.size(), focusCoordinate, node, next, t);
    const double a = TableRadiusAt(distortionRadius[node], distortionStepRad, angle);
    return t > 0.0 ? a + (TableRadiusAt(distortionRadius[next], distortionStepRad, angle) - a) * t : a;
}

double LensInfo::RemapFieldAt(double focusCoordinate) const {
    if (focusNodes.empty()) return 0.0;
    size_t node, next;
    double t;
    FocusNodesAt(focusNodes.size(), focusCoordinate, node, next, t);
    return focusNodes[node].remapFieldRad + (focusNodes[next].remapFieldRad - focusNodes[node].remapFieldRad) * t;
}

double LensInfo::NormalizationAt(double focusCoordinate) const {
    if (normalizationTable.empty() || focusNodes.size() < 2) return focusNodes.empty() ? 0.0 : focusNodes.front().normalization;
    // As the shader (LensInverseNormalization): the table, 1 / sqrt(K) linearly.
    size_t node, next;
    double t;
    const double x = std::clamp(focusCoordinate / double(focusNodes.size() - 1), 0.0, 1.0) * double(normalizationTable.size() - 1);
    FocusNodesAt(normalizationTable.size(), x, node, next, t);
    const double a = 1.0 / std::sqrt(normalizationTable[node]), b = 1.0 / std::sqrt(normalizationTable[next]);
    const double root = a + (b - a) * t;
    return 1.0 / (root * root);
}

std::shared_ptr<const LensStatus> LensManager::Status() { return g_status.load(std::memory_order_acquire); }

std::filesystem::path LensManager::LensDirectory() {
    std::vector<std::filesystem::path> candidates;
    wchar_t module[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH) > 0) {
        candidates.push_back(std::filesystem::path(module).parent_path() / "lenses");
    }
    std::error_code error;
    candidates.push_back(std::filesystem::current_path(error) / "lenses");
#ifdef BASICRENDERER_LENS_SOURCE_DIR
    candidates.push_back(std::filesystem::path(BASICRENDERER_LENS_SOURCE_DIR));
#endif
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_directory(candidate, error)) return candidate;
    }
    return candidates.front();
}

std::vector<std::string> LensManager::ListLenses() {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(LensDirectory(), error)) {
        const auto name = entry.path().filename().string();
        if (name.size() > kPrescriptionSuffix.size() && name.ends_with(kPrescriptionSuffix)) {
            names.push_back(name.substr(0, name.size() - kPrescriptionSuffix.size()));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::pair<std::wstring, std::wstring>> LensManager::ShaderDefines(const LensPublishedState& state) {
    if (state.polynomialInclude.empty()) return {};
    return { { L"LENS_POLYNOMIAL_INCLUDE", state.polynomialInclude } };
}

LensManager::LensManager(br::render::RendererStateRequestService* requests, std::shared_ptr<org::runtime::IUploadService> uploads)
    : m_shared(std::make_shared<Shared>()), m_requests(requests), m_uploads(std::move(uploads)) {
    m_shared->published.store(std::make_shared<LensPublishedState>());
    // No magic, so a pinhole camera however the GPU finds it: its contents are never read past the header's first word.
    m_fallback = CreateIndexedStructuredBuffer(LENS_HEADER_ROWS, sizeof(Row));
    m_fallback->SetName("Lens data (none)");
    m_shared->fallback = m_fallback;
    m_selfTestOutput = CreateIndexedStructuredBuffer(kSelfTestOutputRows, sizeof(Row), true);
    m_selfTestOutput->SetName("Lens self-test output");
    // The per-frame focus (lensDataLayout.h): never rebuilt, so its state carries across lens revisions and focus changes.
    m_focusBuffer = CreateIndexedStructuredBuffer(LENS_FOCUS_ROWS, sizeof(Row), true);
    m_focusBuffer->SetName("Lens focus");
    if (m_requests) {
        m_family = std::make_shared<br::render::VersionedBufferFamily>(m_requests->VersionedBuffers(),
            br::render::VersionedBufferFamily::Config{
                { br::render::ArtifactKind::BufferVersion, kLensBufferPrimaryId, 0 }, "Lens::Data", sizeof(Row), false, false,
                br::render::PublishedFragmentKind::Views, br::render::PublishedResourceUsage::ShaderResource, kLensBufferPrimaryId });
        m_shared->family = m_family;
    }
    m_resolver = std::make_shared<LensDataResolver>(m_shared);

    auto& settings = SettingsManager::GetInstance();
    m_desired.mode = settings.getSettingGetter<uint32_t>("lensMode")();
    m_desired.file = settings.getSettingGetter<std::string>("lensFile")();
    m_desired.monochromatic = settings.getSettingGetter<bool>("lensMonochromatic")();
    m_desired.fNumber = settings.getSettingGetter<float>("lensFNumber")();
    m_desired.fieldMode = settings.getSettingGetter<uint32_t>("lensFieldMode")();
    m_desired.polynomialDegree = settings.getSettingGetter<uint32_t>("lensPolynomialDegree")();
    m_desired.generation = 1;
    // Observers run on whichever thread set the setting: they only record it and wake the worker.
    m_subscriptions.push_back(settings.addObserver<uint32_t>("lensMode", [this](const uint32_t& value) {
        { std::scoped_lock lock(m_mutex); m_desired.mode = value; }
        MarkDesiredChanged();
    }));
    m_subscriptions.push_back(settings.addObserver<std::string>("lensFile", [this](const std::string& value) {
        { std::scoped_lock lock(m_mutex); m_desired.file = value; }
        MarkDesiredChanged();
    }));
    // lensFocusDistance and the autofocus settings are read per frame (FocusParameters): focus never rebuilds.
    m_subscriptions.push_back(settings.addObserver<bool>("lensMonochromatic", [this](const bool& value) {
        { std::scoped_lock lock(m_mutex); m_desired.monochromatic = value; }
        MarkDesiredChanged();
    }));
    // The f-number rebuilds (the stop is part of the exact pupil): about as long as any build.
    m_subscriptions.push_back(settings.addObserver<float>("lensFNumber", [this](const float& value) {
        { std::scoped_lock lock(m_mutex); m_desired.fNumber = value; }
        MarkDesiredChanged();
    }));
    m_subscriptions.push_back(settings.addObserver<uint32_t>("lensFieldMode", [this](const uint32_t& value) {
        { std::scoped_lock lock(m_mutex); m_desired.fieldMode = value; }
        MarkDesiredChanged();
    }));
    m_subscriptions.push_back(settings.addObserver<uint32_t>("lensPolynomialDegree", [this](const uint32_t& value) {
        { std::scoped_lock lock(m_mutex); m_desired.polynomialDegree = value; }
        MarkDesiredChanged();
    }));
    m_worker = std::thread([this] { WorkerLoop(); });
}

LensManager::~LensManager() { Shutdown(); }

void LensManager::Shutdown() {
    m_subscriptions.clear();
    {
        std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
    m_awaiter.reset();
}

void LensManager::MarkDesiredChanged() {
    {
        std::scoped_lock lock(m_mutex);
        ++m_desired.generation;
    }
    m_wake.notify_one();
}

void LensManager::WorkerLoop() {
    for (;;) {
        Desired desired;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || m_desired.generation != m_builtGeneration; });
            if (m_stop) return;
            desired = m_desired;
            m_builtGeneration = desired.generation;
        }
        try {
            Build(desired);
        } catch (const std::exception& exception) {
            spdlog::error("LensManager: building '{}' failed: {}", desired.file, exception.what());
            std::scoped_lock lock(m_shared->statusMutex);
            m_shared->lastError = exception.what();
        }
    }
}

void LensManager::Build(const Desired& desired) {
    const auto started = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(m_shared->statusMutex);
        m_shared->lastError.clear();
    }
    if (desired.mode == static_cast<uint32_t>(LensMode::Pinhole)) {
        auto state = std::make_shared<LensPublishedState>();
        state->revision = ++m_revision;
        {
            std::scoped_lock lock(m_shared->statusMutex);
            m_shared->requestedRevision = state->revision;
        }
        m_shared->Publish(std::move(state));
        spdlog::info("LensManager: pinhole camera");
        return;
    }
    if (!m_family || !m_requests || !m_uploads) throw std::runtime_error("no state graph to publish the lens through");

    const auto path = ResolveLensPath(desired.file);
    const ::lens::LoadResult loaded = ::lens::LoadPrescription(path);
    if (!loaded.lens) throw std::runtime_error(loaded.error);
    const ::lens::LensPrescription& prescription = *loaded.lens;
    if (prescription.SurfaceCount() > LENS_MAX_SURFACES) throw std::runtime_error("too many surfaces for the GPU tracer");
    for (const auto& surface : prescription.surfaces) {
        if (surface.aspheric.size() > LENS_MAX_ASPHERIC) throw std::runtime_error("more aspheric terms than the GPU tracer has");
    }

    std::filesystem::path polynomialFile;
    std::string polynomialError;
    std::string polynomialFitJson;
    ::lens::SamplingDomain polynomialDomain;
    std::optional<::lens::LensPolynomial> polynomial;
    if (desired.mode == static_cast<uint32_t>(LensMode::Polynomial)) {
        polynomialFile = PolynomialPathFor(path);
        polynomial = LoadPolynomial(polynomialFile, prescription, desired.polynomialDegree, polynomialDomain, polynomialFitJson, polynomialError);
    }
    const uint32_t activeMode = polynomial ? static_cast<uint32_t>(LensMode::Polynomial) : static_cast<uint32_t>(LensMode::Explicit);

    const ::lens::ParaxialData paraxial = ::lens::ComputeParaxial(prescription);
    // The f-number maps to the stop scale through the paraxial EFL, which stopping down doesn't change: the entrance pupil's
    // radius scales with the stop's.
    ::lens::TraceOptions options;
    const double wideOpen = paraxial.fNumber;
    const double fNumber = desired.fNumber > 0.0f && wideOpen > 0.0 ? std::max(double(desired.fNumber), wideOpen) : wideOpen;
    options.stopScale = std::clamp(wideOpen > 0.0 ? wideOpen / fNumber : 1.0, 0.01, 1.0);
    const ::lens::LensPath backward = ::lens::TransmissionPath(prescription, false);

    // The focus range: nodes uniform in the focus shift from infinity to kNearFocusMm, inside the polynomial's fitted sensor
    // planes (the same range when it was fitted to 300 mm, as the shipped ones are).
    const ::lens::FocusRange range = polynomial
        ? ::lens::MakeFocusRange(prescription, kNearFocusMm, FocusNodeCount(), polynomialDomain.sensorZMin, polynomialDomain.sensorZMax)
        : ::lens::MakeFocusRange(prescription, kNearFocusMm, FocusNodeCount());
    const uint32_t focusCount = range.Count();
    if (focusCount == 0 || focusCount > LENS_MAX_FOCUS_NODES) throw std::runtime_error("no focus range");

    // The chief-ray tables past the design field, so a camera field wider than the lens covers still maps; one per focus node.
    const double designField = prescription.imageCircleRadiusMm > 0.0 && paraxial.efl > 0.0
        ? std::atan(prescription.imageCircleRadiusMm / paraxial.efl) : 25.0 * kDegrees;
    const std::vector<::lens::DistortionTable> tables =
        ::lens::ComputeDistortionTables(prescription, range.sensorZ, std::min(designField * 1.4, 85.0 * kDegrees), kDistortionSamples);
    if (tables.empty() || tables.front().fieldAngle.size() < 2) throw std::runtime_error("no chief ray through the lens");
    const ::lens::DistortionTable& table = tables.front();

    const double imageCircle = prescription.imageCircleRadiusMm > 0.0 ? prescription.imageCircleRadiusMm : table.imageRadius.back();
    const double maxSensorRadius = imageCircle * 1.25;
    // The pupil tables are built on half the hardware threads (this is the lens worker; the renderer keeps the rest).
    const uint32_t buildThreads = std::max(1u, std::thread::hardware_concurrency() / 2u);
    // Pupil boxes, at focus node 0 only: the remap's throughput cut, the covered radius and the self-test's ray placement.
    ::lens::ExitPupilBounds pupil = ::lens::ComputeExitPupilBounds(prescription, range.sensorZ.front(), maxSensorRadius, kPupilBins, kPupilGrid,
        ::lens::kLambdaD, options, buildThreads);
    // Other wavelengths refract a little differently from the d line the boxes were found at: widen them by 2%.
    uint32_t lastValidBin = 0;
    bool anyValid = false;
    for (uint32_t bin = 0; bin < pupil.bins.size(); ++bin) {
        auto& box = pupil.bins[bin];
        if (!box.valid) continue;
        const double padX = 0.02 * (box.maxX - box.minX);
        const double padY = 0.02 * (box.maxY - box.minY);
        box.minX -= padX;
        box.maxX += padX;
        box.minY -= padY;
        box.maxY += padY;
        lastValidBin = bin;
        anyValid = true;
    }
    if (!anyValid || !pupil.bins[0].valid) throw std::runtime_error("no ray from the sensor centre clears the lens");

    double remapRadius = std::min(RemapRadius(pupil, imageCircle, maxSensorRadius), table.imageRadius.back());
    if (polynomial) remapRadius = std::min(remapRadius, 0.999 * polynomialDomain.sensorRadius);
    // The covered field per focus node: the remap radius's chief-ray field at that node's plane (the image grows as focus
    // comes in, so the same radius covers a little less field).
    std::vector<double> remapFields(focusCount);
    for (uint32_t f = 0; f < focusCount; ++f) remapFields[f] = tables[f].FieldAngleAt(remapRadius);
    // The polynomial is used out to the image circle: between it and the fitted disk's edge (1.1x) few rays clear the lens,
    // and its error there reached +5% of the light; physical mode's dark corners past it trace the surfaces.
    // (After the remap radius, which stays the explicit lens's: it is at most the image circle.)
    if (polynomial) polynomialDomain.sensorRadius = std::min(polynomialDomain.sensorRadius, imageCircle);
    const bool fieldRemap = desired.fieldMode == static_cast<uint32_t>(LensFieldMode::Remap);

    // The exact pupil over the focus range: per focus node and sensor radius, the discs whose intersection camera rays sample
    // (LensCore ComputeFocusedPupilDiscs), held conservative between focus nodes too; at the d line alone when monochromatic.
    ::lens::PupilDiscOptions discOptions;
    discOptions.nodes = kPupilBins + 1u;
    discOptions.angles = kPupilDiscAngles;
    discOptions.threads = buildThreads;
    if (desired.monochromatic) discOptions.wavelengths = { ::lens::kLambdaD };
    const ::lens::FocusedPupilDiscs focused = ::lens::ComputeFocusedPupilDiscs(prescription, range.sensorZ, maxSensorRadius, options, discOptions, 1u);
    for (const auto& discs : focused.focus) {
        if (discs.nodes.empty() || discs.nodes[0].count == 0) throw std::runtime_error("no exact pupil at the sensor centre");
    }

    // K at LENS_NORMALIZATION_PER_INTERVAL focus coordinates per focus interval (it bends between focus nodes), each over the
    // bounding box of the interpolated exact pupil at the centre (which holds all the light). Every LENS_NORMALIZATION_PER_INTERVAL-th
    // entry is a focus node's.
    const auto normalizationStarted = std::chrono::steady_clock::now();
    const uint32_t normalizationCount = (focusCount - 1u) * LENS_NORMALIZATION_PER_INTERVAL + 1u;
    std::vector<double> normalizationTable(normalizationCount, 0.0);
    {
        std::atomic<uint32_t> next{ 0 };
        const auto work = [&] {
            for (uint32_t k = next.fetch_add(1); k < normalizationCount; k = next.fetch_add(1)) {
                const double s = double(k) / LENS_NORMALIZATION_PER_INTERVAL;
                normalizationTable[k] = ::lens::CentreNormalization(prescription, focused.AtFocus(s), options, kNormalizationGrid);
            }
        };
        std::vector<std::thread> pool;
        for (uint32_t t = 1; t < std::min(buildThreads, normalizationCount); ++t) pool.emplace_back(work);
        work();
        for (auto& thread : pool) thread.join();
    }
    for (const double normalization : normalizationTable) {
        if (!(normalization > 0.0)) throw std::runtime_error("the centre pixel's normalization is zero");
    }
    std::vector<double> normalizations(focusCount, 0.0);
    for (uint32_t f = 0; f < focusCount; ++f) normalizations[f] = normalizationTable[f * LENS_NORMALIZATION_PER_INTERVAL];
    const double normalizationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - normalizationStarted).count();
    const nlohmann::json pupilSampling = MeasurePupilSampling(prescription, focused, options, desired.monochromatic, maxSensorRadius, remapRadius, buildThreads);

    // The polynomial lens: its per-node transmission at each focus node and its generated shader code, registered as an in-memory
    // include.
    const uint32_t discNodeCount = focused.focus.front().NodeCount();
    std::vector<std::vector<std::array<float, 3>>> polynomialTransmission(focusCount,
        std::vector<std::array<float, 3>>(discNodeCount, { 1.0f, 1.0f, 1.0f }));
    nlohmann::json polynomialJson = nlohmann::json::object();
    uint64_t polynomialKey = 0;
    std::wstring polynomialInclude;
    if (polynomial) {
        const auto transmissionStarted = std::chrono::steady_clock::now();
        nlohmann::json perFocus = nlohmann::json::array();
        double minimum = 1e30, maximum = 0.0, blockedMax = 0.0;
        for (uint32_t f = 0; f < focusCount; ++f) {
            nlohmann::json stats;
            polynomialTransmission[f] = PolynomialTransmission(prescription, backward, focused, f, *polynomial, polynomialDomain, options,
                desired.monochromatic, buildThreads, stats);
            minimum = std::min(minimum, stats.value("transmission_min", 1.0));
            maximum = std::max(maximum, stats.value("transmission_max", 1.0));
            blockedMax = std::max(blockedMax, stats.value("region_blocked_max", 0.0));
            perFocus.push_back(stats);
        }
        nlohmann::json transmissionStats = { { "transmission_min", minimum }, { "transmission_max", maximum }, { "region_blocked_max", blockedMax },
            { "per_focus_node", perFocus } };
        transmissionStats["ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - transmissionStarted).count();
        polynomialJson = RegisterPolynomialInclude(*polynomial, prescription.name, polynomialFile.filename().string(), polynomialFitJson, polynomialKey,
            polynomialInclude);
        polynomialJson["validity"] = transmissionStats;
    }

    auto selfTest = std::make_shared<std::vector<LensSelfTestRay>>(MakeSelfTestRays(prescription, backward, options, pupil, maxSensorRadius,
        range.sensorZ.front()));

    // The autofocus pass's distance-to-shift table, uniform in 1 / distance from infinity to the near limit.
    std::vector<float> focusTable(kFocusTableEntries, 0.0f);
    for (uint32_t i = 1; i < kFocusTableEntries; ++i) {
        const double distance = kNearFocusMm * double(kFocusTableEntries - 1u) / double(i);
        const double shift = ::lens::SensorShiftForFocus(prescription, distance);
        focusTable[i] = std::isfinite(shift) ? float(shift) : focusTable[i - 1u];
    }

    // Pack: header, focus nodes, surfaces, media, pupil nodes (per focus node), distortion tables (per focus node), focus table,
    // self-test rays.
    const uint32_t surfaceCount = prescription.SurfaceCount();
    const uint32_t mediumCount = static_cast<uint32_t>(prescription.media.size());
    const uint32_t distortionCount = static_cast<uint32_t>(table.imageRadius.size());
    const uint32_t distortionRowsPerFocus = (distortionCount + 3u) / 4u;
    const uint32_t focusNodeOffset = LENS_HEADER_ROWS;
    const uint32_t surfaceOffset = focusNodeOffset + focusCount * LENS_FOCUS_NODE_ROWS;
    const uint32_t mediumOffset = surfaceOffset + surfaceCount * LENS_SURFACE_ROWS;
    const uint32_t discNodeOffset = mediumOffset + mediumCount * LENS_MEDIUM_ROWS;
    const uint32_t distortionOffset = discNodeOffset + focusCount * discNodeCount * LENS_PUPIL_NODE_ROWS;
    const uint32_t focusTableOffset = distortionOffset + focusCount * distortionRowsPerFocus;
    const uint32_t normalizationOffset = focusTableOffset + (kFocusTableEntries + 3u) / 4u;
    const uint32_t selfTestOffset = normalizationOffset + (normalizationCount + 3u) / 4u;
    const uint32_t selfTestCount = static_cast<uint32_t>(std::min<size_t>(selfTest->size(), LENS_MAX_SELFTEST_RAYS));
    const uint64_t revision = ++m_revision;
    const double stepRad = table.fieldAngle[1] - table.fieldAngle[0];

    std::vector<Row> rows(selfTestOffset + selfTestCount * LENS_SELFTEST_INPUT_ROWS, Row{});
    const uint32_t flags = (desired.monochromatic ? LENS_FLAG_MONOCHROMATIC : 0u) | (polynomial ? LENS_FLAG_POLYNOMIAL : 0u)
        | (fieldRemap ? LENS_FLAG_FIELD_REMAP : 0u);
    rows[LENS_ROW_HEADER0] = UintRow(LENS_DATA_MAGIC, surfaceCount, prescription.stopIndex, flags);
    rows[LENS_ROW_HEADER1] = UintRow(mediumCount, discNodeCount, distortionCount, selfTestCount);
    rows[LENS_ROW_HEADER2] = { float(range.sensorZ.front()), float(pupil.rearPlaneZ), float(prescription.FrontPlaneZ()), float(options.stopScale) };
    rows[LENS_ROW_HEADER3] = { float(maxSensorRadius), float(stepRad), float(1.0 / normalizations.front()), float(::lens::kLambdaD) };
    rows[LENS_ROW_HEADER4] = UintRow(surfaceOffset, mediumOffset, discNodeOffset, distortionOffset);
    rows[LENS_ROW_HEADER5] = UintRow(selfTestOffset, static_cast<uint32_t>(revision), static_cast<uint32_t>(focused.apertures.size()),
        AsUint(float(remapFields.front())));
    rows[LENS_ROW_HEADER6] = UintRow(static_cast<uint32_t>(polynomialKey), static_cast<uint32_t>(polynomialKey >> 32u), polynomial ? polynomial->degree : 0u, 0u);
    rows[LENS_ROW_HEADER7] = { float(polynomialDomain.sensorRadius), float(polynomialDomain.lambdaMinNm), float(polynomialDomain.lambdaMaxNm),
        float(polynomialDomain.rearHalfExtent) };
    rows[LENS_ROW_HEADER8] = { float(range.baseSensorZ), float(range.shiftMin), float(range.shiftMax), float(kNearFocusMm) };
    rows[LENS_ROW_HEADER9] = UintRow(focusCount, focusNodeOffset, focusTableOffset, kFocusTableEntries);
    rows[LENS_ROW_HEADER10] = UintRow(normalizationOffset, normalizationCount, 0u, 0u);
    for (uint32_t index = 0; index < normalizationCount; ++index) {
        rows[normalizationOffset + index / 4u][index % 4u] = float(1.0 / normalizationTable[index]);
    }
    for (uint32_t f = 0; f < focusCount; ++f) {
        rows[focusNodeOffset + f * LENS_FOCUS_NODE_ROWS] = { float(range.sensorZ[f]), float(1.0 / normalizations[f]), float(remapFields[f]),
            std::isfinite(range.distanceMm[f]) ? float(range.distanceMm[f]) : 0.0f };
        const ::lens::PupilDiscs& discs = focused.focus[f];
        for (uint32_t node = 0; node < discNodeCount; ++node) {
            const auto& region = discs.nodes[node];
            Row* row = &rows[discNodeOffset + (f * discNodeCount + node) * LENS_PUPIL_NODE_ROWS];
            for (uint32_t disc = 0; disc < region.count && disc < LENS_MAX_PUPIL_DISCS; ++disc) {
                row[disc / 2u][(disc % 2u) * 2u] = float(region.centre[disc]);
                row[disc / 2u][(disc % 2u) * 2u + 1u] = float(region.radius[disc]);
            }
            const auto& transmission = polynomialTransmission[f][node];
            row[2] = { transmission[0], transmission[1], transmission[2], 0.0f };
        }
        for (uint32_t index = 0; index < distortionCount; ++index) {
            rows[distortionOffset + f * distortionRowsPerFocus + index / 4u][index % 4u] = float(tables[f].imageRadius[index]);
        }
    }
    for (uint32_t index = 0; index < kFocusTableEntries; ++index) {
        rows[focusTableOffset + index / 4u][index % 4u] = focusTable[index];
    }
    for (uint32_t index = 0; index < surfaceCount; ++index) {
        const auto& surface = prescription.surfaces[index];
        Row* row = &rows[surfaceOffset + index * LENS_SURFACE_ROWS];
        row[0] = { float(surface.Curvature()), float(surface.conic), float(surface.vertexZ), float(surface.semiAperture) };
        row[1] = UintRow(surface.isStop ? 1u : 0u, prescription.MediumBefore(index), surface.mediumAfter, surface.coating.enabled ? 1u : 0u);
        row[2] = { float(surface.coating.index), float(surface.coating.designWavelengthNm), AsFloat(static_cast<uint32_t>(surface.aspheric.size())), 0.0f };
        for (size_t term = 0; term < surface.aspheric.size(); ++term) row[3][term] = float(surface.aspheric[term]);
    }
    for (uint32_t index = 0; index < mediumCount; ++index) {
        const auto& glass = prescription.media[index];
        Row* row = &rows[mediumOffset + index * LENS_MEDIUM_ROWS];
        const uint32_t model = glass.model == ::lens::Glass::Model::Cauchy ? LENS_MEDIUM_CAUCHY
            : glass.model == ::lens::Glass::Model::Sellmeier ? LENS_MEDIUM_SELLMEIER : LENS_MEDIUM_CONSTANT;
        row[0] = { AsFloat(model), float(glass.nd), float(glass.cauchyA), float(glass.cauchyB) };
        row[1] = { float(glass.sellmeierB[0]), float(glass.sellmeierB[1]), float(glass.sellmeierB[2]), 0.0f };
        row[2] = { float(glass.sellmeierC[0]), float(glass.sellmeierC[1]), float(glass.sellmeierC[2]), 0.0f };
    }
    for (uint32_t index = 0; index < selfTestCount; ++index) {
        const auto& test = (*selfTest)[index];
        rows[selfTestOffset + index * 2u] = { float(test.origin[0]), float(test.origin[1]), float(test.origin[2]), float(test.lambdaNm) };
        rows[selfTestOffset + index * 2u + 1u] = { float(test.direction[0]), float(test.direction[1]), float(test.direction[2]), 0.0f };
    }
    selfTest->resize(selfTestCount);

    auto info = std::make_shared<LensInfo>();
    info->name = prescription.name;
    info->path = path;
    info->sourceHash = prescription.sourceHash;
    info->requestedMode = desired.mode;
    info->activeMode = activeMode;
    info->polynomialPath = polynomialFile;
    info->polynomialError = polynomialError;
    if (polynomial) {
        info->polynomialDegree = polynomial->degree;
        info->polynomialKey = polynomialKey;
        info->polynomialSensorZMin = polynomialDomain.sensorZMin;
        info->polynomialSensorZMax = polynomialDomain.sensorZMax;
        info->polynomialSensorRadius = polynomialDomain.sensorRadius;
        info->polynomialJson = polynomialJson.dump();
    }
    info->focusClampedToPolynomialDomain = polynomial && range.distanceMm.back() > kNearFocusMm * 1.0001;
    info->focalLengthMm = paraxial.efl;
    info->fNumber = paraxial.fNumber / options.stopScale;
    info->wideOpenFNumber = paraxial.fNumber;
    info->baseSensorZ = range.baseSensorZ;
    info->shiftMinMm = range.shiftMin;
    info->shiftMaxMm = range.shiftMax;
    info->nearFocusMm = range.distanceMm.back();
    for (uint32_t f = 0; f < focusCount; ++f) {
        info->focusNodes.push_back({ range.shift[f], range.sensorZ[f], range.distanceMm[f], normalizations[f], remapFields[f] });
        info->distortionRadius.push_back(tables[f].imageRadius);
    }
    info->normalizationTable = normalizationTable;
    info->maxSensorRadiusMm = maxSensorRadius;
    info->coveredSensorRadiusMm = (lastValidBin + 1.0) / kPupilBins * maxSensorRadius;
    info->distortionStepRad = stepRad;
    info->coveredFieldRad = table.FieldAngleAt(std::min(info->coveredSensorRadiusMm, table.imageRadius.back()));
    info->imageCircleMm = prescription.imageCircleRadiusMm;
    info->fieldMode = fieldRemap ? static_cast<uint32_t>(LensFieldMode::Remap) : static_cast<uint32_t>(LensFieldMode::Physical);
    info->remapRadiusMm = remapRadius;
    info->stopScale = options.stopScale;
    info->monochromatic = desired.monochromatic;
    info->pupilNodes = discNodeCount;
    nlohmann::json sampling = pupilSampling;
    sampling["normalization_ms"] = normalizationMs;
    info->pupilSamplingJson = sampling.dump();
    info->buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    auto pending = std::make_shared<LensPublishedState>();
    pending->revision = revision;
    // Focus is not in it: the focus is per frame (LensFocusPass) and restarts accumulation by itself.
    uint64_t hash = 14695981039346656037ull;
    hash = Mix(hash, activeMode);
    hash = Mix(hash, prescription.sourceHash);
    hash = Mix(hash, desired.monochromatic ? 1u : 0u);
    hash = MixDouble(hash, options.stopScale);
    hash = Mix(hash, info->fieldMode);
    hash = Mix(hash, polynomialKey);
    hash = Mix(hash, focusCount);
    pending->stateHash = hash == 0u ? 1u : hash;
    pending->info = info;
    pending->selfTest = selfTest;
    pending->prescription = std::make_shared<const ::lens::LensPrescription>(prescription);
    if (polynomial) {
        auto reference = std::make_shared<LensPolynomialReference>();
        reference->prescription = prescription;
        reference->path = backward;
        reference->polynomial = *polynomial;
        reference->domain = polynomialDomain;
        pending->polynomial = std::move(reference);
        pending->polynomialInclude = polynomialInclude;
    }

    const auto bytes = std::as_bytes(std::span(rows));
    const auto request = m_family->RequestSnapshot(*m_requests, m_uploads, revision, bytes, rows.size());
    if (!request) throw std::runtime_error("the state graph refused the lens buffer (status " + std::to_string(static_cast<int>(request.status)) + ")");
    {
        std::scoped_lock lock(m_shared->statusMutex);
        m_shared->requestedRevision = revision;
    }
    spdlog::info("LensManager: '{}' ({}) built in {:.1f} ms (exact pupil {:.1f} ms; EFL {:.2f} mm, f/{:.2f} (wide open f/{:.2f}), {} focus nodes from "
        "infinity to {:.3f} m (sensor shift 0 to {:+.4f} mm), K {:.6g} at infinity, covered field {:.1f} deg, {} self-test rays); revision {} posted",
        info->name, ModeName(activeMode), info->buildMs, focused.buildMs, info->focalLengthMm, info->fNumber, info->wideOpenFNumber, focusCount,
        info->nearFocusMm / 1000.0, range.shiftMax, normalizations.front(), info->coveredFieldRad / kDegrees, selfTestCount, revision);

    const std::weak_ptr<Shared> weak = m_shared;
    auto awaiter = std::make_shared<br::render::ArtifactAwaiter>(m_requests->AwaitExact(request.Handle(), br::render::ArtifactReadiness::GpuReady,
        TaskLane::Background, TaskDomain::GraphControl,
        [weak, pending](const br::render::ArtifactSnapshot& snapshot) {
            const auto shared = weak.lock();
            if (!shared) return;
            const auto fragment = snapshot.payload.Get<br::render::RendererStateFragmentArtifact>();
            const auto version = fragment ? fragment->fragment.payload.Get<br::render::PublishedGpuBufferVersion>() : nullptr;
            if (!version || !version->resource) {
                spdlog::error("LensManager: lens buffer revision {} is ready without a buffer", pending->revision);
                return;
            }
            auto state = std::make_shared<LensPublishedState>(*pending);
            state->version = version;
            state->buffer = version->resource;
            if (shared->family) shared->family->Acknowledge(version);
            shared->Publish(std::move(state));
            spdlog::info("LensManager: lens buffer revision {} published", pending->revision);
        },
        [weak, revision](const br::render::ArtifactTermination& termination) {
            spdlog::error("LensManager: lens buffer revision {} ended without publishing: {}", revision, termination.error);
            if (const auto shared = weak.lock()) {
                std::scoped_lock lock(shared->statusMutex);
                shared->lastError = termination.error;
            }
        }));
    // The previous wait goes here, on the worker: releasing it enters the graph's mutex.
    m_awaiter = std::move(awaiter);
}


std::shared_ptr<const LensPublishedState> LensManager::Published() const {
    return m_shared->published.load(std::memory_order_acquire);
}

uint64_t LensManager::PendingSelfTest() const {
    const uint64_t requested = m_shared->selfTestRequested.load(std::memory_order_acquire);
    return requested != m_shared->selfTestCompleted.load(std::memory_order_acquire) ? requested : 0u;
}

void LensManager::NoteCamera(float verticalFov, float aspect) {
    m_cameraFov.store(verticalFov, std::memory_order_relaxed);
    m_cameraAspect.store(aspect, std::memory_order_relaxed);
}

LensFocusParameters LensManager::FocusParameters() const {
    m_focusPassPrepares.fetch_add(1, std::memory_order_relaxed);
    LensFocusParameters parameters;
    const auto published = Published();
    if (!published->Active() || !published->info || !published->prescription) return parameters;
    const LensInfo& info = *published->info;
    auto& settings = SettingsManager::GetInstance();
    parameters.active = true;
    parameters.mode = settings.getSettingGetter<uint32_t>("lensFocusMode")() == static_cast<uint32_t>(LensFocusMode::Auto)
        ? static_cast<uint32_t>(LensFocusMode::Auto) : static_cast<uint32_t>(LensFocusMode::Manual);
    parameters.manualShiftMm = static_cast<float>(ManualShift(*published, settings.getSettingGetter<float>("lensFocusDistance")()));
    parameters.rate = std::max(0.01f, settings.getSettingGetter<float>("lensAutofocusSpeed")());
    const double coc = 2.0 * info.remapRadiusMm / kCircleOfConfusionDivisor;
    parameters.deadBandMm = static_cast<float>(kDeadBandDepthOfFocus * info.fNumber * coc);
    const uint64_t source = published->prescription->sourceHash;
    parameters.identity = static_cast<uint32_t>(source ^ (source >> 32u)) | 1u;
    const auto point = settings.getSettingGetter<DirectX::XMFLOAT2>("lensAutofocusPoint")();
    parameters.pointX = std::clamp(point.x, -1.0f, 1.0f);
    parameters.pointY = std::clamp(point.y, -1.0f, 1.0f);
    parameters.region = std::clamp(settings.getSettingGetter<float>("lensAutofocusRegion")(), 0.002f, 1.0f);
    return parameters;
}

uint64_t LensManager::FocusRestartKey() const {
    const auto published = Published();
    if (!published->Active()) return 0u;
    auto& settings = SettingsManager::GetInstance();
    if (settings.getSettingGetter<uint32_t>("lensFocusMode")() == static_cast<uint32_t>(LensFocusMode::Auto)) {
        std::scoped_lock lock(m_shared->focusMutex);
        return (2ull << 40u) | m_shared->focusReport.epoch;
    }
    return (1ull << 40u) | AsUint(static_cast<float>(ManualShift(*published, settings.getSettingGetter<float>("lensFocusDistance")())));
}

namespace {

nlohmann::json CompareSelfTest(const std::vector<LensSelfTestRay>& expected, const std::vector<float>& words, uint32_t rayCount) {
    constexpr double kEdgeTolerance = 1e-4, kDirectionTolerance = 1e-4, kPositionTolerance = 1e-3, kThroughputTolerance = 1e-4;
    uint32_t okBoth = 0, failBoth = 0, statusMismatch = 0, statusMismatchNearEdge = 0, directionFail = 0, positionFail = 0, throughputFail = 0;
    double maxDirection = 0.0, maxPosition = 0.0, maxThroughput = 0.0;
    nlohmann::json failures = nlohmann::json::array();
    const uint32_t count = std::min<uint32_t>(rayCount, static_cast<uint32_t>(expected.size()));
    for (uint32_t index = 0; index < count; ++index) {
        const float* row = words.data() + (1u + index * LENS_SELFTEST_OUTPUT_ROWS) * 4u;
        const uint32_t status = AsUint(row[0]);
        const auto& reference = expected[index];
        if (status != reference.status) {
            if (reference.edgeMarginMm < kEdgeTolerance) {
                ++statusMismatchNearEdge;
            } else {
                ++statusMismatch;
                if (failures.size() < 8) failures.push_back({ { "ray", index }, { "gpu_status", status }, { "cpu_status", reference.status },
                    { "edge_margin_mm", reference.edgeMarginMm } });
            }
            continue;
        }
        if (status != 0u) {
            ++failBoth;
            continue;
        }
        ++okBoth;
        double position = 0.0, direction = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            position = std::max(position, std::abs(double(row[4 + axis]) - reference.exitOrigin[axis]));
            direction = std::max(direction, std::abs(double(row[8 + axis]) - reference.exitDirection[axis]));
        }
        const double throughput = std::abs(double(row[7]) - reference.throughput);
        maxPosition = std::max(maxPosition, position);
        maxDirection = std::max(maxDirection, direction);
        maxThroughput = std::max(maxThroughput, throughput);
        if (position >= kPositionTolerance) ++positionFail;
        if (direction >= kDirectionTolerance) ++directionFail;
        if (throughput >= kThroughputTolerance) ++throughputFail;
        if ((position >= kPositionTolerance || direction >= kDirectionTolerance || throughput >= kThroughputTolerance) && failures.size() < 8) {
            failures.push_back({ { "ray", index }, { "position_error_mm", position }, { "direction_error", direction }, { "throughput_error", throughput } });
        }
    }
    const bool passed = count == expected.size() && count > 0 && statusMismatch == 0 && directionFail == 0 && positionFail == 0 && throughputFail == 0;
    return {
        { "passed", passed }, { "rays", count }, { "expected_rays", expected.size() }, { "ok_both", okBoth }, { "blocked_both", failBoth },
        { "status_mismatches", statusMismatch }, { "status_mismatches_within_edge_tolerance", statusMismatchNearEdge },
        { "direction_failures", directionFail }, { "position_failures", positionFail }, { "throughput_failures", throughputFail },
        { "max_direction_error", maxDirection }, { "max_position_error_mm", maxPosition }, { "max_throughput_error", maxThroughput },
        { "tolerances", { { "edge_mm", kEdgeTolerance }, { "direction", kDirectionTolerance }, { "position_mm", kPositionTolerance },
            { "throughput", kThroughputTolerance } } },
        { "failures", failures },
    };
}

struct ErrorStats {
    std::vector<double> values;
    void Add(double value) { values.push_back(value); }
    nlohmann::json Json() {
        if (values.empty()) return { { "count", 0 } };
        std::sort(values.begin(), values.end());
        double sum = 0.0;
        for (const double value : values) sum += value;
        const auto at = [this](double fraction) { return values[std::min(values.size() - 1u, static_cast<size_t>(fraction * (values.size() - 1u) + 0.5))]; };
        return { { "count", values.size() }, { "mean", sum / values.size() }, { "p50", at(0.5) }, { "p95", at(0.95) }, { "max", values.back() } };
    }
};

// The polynomial rows of the self-test: the GPU's generated code against LensCore's float32 EvalTransport on the same
// canonical inputs (parity), and against TraceCanonical's exact transport of those inputs (the polynomial's error).
nlohmann::json ComparePolynomialSelfTest(const LensPolynomialReference& reference, const std::vector<LensSelfTestRay>& expected,
    const std::vector<float>& words, uint32_t rayCount) {
    const auto& domain = reference.domain;
    // Mixed error |gpu - cpu| / max(1, |cpu|): relative for positions in mm, absolute for the direction and throughput.
    constexpr double kTolerance = 1e-4;
    uint32_t evaluated = 0, outOfDomain = 0, bothValid = 0, exactBlocked = 0;
    double maxInputError = 0.0, maxOutputError = 0.0;
    std::array<double, 6> maxPerOutput{};
    ErrorStats position, angle, throughput;
    const uint32_t count = std::min<uint32_t>(rayCount, static_cast<uint32_t>(expected.size()));
    for (uint32_t index = 0; index < count; ++index) {
        const float* row = words.data() + (1u + index * LENS_SELFTEST_OUTPUT_ROWS) * 4u;
        if (AsUint(row[19]) == 0u) continue;
        ++evaluated;
        const std::array<float, 4> inputs{ row[12], row[13], row[14], row[15] };
        const std::array<float, 6> gpu{ row[16], row[17], row[20], row[21], row[22], row[18] };
        const auto cpu = ::lens::EvalTransport(reference.polynomial, inputs);
        for (size_t output = 0; output < 6; ++output) {
            const double error = std::abs(double(gpu[output]) - cpu[output]) / std::max(1.0, std::abs(double(cpu[output])));
            maxPerOutput[output] = std::max(maxPerOutput[output], error);
            maxOutputError = std::max(maxOutputError, error);
        }
        // The GPU's canonicalization against the CPU's, from the double ray.
        const auto& ray = expected[index];
        const ::lens::Vec3 origin{ ray.origin[0], ray.origin[1], ray.origin[2] };
        const ::lens::Vec3 direction = ::lens::Normalize(::lens::Vec3{ ray.direction[0], ray.direction[1], ray.direction[2] });
        const ::lens::Vec3 rear = origin + direction * ((domain.inputPlaneZ - origin.z) / direction.z);
        ::lens::CanonicalFrame frame;
        const ::lens::CanonicalRay canonical = ::lens::Canonicalize(rear, direction, frame);
        maxInputError = std::max({ maxInputError, std::abs(canonical.r - inputs[0]), std::abs(canonical.dx - inputs[1]), std::abs(canonical.dy - inputs[2]) });

        // Against exact transport, for the rays that clear the lens inside the fitted domain.
        const bool inDomain = std::abs(rear.x) <= domain.rearHalfExtent && std::abs(rear.y) <= domain.rearHalfExtent
            && std::hypot(origin.x, origin.y) <= domain.sensorRadius;
        if (!inDomain) {
            ++outOfDomain;
            continue;
        }
        std::array<double, 6> exact{};
        if (!::lens::TraceCanonical(reference.prescription, reference.path, domain, { double(inputs[0]), double(inputs[1]), double(inputs[2]), double(inputs[3]) },
                exact) || ray.status != 0u) {
            ++exactBlocked;
            continue;
        }
        ++bothValid;
        position.Add(std::hypot(double(gpu[0]) - exact[0], double(gpu[1]) - exact[1]));
        const double dx = gpu[2] - exact[2], dy = gpu[3] - exact[3], dz = gpu[4] - exact[4];
        angle.Add(2.0 * std::asin(std::min(1.0, 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz))) * (180.0 * 60.0 / std::numbers::pi));
        throughput.Add(std::abs(double(gpu[5]) - exact[5]));
    }
    const bool passed = evaluated == count && count > 0 && maxOutputError <= kTolerance;
    nlohmann::json perOutput = nlohmann::json::object();
    const char* names[] = { "px", "py", "dx", "dy", "dz", "throughput" };
    for (size_t output = 0; output < 6; ++output) perOutput[names[output]] = maxPerOutput[output];
    return {
        { "degree", reference.polynomial.degree },
        { "parity", { { "passed", passed }, { "rays", evaluated }, { "tolerance_mixed", kTolerance }, { "max_input_error", maxInputError },
            { "max_output_error_mixed", maxOutputError }, { "max_output_error_mixed_per_output", perOutput } } },
        { "vs_exact", { { "in_domain", evaluated - outOfDomain }, { "out_of_domain", outOfDomain }, { "both_valid", bothValid },
            { "exact_blocked", exactBlocked }, { "position_error_mm", position.Json() }, { "angle_error_arcmin", angle.Json() },
            { "throughput_error", throughput.Json() } } },
    };
}

} // namespace

void LensManager::OnFrame(org::RenderGraph* graph) {
    const auto published = Published();
    if (published->stateHash != m_lastPublishedHash) {
        m_lastPublishedHash = published->stateHash;
        SettingsManager::GetInstance().getSettingSetter<uint64_t>("pathTracingCameraModelHash")(published->stateHash);
        auto status = std::make_shared<LensStatus>(*Status());
        status->active = published->Active();
        status->wideOpenFNumber = published->info ? published->info->wideOpenFNumber : 0.0;
        status->fNumber = published->info ? published->info->fNumber : 0.0;
        status->nearFocusM = published->info ? published->info->nearFocusMm / 1000.0 : 0.0;
        g_status.store(std::move(status), std::memory_order_release);
    }

    // The focus state, read back every few frames while the focus passes run (for lens.get and the menu; nothing on the GPU
    // waits for it).
    const uint64_t focusPrepares = m_focusPassPrepares.load(std::memory_order_relaxed);
    const bool focusLive = focusPrepares != m_focusPassSeen;
    m_focusPassSeen = focusPrepares;
    if (published->Active() && focusLive && graph && ++m_focusReadbackFrame % kFocusReadbackInterval == 0u
        && !m_shared->focusReading.load(std::memory_order_acquire)) {
        if (auto* readbacks = graph->GetReadbackService()) {
            m_shared->focusReading.store(true, std::memory_order_release);
            const std::weak_ptr<Shared> weak = m_shared;
            const uint32_t mode = SettingsManager::GetInstance().getSettingGetter<uint32_t>("lensFocusMode")();
            readbacks->RequestReadbackCaptureAfterGraph(m_focusBuffer.get(), org::RangeSpec{}, [weak, published, mode](org::ReadbackCaptureResult&& result) {
                const auto shared = weak.lock();
                if (!shared) return;
                std::vector<float> words(result.data.size() / sizeof(float));
                std::memcpy(words.data(), result.data.data(), words.size() * sizeof(float));
                shared->focusReading.store(false, std::memory_order_release);
                if (words.size() < LENS_FOCUS_ROWS * 4u || AsUint(words[LENS_FOCUS_ROW_STATE * 4u]) != LENS_FOCUS_MAGIC || !published->prescription) return;
                const auto row = [&words](uint32_t index, uint32_t component) { return double(words[index * 4u + component]); };
                LensFocusReport report;
                report.valid = true;
                report.revision = published->revision;
                report.mode = mode;
                report.shiftMm = row(LENS_FOCUS_ROW_CURRENT, 0);
                report.targetShiftMm = row(LENS_FOCUS_ROW_CURRENT, 3);
                report.velocityMmPerS = row(LENS_FOCUS_ROW_SPRING, 0);
                const auto& prescription = *published->prescription;
                report.focusDistanceM = ::lens::FocusDistanceForShift(prescription, report.shiftMm) / 1000.0;
                report.targetDistanceM = ::lens::FocusDistanceForShift(prescription, report.targetShiftMm) / 1000.0;
                report.measuredDistanceM = row(LENS_FOCUS_ROW_SPRING, 2) / 1000.0;
                report.measuredValidFraction = row(LENS_FOCUS_ROW_MEASURE, 0);
                report.measuredSamples = static_cast<uint32_t>(row(LENS_FOCUS_ROW_MEASURE, 2));
                report.epoch = AsUint(words[LENS_FOCUS_ROW_STATE * 4u + 2u]);
                report.settled = report.velocityMmPerS == 0.0 && report.shiftMm == row(LENS_FOCUS_ROW_PREVIOUS, 0);
                {
                    std::scoped_lock lock(shared->focusMutex);
                    shared->focusReport = report;
                }
                auto status = std::make_shared<LensStatus>(*g_status.load(std::memory_order_acquire));
                status->focus = report;
                g_status.store(std::move(status), std::memory_order_release);
            });
        }
    }
    const float fov = m_cameraFov.load(std::memory_order_relaxed);
    // Physical mode only: remap fits a wider field to what the lens covers.
    if (published->Active() && published->info && published->info->fieldMode == static_cast<uint32_t>(LensFieldMode::Physical) && fov > 0.0f
        && m_coverageLoggedRevision != published->revision) {
        m_coverageLoggedRevision = published->revision;
        const double aspect = m_cameraAspect.load(std::memory_order_relaxed);
        const double halfVertical = 0.5 * fov;
        // At infinity focus: the sensor is sized per frame for the field, so coverage barely changes with focus.
        const double halfHeight = published->info->ImageRadiusAt(halfVertical, 0.0);
        const double halfDiagonalRadius = halfHeight * std::sqrt(1.0 + aspect * aspect);
        if (halfDiagonalRadius > published->info->coveredSensorRadiusMm) {
            spdlog::warn("LensManager: the camera's field ({:.1f} deg vertical, sensor half-height {:.2f} mm, half-diagonal {:.2f} mm) is wider than "
                "'{}' covers (light reaches {:.2f} mm, {:.1f} deg): the frame vignettes to black past it", fov / kDegrees, halfHeight,
                halfDiagonalRadius, published->info->name, published->info->coveredSensorRadiusMm, published->info->coveredFieldRad / kDegrees);
        }
    }

    const uint64_t sequence = PendingSelfTest();
    if (!sequence || !graph || m_shared->selfTestReading.load(std::memory_order_acquire)) return;
    auto* readbacks = graph->GetReadbackService();
    if (!readbacks) return;
    m_shared->selfTestReading.store(true, std::memory_order_release);
    const std::weak_ptr<Shared> weak = m_shared;
    readbacks->RequestReadbackCaptureAfterGraph(m_selfTestOutput.get(), org::RangeSpec{}, [weak, sequence](org::ReadbackCaptureResult&& result) {
        const auto shared = weak.lock();
        if (!shared) return;
        std::vector<float> words(result.data.size() / sizeof(float));
        std::memcpy(words.data(), result.data.data(), words.size() * sizeof(float));
        const auto state = shared->published.load(std::memory_order_acquire);
        // The pass stamps the sequence it ran for, and the lens revision it read: until both match, read again.
        if (words.size() < 4 || AsUint(words[0]) != static_cast<uint32_t>(sequence) || !state || !state->selfTest
            || AsUint(words[2]) != static_cast<uint32_t>(state->revision)) {
            shared->selfTestReading.store(false, std::memory_order_release);
            return;
        }
        const uint32_t rayCount = AsUint(words[1]);
        nlohmann::json report;
        if (words.size() < (1u + rayCount * LENS_SELFTEST_OUTPUT_ROWS) * 4u) {
            report = { { "passed", false }, { "error", "readback too small" } };
        } else {
            report = CompareSelfTest(*state->selfTest, words, rayCount);
            if (state->polynomial) {
                report["polynomial"] = ComparePolynomialSelfTest(*state->polynomial, *state->selfTest, words, rayCount);
                // Whether the pipeline that ran had this polynomial's code: until it compiles, the self-test evaluates none.
                if (!report["polynomial"]["parity"]["passed"].get<bool>()) report["passed"] = false;
            }
        }
        report["sequence"] = sequence;
        report["lens"] = state->info ? state->info->name : "";
        report["revision"] = state->revision;
        report["state"] = "done";
        spdlog::info("LensManager: self-test {}: {}", sequence, report.dump());
        {
            std::scoped_lock lock(shared->reportMutex);
            shared->selfTestReport = std::move(report);
        }
        shared->selfTestCompleted.store(sequence, std::memory_order_release);
        shared->selfTestReading.store(false, std::memory_order_release);
    });
}

std::string LensManager::HandleControl(const std::string& command, const std::string& requestJson) {
    const nlohmann::json request = requestJson.empty() ? nlohmann::json::object() : nlohmann::json::parse(requestJson);
    auto& settings = SettingsManager::GetInstance();
    if (command == "lens.set") {
        // Settings first: the observers wake the worker, which builds off this thread.
        if (request.contains("stop_scale")) throw std::runtime_error("stop_scale is gone: set f_number (the wide-open f-number divided by the old stop scale)");
        if (request.contains("file")) settings.getSettingSetter<std::string>("lensFile")(request.at("file").get<std::string>());
        // Focus is per frame: none of these rebuild the lens.
        if (request.contains("focus_m")) {
            const float focus = request.at("focus_m").get<float>();
            if (!(focus > 0.0f)) throw std::runtime_error("focus_m must be positive");
            settings.getSettingSetter<float>("lensFocusDistance")(focus);
        }
        if (request.contains("focus_mode")) settings.getSettingSetter<uint32_t>("lensFocusMode")(ParseFocusMode(request.at("focus_mode")));
        if (request.contains("af_point")) {
            const auto& point = request.at("af_point");
            if (!point.is_array() || point.size() != 2) throw std::runtime_error("af_point must be [x, y] in NDC (-1..1, +y up)");
            const float x = point.at(0).get<float>(), y = point.at(1).get<float>();
            if (!(std::abs(x) <= 1.0f && std::abs(y) <= 1.0f)) throw std::runtime_error("af_point must be inside [-1, 1]");
            settings.getSettingSetter<DirectX::XMFLOAT2>("lensAutofocusPoint")(DirectX::XMFLOAT2(x, y));
        }
        if (request.contains("af_region")) {
            const float region = request.at("af_region").get<float>();
            if (!(region > 0.0f && region <= 1.0f)) throw std::runtime_error("af_region must be in (0, 1] (a fraction of the frame height)");
            settings.getSettingSetter<float>("lensAutofocusRegion")(region);
        }
        if (request.contains("af_speed")) {
            const float speed = request.at("af_speed").get<float>();
            if (!(speed > 0.0f)) throw std::runtime_error("af_speed must be positive (1/s)");
            settings.getSettingSetter<float>("lensAutofocusSpeed")(speed);
        }
        if (request.contains("monochromatic")) settings.getSettingSetter<bool>("lensMonochromatic")(request.at("monochromatic").get<bool>());
        if (request.contains("f_number")) {
            // 0 is wide open; anything below the lens's wide-open f-number clamps to it (the build does), and so does f/16 above.
            const float fNumber = request.at("f_number").get<float>();
            if (!(fNumber >= 0.0f && fNumber <= 64.0f)) throw std::runtime_error("f_number must be 0 (wide open) or an f-number up to 64");
            settings.getSettingSetter<float>("lensFNumber")(fNumber);
        }
        if (request.contains("field_mode")) settings.getSettingSetter<uint32_t>("lensFieldMode")(ParseFieldMode(request.at("field_mode")));
        if (request.contains("polynomial_degree")) settings.getSettingSetter<uint32_t>("lensPolynomialDegree")(ParseDegree(request.at("polynomial_degree")));
        if (request.contains("mode")) settings.getSettingSetter<uint32_t>("lensMode")(ParseMode(request.at("mode")));
    } else if (command == "lens.selftest") {
        const auto published = Published();
        if (!published->Active() || !published->selfTest) throw std::runtime_error("no lens is published");
        if (!PendingSelfTest()) {
            m_shared->selfTestRequested.fetch_add(1, std::memory_order_acq_rel);
            std::scoped_lock lock(m_shared->reportMutex);
            m_shared->selfTestReport = { { "state", "pending" }, { "sequence", m_shared->selfTestRequested.load() } };
        }
        std::scoped_lock lock(m_shared->reportMutex);
        return m_shared->selfTestReport.dump();
    } else if (command == "lens.selftest.status") {
        std::scoped_lock lock(m_shared->reportMutex);
        return m_shared->selfTestReport.dump();
    } else if (command != "lens.get") {
        throw std::runtime_error("unknown lens command '" + command + "'");
    }

    nlohmann::json reply;
    const uint32_t mode = settings.getSettingGetter<uint32_t>("lensMode")();
    reply["mode"] = ModeName(mode);
    reply["file"] = settings.getSettingGetter<std::string>("lensFile")();
    reply["focus_m"] = settings.getSettingGetter<float>("lensFocusDistance")();
    reply["focus_mode"] = FocusModeName(settings.getSettingGetter<uint32_t>("lensFocusMode")());
    {
        const auto point = settings.getSettingGetter<DirectX::XMFLOAT2>("lensAutofocusPoint")();
        reply["af_point"] = { point.x, point.y };
    }
    reply["af_region"] = settings.getSettingGetter<float>("lensAutofocusRegion")();
    reply["af_speed"] = settings.getSettingGetter<float>("lensAutofocusSpeed")();
    reply["monochromatic"] = settings.getSettingGetter<bool>("lensMonochromatic")();
    reply["f_number"] = settings.getSettingGetter<float>("lensFNumber")();
    reply["field_mode"] = FieldModeName(settings.getSettingGetter<uint32_t>("lensFieldMode")());
    reply["polynomial_degree"] = settings.getSettingGetter<uint32_t>("lensPolynomialDegree")();
    reply["directory"] = LensDirectory().string();
    reply["lenses"] = ListLenses();
    const auto published = Published();
    std::string error;
    uint64_t requested = 0;
    {
        std::scoped_lock lock(m_shared->statusMutex);
        error = m_shared->lastError;
        requested = m_shared->requestedRevision;
    }
    bool building = false;
    {
        std::scoped_lock lock(m_mutex);
        building = m_desired.generation != m_builtGeneration;
    }
    building = building || requested > published->revision;
    reply["published_revision"] = published->revision;
    reply["requested_revision"] = requested;
    const bool polynomialActive = published->Active() && published->info && published->info->activeMode == static_cast<uint32_t>(LensMode::Polynomial);
    reply["state"] = !error.empty() ? "error" : building ? "building" : polynomialActive ? "polynomial" : published->Active() ? "explicit" : "pinhole";
    reply["error"] = error;
    reply["state_hash"] = published->stateHash;
    if (const auto& info = published->info; info && published->Active()) {
        const double fov = m_cameraFov.load(std::memory_order_relaxed);
        const double aspect = m_cameraAspect.load(std::memory_order_relaxed);
        // The focus: the GPU's last report, or before one, the manual distance.
        LensFocusReport focusReport;
        {
            std::scoped_lock lock(m_shared->focusMutex);
            focusReport = m_shared->focusReport;
        }
        const bool reported = focusReport.valid && focusReport.revision == published->revision;
        const double manualShift = ManualShift(*published, settings.getSettingGetter<float>("lensFocusDistance")());
        const double shift = reported ? focusReport.shiftMm : manualShift;
        const double focusCoordinate = info->FocusCoordinate(shift);
        const double focusDistanceM = published->prescription ? ::lens::FocusDistanceForShift(*published->prescription, shift) / 1000.0 : 0.0;
        // As the shader's LensSensorHalfExtent at that focus: remap fits a half-diagonal wider than the covered field to the
        // covered radius.
        const double remapField = info->RemapFieldAt(focusCoordinate);
        const double diagonal = std::sqrt(1.0 + aspect * aspect);
        const double tanHalfDiagonal = std::tan(0.5 * fov) * diagonal;
        const bool remapping = info->fieldMode == static_cast<uint32_t>(LensFieldMode::Remap) && fov > 0.0 && remapField > 0.0
            && tanHalfDiagonal > std::tan(remapField);
        const double halfHeight = remapping ? info->ImageRadiusAt(remapField, focusCoordinate) / diagonal : info->ImageRadiusAt(0.5 * fov, focusCoordinate);
        const double fieldScale = remapping ? tanHalfDiagonal / std::tan(remapField) : 1.0;
        // The circle of confusion: the frame's diagonal on the sensor over 1500. In remap mode the frame is the remapped one,
        // whose blur scales with it, so its sensor diagonal is the one to use.
        const double coc = 2.0 * halfHeight * diagonal / kCircleOfConfusionDivisor;
        nlohmann::json focusNodes = nlohmann::json::array();
        for (const auto& node : info->focusNodes) {
            focusNodes.push_back({ { "distance_m", DistanceJson(node.distanceMm / 1000.0) }, { "shift_mm", node.shiftMm }, { "sensor_z_mm", node.sensorZ },
                { "normalization_k", node.normalization }, { "covered_field_deg", node.remapFieldRad / kDegrees } });
        }
        nlohmann::json focus = {
            { "mode", FocusModeName(settings.getSettingGetter<uint32_t>("lensFocusMode")()) }, { "reported", reported },
            { "focus_m", DistanceJson(focusDistanceM) }, { "shift_mm", shift }, { "sensor_z_mm", info->baseSensorZ + shift },
            { "focus_coordinate", focusCoordinate }, { "manual_focus_m", settings.getSettingGetter<float>("lensFocusDistance")() },
            { "manual_shift_mm", manualShift }, { "normalization_k", info->NormalizationAt(focusCoordinate) },
            { "range_m", { DistanceJson(::lens::FocusDistanceForShift(*published->prescription, info->shiftMinMm) / 1000.0), info->nearFocusMm / 1000.0 } },
            { "shift_range_mm", { info->shiftMinMm, info->shiftMaxMm } }, { "nodes", focusNodes },
            { "depth_of_field", DepthOfField(info->focalLengthMm, info->fNumber, coc, focusDistanceM) },
        };
        if (reported) {
            focus["autofocus"] = { { "target_m", DistanceJson(focusReport.targetDistanceM) }, { "target_shift_mm", focusReport.targetShiftMm },
                { "measured_m", focusReport.measuredDistanceM > 0.0 ? nlohmann::json(focusReport.measuredDistanceM) : nlohmann::json(nullptr) },
                { "measured_valid_fraction", focusReport.measuredValidFraction }, { "measured_samples", focusReport.measuredSamples },
                { "velocity_mm_per_s", focusReport.velocityMmPerS }, { "settled", focusReport.settled }, { "epoch", focusReport.epoch } };
        }
        reply["lens"] = {
            { "name", info->name }, { "path", info->path.string() }, { "source_hash", info->sourceHash },
            { "efl_mm", info->focalLengthMm }, { "f_number", info->fNumber }, { "wide_open_f_number", info->wideOpenFNumber }, { "focus", focus },
            { "image_circle_mm", info->imageCircleMm },
            { "max_sensor_radius_mm", info->maxSensorRadiusMm }, { "covered_sensor_radius_mm", info->coveredSensorRadiusMm },
            { "covered_field_deg", info->coveredFieldRad / kDegrees },
            { "monochromatic", info->monochromatic }, { "stop_scale", info->stopScale }, { "build_ms", info->buildMs },
            { "pupil_nodes", info->pupilNodes },
            { "pupil_sampling", info->pupilSamplingJson.empty() ? nlohmann::json(nullptr) : nlohmann::json::parse(info->pupilSamplingJson) },
            { "active_mode", ModeName(info->activeMode) }, { "polynomial_file", info->polynomialPath.string() },
            { "polynomial_error", info->polynomialError }, { "polynomial_degree", info->polynomialDegree },
            { "polynomial_key", info->polynomialKey }, { "polynomial_sensor_z_range_mm", { info->polynomialSensorZMin, info->polynomialSensorZMax } },
            { "polynomial_sensor_radius_mm", info->polynomialSensorRadius }, { "focus_clamped_to_polynomial_domain", info->focusClampedToPolynomialDomain },
            { "polynomial", info->polynomialJson.empty() ? nlohmann::json(nullptr) : nlohmann::json::parse(info->polynomialJson) },
            { "camera_vertical_fov_deg", fov / kDegrees }, { "camera_aspect", aspect },
            { "sensor_half_height_mm", halfHeight }, { "sensor_half_width_mm", halfHeight * aspect },
            { "sensor_half_diagonal_mm", halfHeight * std::sqrt(1.0 + aspect * aspect) },
            { "field_exceeds_coverage", halfHeight * diagonal > info->coveredSensorRadiusMm },
            { "field_mode", FieldModeName(info->fieldMode) }, { "remap_radius_mm", info->remapRadiusMm },
            { "remap_field_deg", remapField / kDegrees }, { "camera_half_diagonal_deg", std::atan(tanHalfDiagonal) / kDegrees },
            { "field_remapped", remapping }, { "field_scale", fieldScale },
        };
    }
    return reply.dump();
}

std::shared_ptr<org::Resource> LensManager::ProvideResource(org::ResourceIdentifier const& key) {
    return key == org::ResourceIdentifier{ Builtin::RayTracing::LensFocus } ? m_focusBuffer : nullptr;
}

std::vector<org::ResourceIdentifier> LensManager::GetSupportedKeys() { return { org::ResourceIdentifier{ Builtin::RayTracing::LensFocus } }; }

std::shared_ptr<org::IResourceResolver> LensManager::ProvideResolver(org::ResourceIdentifier const& key) {
    return key == org::ResourceIdentifier{ Builtin::RayTracing::LensData } ? m_resolver : nullptr;
}

std::vector<org::ResourceIdentifier> LensManager::GetSupportedResolverKeys() { return { org::ResourceIdentifier{ Builtin::RayTracing::LensData } }; }

} // namespace br::lens
