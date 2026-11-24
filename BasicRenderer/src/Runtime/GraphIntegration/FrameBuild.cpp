#include "Runtime/GraphIntegration/FrameBuild.h"

#include <unordered_map>

#include "Interfaces/IResourceProvider.h"
#include "Resources/Resource.h"
#include "Scene/ECS/RendererECSManager.h"

namespace br::render {

namespace {
thread_local FrameBuild* t_currentFrameBuild = nullptr;

// What a provider gave at one moment (SnapshotResourceProvider).
class ResourceProviderSnapshot final : public org::IResourceProvider {
public:
    explicit ResourceProviderSnapshot(org::IResourceProvider& provider) {
        m_keys = provider.GetSupportedKeys();
        for (const auto& key : m_keys) m_resources.emplace(key, provider.ProvideResource(key));
        m_resolverKeys = provider.GetSupportedResolverKeys();
        for (const auto& key : m_resolverKeys) m_resolvers.emplace(key, provider.ProvideResolver(key));
    }

    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override {
        const auto found = m_resources.find(key);
        return found == m_resources.end() ? nullptr : found->second;
    }
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override { return m_keys; }
    std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override {
        const auto found = m_resolvers.find(key);
        return found == m_resolvers.end() ? nullptr : found->second;
    }
    std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override { return m_resolverKeys; }

private:
    std::vector<org::ResourceIdentifier> m_keys;
    std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>, org::ResourceIdentifier::Hasher> m_resources;
    std::vector<org::ResourceIdentifier> m_resolverKeys;
    std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::IResourceResolver>, org::ResourceIdentifier::Hasher> m_resolvers;
};
}

std::shared_ptr<org::IResourceProvider> SnapshotResourceProvider(org::IResourceProvider& provider) {
    return std::make_shared<ResourceProviderSnapshot>(provider);
}

FrameBuild* CurrentFrameBuild() noexcept {
    return t_currentFrameBuild;
}

ScopedFrameBuild::ScopedFrameBuild(FrameBuild& build) noexcept
    : m_previous(t_currentFrameBuild) {
    t_currentFrameBuild = &build;
}

ScopedFrameBuild::~ScopedFrameBuild() {
    t_currentFrameBuild = m_previous;
}

void WriteResourceEntity(std::shared_ptr<org::Resource> resource, std::function<void(flecs::world&, flecs::entity)> write,
    std::string directoryKey) {
    if (!resource || !write) return;
    if (auto* build = CurrentFrameBuild()) {
        build->outputs.resourceEntityWrites.push_back({ std::move(resource), std::move(write), std::move(directoryKey) });
        return;
    }
    auto& world = RendererECSManager::GetInstance().GetWorld();
    if (auto entity = resource->GetECSEntity()) write(world, entity);
}

} // namespace br::render
