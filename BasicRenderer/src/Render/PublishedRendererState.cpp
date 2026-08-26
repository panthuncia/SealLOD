#include "Render/PublishedRendererState.h"

#include <chrono>

#include <spdlog/spdlog.h>
#include <BasicTelemetry/Telemetry.h>

namespace br::render {
namespace {
std::mutex g_processSourceMutex;
std::weak_ptr<PublishedStateSource> g_processSource;
}

std::size_t PublishedResourceKey::Hasher::operator()(const PublishedResourceKey& key) const noexcept {
    std::size_t value = static_cast<std::size_t>(key.usage);
    const auto mix = [&value](std::uint64_t part) {
        value ^= std::hash<std::uint64_t>{}(part) + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    };
    mix(static_cast<std::uint64_t>(key.owner));
    mix(key.renderPhaseHash); mix(key.viewOrWorkloadID); mix(key.variant);
    return value;
}

std::shared_ptr<const PublishedResourceCatalog::ResourceList> PublishedResourceCatalog::Find(
    const PublishedResourceKey& key) const {
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : found->second;
}

std::uint64_t PublishedResourceCatalog::ContentVersion(
    const PublishedResourceKey& key) const noexcept {
    const auto found = contentVersions.find(key);
    return found == contentVersions.end() ? 0u : found->second;
}

std::uint64_t PublishedResourceCatalog::ContentVersion(
    const PublishedResourceQuery& query) const noexcept {
    std::uint64_t version = 0;
    bool matched = false;
    for (const auto& [key, entryVersion] : contentVersions) {
        if (!query.Matches(key)) continue;
        matched = true;
        const auto keyHash = static_cast<std::uint64_t>(PublishedResourceKey::Hasher{}(key));
        auto entryHash = keyHash ^ (entryVersion + 0x9e3779b97f4a7c15ull +
            (keyHash << 6u) + (keyHash >> 2u));
        entryHash ^= entryHash >> 30u;
        entryHash *= 0xbf58476d1ce4e5b9ull;
        entryHash ^= entryHash >> 27u;
        entryHash *= 0x94d049bb133111ebull;
        version ^= entryHash ^ (entryHash >> 31u);
    }
    return matched ? version : 0u;
}

bool PublishedResourceQuery::Matches(const PublishedResourceKey& key) const noexcept {
    return (!owner || key.owner == *owner) && (!usage || key.usage == *usage) &&
        (!renderPhaseHash || key.renderPhaseHash == *renderPhaseHash) &&
        (!viewOrWorkloadID || key.viewOrWorkloadID == *viewOrWorkloadID) &&
        (key.variant & requiredVariantMask) == requiredVariantMask &&
        (key.variant & forbiddenVariantMask) == 0u;
}

PublishedResourceCatalog::ResourceList PublishedResourceCatalog::FindAll(
    const PublishedResourceQuery& query) const {
    ResourceList result;
    for (const auto& [key, resources] : entries) {
        if (!query.Matches(key) || !resources) continue;
        result.insert(result.end(), resources->begin(), resources->end());
    }
    return result;
}

void PublishedStateSource::SetProcessSource(std::shared_ptr<PublishedStateSource> source) noexcept {
    std::lock_guard lock(g_processSourceMutex);
    g_processSource = std::move(source);
}
std::shared_ptr<PublishedStateSource> PublishedStateSource::ProcessSource() noexcept {
    std::lock_guard lock(g_processSourceMutex);
    return g_processSource.lock();
}

void PublishedStateSource::Store(std::shared_ptr<const PublishedRendererState> state) noexcept {
    m_state.store(std::move(state), std::memory_order_release);
}
std::shared_ptr<const PublishedRendererState> PublishedStateSource::Load() const noexcept {
    return m_state.load(std::memory_order_acquire);
}
std::uint64_t PublishedStateSource::Epoch() const noexcept {
    const auto state = Load();
    return state ? state->epoch : 0u;
}

PublishedStateFragment& PublishedRendererState::Fragment(PublishedFragmentKind kind) {
    switch (kind) {
    case PublishedFragmentKind::Materials: return materials;
    case PublishedFragmentKind::Terrain: return terrain;
    case PublishedFragmentKind::Geometry: return geometry;
    case PublishedFragmentKind::DrawRecords: return drawRecords;
    case PublishedFragmentKind::ActiveDrawLists: return activeDrawLists;
    case PublishedFragmentKind::IndirectWorkloads: return indirectWorkloads;
    case PublishedFragmentKind::Count: break;
    }
    throw std::out_of_range("published renderer fragment kind");
}

const PublishedStateFragment& PublishedRendererState::Fragment(PublishedFragmentKind kind) const {
    return const_cast<PublishedRendererState*>(this)->Fragment(kind);
}

RendererStatePublisher::RendererStatePublisher(std::size_t framesInFlight) {
    auto fallback = std::make_shared<PublishedRendererState>();
    Bootstrap(std::move(fallback), framesInFlight);
}

void RendererStatePublisher::Bootstrap(std::shared_ptr<const PublishedRendererState> fallback,
    std::size_t framesInFlight) {
    std::lock_guard lock(m_mutex);
    m_candidate = {};
    m_active = fallback ? std::move(fallback) : std::make_shared<PublishedRendererState>();
    m_source->Store(m_active);
    m_frameStates.assign(framesInFlight, {});
    m_stats = {};
}

bool RendererStatePublisher::PublishCandidate(RendererStateCandidate candidate) {
    if (!candidate.state || candidate.state->epoch <= candidate.baseEpoch) return false;
    std::unique_lock lock(m_mutex);
    ++m_stats.candidates;
    if (m_candidate.state) {
        ++m_stats.replacedCandidates;
        basic_telemetry::AddCounter("SARP.RendererStatePublisher.CandidateReplacements");
    }
    auto retired = std::move(m_candidate.state);
    m_candidate = std::move(candidate);
    lock.unlock();
    retired.reset();
    return true;
}

bool RendererStatePublisher::PublishArtifact(const ArtifactSnapshot& artifact) {
    if (!artifact.payload.Valid() ||
        (artifact.readiness != ArtifactReadiness::GpuReady && artifact.readiness != ArtifactReadiness::Published)) {
        return false;
    }
    if (artifact.key.kind != ArtifactKind::FrameManifest) return false;
    const auto manifest = artifact.payload.Get<FrameManifestPayload>();
    if (!manifest || !manifest->state) return false;
    const auto baseEpoch = manifest->baseEpoch;
    auto state = std::make_shared<PublishedRendererState>(*manifest->state);
    state->epoch = baseEpoch + 1u;
    std::unique_lock lock(m_mutex);
    ++m_stats.candidates;
    if (m_candidate.state) ++m_stats.replacedCandidates;
    auto retired = std::move(m_candidate.state);
    m_candidate = RendererStateCandidate{ baseEpoch, std::move(state) };
    lock.unlock();
    retired.reset();
    return true;
}

void RendererStateCommitResult::RunDeferred() noexcept {
    if (rejectedCallback) {
        try { rejectedCallback(rejectedEpoch); }
        catch (const std::exception& exception) {
            spdlog::error("Renderer-state candidate rejection callback failed: {}", exception.what());
        } catch (...) {
            spdlog::error("Renderer-state candidate rejection callback failed");
        }
        rejectedCallback = {};
    }
    for (std::uint8_t index = 0; index < retiredStateCount; ++index) retiredStates[index].reset();
    retiredStateCount = 0;
}

RendererStateCommitResult RendererStatePublisher::Commit(std::size_t frameSlot) {
    const auto started = std::chrono::steady_clock::now();
    RendererStateCommitResult result;
    std::lock_guard lock(m_mutex);
    if (frameSlot >= m_frameStates.size()) {
        result.state = m_active;
        return result;
    }
    if (m_frameStates[frameSlot]) {
        result.retiredStates[result.retiredStateCount++] = std::move(m_frameStates[frameSlot]);
        if (m_stats.retainedFrameStates) --m_stats.retainedFrameStates;
    }
    if (m_candidate.state) {
        const auto activeEpoch = m_active ? m_active->epoch : 0u;
        if (m_candidate.baseEpoch == activeEpoch) {
            if (m_active) result.retiredStates[result.retiredStateCount++] = std::move(m_active);
            m_active = std::move(m_candidate.state);
            result.committed = true;
            ++m_stats.committed;
        } else {
            ++m_stats.rejectedBaseEpoch;
            basic_telemetry::AddCounter("SARP.RendererStatePublisher.CandidateRejections");
            result.rejectedCallback = m_candidateRejected;
            result.rejectedEpoch = activeEpoch;
            result.retiredStates[result.retiredStateCount++] = std::move(m_candidate.state);
        }
        m_candidate.baseEpoch = 0;
    }
    m_frameStates[frameSlot] = m_active;
    if (m_active) ++m_stats.retainedFrameStates;
    m_stats.commitMicros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
    basic_telemetry::Record("SARP.RendererStatePublisher.CommitDurationNs",
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count()));
    basic_telemetry::SetGauge("SARP.RendererStatePublisher.RetainedFrameStates",
        static_cast<std::int64_t>(m_stats.retainedFrameStates));
    result.state = m_active;
    m_source->Store(result.state);
    return result;
}

void RendererStatePublisher::SetCandidateRejectedCallback(std::function<void(std::uint64_t)> callback) {
    std::lock_guard lock(m_mutex);
    m_candidateRejected = std::move(callback);
}

void RendererStatePublisher::ReleaseFrameSlot(std::size_t frameSlot) {
    std::lock_guard lock(m_mutex);
    if (frameSlot < m_frameStates.size()) m_frameStates[frameSlot].reset();
}

void RendererStatePublisher::DiscardCandidate() {
    std::unique_lock lock(m_mutex);
    auto retired = std::move(m_candidate.state);
    m_candidate.baseEpoch = 0;
    lock.unlock();
    retired.reset();
}

std::shared_ptr<const PublishedRendererState> RendererStatePublisher::Active() const {
    std::lock_guard lock(m_mutex);
    return m_active;
}

std::uint64_t RendererStatePublisher::ActiveEpoch() const {
    std::lock_guard lock(m_mutex);
    return m_active ? m_active->epoch : 0u;
}

RendererStatePublisherStats RendererStatePublisher::Stats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

} // namespace br::render
