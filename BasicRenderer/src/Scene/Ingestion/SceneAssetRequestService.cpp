#include <BasicRenderer/Assets/TexturePipeline.h>
#include <BasicRenderer/Streaming/SceneAssetRequestService.h>

#include <stdexcept>

#include "Terrain/Residency/TerrainManager.h"
#include "BasicRenderer/Assets/Texture.h"

namespace br::render {

void SceneAssetRequestService::Configure(TextureFactory* textures, TerrainManager* terrain,
    MaterialManager* materials) noexcept {
    m_textures = textures;
    m_terrain = terrain;
    m_materials = materials;
}

bool SceneAssetRequestService::CanUploadTextures() const noexcept { return m_textures != nullptr; }
bool SceneAssetRequestService::CanPublishTerrain() const noexcept {
    return m_terrain != nullptr && m_textures != nullptr;
}

void SceneAssetRequestService::LoadTexture(
    const std::shared_ptr<TextureAsset>& texture, std::function<void(bool loaded)> ready) const {
    br::textures::LoadTexture(texture, std::move(ready));
}

std::uint32_t SceneAssetRequestService::PublishTerrain(
    const TerrainMaterialDesc& description) const {
    if (!CanPublishTerrain()) throw std::runtime_error("scene terrain service is unavailable");
    return m_terrain->SetActiveTerrain(description, m_textures, m_materials);
}

void SceneAssetRequestService::ProcessPendingTerrainUpdates() const {
    if (m_terrain) m_terrain->ProcessPendingUpdates();
}

bool SceneAssetRequestService::ActivatePublishedTerrainState(
    const std::shared_ptr<const PublishedRendererState>& published) const {
    return m_terrain && m_terrain->TryActivatePublishedTerrainState(published);
}

}
