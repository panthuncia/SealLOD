#include "Render/PublishedRendererState.h"

#include <chrono>

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
    if (m_candidate.state) ++m_stats.replacedCandidates;
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

std::shared_ptr<const PublishedRendererState> RendererStatePublisher::Commit(std::size_t frameSlot) {
    const auto started = std::chrono::steady_clock::now();
    std::function<void(std::uint64_t)> rejected;
    std::uint64_t rejectedEpoch = 0;
    std::shared_ptr<const PublishedRendererState> retiredFrameState;
    std::shared_ptr<const PublishedRendererState> retiredActiveState;
    std::shared_ptr<const PublishedRendererState> retiredCandidateState;
    std::unique_lock lock(m_mutex);
    if (frameSlot >= m_frameStates.size()) m_frameStates.resize(frameSlot + 1u);
    retiredFrameState = std::move(m_frameStates[frameSlot]);
    if (m_candidate.state) {
        const auto activeEpoch = m_active ? m_active->epoch : 0u;
        if (m_candidate.baseEpoch == activeEpoch) {
            retiredActiveState = std::move(m_active);
            m_active = std::move(m_candidate.state);
            ++m_stats.committed;
        } else {
            ++m_stats.rejectedBaseEpoch;
            rejected = m_candidateRejected;
            rejectedEpoch = activeEpoch;
            retiredCandidateState = std::move(m_candidate.state);
        }
        m_candidate.baseEpoch = 0;
    }
    m_frameStates[frameSlot] = m_active;
    m_stats.retainedFrameStates = 0;
    for (const auto& state : m_frameStates) if (state) ++m_stats.retainedFrameStates;
    m_stats.commitMicros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
    auto result = m_active;
    lock.unlock();
    try {
        m_source->Store(result);
    } catch (...) {
        std::throw_with_nested(std::runtime_error("PublishedStateSource::Store failed"));
    }
    if (rejected) {
        try {
            rejected(rejectedEpoch);
        } catch (...) {
            std::throw_with_nested(std::runtime_error("candidate rejection callback failed"));
        }
    }
    try {
        retiredCandidateState.reset();
        retiredActiveState.reset();
        retiredFrameState.reset();
    } catch (...) {
        std::throw_with_nested(std::runtime_error("published state retirement failed"));
    }
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
