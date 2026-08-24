#include "Managers/Singletons/RendererECSManager.h"

#include <stdexcept>
#include <format>

#include "Scene/Components.h"

void RendererECSManager::Initialize() {
    if (m_world) {
        return;
    }

    m_mainThreadId = std::this_thread::get_id();
    m_world = std::make_unique<flecs::world>();
    auto& world = *m_world;
    world.component<Components::GlobalMeshLibrary>().add(flecs::Exclusive);
    world.component<Components::DrawStats>("DrawStats").add(flecs::Exclusive);
    world.set<Components::DrawStats>({ 0, {} });
    m_entityPool.Attach(world, "Renderer ECS Entity Pool");
}

void RendererECSManager::Cleanup() {
	RequireMainThread("Cleanup");
    FlushDeferredWorldOperations();
    m_entityPool.Clear();
    m_renderPhaseEntities.clear();
    m_world.reset();
    std::scoped_lock lock(m_deferredWorldOperationsMutex);
    m_deferredWorldOperations.clear();
    m_mainThreadId = {};
}

bool RendererECSManager::IsAlive() const {
    return m_world != nullptr;
}

bool RendererECSManager::IsMainThread() const {
    return m_mainThreadId != std::thread::id{} && std::this_thread::get_id() == m_mainThreadId;
}

flecs::world& RendererECSManager::GetWorld() {
	if (!m_world) {
        throw std::runtime_error("RendererECSManager::GetWorld called before Initialize");
	}
	RequireMainThread("GetWorld");
    return *m_world;
}

flecs::entity RendererECSManager::GetRenderPhaseEntity(const RenderPhase& phase) {
	RequireMainThread("GetRenderPhaseEntity");
    auto it = m_renderPhaseEntities.find(phase);
    if (it != m_renderPhaseEntities.end()) {
        return it->second;
    }

    auto entity = GetWorld().entity(phase.name.c_str());
    m_renderPhaseEntities[phase] = entity;
    return entity;
}

void RendererECSManager::CreateRenderPhaseEntity(const RenderPhase& phase) {
	RequireMainThread("CreateRenderPhaseEntity");
    if (m_renderPhaseEntities.contains(phase)) {
        return;
    }

    m_renderPhaseEntities[phase] = GetWorld().entity(phase.name.c_str());
}

void RendererECSManager::ReserveEntityPool(std::size_t count) {
	RequireMainThread("ReserveEntityPool");
    m_entityPool.Reserve(count);
}

flecs::entity RendererECSManager::AcquirePooledEntity() {
	RequireMainThread("AcquirePooledEntity");
    return m_entityPool.Acquire();
}

void RendererECSManager::ReleasePooledEntity(flecs::entity entity, const std::function<void(flecs::entity)>& cleanup) {
	RequireMainThread("ReleasePooledEntity");
    m_entityPool.Release(entity, cleanup);
}

br::ecs::EcsEntityPoolStats RendererECSManager::GetEntityPoolStats() const {
    return m_entityPool.GetStats();
}

void RendererECSManager::EnqueueDeferredWorldOperation(std::function<void(flecs::world&)>&& op) {
    std::scoped_lock lock(m_deferredWorldOperationsMutex);
    m_deferredWorldOperations.push_back(std::move(op));
}

void RendererECSManager::FlushDeferredWorldOperations() {
	if (m_world) RequireMainThread("FlushDeferredWorldOperations");
    if (!m_world) {
        std::scoped_lock lock(m_deferredWorldOperationsMutex);
        m_deferredWorldOperations.clear();
        return;
    }

    std::deque<std::function<void(flecs::world&)>> pending;
    {
        std::scoped_lock lock(m_deferredWorldOperationsMutex);
        pending.swap(m_deferredWorldOperations);
    }

    auto& world = *m_world;
    for (auto& op : pending) {
        op(world);
    }
}

const std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher>& RendererECSManager::GetRenderPhaseEntities() const {
	RequireMainThread("GetRenderPhaseEntities");
	return m_renderPhaseEntities;
}

void RendererECSManager::RequireMainThread(const char* operation) const {
	if (m_mainThreadId != std::thread::id{} && std::this_thread::get_id() != m_mainThreadId) {
		throw std::runtime_error(std::format(
			"RendererECSManager::{} may only run on the renderer thread", operation));
	}
}
