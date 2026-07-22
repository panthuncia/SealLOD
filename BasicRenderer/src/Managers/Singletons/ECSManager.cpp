#include "Managers/Singletons/ECSManager.h"

#include "Scene/Components.h"

void ECSManager::Initialize() {
	br::scene::SceneWorldManager::GetInstance().Initialize([](flecs::world& world) {
		world.component<Components::ActiveScene>().add(flecs::Exclusive);

		flecs::entity game = world.pipeline()
			.with(flecs::System)
			.build();
		world.set<Components::GameScene>({ game });

		FlecsStatsImport(world);

		// Creates REST server on default port (27750)
		world.set<flecs::Rest>({});

		world.set_threads(8);
	});
}

void ECSManager::Cleanup()
{
	m_renderPhaseEntities.clear();
	br::scene::SceneWorldManager::GetInstance().Cleanup();
}

