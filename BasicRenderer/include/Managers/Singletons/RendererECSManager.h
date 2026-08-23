#pragma once

#include <deque>
#include <cstddef>
#include <flecs.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "BasicScene/EcsEntityPool.h"
#include "Render/RenderPhase.h"

class RendererECSManager {
public:
    static RendererECSManager& GetInstance();

    void Initialize();
    void Cleanup();
    bool IsAlive() const;
    bool IsMainThread() const;
    flecs::world& GetWorld();
    flecs::entity GetRenderPhaseEntity(const RenderPhase& phase);
    void CreateRenderPhaseEntity(const RenderPhase& phase);
    void ReserveEntityPool(std::size_t count);
    flecs::entity AcquirePooledEntity();
    void ReleasePooledEntity(flecs::entity entity, const std::function<void(flecs::entity)>& cleanup = {});
    br::ecs::EcsEntityPoolStats GetEntityPoolStats() const;
    void EnqueueDeferredWorldOperation(std::function<void(flecs::world&)>&& op);
    void FlushDeferredWorldOperations();
    const std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher>& GetRenderPhaseEntities() const;

private:
    std::unique_ptr<flecs::world> m_world;
    br::ecs::EcsEntityPool m_entityPool;
    std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher> m_renderPhaseEntities;
    std::deque<std::function<void(flecs::world&)>> m_deferredWorldOperations;
    mutable std::mutex m_deferredWorldOperationsMutex;
    std::thread::id m_mainThreadId{};
    RendererECSManager() = default;
};

inline RendererECSManager& RendererECSManager::GetInstance() {
    static RendererECSManager instance;
    return instance;
}
