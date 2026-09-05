#pragma once

#include <memory>
#include <atomic>
#include <algorithm>
#include <stdexcept>

#include "Interfaces/IResourceResolver.h"
#include "Render/PublishedRendererState.h"
#include "Resources/Resource.h"

class PublishedStateResourceResolver final : public org::ClonableResolver<PublishedStateResourceResolver> {
public:
    PublishedStateResourceResolver() = default;
    PublishedStateResourceResolver(std::shared_ptr<br::render::PublishedStateSource> source,
        br::render::PublishedResourceKey key, std::shared_ptr<org::Resource> fallback = {},
        bool publishedEnabled = true)
        : m_source(std::move(source)), m_key(key), m_exact(true),
          m_configured(true),
          m_fallback(std::make_shared<FallbackState>()),
          m_selection(std::make_shared<SelectionState>()),
          m_leaseBinding(std::make_shared<LeaseBinding>()) {
        m_fallback->resource.store(std::move(fallback), std::memory_order_release);
        m_selection->publishedEnabled.store(publishedEnabled, std::memory_order_release);
        // Exact resolvers have independently mutable fallback/selection policy.
        // Clones share this state; unrelated policies cannot share capture keys.
        m_dependencyIdentity = m_selection;
        CaptureLatestLease();
    }
    PublishedStateResourceResolver(std::shared_ptr<br::render::PublishedStateSource> source,
        br::render::PublishedResourceQuery query)
        : m_source(std::move(source)), m_query(std::move(query)),
          m_configured(true),
          m_leaseBinding(std::make_shared<LeaseBinding>()) {
		m_dependencyIdentity = m_source ? m_source->ResolverDependencyIdentity(m_query) : nullptr;
		CaptureLatestLease();
	}

    std::vector<std::shared_ptr<org::Resource>> Resolve() const override {
        if (m_selection && !m_selection->publishedEnabled.load(std::memory_order_acquire)) {
            return ResolveFallback();
        }
        // Resolution is permitted without a preceding content-version query.
        // Bind the newest published lease here so a resolver cannot remain on
        // its bootstrap fallback merely because the graph reused its layout.
        CaptureLatestLease();
        const auto lease = BoundLease();
        const auto state = lease ? lease->state : nullptr;
        if (!state || !state->resourceCatalog) return ResolveFallback();
        if (!m_exact) return state->resourceCatalog->FindAll(m_query);
        const auto selection = state->resourceCatalog->FindSelection(m_key);
        const auto resources = selection ? selection->resources : state->resourceCatalog->Find(m_key);
        return resources && !resources->empty() ? *resources : ResolveFallback();
    }

    std::shared_ptr<const org::ResolverDeclarationState> CaptureDeclarationState() const override {
        CaptureLatestLease();
        return CaptureDeclarationState(org::ResolverCaptureContext(BoundLease()));
    }

    std::shared_ptr<const org::ResolverDeclarationState> CaptureDeclarationState(
        const org::ResolverCaptureContext& context) const override {
        org::ResolverDeclarationState result;
        if (!m_configured) return std::make_shared<const org::ResolverDeclarationState>();
        if (!context.Is<br::render::PublishedManifestLease>())
            throw std::invalid_argument("Published resolver requires a manifest capture lease");
        const auto lease = context.Get<br::render::PublishedManifestLease>();
        // A null lease represents bootstrap. It must never fall through to the
        // process source: the caller selected this publication explicitly.
        const auto leaseSequence = lease ? lease->sequence : 0u;
        const bool publishedEnabled = !m_selection ||
            m_selection->publishedEnabled.load(std::memory_order_acquire);
        const auto fallbackGeneration = m_fallback
            ? m_fallback->generation.load(std::memory_order_acquire) : 0u;
        const auto selectionGeneration = m_selection
            ? m_selection->generation.load(std::memory_order_acquire) : 0u;
        if (const auto cached = m_declarationCache->value.load(std::memory_order_acquire);
            cached && cached->lease == lease && cached->leaseSequence == leaseSequence &&
            cached->fallbackGeneration == fallbackGeneration &&
            cached->selectionGeneration == selectionGeneration) {
            return cached->state;
        }
        result.dependencyIdentity = m_dependencyIdentity;
        result.publicationLease = lease;
        result.tracked = true;
        std::uint64_t entryVersion = 0;
        std::vector<std::shared_ptr<org::Resource>> resources;
        std::vector<org::ExternalTimelinePoint> waits;
        if (publishedEnabled) {
            const auto state = lease ? lease->state : nullptr;
            if (state && state->resourceCatalog) {
                entryVersion = m_exact
                    ? state->resourceCatalog->ContentVersion(m_key)
                    : state->resourceCatalog->ContentVersion(m_query);
                resources = ResolveFrom(state);
                const auto appendSelection = [&](const br::render::PublishedResourceSelection& selection) {
                    for (const auto& submissions : selection.gpuSubmissions) {
                        if (!submissions) continue;
                        for (const auto& submission : submissions->submissions) {
                            const auto owner = std::static_pointer_cast<const rhi::TimelinePtr>(submission.TimelineOwner());
                            if (!owner || !*owner) continue;
                            waits.push_back({ owner->Get(), submission.TimelineValue() });
                        }
                    }
                };
                if (m_exact) {
                    if (const auto* selection = state->resourceCatalog->FindSelection(m_key)) appendSelection(*selection);
                } else {
                    for (const auto* selection : state->resourceCatalog->FindSelections(m_query))
                        if (selection) appendSelection(*selection);
                }
            }
        } else {
            resources = ResolveFallback();
        }
        result.contentRevision = entryVersion;

        auto mix = [](std::uint64_t& hash, std::uint64_t value) {
            hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
        };
        std::uint64_t setLow = 0x7365746964656e74ull;
        std::uint64_t setHigh = 0x7265736f75726365ull;
        for (const auto& resource : resources) {
            const auto id = resource ? resource->GetGlobalResourceID() : 0u;
            mix(setLow, id); mix(setHigh, id ^ 0x94d049bb133111ebull);
        }
        mix(setLow, resources.size());
        result.resourceSetIdentity = { setLow, setHigh };

        std::sort(waits.begin(), waits.end(), [](const auto& lhs, const auto& rhs) {
            const auto lh = lhs.timeline.GetHandle();
            const auto rh = rhs.timeline.GetHandle();
            return lh.index != rh.index ? lh.index < rh.index :
                (lh.generation != rh.generation ? lh.generation < rh.generation : lhs.value < rhs.value);
        });
        std::vector<org::ExternalTimelinePoint> normalized;
        for (const auto& wait : waits) {
            if (!normalized.empty()) {
                const auto previous = normalized.back().timeline.GetHandle();
                const auto current = wait.timeline.GetHandle();
                if (previous.index == current.index && previous.generation == current.generation) {
                    normalized.back().value = (std::max)(normalized.back().value, wait.value);
                    continue;
                }
            }
            normalized.push_back(wait);
        }
        std::uint64_t waitRevision = 0x7761697472657601ull;
        for (const auto& wait : normalized) {
            mix(waitRevision, wait.timeline.GetHandle().index);
            mix(waitRevision, wait.timeline.GetHandle().generation);
            mix(waitRevision, wait.value);
        }
        result.waitRevision = waitRevision;
        result.resources = std::make_shared<const org::ResolverResourceList>(std::move(resources));
        result.waits = std::make_shared<const std::vector<org::ExternalTimelinePoint>>(std::move(normalized));
        auto cached = std::make_shared<CachedDeclaration>();
        cached->lease = lease;
        cached->leaseSequence = leaseSequence;
        cached->fallbackGeneration = fallbackGeneration;
        cached->selectionGeneration = selectionGeneration;
        cached->state = std::make_shared<const org::ResolverDeclarationState>(std::move(result));
        const auto captured = cached->state;
        m_declarationCache->value.store(std::move(cached), std::memory_order_release);
        return captured;
    }

    // Returns the immutable manifest lease used by this resolver. Consumers
    // that need logical metadata alongside a capacity-classed resource must
    // read both from the same published state instead of inferring counts from
    // the backing allocation size.
    [[nodiscard]] std::shared_ptr<const br::render::PublishedRendererState> ResolvePublishedState() const {
        CaptureLatestLease();
        const auto lease = BoundLease();
        return lease ? lease->state : nullptr;
    }

    // Resolve against an explicit frame lease. Pass execution must use this
    // form when metadata and backing resources have to remain coherent for the
    // whole frame; Resolve() intentionally follows the newest process state.
    std::vector<std::shared_ptr<org::Resource>> ResolveFrom(
        const std::shared_ptr<const br::render::PublishedRendererState>& state) const {
        if (m_selection && !m_selection->publishedEnabled.load(std::memory_order_acquire)) {
            return ResolveFallback();
        }
        if (!state || !state->resourceCatalog) return ResolveFallback();
        if (!m_exact) return state->resourceCatalog->FindAll(m_query);
        const auto selection = state->resourceCatalog->FindSelection(m_key);
        const auto resources = selection ? selection->resources : state->resourceCatalog->Find(m_key);
        return resources && !resources->empty() ? *resources : ResolveFallback();
    }

    void SetPublishedEnabled(bool enabled) noexcept {
        if (!m_selection) return;
        const bool previous = m_selection->publishedEnabled.exchange(enabled, std::memory_order_acq_rel);
        if (previous != enabled) m_selection->generation.fetch_add(1, std::memory_order_acq_rel);
    }

    void ClearFallback() noexcept {
        if (!m_fallback) return;
        m_fallback->resource.store({}, std::memory_order_release);
        m_fallback->generation.fetch_add(1, std::memory_order_acq_rel);
    }

private:
    struct FallbackState {
        std::atomic<std::shared_ptr<org::Resource>> resource;
        std::atomic<std::uint64_t> generation{ 0 };
    };
    struct SelectionState {
        std::atomic<bool> publishedEnabled{ true };
        std::atomic<std::uint64_t> generation{ 0 };
    };
    struct LeaseBinding {
        std::atomic<std::shared_ptr<const br::render::PublishedManifestLease>> lease;
        std::atomic<std::uint64_t> sequence{ 0 };
    };
    struct CachedDeclaration {
        std::shared_ptr<const br::render::PublishedManifestLease> lease;
        std::uint64_t leaseSequence = 0;
        std::uint64_t fallbackGeneration = 0;
        std::uint64_t selectionGeneration = 0;
        std::shared_ptr<const org::ResolverDeclarationState> state;
    };
    struct DeclarationCache {
        std::atomic<std::shared_ptr<const CachedDeclaration>> value;
    };
    void CaptureLatestLease() const {
        if (!m_leaseBinding) return;
        if (!m_source) return;
        auto lease = m_source->LoadLease();
        if (!lease) lease = m_source->AcquireLease(0u);
        const auto current = m_leaseBinding->lease.load(std::memory_order_acquire);
        if (!lease || (current && current->sequence >= lease->sequence)) return;
        m_leaseBinding->lease.store(lease, std::memory_order_release);
        m_leaseBinding->sequence.store(lease->sequence, std::memory_order_release);
    }
    [[nodiscard]] std::shared_ptr<const br::render::PublishedManifestLease> BoundLease() const {
        if (!m_leaseBinding) return {};
        return m_leaseBinding->lease.load(std::memory_order_acquire);
    }
    std::vector<std::shared_ptr<org::Resource>> ResolveFallback() const {
        const auto fallback = m_fallback
            ? m_fallback->resource.load(std::memory_order_acquire) : nullptr;
        return fallback ? std::vector<std::shared_ptr<org::Resource>>{ fallback }
                        : std::vector<std::shared_ptr<org::Resource>>{};
    }
    std::shared_ptr<br::render::PublishedStateSource> m_source;
    br::render::PublishedResourceKey m_key;
    br::render::PublishedResourceQuery m_query;
    bool m_exact = false;
    bool m_configured = false;
    std::shared_ptr<FallbackState> m_fallback;
    std::shared_ptr<SelectionState> m_selection;
    std::shared_ptr<LeaseBinding> m_leaseBinding;
    std::shared_ptr<DeclarationCache> m_declarationCache = std::make_shared<DeclarationCache>();
	std::shared_ptr<const void> m_dependencyIdentity;
};
