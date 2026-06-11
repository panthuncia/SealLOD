#pragma once

class MeshManager;
class ObjectManager;
class IndirectCommandBufferManager;
class ViewManager;
class LightManager;
class EnvironmentManager;
class MaterialManager;
class SkeletonManager;
class TextureFactory;
class TerrainManager;
class ShaderVariantRequestService;

class ManagerInterface {
public:
	ManagerInterface() = default;
	ManagerInterface(
		MeshManager* meshManager,
		ObjectManager* objectManager,
		IndirectCommandBufferManager* indirectCommandBufferManager,
		ViewManager* viewManager,
		LightManager* lightManager,
		EnvironmentManager*  environmentManager,
		MaterialManager* materialManager,
		SkeletonManager* skeletonManager,
		TextureFactory* textureFactory,
		TerrainManager* terrainManager = nullptr,
		ShaderVariantRequestService* shaderVariantRequestService = nullptr
	) : m_pMeshManager(meshManager),
		m_pObjectManager(objectManager),
		m_pIndirectCommandBufferManager(indirectCommandBufferManager),
		m_pViewManager(viewManager),
		m_pLightManager(lightManager), 
		m_pEnvironmentManager(environmentManager),
		m_pMaterialManager(materialManager),
		m_pSkeletonManager(skeletonManager),
		m_pTextureFactory(textureFactory),
		m_pTerrainManager(terrainManager),
		m_pShaderVariantRequestService(shaderVariantRequestService) {
	}

	void SetManagers(MeshManager* meshManager,
		ObjectManager* objectManager,
		IndirectCommandBufferManager* indirectCommandBufferManager,
		ViewManager* viewManager,
		LightManager* lightManager,
		EnvironmentManager* environmentManager,
		MaterialManager* materialManager,
		SkeletonManager* skeletonManager,
		TextureFactory* textureFactory,
		TerrainManager* terrainManager = nullptr,
		ShaderVariantRequestService* shaderVariantRequestService = nullptr) {
		m_pMeshManager = meshManager;
		m_pObjectManager = objectManager;
		m_pIndirectCommandBufferManager = indirectCommandBufferManager;
		m_pViewManager = viewManager;
		m_pLightManager = lightManager;
		m_pEnvironmentManager = environmentManager;
		m_pMaterialManager = materialManager;
		m_pSkeletonManager = skeletonManager;
		m_pTextureFactory = textureFactory;
		m_pTerrainManager = terrainManager;
		m_pShaderVariantRequestService = shaderVariantRequestService;
	}

	MeshManager* GetMeshManager() const { return m_pMeshManager; }
	ObjectManager* GetObjectManager() const { return m_pObjectManager; }
	IndirectCommandBufferManager* GetIndirectCommandBufferManager() const { return m_pIndirectCommandBufferManager; }
	ViewManager* GetViewManager() const { return m_pViewManager; }
	LightManager* GetLightManager() const { return m_pLightManager; }
	EnvironmentManager* GetEnvironmentManager() const { return m_pEnvironmentManager; }
	MaterialManager* GetMaterialManager() const { return m_pMaterialManager; }
	SkeletonManager* GetSkeletonManager() const { return m_pSkeletonManager; }
	TextureFactory* GetTextureFactory() const { return m_pTextureFactory; }
	TerrainManager* GetTerrainManager() const { return m_pTerrainManager; }
	ShaderVariantRequestService* GetShaderVariantRequestService() const { return m_pShaderVariantRequestService; }
private:
	MeshManager* m_pMeshManager = nullptr;
	ObjectManager* m_pObjectManager = nullptr;
	IndirectCommandBufferManager* m_pIndirectCommandBufferManager = nullptr;
	ViewManager* m_pViewManager = nullptr;
	LightManager* m_pLightManager = nullptr;
	EnvironmentManager* m_pEnvironmentManager = nullptr;
	MaterialManager* m_pMaterialManager = nullptr;
	SkeletonManager* m_pSkeletonManager = nullptr;
	TextureFactory* m_pTextureFactory = nullptr;
	TerrainManager* m_pTerrainManager = nullptr;
	ShaderVariantRequestService* m_pShaderVariantRequestService = nullptr;
};
