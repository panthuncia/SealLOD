#pragma once

#include <memory>
#include <atomic>

#include "Interfaces/IResourceResolver.h"
#include "Render/PublishedRendererState.h"

class PublishedStateResourceResolver final : public ClonableResolver<PublishedStateResourceResolver> {
public:
    PublishedStateResourceResolver() = default;
    PublishedStateResourceResolver(std::shared_ptr<br::render::PublishedStateSource> source,
        br::render::PublishedResourceKey key, std::shared_ptr<org::Resource> fallback = {})
        : m_source(std::move(source)), m_key(key), m_exact(true),
          m_fallback(std::make_shared<FallbackState>()) {
        m_fallback->resource.store(std::move(fallback), std::memory_order_release);
    }
    PublishedStateResourceResolver(std::shared_ptr<br::render::PublishedStateSource> source,
        br::render::PublishedResourceQuery query)
        : m_source(std::move(source)), m_query(std::move(query)) {}

    std::vector<std::shared_ptr<org::Resource>> Resolve() const override {
        const auto source = m_source;
        const auto state = source ? source->Load() : nullptr;
        if (!state || !state->resourceCatalog) return ResolveFallback();
        if (!m_exact) return state->resourceCatalog->FindAll(m_query);
        const auto resources = state->resourceCatalog->Find(m_key);
        return resources && !resources->empty() ? *resources : ResolveFallback();
    }

    std::uint64_t GetContentVersion() const override {
        const auto epoch = m_source ? m_source->Epoch() : 0u;
        const auto fallbackGeneration = m_fallback
            ? m_fallback->generation.load(std::memory_order_acquire) : 0u;
        return (epoch << 1u) ^ fallbackGeneration;
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
    std::shared_ptr<FallbackState> m_fallback;
};
