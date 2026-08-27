#include "Render/PublishedRendererState.h"

#include <chrono>

#include <spdlog/spdlog.h>
#include <BasicTelemetry/Telemetry.h>

namespace br::render {

void ArtifactLeaseSet::Add(const ArtifactLease& lease) {
    if (!lease) return;
    const auto identity = lease.Token().get();
    if (std::ranges::any_of(m_leases, [identity](const ArtifactLease& value) {
        return value.Token().get() == identity;
    })) return;
    m_leases.push_back(lease);
}

void ArtifactLeaseSet::Merge(const ArtifactLeaseSet& other) {
    for (const auto& lease : other.m_leases) Add(lease);
}
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

const PublishedResourceSelection* PublishedResourceCatalog::FindSelection(
    const PublishedResourceKey& key) const noexcept {
    const auto found = selections.find(key);
    return found == selections.end() ? nullptr : &found->second;
}

std::vector<const PublishedResourceSelection*> PublishedResourceCatalog::FindSelections(
    const PublishedResourceQuery& query) const {
    std::vector<const PublishedResourceSelection*> result;
    result.reserve(selections.size());
    for (const auto& [key, selection] : selections) {
        if (query.Matches(key)) result.push_back(&selection);
    }
    return result;
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

std::shared_ptr<const PublishedManifestLease> PublishedStateSource::AcquireLease(
    std::size_t frameSlot, std::shared_ptr<const PublishedRendererState> state) noexcept {
    if (!state) state = Load();
    auto lease = std::make_shared<PublishedManifestLease>();
    lease->state = std::move(state);
    lease->epoch = lease->state ? lease->state->epoch : 0u;
    lease->sequence = m_leaseSequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    lease->frameSlot = frameSlot;
    m_lease.store(lease, std::memory_order_release);
    return lease;
}

std::shared_ptr<const PublishedManifestLease> PublishedStateSource::LoadLease() const noexcept {
    return m_lease.load(std::memory_order_acquire);
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
    m_patches.clear();
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

bool RendererStatePublisher::PublishPatch(PublishedStatePatch patch) {
    const bool hasFragment = std::ranges::any_of(patch.fragments,
        [](const auto& fragment) { return fragment.has_value(); });
    if (!hasFragment) return false;
    std::lock_guard lock(m_mutex);
    ++m_stats.candidates;
    // A newer patch for the same fragment supersedes pending work for that
    // fragment, while disjoint patches remain independently commit-able.
    std::erase_if(m_patches, [&](const PublishedStatePatch& pending) {
        for (std::size_t index = 0; index < patch.fragments.size(); ++index) {
            if (patch.fragments[index] && pending.fragments[index]) return true;
        }
        return false;
    });
    m_patches.push_back(std::move(patch));
    return true;
}

bool RendererStatePublisher::PublishArtifact(const ArtifactSnapshot& artifact) {
    if (!artifact.payload.Valid() ||
        (artifact.readiness != ArtifactReadiness::UploadSubmitted &&
         artifact.readiness != ArtifactReadiness::GpuReady &&
         artifact.readiness != ArtifactReadiness::Published)) {
        return false;
    }
    if (artifact.key.kind != ArtifactKind::FrameManifest) return false;
    const auto manifest = artifact.payload.Get<FrameManifestPayload>();
    if (!manifest || !manifest->state) return false;
    if (manifest->patch) return PublishPatch(*manifest->patch);
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
    if (!m_patches.empty()) {
        auto patched = m_active ? std::make_shared<PublishedRendererState>(*m_active)
                                : std::make_shared<PublishedRendererState>();
        bool changed = false;
        for (const auto& patch : m_patches) {
            const bool preconditionsSatisfied = std::ranges::all_of(
                patch.preconditions, [&](const PublishedFragmentPrecondition& precondition) {
                    const auto& active = patched->Fragment(precondition.kind);
                    return active.publicationRoot == precondition.publicationRoot;
                });
            if (!preconditionsSatisfied) {
                ++m_stats.rejectedPatchPreconditions;
                result.rejectedCallback = m_candidateRejected;
                result.rejectedEpoch = patched->epoch;
                continue;
            }
            if (patch.sourceEpoch != patched->epoch) ++m_stats.rebasedPatches;
            for (std::size_t index = 0; index < patch.fragments.size(); ++index) {
                if (patch.fragments[index]) {
                    patched->Fragment(static_cast<PublishedFragmentKind>(index)) =
                        *patch.fragments[index];
                    changed = true;
                }
            }
            auto catalog = patched->resourceCatalog
                ? std::make_shared<PublishedResourceCatalog>(*patched->resourceCatalog)
                : std::make_shared<PublishedResourceCatalog>();
            for (auto entry = catalog->entries.begin(); entry != catalog->entries.end();) {
                if ((patch.catalogOwnerMask & PublishedFragmentMask(entry->first.owner)) != 0) {
                    catalog->contentVersions.erase(entry->first);
                    catalog->selections.erase(entry->first);
                    entry = catalog->entries.erase(entry);
                } else ++entry;
            }
            for (const auto& [key, resources] : patch.catalogEntries) {
                catalog->entries[key] = resources;
                catalog->contentVersions[key] =
                    patched->Fragment(key.owner).revision;
            }
            for (const auto& [key, selection] : patch.catalogSelections) {
                catalog->selections.insert_or_assign(key, selection);
            }
            patched->resourceCatalog = std::move(catalog);
        }
        m_patches.clear();
        if (changed) {
            auto manifestBundle = std::make_shared<PublicationBundle>();
            for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
                const auto& fragment = patched->Fragment(
                    static_cast<PublishedFragmentKind>(index));
                if (!fragment.publicationBundle) continue;
                manifestBundle->versions.insert(manifestBundle->versions.end(),
                    fragment.publicationBundle->versions.begin(),
                    fragment.publicationBundle->versions.end());
                manifestBundle->leases.Merge(fragment.publicationBundle->leases);
                for (const auto& submissions : fragment.publicationBundle->gpuSubmissions) {
                    if (submissions && !std::ranges::contains(
                        manifestBundle->gpuSubmissions, submissions)) {
                        manifestBundle->gpuSubmissions.push_back(submissions);
                    }
                }
                manifestBundle->resourceHolds.insert(manifestBundle->resourceHolds.end(),
                    fragment.publicationBundle->resourceHolds.begin(),
                    fragment.publicationBundle->resourceHolds.end());
            }
            std::ranges::sort(manifestBundle->versions);
            manifestBundle->versions.erase(std::unique(manifestBundle->versions.begin(),
                manifestBundle->versions.end()), manifestBundle->versions.end());
            patched->publicationBundle = std::move(manifestBundle);
            patched->epoch = (m_active ? m_active->epoch : 0u) + 1u;
            if (patched->resourceCatalog) {
                auto stampedCatalog = std::make_shared<PublishedResourceCatalog>(
                    *patched->resourceCatalog);
                for (auto& [_, selection] : stampedCatalog->selections) {
                    selection.manifestEpoch = patched->epoch;
                }
                patched->resourceCatalog = std::move(stampedCatalog);
            }
            if (m_active) result.retiredStates[result.retiredStateCount++] = std::move(m_active);
            m_active = std::move(patched);
            result.committed = true;
            ++m_stats.committed;
        }
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
    result.lease = m_source->AcquireLease(frameSlot, result.state);
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
    m_patches.clear();
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
