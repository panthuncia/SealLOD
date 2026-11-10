#include "Camera/Lens/GhostFlareManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "LensCore/Lens.h"
#include "LensCore/LensAnalysis.h"
#include "LensCore/LensTrace.h"

#include "Camera/Lens/LensManager.h"
#include "../shaders/Include/lens/lensDataLayout.h"
#include "../shaders/Include/lens/lensGhostLayout.h"
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
// The ghost buffer's state-graph address: its own primary id ("GHST"), apart from the lens buffer's.
constexpr uint64_t kGhostBufferPrimaryId = 0x47485354ull;
// Ranking (LensCore RankGhostPaths, as LensBake rank): the F, d and C lines, five field angles over the covered field, this grid.
constexpr uint32_t kRankGrid = 48u;
// The ghosts' boxes on the front plane: this grid over the front clear aperture per field node.
constexpr uint32_t kBoxGrid = 120u;
// The front-plane square searched (and integrated over by the self-test), in front clear radii.
constexpr double kFrontSearch = 1.25;
// Sources up to this multiple of the covered field make ghosts (a source a little outside the frame still does).
constexpr double kFieldMargin = 1.5;
constexpr double kMaxField = 75.0 * kDegrees;

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

float HalfToFloat(uint16_t half) {
    const uint32_t sign = uint32_t(half >> 15u) << 31u;
    const uint32_t exponent = (half >> 10u) & 0x1Fu;
    const uint32_t mantissa = half & 0x3FFu;
    if (exponent == 0u) {
        const float value = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -value : value;
    }
    if (exponent == 31u) return AsFloat(sign | 0x7F800000u | (mantissa << 13u));
    return AsFloat(sign | ((exponent + 112u) << 23u) | (mantissa << 13u));
}

uint32_t SettingU32(const char* name) { return SettingsManager::GetInstance().getSettingGetter<uint32_t>(name)(); }

// The path's stop and rim ratios from its hits, as lensGhost.hlsli records them.
void Ratios(const ::lens::LensPrescription& prescription, const ::lens::LensPath& path, const std::vector<::lens::Vec3>& hits, double stopScale,
    double& stopRatio, double& rimRatio) {
    stopRatio = 0.0;
    rimRatio = 0.0;
    for (size_t step = 0; step < hits.size() && step < path.steps.size(); ++step) {
        const auto& surface = prescription.surfaces[path.steps[step].surface];
        const double radius = std::hypot(hits[step].x, hits[step].y);
        if (surface.isStop) stopRatio = std::max(stopRatio, radius / std::max(surface.semiAperture * stopScale, 1e-6));
        else rimRatio = std::max(rimRatio, radius / std::max(surface.semiAperture, 1e-6));
    }
}

// The box on the front plane holding the rays of `path` that clear the lens (every aperture clipping, the stop scaled) and
// leave toward the sensor, for parallel light at `field` travelling toward +x; padded by two grid cells so that the soft
// edge the draw cuts at the apertures stays inside. x min > x max when none does. The search covers a square 1.25 times the
// front clear radius: an oblique ray can cross the front plane outside that radius and still meet the curved front surface
// inside it (LensCore's RankGhostPaths, which starts rays only inside the disc, misses those: up to 10% of a ghost).
std::array<float, 3> GhostBox(const ::lens::LensPrescription& prescription, const ::lens::LensPath& path, double field, double stopScale) {
    ::lens::TraceOptions options;
    options.stopScale = stopScale;
    const double extent = kFrontSearch * prescription.surfaces.front().semiAperture;
    const double cell = 2.0 * extent / kBoxGrid;
    const double frontZ = prescription.FrontPlaneZ();
    const ::lens::Vec3 direction{ std::sin(field), 0.0, std::cos(field) };
    double minX = 1e30, maxX = -1e30, maxY = 0.0;
    for (uint32_t row = 0; row <= kBoxGrid; ++row) {
        for (uint32_t column = 0; column <= kBoxGrid; ++column) {
            const double x = -extent + column * cell, y = -extent + row * cell;
            const ::lens::Ray ray{ ::lens::Vec3{ x, y, frontZ } - direction * (1.0 / direction.z), direction };
            const ::lens::TraceResult result = ::lens::TraceSequential(prescription, ray, ::lens::kLambdaD, path, options);
            if (result.status != ::lens::TraceStatus::Ok || result.exit.dir.z <= 0.0) continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, std::abs(y));
        }
    }
    if (minX > maxX) return { 1.0f, -1.0f, 0.0f };
    const double pad = 2.0 * cell;
    return { float(std::max(minX - pad, -extent)), float(std::min(maxX + pad, extent)), float(std::min(maxY + pad, extent)) };
}

// The box at a field angle, interpolated between nodes as lensGhost.hlsli's LensGhostLoad does (in float).
bool InterpolateBox(const GhostFlarePath& ghost, float field, float step, std::array<float, 3>& box) {
    const uint32_t nodes = static_cast<uint32_t>(ghost.boxes.size());
    const float scaled = std::clamp(field / std::max(step, 1e-6f), 0.0f, float(nodes - 1u));
    const uint32_t node = std::min(static_cast<uint32_t>(scaled), nodes - 2u);
    const float t = scaled - float(node);
    const auto& b0 = ghost.boxes[node];
    const auto& b1 = ghost.boxes[node + 1u];
    const bool empty0 = b0[0] > b0[1], empty1 = b1[0] > b1[1];
    if (empty0 && empty1) return false;
    const auto& from = empty0 ? b1 : b0;
    const auto& to = empty1 ? b0 : b1;
    for (int i = 0; i < 3; ++i) box[i] = from[i] + (to[i] - from[i]) * t;
    return box[0] <= box[1] && box[2] > 0.0f;
}

} // namespace


struct GhostFlareManager::Shared {
    std::atomic<std::shared_ptr<const GhostFlarePublished>> published;
    std::shared_ptr<org::Buffer> fallback;
    std::shared_ptr<br::render::VersionedBufferFamily> family;
    std::mutex publishMutex;

    // The worker's wait: desired builds (the manager's) and self-test captures (posted by readback callbacks, which hold only
    // this).
    std::mutex workMutex;
    std::condition_variable wake;

    std::mutex statusMutex;
    std::string lastError;
    uint64_t requestedRevision = 0;

    // The pipe's test source.
    std::mutex testMutex;
    bool testSource = false;
    std::array<float, 3> testDirection{ 0.0f, 0.0f, -1.0f };
    float testDistance = 0.0f;
    std::array<float, 3> testIrradiance{};

    std::atomic<uint64_t> selfTestRequested{ 0 };
    std::atomic<uint64_t> selfTestCompleted{ 0 };
    std::atomic<bool> selfTestReading{ false };
    std::mutex reportMutex;
    nlohmann::json selfTestReport = nlohmann::json::object();
    // lens.flare.sources: a readback of the sources buffer after the next frame, for tests.
    std::atomic<bool> sourcesRequested{ false };
    std::atomic<bool> sourcesReading{ false };
    nlohmann::json sourcesReport = nlohmann::json::object(); // under reportMutex

    // A self-test frame read back: the sources and vertices buffers, for the worker to compare with LensCore.
    struct Capture {
        uint64_t sequence = 0;
        std::vector<float> sources, vertices;
        int arrived = 0;
        std::shared_ptr<const GhostFlarePublished> ghosts;
    };
    std::shared_ptr<Capture> capture; // under workMutex: complete, waiting for the worker

    void Publish(std::shared_ptr<const GhostFlarePublished> state) {
        std::scoped_lock lock(publishMutex);
        const auto current = published.load(std::memory_order_acquire);
        if (current && current->revision > state->revision) return;
        published.store(std::move(state), std::memory_order_release);
    }
};

namespace {

// Builtin::LensFlare::Ghosts: the published ghost buffer, or the fallback (no LENS_GHOST_MAGIC: no ghosts).
class GhostDataResolver final : public org::ClonableResolver<GhostDataResolver> {
public:
    explicit GhostDataResolver(std::shared_ptr<GhostFlareManager::Shared> shared) : m_shared(std::move(shared)) {}

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
        state->resourceSetIdentity = { resource ? resource->GetGlobalResourceID() : 0u, 0x67686f7374736cull };
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
    std::shared_ptr<GhostFlareManager::Shared> m_shared;
    std::shared_ptr<Cache> m_cache = std::make_shared<Cache>();
};

struct ErrorStats {
    std::vector<double> values;
    void Add(double value) { values.push_back(value); }
    nlohmann::json Json() {
        if (values.empty()) return { { "count", 0 } };
        std::sort(values.begin(), values.end());
        double sum = 0.0;
        for (const double value : values) sum += value;
        const auto at = [this](double fraction) { return values[std::min(values.size() - 1u, static_cast<size_t>(fraction * (values.size() - 1u) + 0.5))]; };
        return { { "count", values.size() }, { "mean", sum / values.size() }, { "p50", at(0.5) }, { "p99", at(0.99) }, { "max", values.back() } };
    }
};

// The energy of parallel light at `field` that `path` carries through the lens (every aperture clipping, the stop scaled) out
// toward the sensor: the integral of throughput times the cosine over the front plane, on a fine grid over the square
// GhostBox searches.
double PathEnergy(const ::lens::LensPrescription& prescription, const ::lens::LensPath& path, double field, double lambdaNm, double stopScale) {
    constexpr uint32_t kGrid = 320u;
    ::lens::TraceOptions options;
    options.stopScale = stopScale;
    const double extent = kFrontSearch * prescription.surfaces.front().semiAperture;
    const double cell = 2.0 * extent / kGrid;
    const double frontZ = prescription.FrontPlaneZ();
    const ::lens::Vec3 direction{ std::sin(field), 0.0, std::cos(field) };
    double energy = 0.0;
    for (uint32_t row = 0; row < kGrid; ++row) {
        for (uint32_t column = 0; column < kGrid; ++column) {
            const double x = -extent + (column + 0.5) * cell, y = -extent + (row + 0.5) * cell;
            const ::lens::Ray ray{ ::lens::Vec3{ x, y, frontZ } - direction * (1.0 / direction.z), direction };
            const ::lens::TraceResult result = ::lens::TraceSequential(prescription, ray, lambdaNm, path, options);
            if (result.status == ::lens::TraceStatus::Ok && result.exit.dir.z > 0.0) energy += result.throughput * direction.z;
        }
    }
    return energy * cell * cell;
}

// lens.flare.selftest: the GPU's lattice for the self-test source (lensFlareSources.hlsl: parallel, 0.6 of the field, azimuth
// 0.5 rad) against LensCore's TraceSequential on the same rays and ghost paths without clipping (status, sensor point,
// throughput times cosine, stop and rim ratios), per ghost and wavelength; and each ghost's energy (the vertices that clear
// the lens: throughput times cosine times the cell's entrance area) relative to the transmitted energy, against the same from
// exact clipped traces on a fine grid (PathEnergy), with LensCore's RankGhostPaths for that field and wavelength beside it
// (it starts rays only inside the front clear disc, so it differs by up to about 10%).
nlohmann::json CompareSelfTest(const GhostFlareManager::Shared::Capture& capture) {
    const auto& published = *capture.ghosts;
    if (!published.prescription) throw std::runtime_error("no ghosts are published");
    const ::lens::LensPrescription& prescription = *published.prescription;
    if (capture.sources.size() < size_t(LENS_FLARE_SOURCES_ROWS) * 4u) throw std::runtime_error("sources readback too small");
    const auto row = [&](uint32_t index) { return &capture.sources[size_t(index) * 4u]; };
    const float* frame0 = row(LENS_FLARE_ROW_FRAME0);
    const float* frame1 = row(LENS_FLARE_ROW_FRAME1);
    const float* frame3 = row(LENS_FLARE_ROW_FRAME3);
    const uint32_t wavelengths = AsUint(frame1[1]), grid = AsUint(frame1[2]), flags = AsUint(frame1[3]);
    const uint32_t ghostSlots = AsUint(frame3[3]);
    if ((flags & LENS_FLARE_FLAG_SELF_TEST) == 0u) throw std::runtime_error("the frame read back was not a self-test frame");
    if (AsUint(frame3[2]) != static_cast<uint32_t>(published.lensRevision)) throw std::runtime_error("the GPU traced another lens revision's ghosts");
    const double sensorZ = frame0[2];
    const float* source0 = row(LENS_FLARE_ROW_SOURCES);
    const float* source2 = row(LENS_FLARE_ROW_SOURCES + 2u);
    if (AsUint(source2[0]) == 0u) throw std::runtime_error("the self-test source is not valid");
    const ::lens::Vec3 direction = ::lens::Normalize(::lens::Vec3{ source0[0], source0[1], source0[2] });
    const float azimuthCos = source2[1], azimuthSin = source2[2], field = source2[3];
    const size_t instanceRows = LENS_FLARE_INSTANCE_ROWS(grid);
    const uint32_t side = grid + 1u;
    const double frontZ = float(prescription.FrontPlaneZ());

    constexpr double kPositionTolerance = 5e-3, kThroughputTolerance = 2e-3, kRatioTolerance = 3e-3, kEnergyTolerance = 0.05;
    uint64_t compared = 0, okBoth = 0, failBoth = 0, statusMismatch = 0;
    ErrorStats position, throughput, ratio;
    nlohmann::json perGhost = nlohmann::json::array();
    std::map<double, double> transmitted;
    std::map<double, std::map<std::string, double>> reference;
    bool energyPassed = true;
    for (uint32_t g = 0; g < ghostSlots && g < published.ghosts.size(); ++g) {
        const GhostFlarePath& ghost = published.ghosts[g];
        const ::lens::LensPath path = ::lens::GhostPath(prescription, ghost.a, ghost.b);
        std::array<float, 3> box{};
        const bool hasBox = InterpolateBox(ghost, field, float(published.fieldStepRad), box);
        nlohmann::json energies = nlohmann::json::array();
        for (uint32_t w = 0; w < wavelengths; ++w) {
            const size_t base = (size_t(g) * wavelengths + w) * instanceRows; // source slot 0
            if ((base + instanceRows) * 4u > capture.vertices.size()) throw std::runtime_error("vertices readback too small");
            const float* header = &capture.vertices[base * 4u];
            const double lambdaNm = header[1];
            const bool valid = AsUint(header[2]) != 0u;
            if (valid != hasBox) {
                ++statusMismatch;
                continue;
            }
            if (!valid) continue;
            const double cellArea = header[0];
            const float cell = 1.0f / float(grid);
            double gpuEnergy = 0.0;
            for (uint32_t vertex = 0; vertex < side * side; ++vertex) {
                const float* v = &capture.vertices[(base + LENS_FLARE_INSTANCE_HEADER_ROWS + vertex) * 4u];
                const uint32_t packed = AsUint(v[3]);
                const bool gpuOk = packed != LENS_FLARE_VERTEX_FAILED;
                const double gpuStop = gpuOk ? HalfToFloat(uint16_t(packed & 0xFFFFu)) : 0.0;
                const double gpuRim = gpuOk ? HalfToFloat(uint16_t(packed >> 16u)) : 0.0;
                if (gpuOk && gpuStop <= 1.0 && gpuRim <= 1.0) gpuEnergy += double(v[2]) * cellArea;
                // The GPU's ray, from its float box.
                const uint32_t column = vertex % side, rowIndex = vertex / side;
                const float cx = box[0] + (box[1] - box[0]) * (float(column) * cell);
                const float cy = -box[2] + (2.0f * box[2]) * (float(rowIndex) * cell);
                const ::lens::Vec3 front{ double(azimuthCos * cx - azimuthSin * cy), double(azimuthSin * cx + azimuthCos * cy), frontZ };
                const ::lens::Ray ray{ front - direction * (1.0 / direction.z), direction };
                ::lens::TraceOptions options;
                options.clipHousing = false;
                options.clipStop = false;
                std::vector<::lens::Vec3> hits;
                const ::lens::TraceResult result = ::lens::TraceSequential(prescription, ray, lambdaNm, path, options, &hits);
                const bool cpuOk = result.status == ::lens::TraceStatus::Ok && result.exit.dir.z > 1e-6 && result.exit.origin.z <= sensorZ;
                ++compared;
                if (cpuOk != gpuOk) {
                    ++statusMismatch;
                    continue;
                }
                if (!cpuOk) {
                    ++failBoth;
                    continue;
                }
                ++okBoth;
                ::lens::Ray exit = result.exit;
                ::lens::PropagateToPlane(exit, sensorZ);
                position.Add(std::hypot(exit.origin.x - v[0], exit.origin.y - v[1]));
                const double cpuThroughput = result.throughput * direction.z;
                if (cpuThroughput > 1e-12) throughput.Add(std::abs(double(v[2]) - cpuThroughput) / cpuThroughput);
                double stop = 0.0, rim = 0.0;
                Ratios(prescription, path, hits, published.stopScale, stop, rim);
                // f16 on the GPU: relative to the value, near 1 where it matters.
                if (stop < 4.0) ratio.Add(std::abs(stop - gpuStop) / std::max(1.0, stop));
                if (rim < 4.0) ratio.Add(std::abs(rim - gpuRim) / std::max(1.0, rim));
            }
            if (!transmitted.contains(lambdaNm)) {
                transmitted[lambdaNm] = PathEnergy(prescription, ::lens::TransmissionPath(prescription, true), field, lambdaNm, published.stopScale);
            }
            if (!reference.contains(lambdaNm)) {
                auto& codes = reference[lambdaNm];
                for (const auto& ranked : ::lens::RankGhostPaths(prescription, { double(field) }, 64u, lambdaNm, 0.0)) codes[ranked.path.Code()] = ranked.energy;
            }
            const double relative = transmitted[lambdaNm] > 0.0 ? gpuEnergy / transmitted[lambdaNm] : 0.0;
            const double exact = transmitted[lambdaNm] > 0.0 ? PathEnergy(prescription, path, field, lambdaNm, published.stopScale) / transmitted[lambdaNm] : 0.0;
            const double ranked = reference[lambdaNm][ghost.code];
            const double agreement = exact > 0.0 ? relative / exact : 0.0;
            if (exact > 1e-7 && std::abs(agreement - 1.0) > kEnergyTolerance) energyPassed = false;
            energies.push_back({ { "lambda_nm", lambdaNm }, { "gpu_relative_energy", relative }, { "exact_relative_energy", exact }, { "ratio", agreement },
                { "rank_ghost_paths", ranked } });
        }
        perGhost.push_back({ { "code", ghost.code }, { "energy", energies } });
    }
    auto positionJson = position.Json(), throughputJson = throughput.Json(), ratioJson = ratio.Json();
    const auto maxOf = [](const nlohmann::json& stats) { return stats.value("max", 0.0); };
    const bool parity = compared > 0 && statusMismatch * 1000u <= compared && maxOf(positionJson) < kPositionTolerance
        && maxOf(throughputJson) < kThroughputTolerance && maxOf(ratioJson) < kRatioTolerance;
    return {
        { "passed", parity && energyPassed }, { "parity_passed", parity }, { "energy_passed", energyPassed },
        { "lens", published.lensName }, { "field_deg", field / kDegrees }, { "sensor_z_mm", sensorZ }, { "grid", grid }, { "wavelengths", wavelengths },
        { "vertices_compared", compared }, { "ok_both", okBoth }, { "failed_both", failBoth }, { "status_mismatches", statusMismatch },
        { "sensor_position_error_mm", positionJson }, { "throughput_relative_error", throughputJson }, { "ratio_error", ratioJson },
        { "tolerances", { { "position_mm", kPositionTolerance }, { "throughput_relative", kThroughputTolerance }, { "ratio", kRatioTolerance },
            { "energy_relative", kEnergyTolerance } } },
        { "ghosts", perGhost },
    };
}

} // namespace

std::array<uint32_t, 4> GhostFlareManager::GraphSettings() {
    return { std::clamp(SettingU32("lensFlareGridResolution"), 4u, 256u), std::clamp(SettingU32("lensFlareMaxGhosts"), 1u, LENS_GHOST_MAX_GHOSTS),
        std::clamp(SettingU32("lensFlareWavelengths"), 1u, LENS_FLARE_MAX_WAVELENGTHS), std::min(SettingU32("lensFlareMaxLights"), LENS_FLARE_MAX_SOURCES - 1u) };
}

GhostFlareManager::GhostFlareManager(br::render::RendererStateRequestService* requests, std::shared_ptr<org::runtime::IUploadService> uploads,
    std::shared_ptr<LensManager> lens)
    : m_shared(std::make_shared<Shared>()), m_requests(requests), m_uploads(std::move(uploads)), m_lens(std::move(lens)) {
    m_shared->published.store(std::make_shared<GhostFlarePublished>());
    // No magic, so no ghosts however the GPU finds it.
    m_fallback = CreateIndexedStructuredBuffer(LENS_GHOST_HEADER_ROWS, sizeof(Row));
    m_fallback->SetName("Lens flare ghosts (none)");
    m_shared->fallback = m_fallback;
    m_sources = CreateIndexedStructuredBuffer(LENS_FLARE_SOURCES_ROWS, sizeof(Row), true);
    m_sources->SetName("Lens flare sources");
    if (m_requests) {
        m_family = std::make_shared<br::render::VersionedBufferFamily>(m_requests->VersionedBuffers(),
            br::render::VersionedBufferFamily::Config{
                { br::render::ArtifactKind::BufferVersion, kGhostBufferPrimaryId, 0 }, "LensFlare::Ghosts", sizeof(Row), false, false,
                br::render::PublishedFragmentKind::Views, br::render::PublishedResourceUsage::ShaderResource, kGhostBufferPrimaryId });
        m_shared->family = m_family;
    }
    m_resolver = std::make_shared<GhostDataResolver>(m_shared);

    m_desired.maxGhosts = GraphSettings()[1];
    // Observers run on whichever thread set the setting: they only record it and wake the worker.
    m_subscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>("lensFlareMaxGhosts", [this](const uint32_t& value) {
        {
            std::scoped_lock lock(m_shared->workMutex);
            m_desired.maxGhosts = std::clamp(value, 1u, LENS_GHOST_MAX_GHOSTS);
            ++m_desired.generation;
        }
        m_shared->wake.notify_one();
    }));
    m_worker = std::thread([this] { WorkerLoop(); });
}

GhostFlareManager::~GhostFlareManager() { Shutdown(); }

void GhostFlareManager::Shutdown() {
    m_subscriptions.clear();
    {
        std::scoped_lock lock(m_shared->workMutex);
        m_stop = true;
    }
    m_shared->wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
    m_awaiter.reset();
}

std::shared_ptr<const GhostFlarePublished> GhostFlareManager::Published() const { return m_shared->published.load(std::memory_order_acquire); }

std::shared_ptr<org::Buffer> GhostFlareManager::EnsureVertexBuffer() {
    const auto [grid, ghosts, wavelengths, lights] = GraphSettings();
    const size_t rows = size_t(1u + lights) * ghosts * wavelengths * LENS_FLARE_INSTANCE_ROWS(grid);
    if (!m_vertices || m_vertexRows != rows) {
        m_vertices = CreateIndexedStructuredBuffer(rows, sizeof(Row), true);
        m_vertices->SetName("Lens flare vertices");
        m_vertexRows = rows;
    }
    return m_vertices;
}

GhostFlareFrame GhostFlareManager::FrameConfig(uint64_t frameNumber) {
    std::scoped_lock lock(m_frameMutex);
    if (frameNumber == m_frameNumber) return m_frame;
    m_frameNumber = frameNumber;
    GhostFlareFrame frame;
    const auto lensState = m_lens ? m_lens->Published() : nullptr;
    const auto ghosts = Published();
    const auto [grid, maxGhosts, wavelengths, lights] = GraphSettings();
    frame.active = lensState && lensState->Active() && lensState->info && ghosts->buffer && ghosts->lensRevision == lensState->revision;
    frame.sourceSlots = 1u + lights;
    frame.ghostSlots = std::min<uint32_t>(static_cast<uint32_t>(ghosts->ghosts.size()), maxGhosts);
    frame.wavelengthSlots = frame.active && lensState->info->monochromatic ? 1u : wavelengths;
    frame.grid = grid;
    frame.intensity = SettingsManager::GetInstance().getSettingGetter<float>("lensFlareIntensity")();
    const uint64_t requested = m_shared->selfTestRequested.load(std::memory_order_acquire);
    frame.selfTest = requested != m_shared->selfTestCompleted.load(std::memory_order_acquire) ? requested : 0u;
    {
        std::scoped_lock testLock(m_shared->testMutex);
        frame.testSource = m_shared->testSource;
        frame.testDirection = m_shared->testDirection;
        frame.testDistance = m_shared->testDistance;
        frame.testIrradiance = m_shared->testIrradiance;
    }
    if (frame.ghostSlots == 0u) frame.active = false;
    m_frame = frame;
    return frame;
}

void GhostFlareManager::WorkerLoop() {
    for (;;) {
        Desired desired;
        bool build = false;
        std::shared_ptr<Shared::Capture> capture;
        {
            std::unique_lock lock(m_shared->workMutex);
            m_shared->wake.wait(lock, [this] { return m_stop || m_desired.generation != m_builtGeneration || m_shared->capture != nullptr; });
            if (m_stop) return;
            desired = m_desired;
            build = m_desired.generation != m_builtGeneration;
            m_builtGeneration = desired.generation;
            capture = std::move(m_shared->capture);
        }
        if (build) {
            try {
                Build(desired);
            } catch (const std::exception& exception) {
                spdlog::error("GhostFlareManager: building the ghosts failed: {}", exception.what());
                std::scoped_lock lock(m_shared->statusMutex);
                m_shared->lastError = exception.what();
            }
        }
        if (capture) {
            const auto started = std::chrono::steady_clock::now();
            nlohmann::json report;
            try {
                report = CompareSelfTest(*capture);
            } catch (const std::exception& exception) {
                report = { { "passed", false }, { "error", exception.what() } };
            }
            report["sequence"] = capture->sequence;
            report["state"] = "done";
            report["compare_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            spdlog::info("GhostFlareManager: self-test {}: {}", capture->sequence, report.dump());
            {
                std::scoped_lock lock(m_shared->reportMutex);
                m_shared->selfTestReport = std::move(report);
            }
            m_shared->selfTestCompleted.store(capture->sequence, std::memory_order_release);
            m_shared->selfTestReading.store(false, std::memory_order_release);
        }
    }
}

void GhostFlareManager::Build(const Desired& desired) {
    const auto started = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(m_shared->statusMutex);
        m_shared->lastError.clear();
    }
    const auto lensState = m_lens ? m_lens->Published() : nullptr;
    if (!lensState || !lensState->Active() || !lensState->info) {
        auto state = std::make_shared<GhostFlarePublished>();
        state->revision = ++m_revision;
        m_shared->Publish(std::move(state));
        return;
    }
    if (!m_family || !m_requests || !m_uploads) throw std::runtime_error("no state graph to publish the ghosts through");
    const LensInfo& info = *lensState->info;
    std::shared_ptr<const ::lens::LensPrescription> prescription = lensState->prescription;
    if (!prescription) {
        const ::lens::LoadResult loaded = ::lens::LoadPrescription(info.path);
        if (!loaded.lens) throw std::runtime_error(loaded.error);
        prescription = std::make_shared<const ::lens::LensPrescription>(*loaded.lens);
    }
    const double stopScale = info.stopScale;
    // The field the frame's diagonal is fitted to (the widest over the focus range), or the light's reach.
    double covered = 0.0;
    for (const auto& node : info.focusNodes) covered = std::max(covered, node.remapFieldRad);
    if (!(covered > 0.0)) covered = info.coveredFieldRad;
    if (!(covered > 0.0)) throw std::runtime_error("the lens has no covered field");
    const double maxField = std::min(kFieldMargin * covered, kMaxField);
    const double fieldStep = maxField / double(LENS_GHOST_FIELD_NODES - 1u);

    // Rank every two-reflection path as LensBake rank does: the mean over the F, d and C lines of the energy that lands in
    // the image circle relative to transmission, for parallel light at five angles across the covered field.
    const auto rankStarted = std::chrono::steady_clock::now();
    const std::vector<double> fields{ 0.0, 0.25 * covered, 0.5 * covered, 0.75 * covered, covered };
    const double lambdas[3] = { ::lens::kLambdaF, ::lens::kLambdaD, ::lens::kLambdaC };
    std::array<std::vector<::lens::RankedPath>, 3> ranked;
    {
        std::vector<std::thread> threads;
        for (int l = 0; l < 3; ++l) {
            threads.emplace_back([&, l] { ranked[l] = ::lens::RankGhostPaths(*prescription, fields, kRankGrid, lambdas[l], info.imageCircleMm); });
        }
        for (auto& thread : threads) thread.join();
    }
    std::map<std::string, double> mean;
    std::map<std::string, ::lens::LensPath> paths;
    for (const auto& list : ranked) {
        for (const auto& entry : list) {
            mean[entry.path.Code()] += entry.energy / 3.0;
            paths[entry.path.Code()] = entry.path;
        }
    }
    std::vector<std::pair<double, std::string>> sorted;
    for (const auto& [code, energy] : mean) sorted.push_back({ energy, code });
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const double rankMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rankStarted).count();

    // The kept ghosts' boxes over the field nodes, on half the hardware threads.
    const auto boxesStarted = std::chrono::steady_clock::now();
    std::vector<GhostFlarePath> ghosts;
    for (size_t i = 0; i < sorted.size() && ghosts.size() < desired.maxGhosts; ++i) {
        if (!(sorted[i].first > 0.0)) break;
        GhostFlarePath ghost;
        ghost.code = sorted[i].second;
        ghost.energy = sorted[i].first;
        uint32_t reflections[2] = { 0, 0 };
        uint32_t found = 0;
        for (const auto& step : paths[ghost.code].steps) {
            if (step.event == ::lens::Event::Reflect && found < 2) reflections[found++] = step.surface;
        }
        if (found != 2) continue;
        ghost.a = reflections[0];
        ghost.b = reflections[1];
        ghost.boxes.resize(LENS_GHOST_FIELD_NODES);
        ghosts.push_back(std::move(ghost));
    }
    {
        const uint32_t jobs = static_cast<uint32_t>(ghosts.size()) * LENS_GHOST_FIELD_NODES;
        std::atomic<uint32_t> next{ 0 };
        const auto work = [&] {
            for (uint32_t job = next.fetch_add(1); job < jobs; job = next.fetch_add(1)) {
                auto& ghost = ghosts[job / LENS_GHOST_FIELD_NODES];
                const uint32_t node = job % LENS_GHOST_FIELD_NODES;
                ghost.boxes[node] = GhostBox(*prescription, ::lens::GhostPath(*prescription, ghost.a, ghost.b), node * fieldStep, stopScale);
            }
        };
        std::vector<std::thread> pool;
        const uint32_t threads = std::max(1u, std::thread::hardware_concurrency() / 2u);
        for (uint32_t t = 1; t < threads; ++t) pool.emplace_back(work);
        work();
        for (auto& thread : pool) thread.join();
    }
    std::erase_if(ghosts, [](const GhostFlarePath& ghost) {
        return std::all_of(ghost.boxes.begin(), ghost.boxes.end(), [](const auto& box) { return box[0] > box[1]; });
    });
    if (ghosts.empty()) throw std::runtime_error("no ghost of this lens reaches the sensor");
    const double boxesMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - boxesStarted).count();

    // Pack (lensGhostLayout.h).
    const uint32_t ghostCount = static_cast<uint32_t>(ghosts.size());
    std::vector<Row> rows(LENS_GHOST_HEADER_ROWS + ghostCount * LENS_GHOST_ROWS_PER_GHOST(LENS_GHOST_FIELD_NODES), Row{});
    rows[0] = UintRow(LENS_GHOST_MAGIC, ghostCount, LENS_GHOST_FIELD_NODES, static_cast<uint32_t>(lensState->revision));
    rows[1] = { float(maxField), float(fieldStep), 0.0f, 0.0f };
    for (uint32_t g = 0; g < ghostCount; ++g) {
        Row* row = &rows[LENS_GHOST_HEADER_ROWS + g * LENS_GHOST_ROWS_PER_GHOST(LENS_GHOST_FIELD_NODES)];
        row[0] = UintRow(ghosts[g].a, ghosts[g].b, 0u, 0u);
        for (uint32_t node = 0; node < LENS_GHOST_FIELD_NODES; ++node) {
            const auto& box = ghosts[g].boxes[node];
            row[1u + node] = { box[0], box[1], box[2], 0.0f };
        }
    }

    auto pending = std::make_shared<GhostFlarePublished>();
    pending->revision = ++m_revision;
    pending->lensRevision = lensState->revision;
    pending->prescription = prescription;
    pending->ghosts = std::move(ghosts);
    pending->pathsRanked = static_cast<uint32_t>(sorted.size());
    pending->stopScale = stopScale;
    pending->maxFieldRad = maxField;
    pending->fieldStepRad = fieldStep;
    pending->rankMs = rankMs;
    pending->boxesMs = boxesMs;
    pending->lensName = info.name;
    pending->buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    const auto bytes = std::as_bytes(std::span(rows));
    const uint64_t revision = pending->revision;
    const auto request = m_family->RequestSnapshot(*m_requests, m_uploads, revision, bytes, rows.size());
    if (!request) throw std::runtime_error("the state graph refused the ghost buffer (status " + std::to_string(static_cast<int>(request.status)) + ")");
    {
        std::scoped_lock lock(m_shared->statusMutex);
        m_shared->requestedRevision = revision;
    }
    std::string codes;
    for (const auto& ghost : pending->ghosts) codes += (codes.empty() ? "" : " ") + ghost.code;
    spdlog::info("GhostFlareManager: '{}' (lens revision {}): {} of {} ghosts kept ({}), max field {:.1f} deg; ranked in {:.0f} ms, boxes {:.0f} ms, "
        "{:.0f} ms in all; revision {} posted", info.name, lensState->revision, pending->ghosts.size(), pending->pathsRanked, codes, maxField / kDegrees,
        rankMs, boxesMs, pending->buildMs, revision);

    const std::weak_ptr<Shared> weak = m_shared;
    auto awaiter = std::make_shared<br::render::ArtifactAwaiter>(m_requests->AwaitExact(request.Handle(), br::render::ArtifactReadiness::GpuReady,
        TaskLane::Background, TaskDomain::GraphControl,
        [weak, pending](const br::render::ArtifactSnapshot& snapshot) {
            const auto shared = weak.lock();
            if (!shared) return;
            const auto fragment = snapshot.payload.Get<br::render::RendererStateFragmentArtifact>();
            const auto version = fragment ? fragment->fragment.payload.Get<br::render::PublishedGpuBufferVersion>() : nullptr;
            if (!version || !version->resource) {
                spdlog::error("GhostFlareManager: ghost buffer revision {} is ready without a buffer", pending->revision);
                return;
            }
            auto state = std::make_shared<GhostFlarePublished>(*pending);
            state->version = version;
            state->buffer = version->resource;
            if (shared->family) shared->family->Acknowledge(version);
            shared->Publish(std::move(state));
            spdlog::info("GhostFlareManager: ghost buffer revision {} published", pending->revision);
        },
        [weak, revision](const br::render::ArtifactTermination& termination) {
            spdlog::error("GhostFlareManager: ghost buffer revision {} ended without publishing: {}", revision, termination.error);
            if (const auto shared = weak.lock()) {
                std::scoped_lock lock(shared->statusMutex);
                shared->lastError = termination.error;
            }
        }));
    // The previous wait goes here, on the worker: releasing it enters the graph's mutex.
    m_awaiter = std::move(awaiter);
}

namespace {

// lens.flare.sources: the sources buffer as the last frame wrote it (lensGhostLayout.h).
nlohmann::json DescribeSources(const std::vector<float>& words) {
    if (words.size() < size_t(LENS_FLARE_SOURCES_ROWS) * 4u) return { { "error", "readback too small" } };
    const auto row = [&](uint32_t index) { return &words[size_t(index) * 4u]; };
    const float* frame0 = row(LENS_FLARE_ROW_FRAME0);
    const float* frame1 = row(LENS_FLARE_ROW_FRAME1);
    const float* frame2 = row(LENS_FLARE_ROW_FRAME2);
    const float* frame3 = row(LENS_FLARE_ROW_FRAME3);
    const uint32_t count = AsUint(frame1[0]), wavelengths = AsUint(frame1[1]);
    nlohmann::json report = {
        { "sensor_half_extent_mm", { frame0[0], frame0[1] } }, { "sensor_z_mm", frame0[2] }, { "inverse_k", frame0[3] },
        { "source_slots", count }, { "wavelengths", wavelengths }, { "grid", AsUint(frame1[2]) }, { "flags", AsUint(frame1[3]) },
        { "field_scale", frame2[0] }, { "intensity", frame2[1] }, { "min_cell_area_mm2", frame2[2] },
        { "lens_revision", AsUint(frame3[1]) }, { "ghost_lens_revision", AsUint(frame3[2]) }, { "ghost_slots", AsUint(frame3[3]) },
    };
    nlohmann::json lambdas = nlohmann::json::array();
    for (uint32_t w = 0; w < wavelengths && w < LENS_FLARE_MAX_WAVELENGTHS; ++w) {
        const float* r = row(LENS_FLARE_ROW_WAVELENGTHS + w);
        lambdas.push_back({ { "lambda_nm", r[3] }, { "rgb", { r[0], r[1], r[2] } } });
    }
    report["wavelength_weights"] = lambdas;
    nlohmann::json sources = nlohmann::json::array();
    for (uint32_t slot = 0; slot < count && slot < LENS_FLARE_MAX_SOURCES; ++slot) {
        const float* r0 = row(LENS_FLARE_ROW_SOURCES + slot * LENS_FLARE_SOURCE_ROWS);
        const float* r1 = r0 + 4;
        const float* r2 = r0 + 8;
        sources.push_back({ { "valid", AsUint(r2[0]) != 0u }, { "point", r0[3] == LENS_FLARE_SOURCE_POINT }, { "lens_vector", { r0[0], r0[1], r0[2] } },
            { "irradiance", { r1[0], r1[1], r1[2] } }, { "visibility", r1[3] }, { "field_deg", r2[3] / kDegrees } });
    }
    report["sources"] = sources;
    return report;
}

} // namespace

void GhostFlareManager::OnFrame(org::RenderGraph* graph) {
    // A new lens revision: rebuild the ghosts for it. One atomic load a frame.
    if (m_lens) {
        const uint64_t lensRevision = m_lens->Published()->revision;
        if (lensRevision != m_seenLensRevision) {
            m_seenLensRevision = lensRevision;
            {
                std::scoped_lock lock(m_shared->workMutex);
                m_desired.lensRevision = lensRevision;
                ++m_desired.generation;
            }
            m_shared->wake.notify_one();
        }
    }

    if (graph && !m_shared->sourcesReading.load(std::memory_order_acquire) && m_shared->sourcesRequested.exchange(false)) {
        if (auto* readbacks = graph->GetReadbackService()) {
            m_shared->sourcesReading.store(true, std::memory_order_release);
            const std::weak_ptr<Shared> weakShared = m_shared;
            readbacks->RequestReadbackCaptureAfterGraph(m_sources.get(), org::RangeSpec{}, [weakShared](org::ReadbackCaptureResult&& result) {
                const auto shared = weakShared.lock();
                if (!shared) return;
                std::vector<float> words(result.data.size() / sizeof(float));
                std::memcpy(words.data(), result.data.data(), words.size() * sizeof(float));
                auto report = DescribeSources(words);
                report["state"] = "done";
                {
                    std::scoped_lock lock(shared->reportMutex);
                    shared->sourcesReport = std::move(report);
                }
                shared->sourcesReading.store(false, std::memory_order_release);
            });
        }
    }

    const uint64_t requested = m_shared->selfTestRequested.load(std::memory_order_acquire);
    const uint64_t sequence = requested != m_shared->selfTestCompleted.load(std::memory_order_acquire) ? requested : 0u;
    if (!sequence || !graph || !m_vertices || m_shared->selfTestReading.load(std::memory_order_acquire)) return;
    auto* readbacks = graph->GetReadbackService();
    if (!readbacks) return;
    m_shared->selfTestReading.store(true, std::memory_order_release);
    auto capture = std::make_shared<Shared::Capture>();
    capture->sequence = sequence;
    capture->ghosts = Published();
    const std::weak_ptr<Shared> weak = m_shared;
    const auto arrive = [weak, capture](bool sources, org::ReadbackCaptureResult&& result) {
        const auto shared = weak.lock();
        if (!shared) return;
        std::vector<float> words(result.data.size() / sizeof(float));
        std::memcpy(words.data(), result.data.data(), words.size() * sizeof(float));
        std::scoped_lock lock(shared->workMutex);
        (sources ? capture->sources : capture->vertices) = std::move(words);
        if (++capture->arrived < 2) return;
        // The sources pass stamps the sequence it ran for: until it matches, read again on a later frame.
        const size_t stamp = size_t(LENS_FLARE_ROW_FRAME3) * 4u;
        if (capture->sources.size() <= stamp || AsUint(capture->sources[stamp]) != static_cast<uint32_t>(capture->sequence)) {
            shared->selfTestReading.store(false, std::memory_order_release);
            return;
        }
        shared->capture = capture;
        shared->wake.notify_one();
    };
    readbacks->RequestReadbackCaptureAfterGraph(m_sources.get(), org::RangeSpec{},
        [arrive](org::ReadbackCaptureResult&& result) { arrive(true, std::move(result)); });
    readbacks->RequestReadbackCaptureAfterGraph(m_vertices.get(), org::RangeSpec{},
        [arrive](org::ReadbackCaptureResult&& result) { arrive(false, std::move(result)); });
}

std::string GhostFlareManager::HandleControl(const std::string& command, const std::string& requestJson) {
    const nlohmann::json request = requestJson.empty() ? nlohmann::json::object() : nlohmann::json::parse(requestJson);
    auto& settings = SettingsManager::GetInstance();
    const auto setCount = [&](const char* field, const char* setting, int64_t lowest, int64_t highest) {
        if (!request.contains(field)) return;
        const auto value = request.at(field).get<int64_t>();
        if (value < lowest || value > highest) {
            throw std::runtime_error(std::string(field) + " must be " + std::to_string(lowest) + "-" + std::to_string(highest));
        }
        settings.getSettingSetter<uint32_t>(setting)(static_cast<uint32_t>(value));
    };
    if (command == "lens.flare.set") {
        if (request.contains("enabled")) settings.getSettingSetter<bool>("lensFlareEnabled")(request.at("enabled").get<bool>());
        setCount("max_ghosts", "lensFlareMaxGhosts", 1, LENS_GHOST_MAX_GHOSTS);
        setCount("grid", "lensFlareGridResolution", 4, 256);
        setCount("wavelengths", "lensFlareWavelengths", 1, LENS_FLARE_MAX_WAVELENGTHS);
        setCount("max_lights", "lensFlareMaxLights", 0, LENS_FLARE_MAX_SOURCES - 1u);
        if (request.contains("intensity")) {
            const float value = request.at("intensity").get<float>();
            if (!(value >= 0.0f)) throw std::runtime_error("intensity must be non-negative");
            settings.getSettingSetter<float>("lensFlareIntensity")(value);
        }
        if (request.contains("test_source")) {
            const auto& test = request.at("test_source");
            std::scoped_lock lock(m_shared->testMutex);
            if (test.is_null()) {
                m_shared->testSource = false;
            } else {
                const auto direction = test.at("direction").get<std::array<float, 3>>();
                if (!(std::hypot(direction[0], direction[1], direction[2]) > 0.0f)) throw std::runtime_error("test_source direction must be non-zero");
                m_shared->testDirection = direction;
                m_shared->testDistance = test.value("distance_m", 0.0f);
                m_shared->testIrradiance = test.at("irradiance").get<std::array<float, 3>>();
                m_shared->testSource = true;
            }
        }
    } else if (command == "lens.flare.selftest") {
        if (!Published()->buffer) throw std::runtime_error("no ghosts are published (is a lens on?)");
        if (!settings.getSettingGetter<bool>("lensFlareEnabled")()) throw std::runtime_error("lensFlareEnabled is off: the flare passes aren't in the graph");
        if (m_shared->selfTestRequested.load() == m_shared->selfTestCompleted.load()) {
            m_shared->selfTestRequested.fetch_add(1, std::memory_order_acq_rel);
            std::scoped_lock lock(m_shared->reportMutex);
            m_shared->selfTestReport = { { "state", "pending" }, { "sequence", m_shared->selfTestRequested.load() } };
        }
        std::scoped_lock lock(m_shared->reportMutex);
        return m_shared->selfTestReport.dump();
    } else if (command == "lens.flare.sources") {
        // The sources buffer after the next frame: the first call answers "pending" (or the previous reading); call again.
        m_shared->sourcesRequested.store(true, std::memory_order_release);
        std::scoped_lock lock(m_shared->reportMutex);
        nlohmann::json reply = m_shared->sourcesReport;
        if (reply.empty()) reply = { { "state", "pending" } };
        return reply.dump();
    } else if (command == "lens.flare.selftest.status") {
        std::scoped_lock lock(m_shared->reportMutex);
        return m_shared->selfTestReport.dump();
    } else if (command != "lens.flare.get") {
        throw std::runtime_error("unknown lens flare command '" + command + "'");
    }

    nlohmann::json reply;
    const auto [grid, maxGhosts, wavelengths, lights] = GraphSettings();
    reply["enabled"] = settings.getSettingGetter<bool>("lensFlareEnabled")();
    reply["max_ghosts"] = maxGhosts;
    reply["grid"] = grid;
    reply["wavelengths"] = wavelengths;
    reply["max_lights"] = lights;
    reply["intensity"] = settings.getSettingGetter<float>("lensFlareIntensity")();
    {
        std::scoped_lock lock(m_shared->testMutex);
        reply["test_source"] = m_shared->testSource
            ? nlohmann::json{ { "direction", m_shared->testDirection }, { "distance_m", m_shared->testDistance }, { "irradiance", m_shared->testIrradiance } }
            : nlohmann::json(nullptr);
    }
    const auto published = Published();
    const auto lensState = m_lens ? m_lens->Published() : nullptr;
    std::string error;
    uint64_t requestedRevision = 0;
    {
        std::scoped_lock lock(m_shared->statusMutex);
        error = m_shared->lastError;
        requestedRevision = m_shared->requestedRevision;
    }
    const bool lensOn = lensState && lensState->Active();
    const bool current = published->buffer && lensState && published->lensRevision == lensState->revision;
    reply["state"] = !error.empty() ? "error" : !lensOn ? "no lens" : current ? "ready" : "building";
    reply["error"] = error;
    reply["published_revision"] = published->revision;
    reply["requested_revision"] = requestedRevision;
    reply["lens_revision"] = published->lensRevision;
    if (published->buffer) {
        nlohmann::json ghosts = nlohmann::json::array();
        for (const auto& ghost : published->ghosts) ghosts.push_back({ { "code", ghost.code }, { "a", ghost.a }, { "b", ghost.b }, { "energy", ghost.energy } });
        reply["ghosts"] = ghosts;
        reply["lens"] = published->lensName;
        reply["paths_ranked"] = published->pathsRanked;
        reply["stop_scale"] = published->stopScale;
        reply["max_field_deg"] = published->maxFieldRad / kDegrees;
        reply["rank_ms"] = published->rankMs;
        reply["boxes_ms"] = published->boxesMs;
        reply["build_ms"] = published->buildMs;
    }
    return reply.dump();
}

std::shared_ptr<org::Resource> GhostFlareManager::ProvideResource(org::ResourceIdentifier const& key) {
    return key == org::ResourceIdentifier{ kLensFlareSourcesName } ? std::static_pointer_cast<org::Resource>(m_sources) : nullptr;
}

std::vector<org::ResourceIdentifier> GhostFlareManager::GetSupportedKeys() { return { org::ResourceIdentifier{ kLensFlareSourcesName } }; }

std::shared_ptr<org::IResourceResolver> GhostFlareManager::ProvideResolver(org::ResourceIdentifier const& key) {
    return key == org::ResourceIdentifier{ kLensFlareGhostsName } ? m_resolver : nullptr;
}

std::vector<org::ResourceIdentifier> GhostFlareManager::GetSupportedResolverKeys() { return { org::ResourceIdentifier{ kLensFlareGhostsName } }; }

} // namespace br::lens
