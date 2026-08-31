#pragma once

#include <memory>
#include <atomic>

#include "Interfaces/IResourceResolver.h"
#include "Render/PublishedRendererState.h"

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
        CaptureLatestLease();
    }
    PublishedStateResourceResolver(std::shared_ptr<br::render::PublishedStateSource> source,
        br::render::PublishedResourceQuery query)
        : m_source(std::move(source)), m_query(std::move(query)),
          m_configured(true),
          m_leaseBinding(std::make_shared<LeaseBinding>()) { CaptureLatestLease(); }

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

    std::uint64_t GetContentVersion() const override {
        if (!m_configured) return 0u;
        const bool publishedEnabled = !m_selection ||
            m_selection->publishedEnabled.load(std::memory_order_acquire);
        std::uint64_t entryVersion = 0;
        if (publishedEnabled) {
            CaptureLatestLease();
            const auto lease = BoundLease();
            const auto state = lease ? lease->state : nullptr;
            if (state && state->resourceCatalog) {
                entryVersion = m_exact
                    ? state->resourceCatalog->ContentVersion(m_key)
                    : state->resourceCatalog->ContentVersion(m_query);
            }
        }
        const auto fallbackGeneration = m_fallback
            ? m_fallback->generation.load(std::memory_order_acquire) : 0u;
        const auto selectionGeneration = m_selection
            ? m_selection->generation.load(std::memory_order_acquire) : 0u;
        // Zero means "this resolver is not dynamic" to the render-graph pass
        // builders.  Published resources are dynamic even before their first
        // artifact exists: retaining that absent-state snapshot is what lets a
        // pass refresh its declarations when the resource is first published.
        // Reserve bit zero as the nonzero dynamic-resolver tag.
        return ((entryVersion << 3u) ^ (fallbackGeneration << 2u) ^
            (selectionGeneration << 1u)) | 1u;
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

    std::vector<org::ExternalTimelinePoint> GetExternalTimelineWaits() const override {
        std::vector<org::ExternalTimelinePoint> waits;
        if (m_selection && !m_selection->publishedEnabled.load(std::memory_order_acquire)) return waits;
        CaptureLatestLease();
        const auto lease = BoundLease();
        const auto state = lease ? lease->state : nullptr;
        if (!state || !state->resourceCatalog) return waits;
        const auto appendSelection = [&](const br::render::PublishedResourceSelection& selection) {
            for (const auto& submissions : selection.gpuSubmissions) {
                if (!submissions || submissions->Complete()) continue;
                for (const auto& submission : submissions->submissions) {
                    const auto owner = std::static_pointer_cast<const rhi::TimelinePtr>(
                        submission.TimelineOwner());
                    if (!owner || !*owner) continue;
                    waits.push_back({ owner->Get(), submission.TimelineValue() });
                }
            }
        };
        if (m_exact) {
            if (const auto* selection = state->resourceCatalog->FindSelection(m_key)) {
                appendSelection(*selection);
            }
        } else {
            for (const auto* selection : state->resourceCatalog->FindSelections(m_query)) {
                if (selection) appendSelection(*selection);
            }
        }
        return waits;
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
    void CaptureLatestLease() const {
        if (!m_leaseBinding) return;
        if (!m_source) return;
        auto lease = m_source->LoadLease();
        if (!lease) lease = m_source->AcquireLease(0u);
        const auto current = m_leaseBinding->lease.load(std::memory_order_acquire);
        if (current && current->sequence >= lease->sequence) return;
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
};
