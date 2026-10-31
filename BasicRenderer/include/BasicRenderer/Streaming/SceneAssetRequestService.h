#pragma once

#include <functional>
#include <cstdint>
#include <memory>

class MaterialManager;
class TerrainManager;
class TextureAsset;
class TextureFactory;
struct TerrainMaterialDesc;
enum class TextureUploadAdvanceMode : std::uint8_t;

namespace br::render {
struct PublishedRendererState;

// Narrow renderer-owned boundary for asset work used by scene ingestion.
// The backing factories/managers never escape to producer or worker code.
class SceneAssetRequestService {
public:
    void Configure(TextureFactory* textures, TerrainManager* terrain,
        MaterialManager* materials) noexcept;

    [[nodiscard]] bool CanUploadTextures() const noexcept;
    [[nodiscard]] bool CanPublishTerrain() const noexcept;
    // Loads a texture through the texture pipeline (br::textures::LoadTexture).
    void LoadTexture(const std::shared_ptr<TextureAsset>& texture, std::function<void(bool loaded)> ready = {}) const;
    [[nodiscard]] std::uint32_t PublishTerrain(const TerrainMaterialDesc& description) const;
    void ProcessPendingTerrainUpdates() const;
    [[nodiscard]] bool ActivatePublishedTerrainState(
        const std::shared_ptr<const PublishedRendererState>& published) const;

private:
    TextureFactory* m_textures = nullptr;
    TerrainManager* m_terrain = nullptr;
    MaterialManager* m_materials = nullptr;
};

}
