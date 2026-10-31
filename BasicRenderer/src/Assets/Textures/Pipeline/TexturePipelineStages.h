#pragma once

// Loading steps the texture pipeline's graph jobs (TexturePipelineArtifacts) run.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <BasicRenderer/Assets/Texture.h>
#include <BasicRenderer/Assets/TexturePipeline.h>

namespace br::textures
{
	// The processing description of a request; it keys the processed cache.
	TextureFileMeta TextureProcessingMeta(const TextureRequest& request);

	// Who loads LoadTexture's textures: the texture streaming manager, while it runs.
	using TextureLoader = std::function<void(const std::shared_ptr<TextureAsset>&, std::function<void(bool)>)>;
	void SetTextureLoader(TextureLoader loader);

	// The full description (every mip) recorded in a conditioned cache file's header.
	std::optional<org::TextureDescription> ReadConditionedCacheDescription(const std::wstring& path);

	// Starts a DirectStorage upload of the mip window from topMip down out of a conditioned
	// cache file; null when DirectStorage cannot serve it. Poll its state; Uploading
	// completes through DirectStorageManager::PollRequest.
	std::shared_ptr<TextureDirectStorageReloadJobHandle> BeginCacheWindowUpload(
		const std::wstring& path, std::uint32_t topMip, bool allowRTV, bool allowUAV);

	// The mip window from topMip down of source data holding the full chain.
	std::shared_ptr<TextureSourceData> ShapeSourceForTopMip(
		const std::shared_ptr<TextureSourceData>& data, std::uint32_t topMip);
}
