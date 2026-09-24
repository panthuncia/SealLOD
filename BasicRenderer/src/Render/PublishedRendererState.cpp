#include "Render/PublishedRendererState.h"

#include <chrono>
#include <algorithm>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <BasicTelemetry/Telemetry.h>
#include <BasicTelemetry/Tracy.h>

#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/PublicationBindingBundle.h"

namespace br::render {

namespace {
std::mutex g_processSourceMutex;
std::weak_ptr<PublishedStateSource> g_processSource;

struct SelectionBindingInputs {
    std::vector<org::PublicationBindingBundle::Snapshot> bindings;
    std::vector<std::shared_ptr<const void>> owners;
};
SelectionBindingInputs CaptureSelectionBindingInputs(const PublishedResourceSelection& selection) {
    SelectionBindingInputs inputs;
    std::unordered_set<uint64_t> captured;
    if (selection.resources) for (const auto& resource : *selection.resources) {
        if (!resource) continue;
        auto binding = org::PublicationBindingBundle::Capture(*resource);
        if (binding && captured.insert(binding->resourceID).second)
            inputs.bindings.push_back(std::move(binding));
    }
    inputs.owners = selection.lifetimeHolds;
    if (selection.publicationBundle) inputs.owners.push_back(selection.publicationBundle);
    return inputs;
}
std::shared_ptr<const org::PublicationBindingBundle> BuildSelectionBindings(SelectionBindingInputs inputs) {
    auto result = std::make_shared<const org::PublicationBindingBundle>(std::move(inputs.bindings), std::move(inputs.owners));
    org::TraceBindingHolder(result, result, "PublicationSelection");
    return result;
}
std::shared_ptr<const org::PublicationBindingBundle> CaptureSelectionBindings(const PublishedResourceSelection& selection) {
    return BuildSelectionBindings(CaptureSelectionBindingInputs(selection));
}

std::shared_ptr<const org::PublicationBindingBundle> GatherManifestBindings(
    const PublishedRendererState& state) {
    std::vector<std::shared_ptr<const org::PublicationBindingBundle>> roots;
    if (state.resourceCatalog) for (const auto& shard : state.resourceCatalog->ownerShards)
        if (shard && shard->bindingBundle) roots.push_back(shard->bindingBundle);
    auto result = std::make_shared<const org::PublicationBindingBundle>(
        std::vector<org::PublicationBindingBundle::Snapshot>{}, std::vector<std::shared_ptr<const void>>{}, std::move(roots));
    org::TraceBindingHolder(result, result, "ManifestBindings", state.epoch);
    return result;
}

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
            if (!stamped.bindingBundle) stamped.bindingBundle = CaptureSelectionBindings(stamped);
			shard->selections.insert_or_assign(key, std::move(stamped));
		}
        std::vector<std::shared_ptr<const org::PublicationBindingBundle>> roots;
        for (const auto& [key, selection] : shard->selections) if (selection.bindingBundle) {
            roots.push_back(selection.bindingBundle);
        }
        shard->bindingBundle = std::make_shared<const org::PublicationBindingBundle>(
            std::vector<org::PublicationBindingBundle::Snapshot>{}, std::vector<std::shared_ptr<const void>>{}, std::move(roots));
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
	std::vector<std::pair<PublishedResourceKey, const PublishedResourceSelection*>> matches;
	for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
		if (query.owner && static_cast<std::size_t>(*query.owner) != index) continue;
		if (const auto& shard = ownerShards[index]) {
			for (const auto& [key, selection] : shard->selections)
				if (query.Matches(key)) matches.emplace_back(key, &selection);
		} else {
			for (const auto& [key, selection] : selections)
				if (static_cast<std::size_t>(key.owner) == index && query.Matches(key))
					matches.emplace_back(key, &selection);
		}
	}
	std::ranges::sort(matches, [](const auto& lhs, const auto& rhs) {
		return lhs.first < rhs.first;
	});
	std::vector<const PublishedResourceSelection*> result;
	result.reserve(matches.size());
	for (const auto& [_, selection] : matches) result.push_back(selection);
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
	std::vector<std::pair<PublishedResourceKey, std::shared_ptr<const ResourceList>>> matches;
	for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
		if (query.owner && static_cast<std::size_t>(*query.owner) != index) continue;
		const auto& ownerEntries = ownerShards[index] ? ownerShards[index]->entries : entries;
		for (const auto& [key, resources] : ownerEntries) {
			if (static_cast<std::size_t>(key.owner) != index || !query.Matches(key) || !resources) continue;
			matches.emplace_back(key, resources);
		}
	}
	std::ranges::sort(matches, [](const auto& lhs, const auto& rhs) {
		return lhs.first < rhs.first;
	});
	std::size_t resourceCount = 0;
	for (const auto& [_, resources] : matches) resourceCount += resources->size();
	ResourceList result;
	result.reserve(resourceCount);
	for (const auto& [_, resources] : matches)
		result.insert(result.end(), resources->begin(), resources->end());
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

std::shared_ptr<const void> PublishedStateSource::ResolverDependencyIdentity(
	const PublishedResourceKey& key) const {
	std::scoped_lock lock(m_resolverIdentityMutex);
	for (auto& entry : m_resolverIdentities) {
		if (!entry.exact || entry.key != key) continue;
		if (auto identity = entry.identity.lock()) return identity;
		auto identity = std::make_shared<const std::uint8_t>(0);
		entry.identity = identity;
		return identity;
	}
	auto identity = std::make_shared<const std::uint8_t>(0);
	m_resolverIdentities.push_back({ .exact = true, .key = key, .identity = identity });
	return identity;
}

std::shared_ptr<const void> PublishedStateSource::ResolverDependencyIdentity(
	const PublishedResourceQuery& query) const {
	const auto equalQuery = [](const PublishedResourceQuery& lhs, const PublishedResourceQuery& rhs) {
		return lhs.owner == rhs.owner && lhs.usage == rhs.usage &&
			lhs.renderPhaseHash == rhs.renderPhaseHash && lhs.viewOrWorkloadID == rhs.viewOrWorkloadID &&
			lhs.requiredVariantMask == rhs.requiredVariantMask && lhs.forbiddenVariantMask == rhs.forbiddenVariantMask;
	};
	std::scoped_lock lock(m_resolverIdentityMutex);
	for (auto& entry : m_resolverIdentities) {
		if (entry.exact || !equalQuery(entry.query, query)) continue;
		if (auto identity = entry.identity.lock()) return identity;
		auto identity = std::make_shared<const std::uint8_t>(0);
		entry.identity = identity;
		return identity;
	}
	auto identity = std::make_shared<const std::uint8_t>(0);
	m_resolverIdentities.push_back({ .exact = false, .query = query, .identity = identity });
	return identity;
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
    case PublishedFragmentKind::GeometryResidency: return geometryResidency;
    case PublishedFragmentKind::DrawRecords: return drawRecords;
    case PublishedFragmentKind::ActiveDrawLists: return activeDrawLists;
    case PublishedFragmentKind::IndirectWorkloads: return indirectWorkloads;
    case PublishedFragmentKind::Grass: return grass;
    case PublishedFragmentKind::Views: return views;
    case PublishedFragmentKind::Poses: return poses;
    case PublishedFragmentKind::Lights: return lights;
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

bool MinimumPublicationDependenciesSatisfied(const PublishedRendererState& state) noexcept {
    for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
        const auto& fragment = state.Fragment(static_cast<PublishedFragmentKind>(index));
        if (fragment.revision == 0) continue;
        for (const auto& dependency : fragment.minimumPublicationDependencies) {
            if (dependency.fragmentKind == PublishedFragmentKind::Count) return false;
            const auto& selected = state.Fragment(dependency.fragmentKind);
            if (selected.revision == 0 || selected.coverage < dependency.minimumCoverage) return false;
        }
    }
    return true;
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
    // The manifest was solved against an older base; roots only advance, so this
    // holds unless a rollback moved a depended-on fragment backwards.
    if (!MinimumPublicationDependenciesSatisfied(*state)) return {};

    state->epoch = targetEpoch;
    state->resourceCatalog = MakeCatalogUpdate(state->resourceCatalog,
		patch.catalogOwnerMask, patch.catalogEntries, patch.catalogSelections,
		*state, targetEpoch);
    state->publicationBundle = BuildManifestOwnershipBundle(*state);
    state->bindingBundle = GatherManifestBindings(*state);
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
    for (const auto& pending : m_patches) pending->cancelled.store(true, std::memory_order_release);
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

void RendererStatePublisher::SetPreparationScheduler(std::function<bool(std::function<void()>&&)> scheduler) {
    std::lock_guard lock(m_mutex);
    m_preparePublication = std::move(scheduler);
}
void RendererStatePublisher::SetExecutablePreparation(ExecutablePreparation prepare) {
    std::lock_guard lock(m_mutex);
    m_prepareExecutable = std::move(prepare);
}

bool RendererStatePublisher::PublishPatch(PublishedStatePatch patch) {
    const auto submittedNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool hasFragment = std::ranges::any_of(patch.fragments,
        [](const auto& fragment) { return fragment.has_value(); });
    if (!hasFragment || (patch.policy == ManifestPublicationPolicy::ExplicitRollback && patch.reason.empty())) return false;
    // Capture mutable interfaces on the producer. The queued build consumes
    // only exact-version values and leases, never a Resource or manager.
    std::vector<std::pair<size_t, SelectionBindingInputs>> inputs;
    try {
        for (size_t i = 0; i < patch.catalogSelections.size(); ++i)
            if (!patch.catalogSelections[i].second.bindingBundle)
                inputs.emplace_back(i, CaptureSelectionBindingInputs(patch.catalogSelections[i].second));
    } catch (const std::exception& error) {
        spdlog::error("Publication binding capture failed: {}", error.what());
        basic_telemetry::AddCounter("SARP.RendererStatePublisher.BindingPreparationFailures");
        return false;
    }
    auto candidate = std::make_shared<PendingPatch>();
    candidate->submittedNs = submittedNs;
    candidate->patch = std::move(patch);
    std::function<bool(std::function<void()>&&)> scheduler;
    {
        std::lock_guard lock(m_mutex);
        const auto& next = candidate->patch;
        if (next.policy == ManifestPublicationPolicy::MonotonicSuccessor) {
            for (const auto& pending : m_patches) for (size_t i = 0; i < next.fragments.size(); ++i) {
                if (next.fragments[i] && pending->patch.fragments[i] &&
                    !IsMonotonicFragmentSuccessor(*pending->patch.fragments[i], *next.fragments[i])) {
                    ++m_stats.rejectedFragmentRegressions;
                    basic_telemetry::AddCounter("SARP.RendererStatePublisher.FragmentRegressionRejections");
                    return false;
                }
            }
        }
        ++m_stats.candidates;
        for (const auto& pending : m_patches) {
            const auto& prior = pending->patch;
            bool superseded = true;
            for (size_t i = 0; i < next.fragments.size(); ++i)
                if (prior.fragments[i] && !next.fragments[i]) superseded = false;
            auto keyCovered = [&](const PublishedResourceKey& key, bool selection) {
                return (next.catalogOwnerMask & PublishedFragmentMask(key.owner)) != 0
                    || (selection
                        ? std::ranges::any_of(next.catalogSelections, [&](const auto& entry) { return entry.first == key; })
                        : std::ranges::any_of(next.catalogEntries, [&](const auto& entry) { return entry.first == key; }));
            };
            for (const auto& entry : prior.catalogEntries) superseded &= keyCovered(entry.first, false);
            for (const auto& entry : prior.catalogSelections) superseded &= keyCovered(entry.first, true);
            if (!superseded) continue;
            pending->cancelled.store(true, std::memory_order_release);
            pending->failed.store(true, std::memory_order_relaxed);
            pending->ready.store(true, std::memory_order_release);
            // Commit reports rejection to the producer so an outstanding
            // publication acknowledgement cannot be stranded by supersession.
        }
        m_patches.push_back(candidate);
        // Startup has no previous valid publication to render.
        if (m_active) scheduler = m_preparePublication;
        candidate->base = m_active;
        candidate->materializeOnWorker = static_cast<bool>(scheduler) || static_cast<bool>(m_prepareExecutable);
        candidate->prepareExecutable = m_prepareExecutable;
    }
    const bool needsBuild = !inputs.empty();
    auto prepare = [candidate, inputs = std::move(inputs)]() mutable {
        BT_ZONE_SCOPE("ORG.Publication.BuildBindingBundles");
        // A task service can retain a completed callable. Consume its captures
        // on entry so completion, cancellation and failure all release them.
        auto pending = std::move(candidate);
        auto capturedInputs = std::move(inputs);
        try {
            for (auto& [index, captured] : capturedInputs) {
                if (pending->cancelled.load(std::memory_order_acquire)) return;
                pending->patch.catalogSelections[index].second.bindingBundle = BuildSelectionBindings(std::move(captured));
            }
            if (pending->materializeOnWorker && !pending->cancelled.load(std::memory_order_acquire)) {
                BT_ZONE_SCOPE("ORG.Publication.MaterializeSuccessor");
                auto prepared = MaterializePublishedState(pending->base,pending->patch,
                    (pending->base ? pending->base->epoch : 0u) + 1u);
                if (prepared && pending->prepareExecutable) {
                    auto executableState = std::make_shared<PublishedRendererState>(*prepared);
                    pending->prepareExecutable(pending->base,*executableState);
                    prepared = std::move(executableState);
                }
                pending->prepared = std::move(prepared);
            }
        } catch (const std::exception& error) {
            spdlog::error("Publication binding build failed: {}", error.what());
            pending->failed.store(true, std::memory_order_relaxed);
        } catch (...) {
            spdlog::error("Publication successor build failed with an unknown exception");
            pending->failed.store(true, std::memory_order_relaxed);
        }
        pending->ready.store(true, std::memory_order_release);
    };
    if (scheduler && (needsBuild || candidate->materializeOnWorker)) {
        // inputs have moved into the closure; its work is always immutable.
        if (!scheduler(std::move(prepare))) {
            candidate->failed.store(true, std::memory_order_relaxed);
            candidate->ready.store(true, std::memory_order_release);
            basic_telemetry::AddCounter("SARP.RendererStatePublisher.BindingPreparationRejected");
            return false;
        }
    } else prepare();
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
    retiredPreparations.clear();
    // Commit is called only after this frame slot's fence completes. Notify
    // bounded mutable-resource pools after the retired manifests have released
    // their resource holds so suspended builds can retry without polling.
    NotifyVersionedGpuBufferFrameRetirement();
}

RendererStateCommitResult RendererStatePublisher::Commit(std::size_t frameSlot) {
    const auto started = std::chrono::steady_clock::now();
    RendererStateCommitResult result;
    std::unique_lock lock(m_mutex);
    BT_ZONE_SCOPE("RendererStatePublisher::Commit::Locked");
    std::vector<std::pair<std::shared_ptr<PendingPatch>,std::function<void()>>> rebuilds;
    const auto preparationScheduler = m_preparePublication;
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
        // Scheduled publication preparation produces complete immutable states.
        // Select only an exact-base result. Independent patches whose base moved
        // retry on workers rather than reconstructing a catalog on this thread.
        for (const auto& pending : m_patches) {
            if (!pending->materializeOnWorker || !pending->ready.load(std::memory_order_acquire)) continue;
            if (pending->failed.load(std::memory_order_relaxed)
                || pending->cancelled.load(std::memory_order_acquire)) continue;
            if (pending->base != m_active) {
                result.retiredPreparations.push_back(std::move(pending->prepared));
                result.retiredPreparations.push_back(std::move(pending->base));
                pending->base = m_active;
                pending->ready.store(false,std::memory_order_release);
                rebuilds.emplace_back(pending,[pending] {
                    try {
                        BT_ZONE_SCOPE("ORG.Publication.RebaseSuccessor");
                        if (!pending->cancelled.load(std::memory_order_acquire)) {
                            auto prepared = MaterializePublishedState(pending->base,pending->patch,
                                (pending->base ? pending->base->epoch : 0u) + 1u);
                            if (prepared && pending->prepareExecutable) {
                                auto executableState = std::make_shared<PublishedRendererState>(*prepared);
                                pending->prepareExecutable(pending->base,*executableState);
                                prepared = std::move(executableState);
                            }
                            pending->prepared = std::move(prepared);
                        }
                    } catch (const std::exception& error) {
                        spdlog::error("Publication successor rebase failed: {}",error.what());
                        pending->failed.store(true,std::memory_order_relaxed);
                    } catch (...) {
                        spdlog::error("Publication successor rebase failed with an unknown exception");
                        pending->failed.store(true,std::memory_order_relaxed);
                    }
                    pending->ready.store(true,std::memory_order_release);
                });
                basic_telemetry::AddCounter("SARP.RendererStatePublisher.SuccessorRebuilds");
            }
        }
        BT_ZONE_SCOPE("RendererStatePublisher::Commit::ApplyPatches");
        const bool hasInlinePatch = std::ranges::any_of(m_patches,[](const auto& pending) {
            return !pending->materializeOnWorker && pending->ready.load(std::memory_order_acquire);
        });
        auto patched = hasInlinePatch ? (m_active ? std::make_shared<PublishedRendererState>(*m_active)
                                : std::make_shared<PublishedRendererState>()) : nullptr;
        // Catalog patches are persistent overlays. Commit work is proportional
        // to changed entries rather than total renderer catalog size.
		std::shared_ptr<const PublishedResourceCatalog> catalog = patched ? patched->resourceCatalog : nullptr;
		const auto targetEpoch = (m_active ? m_active->epoch : 0u) + 1u;
        bool changed = false;
        for (const auto& pending : m_patches) {
            if (!pending->ready.load(std::memory_order_acquire)) continue;
            const auto& patch = pending->patch;
            if (pending->materializeOnWorker && !pending->failed.load(std::memory_order_relaxed)) {
                if (pending->base != m_active) continue;
                if (pending->prepared) {
                    BT_ZONE_SCOPE("ORG.Publication.SelectReadySuccessor");
                    if (m_active) result.retiredPreparations.push_back(std::move(m_active));
                    m_active = pending->prepared;
                    result.committed = true;
                    ++m_stats.committed;
                    basic_telemetry::AddCounter("SARP.RendererStatePublisher.PreparedSuccessorsSelected");
                    const auto selectedNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                    basic_telemetry::Record("SARP.RendererStatePublisher.PublicationLatencyNs",selectedNs - pending->submittedNs);
                    if (patch.sourceEpoch != pending->base->epoch) ++m_stats.rebasedPatches;
                    if (patch.policy == ManifestPublicationPolicy::ExplicitRollback) ++m_stats.explicitRollbacks;
                } else {
                    result.rejectedCallback = m_candidateRejected;
                    result.rejectedEpoch = m_active ? m_active->epoch : 0u;
                    const bool preconditions = std::ranges::all_of(patch.preconditions,[&](const auto& expected) {
                        return m_active && m_active->Fragment(expected.kind).publicationRoot == expected.publicationRoot;
                    });
                    if (preconditions) ++m_stats.rejectedFragmentRegressions;
                    else ++m_stats.rejectedPatchPreconditions;
                }
                continue;
            }
            if (pending->failed.load(std::memory_order_relaxed)) {
                basic_telemetry::AddCounter(pending->cancelled.load(std::memory_order_acquire)
                    ? "SARP.RendererStatePublisher.SupersededPublicationResults"
                    : "SARP.RendererStatePublisher.BindingPreparationFailures");
                result.rejectedCallback = m_candidateRejected;
                result.rejectedEpoch = m_active ? m_active->epoch : 0u;
                continue;
            }
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
        std::erase_if(m_patches, [&](const auto& pending) {
            if (!pending->ready.load(std::memory_order_acquire)) return false;
            if (pending->materializeOnWorker && !pending->failed.load(std::memory_order_relaxed)
                && pending->base != m_active && pending->prepared != m_active) return false;
            result.retiredPreparations.push_back(pending);
            return true;
        });
        if (changed) {
            BT_ZONE_SCOPE("RendererStatePublisher::Commit::BuildManifestBundle");
            patched->publicationBundle = BuildManifestOwnershipBundle(*patched);
            patched->epoch = targetEpoch;
            patched->resourceCatalog = std::move(catalog);
            patched->bindingBundle = GatherManifestBindings(*patched);
            if (m_active) result.retiredStates[result.retiredStateCount++] = std::move(m_active);
            m_active = std::move(patched);
            result.committed = true;
            ++m_stats.committed;
        }
    }
    m_frameStates[frameSlot] = m_active;
    uint64_t oldestPendingNs = UINT64_MAX;
    size_t readyPublications = 0;
    for (const auto& pending : m_patches) {
        oldestPendingNs = (std::min)(oldestPendingNs,pending->submittedNs);
        readyPublications += pending->ready.load(std::memory_order_acquire);
    }
    const auto nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    basic_telemetry::SetGauge("SARP.RendererStatePublisher.PendingPublications",static_cast<int64_t>(m_patches.size()));
    basic_telemetry::SetGauge("SARP.RendererStatePublisher.ReadyPublications",static_cast<int64_t>(readyPublications));
    basic_telemetry::SetGauge("SARP.RendererStatePublisher.OldestPendingAgeUs",
        oldestPendingNs == UINT64_MAX ? 0 : static_cast<int64_t>((nowNs - oldestPendingNs)/1000));
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
    lock.unlock();
    for (auto& [pending,rebuild] : rebuilds) {
        // No publisher lock is held while entering the task service. Rejection
        // executes the immutable rebuild inline only in scheduler-free bootstrap.
        if (preparationScheduler) {
            auto operation = std::make_shared<std::function<void()>>(std::move(rebuild));
            if (!preparationScheduler([operation] { auto work = std::move(*operation); work(); })) {
                // Run the error path without compiling on the owner thread.
                basic_telemetry::AddCounter("SARP.RendererStatePublisher.SuccessorRebuildRejected");
                pending->failed.store(true,std::memory_order_relaxed);
                pending->ready.store(true,std::memory_order_release);
            }
        } else rebuild();
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
    for (const auto& pending : m_patches) pending->cancelled.store(true, std::memory_order_release);
    m_patches.clear();
    m_candidate.baseEpoch = 0;
    lock.unlock();
    retired.reset();
}

void RendererStatePublisher::Shutdown() {
    RendererStateCandidate candidate;
    std::vector<std::shared_ptr<PendingPatch>> patches;
    std::shared_ptr<const PublishedRendererState> active;
    std::vector<std::shared_ptr<const PublishedRendererState>> frameStates;
    std::shared_ptr<PublishedStateSource> source;
    {
        std::lock_guard lock(m_mutex);
        candidate = std::move(m_candidate);
        for (const auto& pending : m_patches) pending->cancelled.store(true, std::memory_order_release);
        patches = std::move(m_patches);
        m_preparePublication = {};
        m_prepareExecutable = {};
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
