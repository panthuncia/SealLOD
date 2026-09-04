#pragma once

#include <vector>
#include <memory>
#include <atomic>

#include "Interfaces/IResourceResolver.h"
#include "Resources/ResourceGroup.h"

// A resolver that captures any flecs::query<...> by value
class ResourceGroupResolver : public ClonableResolver<ResourceGroupResolver> {
public:
    ResourceGroupResolver() = default;

    explicit ResourceGroupResolver(const std::shared_ptr<ResourceGroup>& resourceGroup)
        : m_resourceGroup(resourceGroup) {
    }

    std::vector<std::shared_ptr<Resource>> Resolve() const override {
		return m_resourceGroup->GetChildren();
    }

    std::shared_ptr<const org::ResolverDeclarationState> CaptureDeclarationState() const override {
        auto state = std::make_shared<org::ResolverDeclarationState>();
        if (!m_resourceGroup) return state;
        const auto version = m_resourceGroup->GetContentVersion();
        if (const auto cached = m_cache->state.load(std::memory_order_acquire);
            cached && cached->contentRevision == version) return cached;
        state->dependencyIdentity = m_cache;
        state->resourceSetIdentity = { version, 0x7267726f75700001ull };
        state->contentRevision = version;
        state->resources = std::make_shared<const org::ResolverResourceList>(m_resourceGroup->GetChildren());
        state->waits = std::make_shared<const std::vector<org::ExternalTimelinePoint>>();
        state->tracked = true;
        m_cache->state.store(state, std::memory_order_release);
        return state;
    }

private:
    struct Cache {
        std::atomic<std::shared_ptr<const org::ResolverDeclarationState>> state;
    };
    std::shared_ptr<ResourceGroup> m_resourceGroup;
    std::shared_ptr<Cache> m_cache = std::make_shared<Cache>();
};
