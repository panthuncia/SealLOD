#include "Render/PublishedRendererState.h"

#include <chrono>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <BasicTelemetry/Telemetry.h>
#include <BasicTelemetry/Tracy.h>

#include "Render/VersionedGpuBufferArtifacts.h"

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

std::shared_ptr<const PublishedResourceCatalog::OwnerShard> LegacyOwnerShard(
	const std::shared_ptr<const PublishedResourceCatalog>& catalog,
	PublishedFragmentKind owner) {
	auto shard = std::make_shared<PublishedResourceCatalog::OwnerShard>();
	if (!catalog) return shard;
	for (const auto& [key, resources] : catalog->entries)
		if (key.owner == owner) shard->entries.emplace(key, resources);
	for (const auto& [key, version] : catalog->contentVersions)
		if (key.owner == owner) shard->contentVersions.emplace(key, version);
	for (const auto& [key, selection] : catalog->selections)
		if (key.owner == owner) shard->selections.emplace(key, selection);
	return shard;
}

std::shared_ptr<PublishedResourceCatalog> MakeCatalogUpdate(
	const std::shared_ptr<const PublishedResourceCatalog>& base,
	std::uint64_t replacedOwnerMask,
	const std::vector<std::pair<PublishedResourceKey,
		std::shared_ptr<const PublishedResourceCatalog::ResourceList>>>& entries,
	const std::vector<std::pair<PublishedResourceKey, PublishedResourceSelection>>& selections,
	const PublishedRendererState& state, std::uint64_t targetEpoch) {
	auto result = std::make_shared<PublishedResourceCatalog>();
	std::array<bool, kPublishedFragmentCount> changed{};
	for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
		const auto owner = static_cast<PublishedFragmentKind>(index);
		result->ownerShards[index] = base && base->ownerShards[index]
			? base->ownerShards[index] : LegacyOwnerShard(base, owner);
		changed[index] = (replacedOwnerMask & PublishedFragmentMask(owner)) != 0;
	}
	for (const auto& [key, _] : entries) changed[static_cast<std::size_t>(key.owner)] = true;
	for (const auto& [key, _] : selections) changed[static_cast<std::size_t>(key.owner)] = true;
	for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
		if (!changed[index]) continue;
		const auto owner = static_cast<PublishedFragmentKind>(index);
		auto shard = (replacedOwnerMask & PublishedFragmentMask(owner)) != 0
			? std::make_shared<PublishedResourceCatalog::OwnerShard>()
			: std::make_shared<PublishedResourceCatalog::OwnerShard>(*result->ownerShards[index]);
		for (const auto& [key, resources] : entries) {
			if (key.owner != owner) continue;
			shard->entries.insert_or_assign(key, resources);
			shard->contentVersions.insert_or_assign(key, state.Fragment(owner).revision);
		}
		for (const auto& [key, selection] : selections) {
			if (key.owner != owner) continue;
			auto stamped = selection;
			stamped.manifestEpoch = targetEpoch;
			shard->selections.insert_or_assign(key, std::move(stamped));
		}
		result->ownerShards[index] = std::move(shard);
	}
	return result;
}
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
	const auto& shard = ownerShards[static_cast<std::size_t>(key.owner)];
	if (shard) {
		const auto selected = shard->entries.find(key);
		return selected == shard->entries.end() ? nullptr : selected->second;
	}
    return found == entries.end() ? nullptr : found->second;
}

const PublishedResourceSelection* PublishedResourceCatalog::FindSelection(
    const PublishedResourceKey& key) const noexcept {
    const auto found = selections.find(key);
	const auto& shard = ownerShards[static_cast<std::size_t>(key.owner)];
	if (shard) {
		const auto selected = shard->selections.find(key);
		return selected == shard->selections.end() ? nullptr : &selected->second;
	}
    return found == selections.end() ? nullptr : &found->second;
}

std::vector<const PublishedResourceSelection*> PublishedResourceCatalog::FindSelections(
    const PublishedResourceQuery& query) const {
    std::vector<const PublishedResourceSelection*> result;
	for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
		if (query.owner && static_cast<std::size_t>(*query.owner) != index) continue;
		if (const auto& shard = ownerShards[index]) {
			for (const auto& [key, selection] : shard->selections)
				if (query.Matches(key)) result.push_back(&selection);
		} else {
			for (const auto& [key, selection] : selections)
				if (static_cast<std::size_t>(key.owner) == index && query.Matches(key))
					result.push_back(&selection);
		}
	}
    return result;
}

std::uint64_t PublishedResourceCatalog::ContentVersion(
    const PublishedResourceKey& key) const noexcept {
    const auto found = contentVersions.find(key);
	const auto& shard = ownerShards[static_cast<std::size_t>(key.owner)];
	if (shard) {
		const auto selected = shard->contentVersions.find(key);
		return selected == shard->contentVersions.end() ? 0u : selected->second;
	}
    return found == contentVersions.end() ? 0u : found->second;
}

std::uint64_t PublishedResourceCatalog::ContentVersion(
    const PublishedResourceQuery& query) const noexcept {
    std::uint64_t version = 0;
    bool matched = false;
    for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
		if (query.owner && static_cast<std::size_t>(*query.owner) != index) continue;
		const auto& versions = ownerShards[index]
			? ownerShards[index]->contentVersions : contentVersions;
		for (const auto& [key, entryVersion] : versions) {
			if (static_cast<std::size_t>(key.owner) != index || !query.Matches(key)) continue;
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
	for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
		if (query.owner && static_cast<std::size_t>(*query.owner) != index) continue;
		const auto& ownerEntries = ownerShards[index] ? ownerShards[index]->entries : entries;
		for (const auto& [key, resources] : ownerEntries) {
			if (static_cast<std::size_t>(key.owner) != index || !query.Matches(key) || !resources) continue;
			result.insert(result.end(), resources->begin(), resources->end());
		}
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

void PublishedStateSource::Clear() noexcept {
    m_lease.store({}, std::memory_order_release);
    m_state.store({}, std::memory_order_release);
}

PublishedStateFragment& PublishedRendererState::Fragment(PublishedFragmentKind kind) {
    switch (kind) {
    case PublishedFragmentKind::Materials: return materials;
    case PublishedFragmentKind::TextureImages: return textureImages;
    case PublishedFragmentKind::Terrain: return terrain;
    case PublishedFragmentKind::Geometry: return geometry;
    case PublishedFragmentKind::DrawRecords: return drawRecords;
    case PublishedFragmentKind::ActiveDrawLists: return activeDrawLists;
    case PublishedFragmentKind::IndirectWorkloads: return indirectWorkloads;
    case PublishedFragmentKind::Grass: return grass;
    case PublishedFragmentKind::Count: break;
    }
    throw std::out_of_range("published renderer fragment kind");
}

const PublishedStateFragment& PublishedRendererState::Fragment(PublishedFragmentKind kind) const {
    return const_cast<PublishedRendererState*>(this)->Fragment(kind);
}

bool IsMonotonicFragmentSuccessor(const PublishedStateFragment& active,
    const PublishedStateFragment& successor) noexcept {
    if (!active.publicationRoot || !successor.publicationRoot) return true;
    if (active.publicationRoot.address != successor.publicationRoot.address) return true;
    return successor.publicationRoot.revision >= active.publicationRoot.revision &&
        successor.revision >= active.revision;
}

namespace {
bool AllowsRollback(ManifestPublicationPolicy policy, const std::string& reason) noexcept {
    return policy == ManifestPublicationPolicy::ExplicitRollback && !reason.empty();
}

bool IsMonotonicStateSuccessor(const PublishedRendererState& active,
    const PublishedRendererState& successor) noexcept {
    for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
        const auto kind = static_cast<PublishedFragmentKind>(index);
        if (!IsMonotonicFragmentSuccessor(active.Fragment(kind), successor.Fragment(kind))) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<const PublicationBundle> BuildManifestOwnershipBundle(
    const PublishedRendererState& state) {
    auto manifest = std::make_shared<PublicationBundle>();
    for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
        const auto& fragment = state.Fragment(static_cast<PublishedFragmentKind>(index));
        if (fragment.publicationBundle) manifest->parents.push_back(fragment.publicationBundle);
    }
    basic_telemetry::Record("SARP.RendererStatePublisher.ManifestRootCount",
        manifest->parents.size());
    return manifest;
}
}

std::shared_ptr<const PublishedRendererState> MaterializePublishedState(
    const std::shared_ptr<const PublishedRendererState>& base,
    const PublishedStatePatch& patch, std::uint64_t targetEpoch) {
    auto state = base ? std::make_shared<PublishedRendererState>(*base)
                      : std::make_shared<PublishedRendererState>();
    const bool preconditionsSatisfied = std::ranges::all_of(
        patch.preconditions, [&](const PublishedFragmentPrecondition& precondition) {
            return state->Fragment(precondition.kind).publicationRoot == precondition.publicationRoot;
        });
    if (!preconditionsSatisfied) return {};

    const bool rollback = AllowsRollback(patch.policy, patch.reason);
    for (std::size_t index = 0; index < patch.fragments.size(); ++index) {
        if (!patch.fragments[index]) continue;
        const auto kind = static_cast<PublishedFragmentKind>(index);
        if (!rollback && !IsMonotonicFragmentSuccessor(
            state->Fragment(kind), *patch.fragments[index])) return {};
        state->Fragment(kind) = *patch.fragments[index];
    }

    state->epoch = targetEpoch;
    state->resourceCatalog = MakeCatalogUpdate(state->resourceCatalog,
		patch.catalogOwnerMask, patch.catalogEntries, patch.catalogSelections,
		*state, targetEpoch);
    state->publicationBundle = BuildManifestOwnershipBundle(*state);
    return state;
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
    m_commitLatencySamples.fill(0);
    m_commitLatencySampleCursor = 0;
    m_commitLatencySampleCount = 0;
}

bool RendererStatePublisher::PublishCandidate(RendererStateCandidate candidate) {
    if (!candidate.state || candidate.state->epoch <= candidate.baseEpoch) return false;
    if (candidate.policy == ManifestPublicationPolicy::ExplicitRollback && candidate.reason.empty()) {
        return false;
    }
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
    if (patch.policy == ManifestPublicationPolicy::ExplicitRollback && patch.reason.empty()) {
        return false;
    }
    if (patch.policy == ManifestPublicationPolicy::MonotonicSuccessor) {
        for (const auto& pending : m_patches) {
            for (std::size_t index = 0; index < patch.fragments.size(); ++index) {
                if (patch.fragments[index] && pending.fragments[index] &&
                    !IsMonotonicFragmentSuccessor(*pending.fragments[index], *patch.fragments[index])) {
                    ++m_stats.rejectedFragmentRegressions;
                    basic_telemetry::AddCounter(
                        "SARP.RendererStatePublisher.FragmentRegressionRejections");
                    return false;
                }
            }
        }
    }
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
    if (!manifest) return false;
    if (manifest->patch) return PublishPatch(*manifest->patch);
    if (!manifest->state) return false;
    const auto baseEpoch = manifest->baseEpoch;
    auto state = manifest->state;
    if (state->epoch != baseEpoch + 1u) {
        auto corrected = std::make_shared<PublishedRendererState>(*state);
        corrected->epoch = baseEpoch + 1u;
        state = std::move(corrected);
    }
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
    // Commit is called only after this frame slot's fence completes. Notify
    // bounded mutable-resource pools after the retired manifests have released
    // their resource holds so suspended builds can retry without polling.
    NotifyVersionedGpuBufferFrameRetirement();
}

RendererStateCommitResult RendererStatePublisher::Commit(std::size_t frameSlot) {
    const auto started = std::chrono::steady_clock::now();
    RendererStateCommitResult result;
    std::lock_guard lock(m_mutex);
    BT_ZONE_SCOPE("RendererStatePublisher::Commit::Locked");
    if (frameSlot >= m_frameStates.size()) {
        result.state = m_active;
        return result;
    }
    if (m_frameStates[frameSlot]) {
        BT_ZONE_SCOPE("RendererStatePublisher::Commit::RetireFrameSlot");
        result.retiredStates[result.retiredStateCount++] = std::move(m_frameStates[frameSlot]);
        if (m_stats.retainedFrameStates) --m_stats.retainedFrameStates;
    }
    if (m_candidate.state) {
        BT_ZONE_SCOPE("RendererStatePublisher::Commit::Candidate");
        const auto activeEpoch = m_active ? m_active->epoch : 0u;
        const bool rollback = AllowsRollback(m_candidate.policy, m_candidate.reason);
        const bool monotonic = !m_active || IsMonotonicStateSuccessor(*m_active, *m_candidate.state);
        if (m_candidate.baseEpoch == activeEpoch && (monotonic || rollback)) {
            if (m_active) result.retiredStates[result.retiredStateCount++] = std::move(m_active);
            m_active = std::move(m_candidate.state);
            result.committed = true;
            ++m_stats.committed;
            if (rollback && !monotonic) ++m_stats.explicitRollbacks;
        } else {
            if (m_candidate.baseEpoch != activeEpoch) ++m_stats.rejectedBaseEpoch;
            else ++m_stats.rejectedFragmentRegressions;
            basic_telemetry::AddCounter("SARP.RendererStatePublisher.CandidateRejections");
            result.rejectedCallback = m_candidateRejected;
            result.rejectedEpoch = activeEpoch;
            result.retiredStates[result.retiredStateCount++] = std::move(m_candidate.state);
        }
        m_candidate.baseEpoch = 0;
    }
    if (!m_patches.empty()) {
        BT_ZONE_SCOPE("RendererStatePublisher::Commit::ApplyPatches");
        auto patched = m_active ? std::make_shared<PublishedRendererState>(*m_active)
                                : std::make_shared<PublishedRendererState>();
        // Catalog patches are persistent overlays. Commit work is proportional
        // to changed entries rather than total renderer catalog size.
		std::shared_ptr<const PublishedResourceCatalog> catalog = patched->resourceCatalog;
		const auto targetEpoch = (m_active ? m_active->epoch : 0u) + 1u;
        bool changed = false;
        for (const auto& patch : m_patches) {
            BT_ZONE_SCOPE("RendererStatePublisher::Commit::ApplyOnePatch");
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
            const bool rollback = AllowsRollback(patch.policy, patch.reason);
            bool monotonic = true;
            for (std::size_t index = 0; index < patch.fragments.size(); ++index) {
                if (patch.fragments[index] && !IsMonotonicFragmentSuccessor(
                    patched->Fragment(static_cast<PublishedFragmentKind>(index)),
                    *patch.fragments[index])) {
                    monotonic = false;
                    break;
                }
            }
            if (!monotonic && !rollback) {
                ++m_stats.rejectedFragmentRegressions;
                basic_telemetry::AddCounter(
                    "SARP.RendererStatePublisher.FragmentRegressionRejections");
                result.rejectedCallback = m_candidateRejected;
                result.rejectedEpoch = patched->epoch;
                continue;
            }
            if (!monotonic) ++m_stats.explicitRollbacks;
            if (patch.sourceEpoch != patched->epoch) ++m_stats.rebasedPatches;
            for (std::size_t index = 0; index < patch.fragments.size(); ++index) {
                if (patch.fragments[index]) {
                    patched->Fragment(static_cast<PublishedFragmentKind>(index)) =
                        *patch.fragments[index];
                    changed = true;
                }
            }
			catalog = MakeCatalogUpdate(catalog, patch.catalogOwnerMask,
				patch.catalogEntries, patch.catalogSelections, *patched, targetEpoch);
        }
        m_patches.clear();
        if (changed) {
            BT_ZONE_SCOPE("RendererStatePublisher::Commit::BuildManifestBundle");
            patched->publicationBundle = BuildManifestOwnershipBundle(*patched);
            patched->epoch = targetEpoch;
            patched->resourceCatalog = std::move(catalog);
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
    m_commitLatencySamples[m_commitLatencySampleCursor] = m_stats.commitMicros;
    m_commitLatencySampleCursor =
        (m_commitLatencySampleCursor + 1u) % kCommitLatencySampleCapacity;
    m_commitLatencySampleCount = (std::min)(
        m_commitLatencySampleCount + 1u, kCommitLatencySampleCapacity);
    m_stats.commitSamples = m_commitLatencySampleCount;
    m_stats.commitMaxMicros = (std::max)(m_stats.commitMaxMicros, m_stats.commitMicros);
    basic_telemetry::Record("SARP.RendererStatePublisher.CommitDurationNs",
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count()));
    basic_telemetry::SetGauge("SARP.RendererStatePublisher.RetainedFrameStates",
        static_cast<std::int64_t>(m_stats.retainedFrameStates));
    result.state = m_active;
    {
        BT_ZONE_SCOPE("RendererStatePublisher::Commit::PublishSourceAndLease");
        m_source->Store(result.state);
        result.lease = m_source->AcquireLease(frameSlot, result.state);
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
    m_patches.clear();
    m_candidate.baseEpoch = 0;
    lock.unlock();
    retired.reset();
}

void RendererStatePublisher::Shutdown() {
    RendererStateCandidate candidate;
    std::vector<PublishedStatePatch> patches;
    std::shared_ptr<const PublishedRendererState> active;
    std::vector<std::shared_ptr<const PublishedRendererState>> frameStates;
    std::shared_ptr<PublishedStateSource> source;
    {
        std::lock_guard lock(m_mutex);
        candidate = std::move(m_candidate);
        patches = std::move(m_patches);
        active = std::move(m_active);
        frameStates = std::move(m_frameStates);
        source = m_source;
        m_candidateRejected = {};
        m_stats.retainedFrameStates = 0;
    }
    // PublishedStateSource owns both the latest state and its most recent
    // frame lease. Clear those roots before destroying the moved ownership
    // graph, and do all resource destruction outside the publisher mutex.
    if (source) source->Clear();
    candidate = {};
    patches.clear();
    active.reset();
    frameStates.clear();
    NotifyVersionedGpuBufferFrameRetirement();
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
    auto result = m_stats;
    if (m_commitLatencySampleCount != 0u) {
        std::vector<std::uint64_t> samples(
            m_commitLatencySamples.begin(),
            m_commitLatencySamples.begin() + m_commitLatencySampleCount);
        const auto rank = ((samples.size() * 99u) + 99u) / 100u - 1u;
        std::nth_element(samples.begin(), samples.begin() + rank, samples.end());
        result.commitP99Micros = samples[rank];
    }
    return result;
}

} // namespace br::render
