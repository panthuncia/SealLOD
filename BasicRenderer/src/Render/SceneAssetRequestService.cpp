#include "Render/SceneAssetRequestService.h"

#include <stdexcept>

#include "Managers/TerrainManager.h"
#include "Resources/Texture.h"

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

void SceneAssetRequestService::AdvanceTextureUpload(
    TextureAsset& texture, TextureUploadAdvanceMode mode) const {
    if (!m_textures) throw std::runtime_error("scene texture service is unavailable");
    (void)texture.EnsureUploaded(*m_textures, mode);
}

void SceneAssetRequestService::RequestTextureResidency(
    TextureAsset& texture, TextureUploadAdvanceMode mode) const {
    if (!m_textures) throw std::runtime_error("scene texture service is unavailable");
    (void)texture.RequestAllOrNothingUpload(*m_textures, mode);
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
