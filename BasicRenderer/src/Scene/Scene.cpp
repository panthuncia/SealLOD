#include <BasicScene/Scene.h>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <execution>
#include <flecs.h>
#include <BasicScene/SceneWorldManager.h>

#include "Utilities/Utilities.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ViewManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include <BasicScene/Components.h>
#include "Materials/Material.h"
#include "Managers/ObjectManager.h"
#include "Managers/MeshManager.h"
#include "Managers/LightManager.h"
#include "Managers/IndirectCommandBufferManager.h"
#include "Managers/SkeletonManager.h"
#include "Managers/MaterialManager.h"
#include "Mesh/MeshInstance.h"
#include "Mesh/VertexFlags.h"
#include "Render/RendererComponents.h"
#include "Animation/AnimationController.h"
#include "Utilities/MathUtils.h"
#include "Resources/Sampler.h"
#include "Resources/components.h"
#include "Resources/PixelBuffer.h"
#include "Render/DrawWorkload.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"

namespace {
	std::atomic<uint64_t> globalStableSceneId = 0;

	void EnsureSceneWorldInitialized() {
		auto& worldManager = br::scene::SceneWorldManager::GetInstance();
		if (worldManager.IsAlive()) {
			return;
		}

		worldManager.Initialize([](flecs::world& world) {
			world.component<Components::ActiveScene>().add(flecs::Exclusive);
			world.component<Components::GlobalMeshLibrary>().add(flecs::Exclusive);
			world.component<Components::DrawStats>("DrawStats").add(flecs::Exclusive);
			world.component<Components::RenderBridgeSceneDiff>().add(flecs::Exclusive);
			world.component<Components::ActiveScene>().add(flecs::OnInstantiate, flecs::Inherit);
			world.add<Components::GlobalMeshLibrary>();
			world.set<Components::DrawStats>({ 0, {} });
			world.set<Components::RenderBridgeSceneDiff>({});

			flecs::entity game = world.pipeline()
				.with(flecs::System)
				.build();
			world.set<Components::GameScene>({ game });
			world.import<flecs::stats>();
			world.set<flecs::Rest>({});
			world.set_threads(8);

				// Mark entities dirty when local transforms change
			world.observer<Components::Position>()
				.event(flecs::OnSet)
				.each([](flecs::entity e, Components::Position&) {
					e.add<Components::TransformDirty>();
				});
			world.observer<Components::Rotation>()
				.event(flecs::OnSet)
				.each([](flecs::entity e, Components::Rotation&) {
					e.add<Components::TransformDirty>();
				});
			world.observer<Components::Scale>()
				.event(flecs::OnSet)
				.each([](flecs::entity e, Components::Scale&) {
					e.add<Components::TransformDirty>();
				});
			world.observer<Components::MeshInstances>()
				.event(flecs::OnSet)
				.each([](flecs::entity e, Components::MeshInstances&) {
					e.add<Components::RenderBridgeContentDirty>();
				});
			world.observer<Components::MeshInstances>()
				.event(flecs::OnRemove)
				.each([](flecs::entity e, Components::MeshInstances&) {
					if (const auto* stableSceneID = e.try_get<Components::StableSceneID>()) {
						auto& diff = e.world().get_mut<Components::RenderBridgeSceneDiff>();
						diff.removedRenderableIDs.push_back(stableSceneID->value);
						++diff.generation;
					}
				});
			world.observer<Components::Camera>()
				.event(flecs::OnRemove)
				.each([](flecs::entity e, Components::Camera&) {
					if (const auto* stableSceneID = e.try_get<Components::StableSceneID>()) {
						auto& diff = e.world().get_mut<Components::RenderBridgeSceneDiff>();
						diff.removedCameraIDs.push_back(stableSceneID->value);
						++diff.generation;
					}
				});
			world.observer<Components::Light>()
				.event(flecs::OnRemove)
				.each([](flecs::entity e, Components::Light&) {
					if (const auto* stableSceneID = e.try_get<Components::StableSceneID>()) {
						auto& diff = e.world().get_mut<Components::RenderBridgeSceneDiff>();
						diff.removedLightIDs.push_back(stableSceneID->value);
						++diff.generation;
					}
				});
			world.observer<Components::Name>()
				.event(flecs::OnSet)
				.each([](flecs::entity e, Components::Name&) {
					e.add<Components::RenderBridgeContentDirty>();
				});

			// Transform system: only recompute matrices for dirty entities
			world.system<const Components::Position, const Components::Rotation, const Components::Scale, const Components::Matrix*, Components::Matrix>()
				.with<Components::Active>()
				.with<Components::TransformDirty>()
				.term_at(3).parent().cascade()
				.cached().cache_kind(flecs::QueryCacheAll)
				.each([](flecs::entity e, const Components::Position& position, const Components::Rotation& rotation, const Components::Scale& scale, const Components::Matrix* matrix, Components::Matrix& output) {
					XMMATRIX matRotation = XMMatrixRotationQuaternion(rotation.rot);
					XMMATRIX matTranslation = XMMatrixTranslationFromVector(position.pos);
					XMMATRIX matScale = XMMatrixScalingFromVector(scale.scale);
					output.matrix = (matScale * matRotation * matTranslation);
					if (matrix != nullptr) {
						output.matrix = output.matrix * matrix->matrix;
					}
					e.remove<Components::TransformDirty>();
					e.add<Components::TransformUpdatedThisFrame>();
				});
		});
	}

	flecs::world& GetSceneWorld() {
		EnsureSceneWorldInitialized();
		return br::scene::SceneWorldManager::GetInstance().GetWorld();
	}

	Components::StableSceneID MakeStableSceneID() {
		return { globalStableSceneId.fetch_add(1, std::memory_order_relaxed) + 1 };
	}

	std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
	}

	MaterialRasterFlags ComposeRuntimeRasterFlags(Mesh& mesh, const Material& material) {
		MaterialRasterFlags rasterFlags = material.Technique().rasterFlags;
		if ((mesh.GetPerMeshCBData().vertexFlags & VERTEX_SKINNED) != 0u) {
			rasterFlags |= MaterialRasterFlagsSkinned;
		}
		return rasterFlags;
	}

	MaterialRasterFlags ComposeRuntimeRasterFlags(Mesh& mesh) {
		return ComposeRuntimeRasterFlags(mesh, *mesh.material);
	}

	void AssignStableSceneID(flecs::entity entity) {
		entity.set<Components::StableSceneID>(MakeStableSceneID());
	}

	void ReassignStableSceneIDsRecursive(flecs::entity entity) {
		if (!entity.is_alive()) {
			return;
		}

		AssignStableSceneID(entity);
		entity.children([](flecs::entity child) {
			ReassignStableSceneIDsRecursive(child);
		});
	}

	void PropagateTransformDirtyToChildren(flecs::entity parent, std::unordered_set<uint64_t>& visited) {
		parent.children([&](flecs::entity child) {
			if (child.has<Components::Active>() && visited.insert(child.id()).second) {
				child.add<Components::TransformDirty>();
				PropagateTransformDirtyToChildren(child, visited);
			}
		});
	}

	template <typename Fn>
	void VisitSceneDescendants(flecs::entity root, Fn&& fn) {
		root.children([&](flecs::entity child) {
			if (child.has<Components::SceneRoot>()) {
				return;
			}

			fn(child);
			VisitSceneDescendants(child, fn);
		});
	}
}

std::atomic<uint64_t> Scene::globalSceneCount = 0;

Scene::Scene(){
	m_sceneID = globalSceneCount.fetch_add(1, std::memory_order_relaxed);

    getNumDirectionalLightCascades = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades");
    setDirectionalLightCascadeSplits = SettingsManager::GetInstance().getSettingSetter<std::vector<float>>("directionalLightCascadeSplits");
	getMeshShadersEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("enableMeshShader");

    //Initialize ECS scene
	auto& world = GetSceneWorld();
    ECSSceneRoot = world.entity().add<Components::SceneRoot>()
		.set<Components::Position>({0, 0, 0})
		.set<Components::Rotation>({0, 0, 0, 1})
		.set<Components::Scale>({1, 1, 1})
		.set<Components::Matrix>(DirectX::XMMatrixIdentity())
		.set<Components::Name>("Scene Root");
	AssignStableSceneID(ECSSceneRoot);
	ECSSceneRoot = ECSSceneRoot;
    world.set_pipeline(world.get<Components::GameScene>().pipeline);
	m_renderableEntityPool.Attach(world, "Scene Renderable Entity Pool");
}

flecs::entity Scene::CreateDirectionalLightECS(std::wstring name, XMFLOAT3 color, float intensity, XMFLOAT3 direction, bool shadowCasting){
	return CreateLightECS(name, Components::LightType::Directional, { 0, 0, 0 }, color, intensity, { 0, 0, 0 }, direction, 0, 0, shadowCasting);
}

flecs::entity Scene::CreatePointLightECS(std::wstring name, XMFLOAT3 position, XMFLOAT3 color, float intensity, float constantAttenuation, float linearAttenuation, float quadraticAttenuation, bool shadowCasting) {
	return CreateLightECS(name, Components::LightType::Point, position, color, intensity, { constantAttenuation, linearAttenuation, quadraticAttenuation }, {0, 0, 0}, 0, 0, shadowCasting);
}

flecs::entity Scene::CreateSpotLightECS(std::wstring name, XMFLOAT3 position, XMFLOAT3 color, float intensity, XMFLOAT3 direction, float innerConeAngle, float outerConeAngle, float constantAttenuation, float linearAttenuation, float quadraticAttenuation, bool shadowCasting) {
	return CreateLightECS(name, Components::LightType::Spot, position, color, intensity, { constantAttenuation, linearAttenuation, quadraticAttenuation }, direction, innerConeAngle, outerConeAngle, shadowCasting);
}

flecs::entity Scene::CreateLightECS(std::wstring name, Components::LightType type, XMFLOAT3 position, XMFLOAT3 color, float intensity, XMFLOAT3 attenuation, XMFLOAT3 direction, float innerConeAngle, float outerConeAngle, bool shadowCasting) {
	auto& world = GetSceneWorld();
	//float maxRange = 20.0f;
	XMVECTOR normalizedAttenuationVec = XMVector3Normalize(XMLoadFloat3(&attenuation));
	XMFLOAT3 normalizedAttenuation;
	XMStoreFloat3(&normalizedAttenuation, normalizedAttenuationVec);
	auto maxRange = CalculateLightRadius(intensity, normalizedAttenuation.x, normalizedAttenuation.y, normalizedAttenuation.z);

	LightInfo lightInfo;
	lightInfo.type = type;
	lightInfo.posWorldSpace = XMLoadFloat3(&position);
	DirectX::XMVECTOR lightColor = XMVector3Normalize(XMLoadFloat3(&color));
	// Set W to intensity
	lightColor = XMVectorSetW(lightColor, intensity);
	lightInfo.color = lightColor;
	float nearPlane = 0.01f;
	float farPlane = maxRange;
	lightInfo.attenuation = normalizedAttenuationVec;
	lightInfo.dirWorldSpace = XMLoadFloat3(&direction);
	lightInfo.innerConeAngle = cos(innerConeAngle);
	lightInfo.outerConeAngle = cos(outerConeAngle);
	lightInfo.shadowViewInfoIndex = -1;
	lightInfo.nearPlane = nearPlane;
	lightInfo.farPlane = farPlane;
	lightInfo.shadowCaster = shadowCasting;
	lightInfo.maxRange = maxRange;
	lightInfo.shadowSourceRadius = 0.0f;
	lightInfo.shadowSourceAngleDegrees = 0.0f;
	switch(type){
	case Components::LightType::Spot:
		lightInfo.boundingSphere = ComputeConeBoundingSphere(XMLoadFloat3(&position), XMLoadFloat3(&direction), maxRange, outerConeAngle);
		break;
	case Components::LightType::Point:
		lightInfo.boundingSphere = { { position.x, position.y, position.z, maxRange } };
		break;
	case Components::LightType::Directional:
		lightInfo.shadowSourceAngleDegrees = CLodVirtualShadowDefaultDirectionalSourceAngleDegrees;
		break;
	}

	flecs::entity entity = world.entity();
	entity.child_of(ECSSceneRoot)
		.set<Components::Light>({ type, color, normalizedAttenuation, maxRange, lightInfo })
		.set<Components::Position>(position)
		.set<Components::Scale>({ 1, 1, 1 })
		.set<Components::Matrix>(DirectX::XMMatrixIdentity())
		.set<Components::Name>(ws2s(name));
	AssignStableSceneID(entity);

	if (direction.x != 0 || direction.y != 0 || direction.z != 0) {
		entity.set<Components::Rotation>(QuaternionFromAxisAngle(direction));
	}
	else {
		entity.set<Components::Rotation>({ 0, 0, 0, 1 });
	}


	if (ECSSceneRoot.has<Components::ActiveScene>()) {
		ActivateLight(entity);
		entity.add<Components::Active>();
	}

	float aspect = 1.0f;

	switch (type) {
	case Components::LightType::Spot:
		entity.set<Components::ProjectionMatrix>({ XMMatrixPerspectiveFovRH(outerConeAngle * 2, aspect, nearPlane, farPlane) });
		break;
	case Components::LightType::Point:
		entity.set<Components::ProjectionMatrix>({ XMMatrixPerspectiveFovRH(XM_PI / 2, aspect, nearPlane, farPlane) });
		break;
	}

	std::vector<std::array<ClippingPlane, 6>> frustumPlanes;
	switch (type) {
	case Components::LightType::Directional:
		break; // Directional is special-cased, frustrums are in world space, calculated during cascade setup
	case Components::LightType::Spot: {
		frustumPlanes.push_back(GetFrustumPlanesPerspective(1.0f, outerConeAngle * 2, nearPlane, farPlane));
		entity.set<Components::FrustumPlanes>({ frustumPlanes });
		break;
	case Components::LightType::Point: {
		for (int i = 0; i < 6; i++) {
			frustumPlanes.push_back(GetFrustumPlanesPerspective(1.0f, XM_PI / 2, nearPlane, farPlane)); // TODO: All of these are the same.
		}
		entity.set<Components::FrustumPlanes>({ frustumPlanes });
		break;
	}
	}
	}
	
	return entity;
}

void Scene::ActivateRenderable(flecs::entity& entity) {
	auto& world = GetSceneWorld();

	auto meshInstances = entity.try_get<Components::MeshInstances>();
	//auto alphaTestMeshInstances = entity.try_get<Components::AlphaTestMeshInstances>();
	//auto blendMeshInstances = entity.try_get<Components::BlendMeshInstances>();

	auto& globalMeshLibrary = world.get_mut<Components::GlobalMeshLibrary>();
	auto& drawStats = world.get_mut<Components::DrawStats>();

	bool useMeshletReorderedVertices = getMeshShadersEnabled();

	if (meshInstances) {
		for (auto& meshInstance : meshInstances->meshInstances) {

			if (meshInstance->HasSkin()) {
				const auto skinBegin = std::chrono::steady_clock::now();
				meshInstance->SetCurrentSkeletonManager(m_managerInterface.GetSkeletonManager());
				auto skinInst = meshInstance->GetSkin();
				m_managerInterface.GetSkeletonManager()->AcquireSkinningInstance(skinInst);
				meshInstance->SetSkinningInstanceSlot(skinInst->GetSkinningInstanceSlot());
				if (skinInst->GetAnimationCount() > 0u) {
					skinInst->SetAnimation(0); // TODO: Animation selection
				}
				meshInstance->SyncSkinningStateFromSkeleton();
				m_renderableActivationSkinUs += ElapsedUs(skinBegin);
			}

			const auto materialBegin = std::chrono::steady_clock::now();
			auto effectiveMaterial = meshInstance->GetEffectiveMaterial();
			if (!effectiveMaterial) {
				effectiveMaterial = Material::GetDefaultMaterial();
				meshInstance->SetMaterialOverride(effectiveMaterial);
			}

			// Register material residency. MaterialManager drains texture/material GPU updates from dirty queues.
			unsigned int materialDataIndex = 0;
			if (m_renderableActivationBatchActive) {
				const auto materialID = effectiveMaterial->GetMaterialID();
				auto materialIt = m_batchedMaterialUsages.find(materialID);
				if (materialIt == m_batchedMaterialUsages.end()) {
					materialDataIndex = m_managerInterface.GetMaterialManager()->IncrementMaterialUsageCount(
						*effectiveMaterial,
						m_managerInterface.GetTextureFactory());
					m_batchedMaterialUsages.emplace(
						materialID,
						BatchedMaterialUsage{ effectiveMaterial.get(), materialDataIndex, 0u });
				} else {
					materialDataIndex = materialIt->second.slot;
					++materialIt->second.deferredCount;
				}
			} else {
				materialDataIndex = m_managerInterface.GetMaterialManager()->IncrementMaterialUsageCount(
					*effectiveMaterial,
					m_managerInterface.GetTextureFactory());
			}
			const MaterialRasterFlags runtimeRasterFlags = ComposeRuntimeRasterFlags(*meshInstance->GetMesh(), *effectiveMaterial);
			unsigned int rasterBucketIndex = 0;
			if (m_renderableActivationBatchActive) {
				const auto rasterKey = static_cast<uint32_t>(runtimeRasterFlags);
				auto rasterIt = m_batchedRasterBucketUsages.find(rasterKey);
				if (rasterIt == m_batchedRasterBucketUsages.end()) {
					rasterBucketIndex = m_managerInterface.GetMaterialManager()->AcquireRasterBucket(runtimeRasterFlags);
					m_batchedRasterBucketUsages.emplace(
						rasterKey,
						BatchedRasterBucketUsage{ runtimeRasterFlags, rasterBucketIndex, 0u });
				} else {
					rasterBucketIndex = rasterIt->second.slot;
					++rasterIt->second.deferredCount;
				}
			} else {
				rasterBucketIndex = m_managerInterface.GetMaterialManager()->AcquireRasterBucket(runtimeRasterFlags);
			}
			if (meshInstance->HasMaterialOverride()) {
				auto meshData = meshInstance->GetMesh()->GetPerMeshCBData();
				meshData.materialDataIndex = materialDataIndex;
				meshData.rasterBucketIndex = rasterBucketIndex;
				meshInstance->SetPerMeshOverrideBufferView(m_managerInterface.GetMeshManager()->AllocatePerMeshOverrideBuffer(meshData));
			} else {
				meshInstance->GetMesh()->SetMaterialDataIndex(materialDataIndex);
				meshInstance->GetMesh()->SetRasterBucketIndex(rasterBucketIndex);
			}
			m_renderableActivationMaterialUs += ElapsedUs(materialBegin);

			// Register mesh if not already present
			const auto globalMeshBegin = std::chrono::steady_clock::now();
			if (!globalMeshLibrary.meshes.contains(meshInstance->GetMesh()->GetGlobalID())) {
				globalMeshLibrary.meshes[meshInstance->GetMesh()->GetGlobalID()] = meshInstance->GetMesh();
				++globalMeshLibrary.generation;
				m_managerInterface.GetMeshManager()->AddMesh(meshInstance->GetMesh(), useMeshletReorderedVertices);
			}
			m_renderableActivationGlobalMeshUs += ElapsedUs(globalMeshBegin);
			const auto meshInstanceBegin = std::chrono::steady_clock::now();
			m_managerInterface.GetMeshManager()->AddMeshInstance(meshInstance.get(), useMeshletReorderedVertices);
			m_renderableActivationMeshInstanceUs += ElapsedUs(meshInstanceBegin);

			// Update draw stats and indirect workload counts
			const auto workloadBegin = std::chrono::steady_clock::now();
            auto& mesh = *meshInstance->GetMesh();
            ForEachMeshDrawWorkload(mesh, *effectiveMaterial, [&](const DrawWorkloadKey& workloadKey) {
                if (drawStats.numDrawsPerTechnique.find(workloadKey) == drawStats.numDrawsPerTechnique.end()) {
                    drawStats.numDrawsPerTechnique[workloadKey] = 0;
                }
                drawStats.numDrawsPerTechnique[workloadKey]++;

				if (m_renderableActivationBatchActive) {
					m_batchedActivationWorkloads.insert(workloadKey);
				} else {
					m_managerInterface.GetIndirectCommandBufferManager()->RegisterWorkload(workloadKey);
					m_managerInterface.GetIndirectCommandBufferManager()->UpdateBuffersForWorkload(
						workloadKey,
						drawStats.numDrawsPerTechnique[workloadKey]);
				}
            });
			m_renderableActivationWorkloadUs += ElapsedUs(workloadBegin);
			drawStats.numDrawsInScene++;
		
		}
	}
}

void Scene::ActivateLight(flecs::entity& entity) {
}

void Scene::ActivateCamera(flecs::entity& entity) {
	entity.add<Components::PrimaryCamera>();
}

void Scene::ProcessEntitySkins(bool overrideExistingSkins) {
	auto& world = GetSceneWorld();
	std::vector<flecs::entity> renderables;
	VisitSceneDescendants(ECSSceneRoot, [&](flecs::entity entity) {
		if (entity.has<Components::MeshInstances>()) {
			renderables.push_back(entity);
		}
	});
	world.defer_begin();
	for (auto entity : renderables) {
		auto oldMeshInstances = entity.try_get<Components::MeshInstances>();
		if (!oldMeshInstances) {
			continue;
		}

		Components::MeshInstances meshInstances;
		meshInstances.generation = oldMeshInstances->generation + 1;

		bool addSkin = false;
		for (const auto& meshInstance : oldMeshInstances->meshInstances) {
			auto rebuiltInstance = MeshInstance::CreateUnique(meshInstance->GetMesh());
			if (rebuiltInstance->HasSkin()) {
				addSkin = true;
			}
			meshInstances.meshInstances.push_back(std::move(rebuiltInstance));
		}

		entity.set<Components::MeshInstances>(meshInstances);
		if (addSkin) {
			entity.add<Components::Skinned>();
		}
		else if (overrideExistingSkins || entity.has<Components::Skinned>()) {
			entity.remove<Components::Skinned>();
		}
	}
	world.defer_end();
}

void Scene::ReserveRenderableEntityPool(std::size_t count) {
	m_renderableEntityPool.Reserve(count);
}

Scene::RenderableEntityPoolStats Scene::GetRenderableEntityPoolStats() const {
	RenderableEntityPoolStats stats;
	stats.pool = m_renderableEntityPool.GetStats();
	stats.entityAcquireUs = m_renderableEntityAcquireUs;
	stats.entitySetupUs = m_renderableEntitySetupUs;
	stats.entityActivateUs = m_renderableEntityActivateUs;
	stats.activationSkinUs = m_renderableActivationSkinUs;
	stats.activationMaterialUs = m_renderableActivationMaterialUs;
	stats.activationGlobalMeshUs = m_renderableActivationGlobalMeshUs;
	stats.activationMeshInstanceUs = m_renderableActivationMeshInstanceUs;
	stats.activationWorkloadUs = m_renderableActivationWorkloadUs;
	stats.activationIndirectFlushUs = m_renderableActivationIndirectFlushUs;
	stats.meshInstanceCreateUs = m_renderableMeshInstanceCreateUs;
	stats.entityReleaseUs = m_renderableEntityReleaseUs;
	return stats;
}

void Scene::BeginRenderableActivationBatch() {
	m_renderableActivationBatchActive = true;
	m_batchedActivationWorkloads.clear();
	m_batchedMaterialUsages.clear();
	m_batchedRasterBucketUsages.clear();
}

void Scene::EndRenderableActivationBatch() {
	if (!m_renderableActivationBatchActive) {
		return;
	}

	m_renderableActivationBatchActive = false;
	const auto materialFlushBegin = std::chrono::steady_clock::now();
	if (auto* materialManager = m_managerInterface.GetMaterialManager()) {
		for (const auto& [_, usage] : m_batchedMaterialUsages) {
			if (usage.material && usage.deferredCount > 0u) {
				materialManager->IncrementMaterialUsageCount(
					*usage.material,
					m_managerInterface.GetTextureFactory(),
					usage.deferredCount);
			}
		}
		for (const auto& [_, usage] : m_batchedRasterBucketUsages) {
			if (usage.deferredCount > 0u) {
				materialManager->AcquireRasterBucket(usage.flags, usage.deferredCount);
			}
		}
	}
	m_renderableActivationMaterialUs += ElapsedUs(materialFlushBegin);
	m_batchedMaterialUsages.clear();
	m_batchedRasterBucketUsages.clear();

	auto* indirectCommandBufferManager = m_managerInterface.GetIndirectCommandBufferManager();
	if (!indirectCommandBufferManager || m_batchedActivationWorkloads.empty()) {
		m_batchedActivationWorkloads.clear();
		return;
	}

	auto& drawStats = GetSceneWorld().get_mut<Components::DrawStats>();
	const auto flushBegin = std::chrono::steady_clock::now();
	for (const auto& workloadKey : m_batchedActivationWorkloads) {
		const auto it = drawStats.numDrawsPerTechnique.find(workloadKey);
		indirectCommandBufferManager->RegisterWorkload(workloadKey);
		indirectCommandBufferManager->UpdateBuffersForWorkload(
			workloadKey,
			it != drawStats.numDrawsPerTechnique.end() ? it->second : 0u);
	}
	m_renderableActivationIndirectFlushUs += ElapsedUs(flushBegin);
	m_batchedActivationWorkloads.clear();
}

flecs::entity Scene::CreateRenderableEntityECS(const std::vector<std::shared_ptr<Mesh>>& meshes, std::wstring name) {
	return CreateRenderableEntityECS(meshes, std::move(name), std::nullopt, DirectX::XMMatrixIdentity());
}

flecs::entity Scene::CreateRenderableEntityECS(
	const std::vector<std::shared_ptr<Mesh>>& meshes,
	std::wstring name,
	std::optional<std::uint64_t> stableSceneID,
	DirectX::XMMATRIX initialMatrix,
	bool assignNames) {
	const auto acquireBegin = std::chrono::steady_clock::now();
	flecs::entity entity = m_renderableEntityPool.Acquire();
	m_renderableEntityAcquireUs += ElapsedUs(acquireBegin);

	const auto setupBegin = std::chrono::steady_clock::now();
	DirectX::XMVECTOR scale{ DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f) };
	DirectX::XMVECTOR rotation{ DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f) };
	DirectX::XMVECTOR translation{ DirectX::XMVectorZero() };
	DirectX::XMMatrixDecompose(std::addressof(scale), std::addressof(rotation), std::addressof(translation), initialMatrix);
	entity.child_of(ECSSceneRoot)
		.set<Components::Rotation>({ rotation })
		.set<Components::Position>({ translation })
		.set<Components::Scale>({ scale })
		.set<Components::Matrix>(initialMatrix);
	if (assignNames) {
		const auto narrowName = ws2s(name);
		entity
			.set_name((narrowName + "_" + std::to_string(entity.id())).c_str())
			.set<Components::Name>(narrowName);
	}
	if (stableSceneID && *stableSceneID != 0) {
		entity.set<Components::StableSceneID>({ *stableSceneID });
	} else {
		AssignStableSceneID(entity);
	}
	Components::MeshInstances meshInstances;
    for (auto& mesh : meshes) {
		if (mesh == nullptr) {
			continue;
		}
		bool skinned = mesh->HasBaseSkin();
		if (skinned) {
			entity.add<Components::Skinned>();
		}
		const auto meshInstanceBegin = std::chrono::steady_clock::now();
		meshInstances.meshInstances.push_back(std::move(MeshInstance::CreateUnique(mesh)));
		m_renderableMeshInstanceCreateUs += ElapsedUs(meshInstanceBegin);
		if (skinned) {
			auto skeleton = mesh->GetBaseSkin()->CopySkeleton();
			meshInstances.meshInstances.back()->SetSkeleton(skeleton);
			entity.add<Components::Skinned>();
		}
    }
	if (!meshInstances.meshInstances.empty()) {
		entity.set<Components::MeshInstances>(meshInstances);
	}

	// If scene is active, add object & manage meshes
	if (ECSSceneRoot.has<Components::ActiveScene>()) {
		const auto activateBegin = std::chrono::steady_clock::now();
		ActivateRenderable(entity);
		m_renderableEntityActivateUs += ElapsedUs(activateBegin);
		entity.add<Components::Active>();
	}

	m_renderableEntitySetupUs += ElapsedUs(setupBegin);
    return entity;
}

flecs::entity Scene::CreateInstancedRenderableEntityECS(
	const std::vector<std::shared_ptr<Mesh>>& meshes,
	std::wstring name,
	std::optional<std::uint64_t> stableSceneID,
	const std::vector<DirectX::XMMATRIX>& instanceMatrices,
	bool assignNames) {
	const auto initialMatrix = instanceMatrices.empty() ? DirectX::XMMatrixIdentity() : instanceMatrices.front();
	const auto acquireBegin = std::chrono::steady_clock::now();
	flecs::entity entity = m_renderableEntityPool.Acquire();
	m_renderableEntityAcquireUs += ElapsedUs(acquireBegin);

	const auto setupBegin = std::chrono::steady_clock::now();
	entity.child_of(ECSSceneRoot)
		.set<Components::Rotation>({})
		.set<Components::Position>({})
		.set<Components::Scale>({})
		.set<Components::Matrix>(DirectX::XMMatrixIdentity());
	if (assignNames) {
		const auto narrowName = ws2s(name);
		entity
			.set_name((narrowName + "_" + std::to_string(entity.id())).c_str())
			.set<Components::Name>(narrowName);
	}
	if (stableSceneID && *stableSceneID != 0) {
		entity.set<Components::StableSceneID>({ *stableSceneID });
	} else {
		AssignStableSceneID(entity);
	}

	Components::MeshInstances meshInstances;
	Components::InstanceTransforms instanceTransforms;
	instanceTransforms.transforms.reserve(instanceMatrices.size());
	for (const auto& matrix : instanceMatrices) {
		instanceTransforms.transforms.push_back({ matrix });
	}

	for (auto& mesh : meshes) {
		if (mesh == nullptr) {
			continue;
		}
		if (mesh->HasBaseSkin()) {
			entity.add<Components::Skinned>();
		}
		const auto meshInstanceBegin = std::chrono::steady_clock::now();
		meshInstances.meshInstances.push_back(std::move(MeshInstance::CreateUnique(mesh)));
		m_renderableMeshInstanceCreateUs += ElapsedUs(meshInstanceBegin);
	}

	if (!meshInstances.meshInstances.empty()) {
		entity.set<Components::MeshInstances>(meshInstances);
		entity.set<Components::InstanceTransforms>(instanceTransforms);
	}

	if (ECSSceneRoot.has<Components::ActiveScene>()) {
		const auto activateBegin = std::chrono::steady_clock::now();
		ActivateRenderable(entity);
		m_renderableEntityActivateUs += ElapsedUs(activateBegin);
		entity.add<Components::Active>();
	}

	m_renderableEntitySetupUs += ElapsedUs(setupBegin);
	return entity;
}

void Scene::ReleaseRenderableEntityECS(flecs::entity entity) {
	const auto releaseBegin = std::chrono::steady_clock::now();
	m_renderableEntityPool.Release(entity, [](flecs::entity e) {
		e.remove<Components::Active>();
		if (e.has<Components::MeshInstances>()) {
			e.remove<Components::MeshInstances>();
		}
		e.remove<Components::InstanceTransforms>();
		e.remove<Components::Skinned>();
		e.remove<Components::SkinningPassEligible>();
		e.remove<Components::SkipShadowPass>();
		e.remove<Components::RenderBridgeContentDirty>();
		e.remove<Components::TransformDirty>();
		e.remove<Components::TransformUpdatedThisFrame>();
		e.remove<Components::StableSceneID>();
		e.remove<Components::Name>();
		e.remove<Components::Matrix>();
		e.remove<Components::Position>();
		e.remove<Components::Rotation>();
		e.remove<Components::Scale>();
	});
	m_renderableEntityReleaseUs += ElapsedUs(releaseBegin);
}

flecs::entity Scene::CreateNodeECS(std::wstring name) {
	auto& world = GetSceneWorld();
	flecs::entity entity = world.entity();
	entity.child_of(ECSSceneRoot)
		.set_name((ws2s(name) + "_" + std::to_string(entity.id())).c_str())
		.add<Components::SceneNode>()
		.set<Components::Rotation>({ 0, 0, 0, 1 })
		.set<Components::Position>({ 0, 0, 0 })
		.set<Components::Scale>({ 1, 1, 1 })
		.set<Components::Matrix>(DirectX::XMMatrixIdentity())
		.set<Components::Name>(ws2s(name));
	AssignStableSceneID(entity);
	return entity;

	if (ECSSceneRoot.has<Components::ActiveScene>()) {
		entity.add<Components::Active>();
	}
}

bool Scene::SetMeshInstanceMaterialOverride(flecs::entity entity, std::size_t meshInstanceIndex, std::shared_ptr<Material> material) {
	if (!entity.is_alive() || !material) {
		return false;
	}

	auto* meshInstances = entity.try_get_mut<Components::MeshInstances>();
	if (!meshInstances || meshInstanceIndex >= meshInstances->meshInstances.size()) {
		return false;
	}

	auto& meshInstance = meshInstances->meshInstances[meshInstanceIndex];
	if (!meshInstance || !meshInstance->GetMesh()) {
		return false;
	}

	auto mesh = meshInstance->GetMesh();
	auto oldMaterial = meshInstance->GetEffectiveMaterial();
	if (oldMaterial == material) {
		return true;
	}

	const bool active = IsActive()
		&& m_managerInterface.GetMeshManager() != nullptr
		&& m_managerInterface.GetMaterialManager() != nullptr;

	if (active && oldMaterial) {
		m_managerInterface.GetMaterialManager()->ReleaseRasterBucket(ComposeRuntimeRasterFlags(*mesh, *oldMaterial));
		m_managerInterface.GetMaterialManager()->DecrementMaterialUsageCount(*oldMaterial);
	}

	if (active && oldMaterial) {
		auto& drawStats = GetSceneWorld().get_mut<Components::DrawStats>();
		ForEachMeshDrawWorkload(*mesh, *oldMaterial, [&](const DrawWorkloadKey& workloadKey) {
			auto it = drawStats.numDrawsPerTechnique.find(workloadKey);
			if (it != drawStats.numDrawsPerTechnique.end() && it->second > 0) {
				--it->second;
			}
			if (m_managerInterface.GetIndirectCommandBufferManager()) {
				m_managerInterface.GetIndirectCommandBufferManager()->UpdateBuffersForWorkload(
					workloadKey,
					it != drawStats.numDrawsPerTechnique.end() ? it->second : 0u);
			}
		});
	}

	meshInstance->SetMaterialOverride(material == mesh->material ? nullptr : material);

	if (active) {
		auto& overrideView = meshInstance->GetPerMeshOverrideBufferView();
		m_managerInterface.GetMeshManager()->ReleasePerMeshOverrideBuffer(overrideView);

		const auto materialDataIndex = m_managerInterface.GetMaterialManager()->IncrementMaterialUsageCount(
			*material,
			m_managerInterface.GetTextureFactory());
		auto meshData = mesh->GetPerMeshCBData();
		meshData.materialDataIndex = materialDataIndex;
		meshData.rasterBucketIndex = m_managerInterface.GetMaterialManager()->AcquireRasterBucket(ComposeRuntimeRasterFlags(*mesh, *material));

		if (material != mesh->material) {
			meshInstance->SetPerMeshOverrideBufferView(m_managerInterface.GetMeshManager()->AllocatePerMeshOverrideBuffer(meshData));
			meshInstance->SetPerMeshBufferIndex(static_cast<uint32_t>(
				meshInstance->GetPerMeshOverrideBufferView()->GetOffset() / sizeof(PerMeshCB)));
		} else if (mesh->GetPerMeshBufferView()) {
			meshInstance->SetPerMeshBufferIndex(static_cast<uint32_t>(
				mesh->GetPerMeshBufferView()->GetOffset() / sizeof(PerMeshCB)));
		}

		if (auto* indirectCommandBufferManager = m_managerInterface.GetIndirectCommandBufferManager()) {
			ForEachMeshDrawWorkload(*mesh, *material, [&](const DrawWorkloadKey& workloadKey) {
				indirectCommandBufferManager->RegisterWorkload(workloadKey);
				auto& drawStats = GetSceneWorld().get_mut<Components::DrawStats>();
				auto& count = drawStats.numDrawsPerTechnique[workloadKey];
				++count;
				indirectCommandBufferManager->UpdateBuffersForWorkload(workloadKey, count);
			});
		}

		spdlog::debug(
			"Scene::SetMeshInstanceMaterialOverride: entity={} meshInstance={} mesh={} oldMaterial={} newMaterial={} perMeshBufferIndex={}",
			entity.id(),
			meshInstanceIndex,
			mesh->GetGlobalID(),
			oldMaterial ? oldMaterial->GetMaterialID() : 0u,
			material->GetMaterialID(),
			meshInstance->GetPerMeshBufferIndex());
	}

	meshInstances->BumpGeneration();
	entity.add<Components::RenderBridgeContentDirty>();
	return true;
}

flecs::entity Scene::GetRoot() const {
    return ECSSceneRoot;
}

uint64_t Scene::GetSceneID() const {
	return m_sceneID;
}

void Scene::Update(float elapsedSeconds) {

	for (auto& node : animatedEntitiesByID) {
		auto& entity = node.second;
		AnimationController* animationController = entity.try_get_mut<AnimationController>();
#if defined(_DEBUG)
		if (animationController == nullptr) {
			spdlog::error("AnimationController is null for entity with ID: {}", node.first);
			return;
		}
#endif
	    auto& transform = animationController->GetUpdatedTransform(elapsedSeconds);
		entity.set<Components::Rotation>(transform.rot);
		entity.set<Components::Position>(transform.pos);
		entity.set<Components::Scale>(transform.scale);
	}
	for (auto& child : m_childScenes) {
		child->Update(elapsedSeconds);
	}
}

void Scene::SetCamera(XMFLOAT3 lookAt, XMFLOAT3 up, float fov, float aspect, float zNear, float zFar) {
		
    if (m_primaryCamera.is_valid()) {

        m_managerInterface.GetIndirectCommandBufferManager()->UnregisterBuffers(m_primaryCamera.id());
    }

    CameraInfo info;
	auto planes = GetFrustumPlanesPerspective(aspect, fov, zNear, zFar);
	info.view = XMMatrixIdentity();
	info.viewInverse = XMMatrixIdentity();
	info.unjitteredProjection = XMMatrixPerspectiveFovRH(fov, aspect, zFar, zNear);  // Note the reversed near/far for reversed Z
	info.jitteredProjection = info.unjitteredProjection;
	info.viewProjection = DirectX::XMMatrixMultiply(info.view, info.unjitteredProjection);
	info.projectionInverse = XMMatrixInverse(nullptr, info.unjitteredProjection);
	info.prevView = info.view;
	info.prevJitteredProjection = info.jitteredProjection;
	info.prevUnjitteredProjection = info.unjitteredProjection;
	info.positionWorldSpace = { 0.0f, 0.0f, 0.0f, 1.0f };
	info.clippingPlanes[0] = planes[0];
	info.clippingPlanes[1] = planes[1];
	info.clippingPlanes[2] = planes[2];
	info.clippingPlanes[3] = planes[3];
	info.clippingPlanes[4] = planes[4];
	info.clippingPlanes[5] = planes[5];
	info.zFar = zFar;
	info.zNear = zNear;
	info.aspectRatio = aspect;
	info.fov = fov;

	auto& world = GetSceneWorld();
	Components::Camera camera = {};
	camera.fov = fov;
	camera.aspect = aspect;
	camera.zNear = zNear;
	camera.zFar = zFar;
	camera.info = info;
	auto entity = world.entity()
		.set<Components::Camera>(camera)
		.set<Components::Position>({ 0, 0, 0 })
		.set<Components::Rotation>({ 0, 0, 0 })
		.set<Components::Scale>({ 1, 1, 1 })
		.set<Components::Matrix>(DirectX::XMMatrixIdentity())
		.set<Components::Name>("Primary Camera")
		.child_of(ECSSceneRoot);
	AssignStableSceneID(entity);

	if (ECSSceneRoot.has<Components::ActiveScene>()) {
		ActivateCamera(entity);
		entity.add<Components::Active>();
	}

	setDirectionalLightCascadeSplits(calculateCascadeSplits(getNumDirectionalLightCascades(), zNear, zFar, zFar));

	m_primaryCamera = entity;
}

flecs::entity& Scene::GetPrimaryCamera() {
    return m_primaryCamera;
}

bool Scene::HasUsablePrimaryCamera() const {
	return m_primaryCamera
		&& m_primaryCamera.is_alive()
		&& m_primaryCamera.has<Components::Camera>();
}

Components::Position& Scene::GetPrimaryCameraPosition() {
	return m_primaryCamera.get_mut<Components::Position>();
}

Components::Rotation& Scene::GetPrimaryCameraRotation() {
	return m_primaryCamera.get_mut<Components::Rotation>();
}

void Scene::PropagateTransforms() {
	auto& world = GetSceneWorld();

	// Lazy-init helper queries (stored as members so they're destroyed with the Scene)
	if (!m_propagateQueriesBuilt) {
		m_updatedCleanupQuery = world.query_builder<>()
			.with<Components::TransformUpdatedThisFrame>()
			.build();
		m_dirtyQuery = world.query_builder<>()
			.with<Components::TransformDirty>()
			.with<Components::Active>()
			.build();
		m_propagateQueriesBuilt = true;
	}

	// Clear previous frame's TransformUpdatedThisFrame tags
	world.defer_begin();
	m_updatedCleanupQuery.each([](flecs::entity e) {
		e.remove<Components::TransformUpdatedThisFrame>();
	});
	world.defer_end();

	// Propagate TransformDirty from dirty entities to their active descendants
	std::vector<flecs::entity> dirtyRoots;
	m_dirtyQuery.each([&](flecs::entity e) {
		dirtyRoots.push_back(e);
	});

	if (!dirtyRoots.empty()) {
		std::unordered_set<uint64_t> visited;
		for (auto& e : dirtyRoots) {
			visited.insert(e.id());
		}
		world.defer_begin();
		for (auto& e : dirtyRoots) {
			PropagateTransformDirtyToChildren(e, visited);
		}
		world.defer_end();
	}

	// Run all systems (transform system now filters by TransformDirty)
	world.progress();
}

void Scene::PostUpdate() {
	for (auto& child : m_childScenes) {
		child->PostUpdate();
	}
}

std::shared_ptr<Scene> Scene::AppendScene(std::shared_ptr<Scene> scene) {
	if (!scene) {
		return nullptr;
	}
	auto root = scene->GetRoot();

	root.child_of(ECSSceneRoot);
	if (ECSSceneRoot.has<Components::ActiveScene>()) { // If this scene is active, activate the new scene
		scene->Activate(m_managerInterface);
	}
	m_childScenes.push_back(scene);

	return scene;
}

void Scene::MakeResident() {
	auto& world = GetSceneWorld();
	std::vector<flecs::entity> renderables;
	std::vector<flecs::entity> cameras;
	std::vector<flecs::entity> lights;

	VisitSceneDescendants(ECSSceneRoot, [&](flecs::entity entity) {
		if (entity.has<Components::MeshInstances>()) {
			renderables.push_back(entity);
		}
		if (entity.has<Components::Camera>()) {
			cameras.push_back(entity);
		}
		if (entity.has<Components::Light>()) {
			lights.push_back(entity);
		}
	});

	for (auto& entity : renderables) {
		ActivateRenderable(entity);
	}
	for (auto& entity : cameras) {
		ActivateCamera(entity);
	}
	for (auto& entity : lights) {
		ActivateLight(entity);
	}
}

void Scene::MakeNonResident() {
	if (m_managerInterface.GetMeshManager() == nullptr || m_managerInterface.GetMaterialManager() == nullptr) {
		return;
	}

	std::vector<flecs::entity> renderables;

	VisitSceneDescendants(ECSSceneRoot, [&](flecs::entity entity) {
		if (entity.has<Components::MeshInstances>()) {
			renderables.push_back(entity);
		}
	});

	for (auto& entity : renderables) {
		auto meshInstances = entity.try_get<Components::MeshInstances>();
		if (!meshInstances) {
			continue;
		}

		for (auto& meshInstance : meshInstances->meshInstances) {
			auto mesh = meshInstance->GetMesh();
			if (!mesh) {
				continue;
			}

			m_managerInterface.GetMeshManager()->RemoveMeshInstance(meshInstance.get());
			auto material = meshInstance->GetEffectiveMaterial();
			if (!material) {
				material = mesh->material;
			}
			if (material) {
				m_managerInterface.GetMaterialManager()->ReleaseRasterBucket(ComposeRuntimeRasterFlags(*mesh, *material));
				m_managerInterface.GetMaterialManager()->DecrementMaterialUsageCount(*material);
			}
			m_managerInterface.GetMeshManager()->ReleasePerMeshOverrideBuffer(meshInstance->GetPerMeshOverrideBufferView());
		}
	}
}

void Scene::Deactivate() {
	for (auto& child : m_childScenes) {
		if (child) {
			child->Deactivate();
		}
	}

	if (!ECSSceneRoot.is_alive()) {
		m_managerInterface = {};
		return;
	}

	if (IsActive()) {
		MakeNonResident();
		ECSSceneRoot.remove<Components::ActiveScene>();
	}

	m_managerInterface = {};
}

Scene::~Scene() {
	m_updatedCleanupQuery = {};
	m_dirtyQuery = {};
	m_propagateQueriesBuilt = false;
	Deactivate();
	m_renderableEntityPool.Clear();
}

void activate_hierarchy(flecs::entity src) {

	src.add<Components::Active>();
	src.add<Components::TransformDirty>();

	src.children([&](flecs::entity e) {
		activate_hierarchy(e);
		});
}

void ActivateHierarchy(flecs::entity src) {
	src.world().defer_begin();
	activate_hierarchy(src);
	src.world().defer_end();
}

void Scene::ActivateAllAnimatedEntities() {
	auto& world = GetSceneWorld();
	world.defer_begin();
	for (auto& e : animatedEntitiesByID) {
		auto& entity = e.second;
		entity.add<Components::Active>();
	}
	world.defer_end();
	for (auto & child : m_childScenes) {
		child->ActivateAllAnimatedEntities();
	}
}

void Scene::Activate(ManagerInterface managerInterface) {
	m_managerInterface = managerInterface;

	ActivateHierarchy(ECSSceneRoot);
	ActivateAllAnimatedEntities();

	ECSSceneRoot.add<Components::ActiveScene>();
	ECSSceneRoot.add<Components::Active>();

	MakeResident();
}

bool Scene::IsActive() const {
	return ECSSceneRoot.is_alive() && ECSSceneRoot.has<Components::ActiveScene>();
}

void recurse_hierarchy(flecs::entity src, flecs::entity dst_parent = {}) {
	if (src.has<Components::SkeletonRoot>()) {
		return; // Skip skeleton roots, they are handled separately
	}
	flecs::entity cloned = src.clone();
	AssignStableSceneID(cloned);

	if (dst_parent.is_alive()) {
		cloned.child_of(dst_parent);
	}

	src.children([&](flecs::entity e) {
		recurse_hierarchy(e, cloned);
		});
}

void CloneHierarchy(flecs::entity src, flecs::entity dst_parent) {
	src.world().defer_begin();
	src.children([&](flecs::entity e) {
		recurse_hierarchy(e, dst_parent);
		});
	src.world().defer_end();
}

std::shared_ptr<Scene> Scene::Clone() const {
	auto newScene = std::make_shared<Scene>();
	newScene->ECSSceneRoot = ECSSceneRoot.clone();
	ReassignStableSceneIDsRecursive(newScene->ECSSceneRoot);
	CloneHierarchy(ECSSceneRoot, newScene->ECSSceneRoot);
	for (auto& childScene : m_childScenes) {
		newScene->m_childScenes.push_back(childScene->Clone());
	}
	newScene->ProcessEntitySkins(true);
	return newScene;
}

void Scene::DisableShadows() {
	auto& world = GetSceneWorld();
	std::vector<flecs::entity> renderables;
	VisitSceneDescendants(ECSSceneRoot, [&](flecs::entity entity) {
		if (entity.has<Components::MeshInstances>()) {
			renderables.push_back(entity);
		}
	});
	world.defer_begin();
	for (auto entity : renderables) {
		entity.add<Components::SkipShadowPass>();
	}
	world.defer_end();
}
