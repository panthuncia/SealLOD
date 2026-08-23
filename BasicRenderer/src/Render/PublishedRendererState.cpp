#include "Render/PublishedRendererState.h"

#include <chrono>

namespace br::render {

RendererStatePublisher::RendererStatePublisher(std::size_t framesInFlight) {
    auto fallback = std::make_shared<PublishedRendererState>();
    Bootstrap(std::move(fallback), framesInFlight);
}

void RendererStatePublisher::Bootstrap(std::shared_ptr<const PublishedRendererState> fallback,
    std::size_t framesInFlight) {
    std::lock_guard lock(m_mutex);
    m_candidate = {};
    m_active = fallback ? std::move(fallback) : std::make_shared<PublishedRendererState>();
    m_frameStates.assign(framesInFlight, {});
    m_stats = {};
}

bool RendererStatePublisher::PublishCandidate(RendererStateCandidate candidate) {
    if (!candidate.state || candidate.state->epoch <= candidate.baseEpoch) return false;
    std::lock_guard lock(m_mutex);
    ++m_stats.candidates;
    if (m_candidate.state) ++m_stats.replacedCandidates;
    m_candidate = std::move(candidate);
    return true;
}

bool RendererStatePublisher::PublishArtifact(const ArtifactSnapshot& artifact) {
    if (!artifact.payload.Valid() ||
        (artifact.readiness != ArtifactReadiness::GpuReady && artifact.readiness != ArtifactReadiness::Published)) {
        return false;
    }
    std::lock_guard lock(m_mutex);
    const auto baseEpoch = m_active ? m_active->epoch : 0u;
    const auto source = m_candidate.state && m_candidate.baseEpoch == baseEpoch
        ? m_candidate.state
        : m_active;
    auto state = source
        ? std::make_shared<PublishedRendererState>(*source)
        : std::make_shared<PublishedRendererState>();
    state->epoch = baseEpoch + 1u;
    PublishedStateFragment fragment{ artifact.revision, artifact.payload, { artifact } };
    switch (artifact.key.kind) {
    case ArtifactKind::TextureBinding:
    case ArtifactKind::Material:
    case ArtifactKind::MaterialTable:
        state->materials = std::move(fragment);
        break;
    case ArtifactKind::Mesh:
    case ArtifactKind::MeshTable:
    case ArtifactKind::BufferVersion:
        state->geometry = std::move(fragment);
        break;
    case ArtifactKind::DrawRecordPage:
    case ArtifactKind::StaticTransaction:
        state->drawRecords = std::move(fragment);
        break;
    case ArtifactKind::ActiveDrawList:
        state->activeDrawLists = std::move(fragment);
        break;
    case ArtifactKind::IndirectWorkload:
        state->indirectWorkloads = std::move(fragment);
        break;
    case ArtifactKind::FrameManifest:
        if (const auto complete = artifact.payload.Get<PublishedRendererState>()) state =
            std::make_shared<PublishedRendererState>(*complete);
        state->epoch = baseEpoch + 1u;
        break;
    default:
        return false;
    }
    ++m_stats.candidates;
    if (m_candidate.state) ++m_stats.replacedCandidates;
    m_candidate = RendererStateCandidate{ baseEpoch, std::move(state) };
    return true;
}

std::shared_ptr<const PublishedRendererState> RendererStatePublisher::Commit(std::size_t frameSlot) {
    const auto started = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);
    if (frameSlot >= m_frameStates.size()) m_frameStates.resize(frameSlot + 1u);
    m_frameStates[frameSlot].reset();
    if (m_candidate.state) {
        const auto activeEpoch = m_active ? m_active->epoch : 0u;
        if (m_candidate.baseEpoch == activeEpoch) {
            m_active = std::move(m_candidate.state);
            ++m_stats.committed;
        } else {
            ++m_stats.rejectedBaseEpoch;
        }
        m_candidate = {};
    }
    m_frameStates[frameSlot] = m_active;
    m_stats.retainedFrameStates = 0;
    for (const auto& state : m_frameStates) if (state) ++m_stats.retainedFrameStates;
    m_stats.commitMicros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
    return m_active;
}

void RendererStatePublisher::ReleaseFrameSlot(std::size_t frameSlot) {
    std::lock_guard lock(m_mutex);
    if (frameSlot < m_frameStates.size()) m_frameStates[frameSlot].reset();
}

void RendererStatePublisher::DiscardCandidate() {
    std::lock_guard lock(m_mutex);
    m_candidate = {};
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
