#pragma once

#include <flecs.h>
#include <functional>
#include <vector>
#include <memory>

#include "Interfaces/IResourceResolver.h"
#include "Resources/components.h"

// A resolver that captures any flecs::query<...> by value
class ECSResourceResolver : public ClonableResolver<ECSResourceResolver> {
public:
    ECSResourceResolver() = default;

    // Capture any flecs query (e.g. flecs::query<flecs::entity>, flecs::query<Cs...>)
    template<typename QueryT>
    explicit ECSResourceResolver(QueryT query) {
        // Move the query into a closure to keep it alive for the resolver lifetime.
        m_enumerator = [q = std::move(query)](std::vector<std::shared_ptr<Resource>>& out) {
            q.each([&](flecs::entity e) {
#if BUILD_TYPE == BUILD_TYPE_DEBUG
                assert(e.has<Components::Resource>() && "Entity does not have Resource component");
#endif
                if (const auto res = e.try_get<Components::Resource>(); res) {
                    if (const auto shared = res->resource.lock(); shared) {
                        out.push_back(shared);
                    }
                }
            });
        };
    }

    std::vector<std::shared_ptr<Resource>> Resolve() const override {
        std::vector<std::shared_ptr<Resource>> resources;
        if (m_enumerator) {
            m_enumerator(resources);
        }
        return resources;
    }

    std::shared_ptr<const org::ResolverDeclarationState> CaptureDeclarationState() const override {
        auto state = std::make_shared<org::ResolverDeclarationState>();
        state->resources = std::make_shared<const org::ResolverResourceList>(Resolve());
        state->waits = std::make_shared<const std::vector<org::ExternalTimelinePoint>>();
        return state;
    }

private:
    std::function<void(std::vector<std::shared_ptr<Resource>>&)> m_enumerator;
};
