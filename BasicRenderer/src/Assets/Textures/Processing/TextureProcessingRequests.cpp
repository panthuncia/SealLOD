#include <BasicRenderer/Assets/TextureProcessingRequests.h>

#include "Assets/Textures/Processing/TextureProcessingManager.h"

namespace br::assets {

std::wstring GetExistingCachePathForFile(
    const TextureFileMeta& meta) {
    return TextureProcessingManager::GetInstance().GetExistingCachePathForFile(meta);
}

StochasticTextureArtifactResult RequestStochasticArtifactsBlocking(
    const std::shared_ptr<TextureSourceData>& sourceData,
    const TextureFileMeta& meta,
    const StochasticTextureArtifactSettings& settings) {
    return TextureProcessingManager::GetInstance().RequestStochasticArtifactsBlocking(
        sourceData, meta, settings);
}

} // namespace br::assets
