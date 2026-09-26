#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <DirectXTex.h>

namespace org { class Sampler; }
class TextureAsset;
struct TextureFileMeta;

struct LoadFlags {
	DirectX::DDS_FLAGS dds = DirectX::DDS_FLAGS_NONE;
	DirectX::TGA_FLAGS tga = DirectX::TGA_FLAGS_NONE;
	DirectX::WIC_FLAGS wic = DirectX::WIC_FLAGS_IGNORE_SRGB;
	// HDR has no flags
};

std::shared_ptr<TextureAsset> LoadTextureFromFile(
	const std::wstring& filePath,
	std::shared_ptr<org::Sampler> sampler = nullptr,
	bool preferSRGB = false,
	const LoadFlags& flags = {}, bool allowRTV = false, bool allowUAV = false);

std::shared_ptr<TextureAsset> LoadTextureFromFileDeferred(
	const std::wstring& filePath,
	std::shared_ptr<org::Sampler> sampler = nullptr,
	bool preferSRGB = false,
	const TextureFileMeta* metaOverride = nullptr,
	bool allowRTV = false,
	bool allowUAV = false);

std::shared_ptr<TextureAsset> LoadTextureFromMemory(
	const void* bytes,
	std::size_t byteCount,
	std::shared_ptr<org::Sampler> sampler = nullptr,
	const LoadFlags& flags = {},
	bool preferSRGB = false, bool allowRTV = false, bool allowUAV = false);

std::shared_ptr<TextureAsset> LoadCubemapFromFile(
	const char* topPath, const char* bottomPath, const char* leftPath,
	const char* rightPath, const char* frontPath, const char* backPath);
std::shared_ptr<TextureAsset> LoadCubemapFromFile(
	std::wstring ddsFilePath, bool allowRTV = false, bool allowUAV = false);
