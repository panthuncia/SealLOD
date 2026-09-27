#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <DirectXMath.h>

#include <BasicRenderer/Assets/TextureTypes.h>

struct TextureFileMeta;
struct TextureSourceData;

enum class StochasticTextureTransformMode : uint8_t {
	None = 0,
	DecorrelatedColor,
	NormalXY,
	Scalar,
};

struct StochasticTextureArtifactSettings {
	TextureSemantic semantic = TextureSemantic::Unknown;
	bool preferSRGB = false;
	NormalMapConvention normalConvention = NormalMapConvention::DirectX;
	std::string sourceIdentity;
	std::uint32_t lutWidth = 256;
	std::uint32_t algorithmVersion = 4;
};

struct StochasticTextureArtifactResult {
	bool ready = false;
	bool loadedFromCache = false;
	std::string failureReason;
	std::wstring gaussianCachePath;
	std::wstring inverseLutCachePath;
	std::uint32_t lutWidth = 0;
	std::uint32_t lutHeight = 0;
	StochasticTextureTransformMode transformMode = StochasticTextureTransformMode::None;
	DirectX::XMFLOAT3 colorSpaceOrigin = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 colorSpaceVector0 = { 1.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 colorSpaceVector1 = { 0.0f, 1.0f, 0.0f };
	DirectX::XMFLOAT3 colorSpaceVector2 = { 0.0f, 0.0f, 1.0f };
};

namespace br::assets {

std::wstring GetExistingCachePathForFile(const TextureFileMeta& meta);
StochasticTextureArtifactResult RequestStochasticArtifactsBlocking(
        const std::shared_ptr<TextureSourceData>& sourceData,
        const TextureFileMeta& meta,
        const StochasticTextureArtifactSettings& settings);

} // namespace br::assets
