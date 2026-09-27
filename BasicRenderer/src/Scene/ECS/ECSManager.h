#pragma once
#include <BasicScene/SceneWorldManager.h>
#include <flecs.h>
#include <unordered_map>

#include "BasicRenderer/Pipeline/RenderPhase.h"

class ECSManager {
public:
    static ECSManager& GetInstance();
	void Initialize();
	flecs::world& GetWorld() { return br::scene::SceneWorldManager::GetInstance().GetWorld(); }
    void Cleanup();
    bool IsAlive() const { return br::scene::SceneWorldManager::GetInstance().IsAlive(); }
    flecs::entity GetRenderPhaseEntity(const RenderPhase& phase) {
        auto it = m_renderPhaseEntities.find(phase);
        if (it != m_renderPhaseEntities.end()) {
            return it->second;
        }
        else {
			m_renderPhaseEntities[phase] = GetWorld().entity(phase.name.c_str());
			return m_renderPhaseEntities[phase];
        }
    }
	void CreateRenderPhaseEntity(const RenderPhase& phase) {
        if (m_renderPhaseEntities.contains(phase)) {
            return;
		}
		m_renderPhaseEntities[phase] = GetWorld().entity(phase.name.c_str());
	}
    const std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher>& GetRenderPhaseEntities() const {
        return m_renderPhaseEntities;
	}

private:
	std::unordered_map<RenderPhase, flecs::entity, RenderPhase::Hasher> m_renderPhaseEntities;
    ECSManager() = default;
};

inline ECSManager& ECSManager::GetInstance() {
    static ECSManager instance;
    return instance;
}