#include "Managers/Singletons/TextureProcessingManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <boost/container_hash/hash.hpp>
#include <DirectXTex.h>
#include <spdlog/spdlog.h>

#include <rhi_dx12.h>
#include <rhi_helpers.h>

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Utilities/CachePathUtilities.h"
#include "Utilities/ProcessedTextureCache.h"

using namespace DirectX;

namespace {
constexpr uint32_t kTextureProcessingCacheVersion = 16u;

std::string FormatHRESULT(HRESULT hr) {
	std::ostringstream oss;
	oss << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
		<< static_cast<uint32_t>(hr);
	return oss.str();
}

uint32_t CalcMipCount(uint32_t width, uint32_t height) {
	uint32_t levels = 1;
	while (width > 1 || height > 1) {
		width = (std::max)(1u, width >> 1);
		height = (std::max)(1u, height >> 1);
		++levels;
	}
	return levels;
}

std::string TextureSemanticToString(TextureSemantic semantic) {
	switch (semantic) {
	case TextureSemantic::BaseColor: return "BaseColor";
	case TextureSemantic::Emissive: return "Emissive";
	case TextureSemantic::Normal: return "Normal";
	case TextureSemantic::Height: return "Height";
	case TextureSemantic::AO: return "AO";
	case TextureSemantic::Opacity: return "Opacity";
	case TextureSemantic::Metallic: return "Metallic";
	case TextureSemantic::Roughness: return "Roughness";
	case TextureSemantic::MetallicRoughness: return "MetallicRoughness";
	case TextureSemantic::OpenPBRColor: return "OpenPBRColor";
	case TextureSemantic::OpenPBRScalar: return "OpenPBRScalar";
	default: return "Unknown";
	}
}

bool NeedsNormalConventionConversion(const TextureFileMeta& meta) {
	return meta.processing.semantic == TextureSemantic::Normal &&
		meta.processing.normalConvention == NormalMapConvention::OpenGL;
}

bool SupportsNormalGreenFlip(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

bool ShouldPreserveAlphaCoverage(const TextureFileMeta& meta, const TextureSourceData& sourceData) {
	if (!meta.processing.isParticipatingMaterialTexture || meta.alphaIsAllOpaque) {
		return false;
	}
	if (sourceData.desc.isArray || sourceData.desc.isCubemap || sourceData.desc.channels != 4 || sourceData.desc.imageDimensions.empty()) {
		return false;
	}

	return rhi::helpers::stripSrgb(sourceData.desc.format) == rhi::Format::R8G8B8A8_UNorm;
}

bool IsSourceBlockCompressed(const TextureSourceData& sourceData) {
	return sourceData.isBlockCompressed || rhi::helpers::IsBlockCompressed(sourceData.desc.format);
}

void FlipNormalGreenChannel(ScratchImage& image) {
	const TexMetadata metadata = image.GetMetadata();
	const DXGI_FORMAT format = metadata.format;
	if (!SupportsNormalGreenFlip(format)) {
		throw std::runtime_error("TextureProcessingManager: unsupported normal map format for green-channel conversion");
	}

	const size_t bytesPerPixel = format == DXGI_FORMAT_R8G8_UNORM ? 2u : 4u;
	const size_t imageCount = image.GetImageCount();
	for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
		const Image* imageView = image.GetImage(imageIndex, 0, 0);
		if (!imageView || !imageView->pixels) {
			throw std::runtime_error("TextureProcessingManager: invalid normal map image during green-channel conversion");
		}

		uint8_t* pixels = const_cast<uint8_t*>(imageView->pixels);
		for (size_t y = 0; y < imageView->height; ++y) {
			uint8_t* row = pixels + (y * imageView->rowPitch);
			for (size_t x = 0; x < imageView->width; ++x) {
				row[x * bytesPerPixel + 1] = static_cast<uint8_t>(255u - row[x * bytesPerPixel + 1]);
			}
		}
	}
}

std::string ResolveProcessingIdentity(const TextureFileMeta& meta) {
	std::string normalizedIdentity = meta.processing.sourceIdentity.empty()
		? NormalizeCacheSourcePath(meta.filePath)
		: NormalizeCacheSourcePath(meta.processing.sourceIdentity);
	if (normalizedIdentity.empty()) {
		normalizedIdentity = meta.processing.sourceIdentity.empty() ? meta.filePath : meta.processing.sourceIdentity;
	}
	return normalizedIdentity;
}

std::string TryGetSourceVersionTag(const TextureFileMeta& meta) {
	const std::string* candidate = meta.processing.sourceIdentity.empty()
		? &meta.filePath
		: &meta.processing.sourceIdentity;
	if (candidate == nullptr || candidate->empty()) {
		return {};
	}

	std::error_code ec;
	const std::filesystem::path path(*candidate);
	if (!std::filesystem::exists(path, ec) || ec) {
		return {};
	}

	auto lastWriteTime = std::filesystem::last_write_time(path, ec);
	if (ec) {
		return {};
	}

	return std::to_string(lastWriteTime.time_since_epoch().count());
}

std::wstring BuildProcessingCachePath(const std::string& key) {
	size_t seed = 0;
	boost::hash_combine(seed, key);

	std::ostringstream fileName;
	fileName << "processed_"
		<< std::hex
		<< std::setw(static_cast<int>(sizeof(size_t) * 2))
		<< std::setfill('0')
		<< seed
		<< ".dds";
	return GetCacheFilePath(s2ws(fileName.str()), L"textures");
}

std::wstring BuildProcessingConditionedCachePath(const std::string& key) {
	size_t seed = 0;
	boost::hash_combine(seed, key);

	std::wostringstream fileName;
	fileName << L"processed_"
		<< std::hex
		<< std::setw(static_cast<int>(sizeof(size_t) * 2))
		<< std::setfill(L'0')
		<< seed
		<< br::processed_texture_cache::kExtension;
	return GetCacheFilePath(fileName.str(), L"textures");
}

std::mutex& GetCacheWriteMutexForKey(const std::string& key) {
	static std::mutex tableMutex;
	static std::unordered_map<std::string, std::shared_ptr<std::mutex>> table;

	std::scoped_lock lock(tableMutex);
	auto& entry = table[key];
	if (!entry) {
		entry = std::make_shared<std::mutex>();
	}
	return *entry;
}

uint32_t GetTextureTotalArraySlices(const TextureDescription& desc) {
	if (desc.isCubemap) {
		return 6u * (std::max)(1u, desc.arraySize);
	}
	if (desc.isArray) {
		return (std::max)(1u, desc.arraySize);
	}
	return 1u;
}

uint32_t GetTextureMipLevelCount(const TextureSourceData& sourceData) {
	const uint32_t totalArraySlices = GetTextureTotalArraySlices(sourceData.desc);
	if (totalArraySlices == 0 || sourceData.desc.imageDimensions.empty() ||
		(sourceData.desc.imageDimensions.size() % totalArraySlices) != 0) {
		return 0;
	}
	return static_cast<uint32_t>(sourceData.desc.imageDimensions.size() / totalArraySlices);
}

bool TryWriteConditionedTextureCache(const std::wstring& cachePath, const TextureSourceData& sourceData) {
	if (sourceData.desc.imageDimensions.empty() || sourceData.subresources.empty()) {
		return false;
	}

	const uint32_t totalArraySlices = GetTextureTotalArraySlices(sourceData.desc);
	const uint32_t mipLevels = GetTextureMipLevelCount(sourceData);
	const uint32_t subresourceCount = static_cast<uint32_t>(sourceData.desc.imageDimensions.size());
	if (mipLevels == 0 || subresourceCount != sourceData.subresources.size()) {
		return false;
	}

	auto* nativeDevice = rhi::dx12::get_device(DeviceManager::GetInstance().GetDevice());
	if (nativeDevice == nullptr) {
		return false;
	}

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Alignment = 0;
	resourceDesc.Width = sourceData.desc.imageDimensions[0].width;
	resourceDesc.Height = sourceData.desc.imageDimensions[0].height;
	resourceDesc.DepthOrArraySize = static_cast<uint16_t>(totalArraySlices);
	resourceDesc.MipLevels = static_cast<uint16_t>(mipLevels);
	resourceDesc.Format = rhi::ToDxgi(sourceData.desc.format);
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
	std::vector<UINT> numRows(subresourceCount);
	std::vector<UINT64> rowSizes(subresourceCount);
	UINT64 totalBytes = 0;
	nativeDevice->GetCopyableFootprints(
		&resourceDesc,
		0,
		subresourceCount,
		0,
		layouts.data(),
		numRows.data(),
		rowSizes.data(),
		&totalBytes);

	if (totalBytes == 0 || totalBytes > static_cast<UINT64>((std::numeric_limits<size_t>::max)())) {
		return false;
	}

	std::vector<uint8_t> conditionedData(static_cast<size_t>(totalBytes), 0u);
	for (uint32_t subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex) {
		const auto& dims = sourceData.desc.imageDimensions[subresourceIndex];
		const auto& bytes = sourceData.subresources[subresourceIndex];
		if (!bytes || bytes->size() < dims.slicePitch) {
			return false;
		}

		const auto& layout = layouts[subresourceIndex];
		const size_t copyRowSize = static_cast<size_t>(rowSizes[subresourceIndex]);
		const size_t srcRowPitch = static_cast<size_t>(dims.rowPitch);
		const size_t dstRowPitch = static_cast<size_t>(layout.Footprint.RowPitch);
		uint8_t* dstBase = conditionedData.data() + static_cast<size_t>(layout.Offset);
		const uint8_t* srcBase = bytes->data();
		for (UINT row = 0; row < numRows[subresourceIndex]; ++row) {
			std::memcpy(
				dstBase + static_cast<size_t>(row) * dstRowPitch,
				srcBase + static_cast<size_t>(row) * srcRowPitch,
				copyRowSize);
		}
	}

	br::processed_texture_cache::FileHeader header{};
	header.flags = 0;
	if (sourceData.desc.isCubemap) {
		header.flags |= br::processed_texture_cache::FlagIsCubemap;
	}
	if (sourceData.desc.isArray) {
		header.flags |= br::processed_texture_cache::FlagIsArray;
	}
	if (sourceData.hasFullMipChain) {
		header.flags |= br::processed_texture_cache::FlagHasFullMipChain;
	}
	if (IsSourceBlockCompressed(sourceData)) {
		header.flags |= br::processed_texture_cache::FlagIsBlockCompressed;
	}
	header.format = static_cast<uint32_t>(sourceData.desc.format);
	header.channels = sourceData.desc.channels;
	header.baseWidth = sourceData.desc.imageDimensions[0].width;
	header.baseHeight = sourceData.desc.imageDimensions[0].height;
	header.mipLevels = mipLevels;
	header.arraySize = (std::max)(1u, sourceData.desc.arraySize);
	header.totalArraySlices = totalArraySlices;
	header.subresourceCount = subresourceCount;
	header.dataOffset = sizeof(header);
	header.dataSizeBytes = totalBytes;

	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);
	std::ofstream file(cachePath, std::ios::binary | std::ios::trunc);
	if (!file) {
		return false;
	}

	file.write(reinterpret_cast<const char*>(&header), sizeof(header));
	file.write(reinterpret_cast<const char*>(conditionedData.data()), static_cast<std::streamsize>(conditionedData.size()));
	return file.good();
}

DXGI_FORMAT ChooseWorkingFormat(const TextureFileMeta& meta) {
	switch (meta.processing.semantic) {
	case TextureSemantic::Normal:
		return DXGI_FORMAT_R8G8_UNORM;
	case TextureSemantic::Height:
	case TextureSemantic::AO:
	case TextureSemantic::Opacity:
	case TextureSemantic::Metallic:
	case TextureSemantic::Roughness:
	case TextureSemantic::OpenPBRScalar:
		return DXGI_FORMAT_R8_UNORM;
	default:
		return meta.preferSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}


DXGI_FORMAT ChooseCompressedFormat(const TextureSourceData& sourceData, const TextureFileMeta& meta) {
	if (!meta.processing.requestBlockCompression) {
		return rhi::ToDxgi(sourceData.desc.format);
	}

	switch (meta.processing.semantic) {
	case TextureSemantic::BaseColor:
	case TextureSemantic::Emissive:
	case TextureSemantic::OpenPBRColor:
		return meta.preferSRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
	case TextureSemantic::Normal:
		return DXGI_FORMAT_BC5_UNORM;
	case TextureSemantic::AO:
	case TextureSemantic::Opacity:
	case TextureSemantic::Metallic:
	case TextureSemantic::Roughness:
	case TextureSemantic::OpenPBRScalar:
		return meta.processing.preservePackedChannels ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_BC4_UNORM;
	case TextureSemantic::MetallicRoughness:
		return DXGI_FORMAT_BC7_UNORM;
	default:
		return meta.preferSRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
	}
}

HRESULT InitializeScratchImageFromSource(const TextureSourceData& sourceData, ScratchImage& scratchImage) {
	const DXGI_FORMAT format = rhi::ToDxgi(sourceData.desc.format);
	if (sourceData.desc.imageDimensions.empty()) {
		return E_INVALIDARG;
	}

	const uint32_t baseWidth = sourceData.desc.imageDimensions[0].width;
	const uint32_t baseHeight = sourceData.desc.imageDimensions[0].height;
	const uint32_t faces = sourceData.desc.isCubemap ? 6u : 1u;
	const uint32_t arraySize = (std::max)(1u, sourceData.desc.arraySize) * faces;
	const uint32_t mipLevels = GetTextureMipLevelCount(sourceData);
	if (mipLevels == 0u) {
		return E_INVALIDARG;
	}

	HRESULT hr = scratchImage.Initialize2D(format, baseWidth, baseHeight, arraySize, mipLevels);
	if (FAILED(hr)) {
		return hr;
	}

	const size_t expectedImages = static_cast<size_t>(arraySize) * mipLevels;
	if (sourceData.subresources.size() != expectedImages || sourceData.desc.imageDimensions.size() != expectedImages) {
		return E_INVALIDARG;
	}

	size_t imageIndex = 0;
	for (uint32_t item = 0; item < arraySize; ++item) {
		for (uint32_t mip = 0; mip < mipLevels; ++mip, ++imageIndex) {
			const Image* dstImage = scratchImage.GetImage(mip, item, 0);
			if (!dstImage || !dstImage->pixels) {
				return E_FAIL;
			}

			const auto& bytes = sourceData.subresources[imageIndex];
			const auto& dims = sourceData.desc.imageDimensions[imageIndex];
			if (!bytes || bytes->size() < dims.slicePitch) {
				return E_INVALIDARG;
			}

			size_t expectedRowPitch = 0;
			size_t expectedSlicePitch = 0;
			if (FAILED(ComputePitch(format, dims.width, dims.height, expectedRowPitch, expectedSlicePitch)) ||
				expectedRowPitch == 0 ||
				expectedSlicePitch == 0)
			{
				return E_INVALIDARG;
			}

			const size_t srcRowPitch = static_cast<size_t>(dims.rowPitch);
			const size_t srcSlicePitch = static_cast<size_t>(dims.slicePitch);
			const size_t dstRowPitch = dstImage->rowPitch;
			const size_t dstSlicePitch = dstImage->slicePitch;
			if (srcRowPitch < expectedRowPitch ||
				dstRowPitch < expectedRowPitch ||
				srcSlicePitch < expectedSlicePitch ||
				dstSlicePitch < expectedSlicePitch)
			{
				return E_INVALIDARG;
			}

			const size_t rowCount = expectedSlicePitch / expectedRowPitch;
			auto* dstPixels = const_cast<uint8_t*>(dstImage->pixels);
			const auto* srcPixels = bytes->data();
			for (size_t row = 0; row < rowCount; ++row) {
				std::memcpy(
					dstPixels + row * dstRowPitch,
					srcPixels + row * srcRowPitch,
					expectedRowPitch);
			}
		}
	}

	return S_OK;
}

std::shared_ptr<TextureSourceData> BuildSourceDataFromScratchImage(const ScratchImage& image) {
	const TexMetadata metadata = image.GetMetadata();
	auto result = std::make_shared<TextureSourceData>();
	result->desc.format = rhi::helpers::ToRHI(metadata.format);
	result->desc.channels = static_cast<unsigned short>(rhi::helpers::FormatChannelCount(result->desc.format));
	result->desc.isCubemap = metadata.IsCubemap();
	result->desc.isArray = metadata.arraySize > 1 && !result->desc.isCubemap;
	result->desc.arraySize = result->desc.isCubemap
		? static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize / size_t(6)))
		: static_cast<uint32_t>((std::max)(size_t(1), metadata.arraySize));
	result->desc.generateMipMaps = false;
	result->isBlockCompressed = rhi::helpers::IsBlockCompressed(result->desc.format);
	result->hasFullMipChain =
		metadata.mipLevels == CalcMipCount(
			static_cast<uint32_t>(metadata.width),
			static_cast<uint32_t>(metadata.height));

	const Image* images = image.GetImages();
	const size_t imageCount = image.GetImageCount();
	result->desc.imageDimensions.reserve(imageCount);
	result->subresources.reserve(imageCount);

	for (size_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
		const Image& src = images[imageIndex];
		if (src.width > std::numeric_limits<uint32_t>::max() || src.height > std::numeric_limits<uint32_t>::max()) {
			throw std::runtime_error("Texture dimensions exceed uint32_t range");
		}

		ImageDimensions dims{};
		dims.width = static_cast<uint32_t>(src.width);
		dims.height = static_cast<uint32_t>(src.height);
		dims.rowPitch = src.rowPitch;
		dims.slicePitch = src.slicePitch;
		result->desc.imageDimensions.push_back(dims);

		const auto* first = reinterpret_cast<const uint8_t*>(src.pixels);
		auto bytes = std::make_shared<std::vector<uint8_t>>(first, first + src.slicePitch);
		result->subresources.push_back(std::move(bytes));
	}

	result->isBlockCompressed = rhi::helpers::IsBlockCompressed(result->desc.format);
	result->hasFullMipChain = metadata.mipLevels == CalcMipCount(
		result->desc.imageDimensions[0].width,
		result->desc.imageDimensions[0].height);
	return result;
}

std::shared_ptr<TextureSourceData> TryLoadTextureSourceDataFromCache(const std::string& key) {
	const std::wstring cachePath = BuildProcessingCachePath(key);
	std::error_code ec;
	if (!std::filesystem::exists(std::filesystem::path(cachePath), ec) || ec) {
		return {};
	}

	ScratchImage cachedImage;
	TexMetadata cachedMetadata{};
	const HRESULT hr = LoadFromDDSFile(cachePath.c_str(), DDS_FLAGS_NONE, &cachedMetadata, cachedImage);
	if (FAILED(hr)) {
		spdlog::warn("TextureProcessingManager: failed to load cache file '{}'", ws2s(cachePath));
		return {};
	}

	try {
		return BuildSourceDataFromScratchImage(cachedImage);
	}
	catch (const std::exception& ex) {
		spdlog::warn("TextureProcessingManager: failed to decode cache file '{}': {}", ws2s(cachePath), ex.what());
		return {};
	}
}

std::wstring TryWriteTextureSourceDataToCache(const std::string& key, const TextureSourceData& sourceData) {
	std::scoped_lock cacheWriteLock(GetCacheWriteMutexForKey(key));

	const std::wstring cachePath = BuildProcessingCachePath(key);
	const std::wstring conditionedCachePath = BuildProcessingConditionedCachePath(key);
	std::wstring writtenConditionedCachePath;
	if (TryWriteConditionedTextureCache(conditionedCachePath, sourceData)) {
		writtenConditionedCachePath = conditionedCachePath;
		spdlog::debug(
			"TextureProcessingManager: wrote conditioned cache file '{}' format={} subresources={}",
			ws2s(conditionedCachePath),
			static_cast<uint32_t>(sourceData.desc.format),
			sourceData.subresources.size());
	}
	else {
		spdlog::warn("TextureProcessingManager: failed to write conditioned cache file '{}'", ws2s(conditionedCachePath));
	}

	ScratchImage cachedImage;
	const HRESULT initHr = InitializeScratchImageFromSource(sourceData, cachedImage);
	if (FAILED(initHr)) {
		spdlog::warn("TextureProcessingManager: failed to initialize cache image for '{}'", ws2s(cachePath));
		return writtenConditionedCachePath;
	}

	const HRESULT writeHr = SaveToDDSFile(
		cachedImage.GetImages(),
		cachedImage.GetImageCount(),
		cachedImage.GetMetadata(),
		DDS_FLAGS_NONE,
		cachePath.c_str());
	if (FAILED(writeHr)) {
		spdlog::warn("TextureProcessingManager: failed to write cache file '{}'", ws2s(cachePath));
		return writtenConditionedCachePath;
	}

	spdlog::debug(
		"TextureProcessingManager: wrote cache file '{}' format={} subresources={} fullMipChain={} blockCompressed={}",
		ws2s(cachePath),
		static_cast<uint32_t>(sourceData.desc.format),
		sourceData.subresources.size(),
		sourceData.hasFullMipChain,
		sourceData.isBlockCompressed);

	return writtenConditionedCachePath;
}

uint32_t ResolveRequestedMipLevelCount(
	const TextureProcessingSettings& settings,
	uint32_t width,
	uint32_t height)
{
	const uint32_t fullMipCount = CalcMipCount(width, height);
	return settings.maxMipLevels == 0u
		? fullMipCount
		: (std::min)(fullMipCount, settings.maxMipLevels);
}

std::shared_ptr<TextureSourceData> ClampTextureSourceMipLevels(
	const std::shared_ptr<TextureSourceData>& sourceData,
	uint32_t maxMipLevels)
{
	if (!sourceData || maxMipLevels == 0u) {
		return sourceData;
	}
	const uint32_t sourceMipLevels = GetTextureMipLevelCount(*sourceData);
	const uint32_t retainedMipLevels = (std::min)(sourceMipLevels, maxMipLevels);
	if (sourceMipLevels == 0u || retainedMipLevels == sourceMipLevels) {
		return sourceData;
	}

	auto result = std::make_shared<TextureSourceData>(*sourceData);
	result->desc.imageDimensions.clear();
	result->subresources.clear();
	const uint32_t slices = GetTextureTotalArraySlices(sourceData->desc);
	result->desc.imageDimensions.reserve(static_cast<size_t>(slices) * retainedMipLevels);
	result->subresources.reserve(static_cast<size_t>(slices) * retainedMipLevels);
	for (uint32_t slice = 0u; slice < slices; ++slice) {
		const size_t sourceBase = static_cast<size_t>(slice) * sourceMipLevels;
		for (uint32_t mip = 0u; mip < retainedMipLevels; ++mip) {
			result->desc.imageDimensions.push_back(sourceData->desc.imageDimensions[sourceBase + mip]);
			result->subresources.push_back(sourceData->subresources[sourceBase + mip]);
		}
	}
	result->hasFullMipChain = true;
	return result;
}

std::wstring BuildStochasticCachePath(const std::string& key, const wchar_t* suffix) {
	size_t seed = 0;
	boost::hash_combine(seed, key);

	std::wostringstream fileName;
	fileName << L"stochastic_"
		<< std::hex
		<< std::setw(static_cast<int>(sizeof(size_t) * 2))
		<< std::setfill(L'0')
		<< seed
		<< suffix;
	return GetCacheFilePath(fileName.str(), L"textures");
}

double InverseNormalCdf(double p) {
	p = std::clamp(p, 1.0e-6, 1.0 - 1.0e-6);
	static constexpr double a[] = {
		-3.969683028665376e+01,
		 2.209460984245205e+02,
		-2.759285104469687e+02,
		 1.383577518672690e+02,
		-3.066479806614716e+01,
		 2.506628277459239e+00
	};
	static constexpr double b[] = {
		-5.447609879822406e+01,
		 1.615858368580409e+02,
		-1.556989798598866e+02,
		 6.680131188771972e+01,
		-1.328068155288572e+01
	};
	static constexpr double c[] = {
		-7.784894002430293e-03,
		-3.223964580411365e-01,
		-2.400758277161838e+00,
		-2.549732539343734e+00,
		 4.374664141464968e+00,
		 2.938163982698783e+00
	};
	static constexpr double d[] = {
		 7.784695709041462e-03,
		 3.224671290700398e-01,
		 2.445134137142996e+00,
		 3.754408661907416e+00
	};
	static constexpr double pLow = 0.02425;
	static constexpr double pHigh = 1.0 - pLow;
	if (p < pLow) {
		const double q = std::sqrt(-2.0 * std::log(p));
		return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
			((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	}
	if (p > pHigh) {
		const double q = std::sqrt(-2.0 * std::log(1.0 - p));
		return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
			((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	}
	const double q = p - 0.5;
	const double r = q * q;
	return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
		(((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

uint8_t QuantizeUnorm8(float value) {
	return static_cast<uint8_t>(std::clamp(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f), 0l, 255l));
}

float GaussianizedValue(float cdf) {
	return std::clamp(0.5f + static_cast<float>(InverseNormalCdf(cdf)) * (1.0f / 6.0f), 0.0f, 1.0f);
}

std::vector<uint8_t> BuildGaussianizedPixels(
	const std::vector<std::array<float, 4>>& sourcePixels,
	uint32_t channels)
{
	const size_t pixelCount = sourcePixels.size();
	std::vector<uint8_t> result(pixelCount * channels, 0u);
	for (uint32_t channel = 0; channel < channels; ++channel) {
		std::vector<std::pair<float, size_t>> sorted;
		sorted.reserve(pixelCount);
		for (size_t i = 0; i < pixelCount; ++i) {
			sorted.emplace_back(std::clamp(sourcePixels[i][channel], 0.0f, 1.0f), i);
		}
		std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
			return a.first < b.first;
		});

		for (size_t rank = 0; rank < sorted.size(); ++rank) {
			const float cdf = (static_cast<float>(rank) + 0.5f) / static_cast<float>((std::max)(size_t(1), sorted.size()));
			result[sorted[rank].second * channels + channel] = QuantizeUnorm8(GaussianizedValue(cdf));
		}
	}
	if (channels == 4) {
		for (size_t i = 0; i < pixelCount; ++i) {
			result[i * channels + 3] = QuantizeUnorm8(sourcePixels[i][3]);
		}
	}
	return result;
}

std::vector<uint8_t> DownsampleUnorm8(
	const std::vector<uint8_t>& src,
	uint32_t srcWidth,
	uint32_t srcHeight,
	uint32_t channels,
	uint32_t& dstWidth,
	uint32_t& dstHeight)
{
	dstWidth = (std::max)(1u, srcWidth >> 1);
	dstHeight = (std::max)(1u, srcHeight >> 1);
	std::vector<uint8_t> dst(static_cast<size_t>(dstWidth) * dstHeight * channels, 0u);
	for (uint32_t y = 0; y < dstHeight; ++y) {
		for (uint32_t x = 0; x < dstWidth; ++x) {
			for (uint32_t c = 0; c < channels; ++c) {
				uint32_t sum = 0;
				uint32_t count = 0;
				for (uint32_t oy = 0; oy < 2; ++oy) {
					for (uint32_t ox = 0; ox < 2; ++ox) {
						const uint32_t sx = (std::min)(srcWidth - 1, x * 2 + ox);
						const uint32_t sy = (std::min)(srcHeight - 1, y * 2 + oy);
						sum += src[(static_cast<size_t>(sy) * srcWidth + sx) * channels + c];
						++count;
					}
				}
				dst[(static_cast<size_t>(y) * dstWidth + x) * channels + c] =
					static_cast<uint8_t>((sum + count / 2u) / count);
			}
		}
	}
	return dst;
}

std::shared_ptr<TextureSourceData> BuildMipmappedUnormSourceData(
	std::vector<uint8_t> basePixels,
	uint32_t width,
	uint32_t height,
	uint32_t channels,
	rhi::Format format)
{
	auto result = std::make_shared<TextureSourceData>();
	result->desc.format = format;
	result->desc.channels = static_cast<unsigned short>(channels);
	result->desc.arraySize = 1;
	result->desc.generateMipMaps = false;
	result->hasFullMipChain = true;
	result->isBlockCompressed = false;

	uint32_t mipWidth = width;
	uint32_t mipHeight = height;
	std::vector<uint8_t> mipPixels = std::move(basePixels);
	for (;;) {
		ImageDimensions dims{};
		dims.width = mipWidth;
		dims.height = mipHeight;
		dims.rowPitch = static_cast<uint64_t>(mipWidth) * channels;
		dims.slicePitch = dims.rowPitch * mipHeight;
		result->desc.imageDimensions.push_back(dims);
		result->subresources.push_back(std::make_shared<std::vector<uint8_t>>(mipPixels));
		if (mipWidth == 1u && mipHeight == 1u) {
			break;
		}
		uint32_t nextWidth = 1;
		uint32_t nextHeight = 1;
		mipPixels = DownsampleUnorm8(mipPixels, mipWidth, mipHeight, channels, nextWidth, nextHeight);
		mipWidth = nextWidth;
		mipHeight = nextHeight;
	}
	return result;
}

float EstimateAverageWindowVariance(
	const std::vector<uint8_t>& gaussianPixels,
	uint32_t width,
	uint32_t height,
	uint32_t channels,
	uint32_t channel,
	uint32_t windowSide)
{
	if (gaussianPixels.empty() || width == 0u || height == 0u || channels == 0u || channel >= channels || windowSide <= 1u) {
		return 0.0f;
	}

	double varianceSum = 0.0;
	uint32_t blockCount = 0;
	for (uint32_t y0 = 0; y0 < height; y0 += windowSide) {
		for (uint32_t x0 = 0; x0 < width; x0 += windowSide) {
			const uint32_t y1 = (std::min)(height, y0 + windowSide);
			const uint32_t x1 = (std::min)(width, x0 + windowSide);
			double sum = 0.0;
			double sumSquares = 0.0;
			uint32_t count = 0;
			for (uint32_t y = y0; y < y1; ++y) {
				for (uint32_t x = x0; x < x1; ++x) {
					const float value = gaussianPixels[(static_cast<size_t>(y) * width + x) * channels + channel] * (1.0f / 255.0f);
					sum += value;
					sumSquares += static_cast<double>(value) * value;
					++count;
				}
			}
			if (count > 1u) {
				const double mean = sum / count;
				varianceSum += (sumSquares / count) - mean * mean;
				++blockCount;
			}
		}
	}
	return blockCount == 0u ? 0.0f : static_cast<float>(varianceSum / blockCount);
}

std::shared_ptr<TextureSourceData> BuildInverseLutSourceData(
	const std::vector<std::array<float, 4>>& sourcePixels,
	uint32_t channels,
	uint32_t lutWidth,
	uint32_t lutHeight,
	rhi::Format format,
	const std::vector<uint8_t>& gaussianizedBasePixels,
	uint32_t sourceWidth,
	uint32_t sourceHeight)
{
	std::vector<uint8_t> pixels(static_cast<size_t>(lutWidth) * lutHeight * channels, 0u);
	for (uint32_t channel = 0; channel < channels; ++channel) {
		std::vector<float> sorted;
		sorted.reserve(sourcePixels.size());
		for (const auto& p : sourcePixels) {
			sorted.push_back(std::clamp(p[channel], 0.0f, 1.0f));
		}
		std::sort(sorted.begin(), sorted.end());
		std::vector<float> baseLut(lutWidth, 0.0f);
		for (uint32_t y = 0; y < lutHeight; ++y) {
			float variance = 0.0f;
			if (y > 0u) {
				const uint32_t windowSide = 1u << (std::min)(y, 12u);
				variance = EstimateAverageWindowVariance(
					gaussianizedBasePixels,
					sourceWidth,
					sourceHeight,
					channels,
					channel,
					windowSide);
			}
			for (uint32_t x = 0; x < lutWidth; ++x) {
				const float g = (static_cast<float>(x) + 0.5f) / static_cast<float>(lutWidth);
				if (y == 0u) {
					const float normal = (g - 0.5f) * 6.0f;
					const float cdf = 0.5f * (1.0f + std::erf(normal / std::sqrt(2.0f)));
					const size_t index = (std::min)(
						sorted.size() - 1u,
						static_cast<size_t>(std::clamp(cdf, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1u)));
					baseLut[x] = sorted[index];
				}
				float filtered = baseLut[x];
				if (y > 0u && variance > 1.0e-8f) {
					const float center = g;
					const float sigma = std::sqrt(variance);
					const int radius = static_cast<int>((std::min)(static_cast<float>(lutWidth), std::ceil(sigma * 4.0f * lutWidth)));
					double weighted = 0.0;
					double weightSum = 0.0;
					for (int dx = -radius; dx <= radius; ++dx) {
						const int sx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(lutWidth) - 1);
						const float sampleCenter = (static_cast<float>(sx) + 0.5f) / static_cast<float>(lutWidth);
						const float d = sampleCenter - center;
						const float w = std::exp(-(d * d) / (2.0f * variance));
						weighted += static_cast<double>(baseLut[static_cast<size_t>(sx)]) * w;
						weightSum += w;
					}
					if (weightSum > 0.0) {
						filtered = static_cast<float>(weighted / weightSum);
					}
				}
				pixels[(static_cast<size_t>(y) * lutWidth + x) * channels + channel] = QuantizeUnorm8(filtered);
			}
		}
	}
	if (channels == 4) {
		for (uint32_t y = 0; y < lutHeight; ++y) {
			for (uint32_t x = 0; x < lutWidth; ++x) {
				pixels[(static_cast<size_t>(y) * lutWidth + x) * channels + 3u] = 255u;
			}
		}
	}

	auto result = std::make_shared<TextureSourceData>();
	result->desc.format = format;
	result->desc.channels = static_cast<unsigned short>(channels);
	result->desc.arraySize = 1;
	result->desc.generateMipMaps = false;
	result->hasFullMipChain = true;
	result->isBlockCompressed = false;
	ImageDimensions dims{};
	dims.width = lutWidth;
	dims.height = lutHeight;
	dims.rowPitch = static_cast<uint64_t>(lutWidth) * channels;
	dims.slicePitch = dims.rowPitch * lutHeight;
	result->desc.imageDimensions.push_back(dims);
	result->subresources.push_back(std::make_shared<std::vector<uint8_t>>(std::move(pixels)));
	return result;
}

bool TryReadStochasticMetadata(const std::wstring& path, StochasticTextureArtifactResult& result) {
	std::ifstream file(path);
	if (!file) {
		return false;
	}
	std::string key;
	while (file >> key) {
		if (key == "lutWidth") file >> result.lutWidth;
		else if (key == "lutHeight") file >> result.lutHeight;
		else if (key == "transformMode") {
			uint32_t mode = 0;
			file >> mode;
			result.transformMode = static_cast<StochasticTextureTransformMode>(mode);
		}
		else if (key == "origin") file >> result.colorSpaceOrigin.x >> result.colorSpaceOrigin.y >> result.colorSpaceOrigin.z;
		else if (key == "vector0") file >> result.colorSpaceVector0.x >> result.colorSpaceVector0.y >> result.colorSpaceVector0.z;
		else if (key == "vector1") file >> result.colorSpaceVector1.x >> result.colorSpaceVector1.y >> result.colorSpaceVector1.z;
		else if (key == "vector2") file >> result.colorSpaceVector2.x >> result.colorSpaceVector2.y >> result.colorSpaceVector2.z;
	}
	return result.lutWidth > 0u && result.lutHeight > 0u;
}

bool TryWriteStochasticMetadata(const std::wstring& path, const StochasticTextureArtifactResult& result) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::ofstream file(path, std::ios::trunc);
	if (!file) {
		return false;
	}
	file << "lutWidth " << result.lutWidth << "\n";
	file << "lutHeight " << result.lutHeight << "\n";
	file << "transformMode " << static_cast<uint32_t>(result.transformMode) << "\n";
	file << "origin " << result.colorSpaceOrigin.x << " " << result.colorSpaceOrigin.y << " " << result.colorSpaceOrigin.z << "\n";
	file << "vector0 " << result.colorSpaceVector0.x << " " << result.colorSpaceVector0.y << " " << result.colorSpaceVector0.z << "\n";
	file << "vector1 " << result.colorSpaceVector1.x << " " << result.colorSpaceVector1.y << " " << result.colorSpaceVector1.z << "\n";
	file << "vector2 " << result.colorSpaceVector2.x << " " << result.colorSpaceVector2.y << " " << result.colorSpaceVector2.z << "\n";
	return file.good();
}

std::string HashTextureSourceBaseSubresource(const TextureSourceData& sourceData) {
	if (sourceData.subresources.empty() || !sourceData.subresources[0]) {
		return {};
	}
	uint64_t hash = 1469598103934665603ull;
	for (uint8_t byte : *sourceData.subresources[0]) {
		hash ^= static_cast<uint64_t>(byte);
		hash *= 1099511628211ull;
	}
	std::ostringstream ss;
	ss << std::hex << hash;
	return ss.str();
}

std::array<float, 3> Normalize3(std::array<float, 3> v) {
	const float lenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if (lenSq <= 1.0e-12f) {
		return { 0.0f, 0.0f, 0.0f };
	}
	const float invLen = 1.0f / std::sqrt(lenSq);
	return { v[0] * invLen, v[1] * invLen, v[2] * invLen };
}

std::array<float, 3> Cross3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
	return {
		a[1] * b[2] - a[2] * b[1],
		a[2] * b[0] - a[0] * b[2],
		a[0] * b[1] - a[1] * b[0]
	};
}

float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<std::array<float, 3>, 3> JacobiEigenvectors3x3(std::array<std::array<float, 3>, 3> a) {
	std::array<std::array<float, 3>, 3> v = {
		std::array<float, 3>{ 1.0f, 0.0f, 0.0f },
		std::array<float, 3>{ 0.0f, 1.0f, 0.0f },
		std::array<float, 3>{ 0.0f, 0.0f, 1.0f }
	};

	for (uint32_t iter = 0; iter < 12u; ++iter) {
		uint32_t p = 0;
		uint32_t q = 1;
		float maxOffDiag = std::abs(a[0][1]);
		if (std::abs(a[0][2]) > maxOffDiag) {
			p = 0; q = 2; maxOffDiag = std::abs(a[0][2]);
		}
		if (std::abs(a[1][2]) > maxOffDiag) {
			p = 1; q = 2; maxOffDiag = std::abs(a[1][2]);
		}
		if (maxOffDiag < 1.0e-8f) {
			break;
		}

		const float app = a[p][p];
		const float aqq = a[q][q];
		const float apq = a[p][q];
		const float theta = 0.5f * std::atan2(2.0f * apq, aqq - app);
		const float c = std::cos(theta);
		const float s = std::sin(theta);

		for (uint32_t k = 0; k < 3u; ++k) {
			const float akp = a[k][p];
			const float akq = a[k][q];
			a[k][p] = c * akp - s * akq;
			a[k][q] = s * akp + c * akq;
		}
		for (uint32_t k = 0; k < 3u; ++k) {
			const float apk = a[p][k];
			const float aqk = a[q][k];
			a[p][k] = c * apk - s * aqk;
			a[q][k] = s * apk + c * aqk;
		}
		for (uint32_t k = 0; k < 3u; ++k) {
			const float vkp = v[k][p];
			const float vkq = v[k][q];
			v[k][p] = c * vkp - s * vkq;
			v[k][q] = s * vkp + c * vkq;
		}
	}

	std::array<uint32_t, 3> order = { 0u, 1u, 2u };
	std::sort(order.begin(), order.end(), [&](uint32_t lhs, uint32_t rhs) {
		return a[lhs][lhs] > a[rhs][rhs];
	});

	std::array<std::array<float, 3>, 3> axes{};
	for (uint32_t out = 0; out < 3u; ++out) {
		const uint32_t column = order[out];
		axes[out] = Normalize3({ v[0][column], v[1][column], v[2][column] });
	}
	if (Dot3(Cross3(axes[0], axes[1]), axes[2]) < 0.0f) {
		axes[2] = { -axes[2][0], -axes[2][1], -axes[2][2] };
	}
	return axes;
}

void ApplyDiffuseDecorrelation(std::vector<std::array<float, 4>>& pixels, StochasticTextureArtifactResult& result) {
	if (pixels.empty()) {
		return;
	}

	std::array<float, 3> mean = { 0.0f, 0.0f, 0.0f };
	for (const auto& pixel : pixels) {
		mean[0] += std::clamp(pixel[0], 0.0f, 1.0f);
		mean[1] += std::clamp(pixel[1], 0.0f, 1.0f);
		mean[2] += std::clamp(pixel[2], 0.0f, 1.0f);
	}
	const float invCount = 1.0f / static_cast<float>(pixels.size());
	mean[0] *= invCount;
	mean[1] *= invCount;
	mean[2] *= invCount;

	std::array<std::array<float, 3>, 3> covariance = {};
	for (const auto& pixel : pixels) {
		const std::array<float, 3> d = {
			std::clamp(pixel[0], 0.0f, 1.0f) - mean[0],
			std::clamp(pixel[1], 0.0f, 1.0f) - mean[1],
			std::clamp(pixel[2], 0.0f, 1.0f) - mean[2]
		};
		for (uint32_t row = 0; row < 3u; ++row) {
			for (uint32_t col = 0; col < 3u; ++col) {
				covariance[row][col] += d[row] * d[col] * invCount;
			}
		}
	}

	auto axes = JacobiEigenvectors3x3(covariance);
	std::array<float, 3> minProjection = {
		(std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::max)()
	};
	std::array<float, 3> maxProjection = {
		-(std::numeric_limits<float>::max)(),
		-(std::numeric_limits<float>::max)(),
		-(std::numeric_limits<float>::max)()
	};
	for (const auto& pixel : pixels) {
		const std::array<float, 3> rgb = {
			std::clamp(pixel[0], 0.0f, 1.0f),
			std::clamp(pixel[1], 0.0f, 1.0f),
			std::clamp(pixel[2], 0.0f, 1.0f)
		};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const float projection = Dot3(axes[axis], rgb);
			minProjection[axis] = (std::min)(minProjection[axis], projection);
			maxProjection[axis] = (std::max)(maxProjection[axis], projection);
		}
	}

	for (auto& pixel : pixels) {
		const std::array<float, 3> rgb = {
			std::clamp(pixel[0], 0.0f, 1.0f),
			std::clamp(pixel[1], 0.0f, 1.0f),
			std::clamp(pixel[2], 0.0f, 1.0f)
		};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const float range = maxProjection[axis] - minProjection[axis];
			pixel[axis] = range > 1.0e-6f
				? std::clamp((Dot3(axes[axis], rgb) - minProjection[axis]) / range, 0.0f, 1.0f)
				: 0.5f;
		}
	}

	const std::array<float, 3> v0 = {
		axes[0][0] * (maxProjection[0] - minProjection[0]),
		axes[0][1] * (maxProjection[0] - minProjection[0]),
		axes[0][2] * (maxProjection[0] - minProjection[0])
	};
	const std::array<float, 3> v1 = {
		axes[1][0] * (maxProjection[1] - minProjection[1]),
		axes[1][1] * (maxProjection[1] - minProjection[1]),
		axes[1][2] * (maxProjection[1] - minProjection[1])
	};
	const std::array<float, 3> v2 = {
		axes[2][0] * (maxProjection[2] - minProjection[2]),
		axes[2][1] * (maxProjection[2] - minProjection[2]),
		axes[2][2] * (maxProjection[2] - minProjection[2])
	};
	const std::array<float, 3> origin = {
		axes[0][0] * minProjection[0] + axes[1][0] * minProjection[1] + axes[2][0] * minProjection[2],
		axes[0][1] * minProjection[0] + axes[1][1] * minProjection[1] + axes[2][1] * minProjection[2],
		axes[0][2] * minProjection[0] + axes[1][2] * minProjection[1] + axes[2][2] * minProjection[2]
	};
	result.colorSpaceOrigin = { origin[0], origin[1], origin[2] };
	result.colorSpaceVector0 = { v0[0], v0[1], v0[2] };
	result.colorSpaceVector1 = { v1[0], v1[1], v1[2] };
	result.colorSpaceVector2 = { v2[0], v2[1], v2[2] };
}

constexpr bool kEnableGpuBc7Compression = true;

struct PreparedTextureProcessingData {
	std::shared_ptr<TextureSourceData> preparedSourceData;
	std::shared_ptr<TextureSourceData> finalResult;
	bool requiresGpuCompression = false;
};

bool ShouldUseGpuBc7Backend(const TextureSourceData& sourceData, const TextureFileMeta& meta) {
	if (!kEnableGpuBc7Compression) {
		return false;
	}

	if (!meta.processing.requestBlockCompression || IsSourceBlockCompressed(sourceData)) {
		return false;
	}

	if (sourceData.desc.isArray || sourceData.desc.isCubemap || sourceData.desc.arraySize > 1) {
		return false;
	}

	if (sourceData.desc.imageDimensions.empty() || sourceData.desc.channels != 4) {
		return false;
	}

	switch (rhi::helpers::stripSrgb(sourceData.desc.format)) {
	case rhi::Format::R8G8B8A8_UNorm:
	case rhi::Format::B8G8R8A8_UNorm:
		break;
	default:
		return false;
	}

	const DXGI_FORMAT compressedFormat = ChooseCompressedFormat(sourceData, meta);
	return compressedFormat == DXGI_FORMAT_BC7_UNORM || compressedFormat == DXGI_FORMAT_BC7_UNORM_SRGB;
}

std::shared_ptr<TextureSourceData> PrepareTextureSourceDataForBackend(
	const std::shared_ptr<TextureSourceData>& sourceData,
	const TextureFileMeta& meta)
{
	if (!sourceData) {
		throw std::runtime_error("TextureProcessingManager: source data is null");
	}

	const auto clampedSourceData = ClampTextureSourceMipLevels(sourceData, meta.processing.maxMipLevels);
	const uint32_t requestedMipLevels = ResolveRequestedMipLevelCount(
		meta.processing,
		clampedSourceData->desc.imageDimensions[0].width,
		clampedSourceData->desc.imageDimensions[0].height);
	const uint32_t sourceMipLevels = GetTextureMipLevelCount(*clampedSourceData);
	const bool needMipChain = meta.processing.requestMipChain && sourceMipLevels < requestedMipLevels;
	const bool sourceIsBlockCompressed = IsSourceBlockCompressed(*clampedSourceData);
	const bool needCompression = meta.processing.requestBlockCompression && !sourceIsBlockCompressed;
	const bool needDecompression = !meta.processing.requestBlockCompression && sourceIsBlockCompressed;
	const bool needNormalConventionConversion = NeedsNormalConventionConversion(meta);
	const bool needGpuAlphaMipChain = needMipChain && ShouldPreserveAlphaCoverage(meta, *sourceData);
	const bool needCpuMipChain = needMipChain && !needGpuAlphaMipChain;

	if (!needMipChain && !needCompression && !needDecompression && !needNormalConventionConversion) {
		return clampedSourceData;
	}

	ScratchImage workingImage;
	HRESULT hr = InitializeScratchImageFromSource(*clampedSourceData, workingImage);
	if (FAILED(hr)) {
		throw std::runtime_error("TextureProcessingManager: failed to initialize working scratch image");
	}

	ScratchImage linearImage;
	if (sourceIsBlockCompressed) {
		hr = Decompress(
			workingImage.GetImages(),
			workingImage.GetImageCount(),
			workingImage.GetMetadata(),
			ChooseWorkingFormat(meta),
			linearImage);
		if (FAILED(hr)) {
			throw std::runtime_error("TextureProcessingManager: DirectXTex decompress failed");
		}
	}
	else {
		linearImage = std::move(workingImage);
	}

	if (needNormalConventionConversion) {
		if (linearImage.GetMetadata().format != ChooseWorkingFormat(meta)) {
			ScratchImage convertedNormalImage;
			hr = Convert(
				linearImage.GetImages(),
				linearImage.GetImageCount(),
				linearImage.GetMetadata(),
				ChooseWorkingFormat(meta),
				TEX_FILTER_DEFAULT,
				TEX_THRESHOLD_DEFAULT,
				convertedNormalImage);
			if (FAILED(hr)) {
				throw std::runtime_error("TextureProcessingManager: DirectXTex normal conversion failed");
			}
			linearImage = std::move(convertedNormalImage);
		}

		FlipNormalGreenChannel(linearImage);
	}

	ScratchImage mipChainImage;
	ScratchImage* currentImage = &linearImage;
	if (needCpuMipChain) {
		hr = GenerateMipMaps(
			linearImage.GetImages(),
			linearImage.GetImageCount(),
			linearImage.GetMetadata(),
			TEX_FILTER_DEFAULT,
			requestedMipLevels,
			mipChainImage);
		if (FAILED(hr)) {
			throw std::runtime_error("TextureProcessingManager: DirectXTex GenerateMipMaps failed");
		}
		currentImage = &mipChainImage;
	}

	return BuildSourceDataFromScratchImage(*currentImage);
}

std::shared_ptr<TextureSourceData> FinalizeTextureSourceDataOnCpu(
	const std::shared_ptr<TextureSourceData>& preparedSourceData,
	const TextureFileMeta& meta)
{
	if (!preparedSourceData) {
		throw std::runtime_error("TextureProcessingManager: prepared source data is null");
	}

	if (!meta.processing.requestBlockCompression) {
		return preparedSourceData;
	}

	ScratchImage preparedImage;
	HRESULT hr = InitializeScratchImageFromSource(*preparedSourceData, preparedImage);
	if (FAILED(hr)) {
		throw std::runtime_error("TextureProcessingManager: failed to initialize prepared scratch image");
	}

	ScratchImage compressedImage;
	const DXGI_FORMAT targetFormat = ChooseCompressedFormat(*preparedSourceData, meta);
	TEX_COMPRESS_FLAGS flags = TEX_COMPRESS_DEFAULT;
	if (meta.processing.semantic != TextureSemantic::BaseColor &&
		meta.processing.semantic != TextureSemantic::Emissive &&
		meta.processing.semantic != TextureSemantic::OpenPBRColor)
	{
		flags = static_cast<TEX_COMPRESS_FLAGS>(flags | TEX_COMPRESS_UNIFORM);
	}
	flags = static_cast<TEX_COMPRESS_FLAGS>(flags | TEX_COMPRESS_PARALLEL);

	spdlog::debug(
		"TextureProcessingManager: CPU finalize begin semantic={} srcFormat={} targetFormat={} dims={}x{} subresources={} fullMipChain={} preservePackedChannels={} preferSRGB={}",
		TextureSemanticToString(meta.processing.semantic),
		static_cast<uint32_t>(preparedSourceData->desc.format),
		static_cast<uint32_t>(targetFormat),
		preparedSourceData->desc.imageDimensions.empty() ? 0u : preparedSourceData->desc.imageDimensions[0].width,
		preparedSourceData->desc.imageDimensions.empty() ? 0u : preparedSourceData->desc.imageDimensions[0].height,
		preparedSourceData->subresources.size(),
		preparedSourceData->hasFullMipChain,
		meta.processing.preservePackedChannels,
		meta.preferSRGB);
	const auto compressionStart = std::chrono::steady_clock::now();

	hr = Compress(
		preparedImage.GetImages(),
		preparedImage.GetImageCount(),
		preparedImage.GetMetadata(),
		targetFormat,
		flags,
		TEX_THRESHOLD_DEFAULT,
		compressedImage);
	if (FAILED(hr)) {
		std::ostringstream oss;
		oss
			<< "TextureProcessingManager: DirectXTex Compress failed"
			<< " hr=" << FormatHRESULT(hr)
			<< " semantic=" << TextureSemanticToString(meta.processing.semantic)
			<< " srcFormat=" << static_cast<uint32_t>(preparedSourceData->desc.format)
			<< " dxgiSrcFormat=" << static_cast<uint32_t>(preparedImage.GetMetadata().format)
			<< " targetFormat=" << static_cast<uint32_t>(targetFormat)
			<< " dims="
			<< (preparedSourceData->desc.imageDimensions.empty() ? 0u : preparedSourceData->desc.imageDimensions[0].width)
			<< "x"
			<< (preparedSourceData->desc.imageDimensions.empty() ? 0u : preparedSourceData->desc.imageDimensions[0].height)
			<< " subresources=" << preparedSourceData->subresources.size()
			<< " imageCount=" << preparedImage.GetImageCount()
			<< " fullMipChain=" << preparedSourceData->hasFullMipChain
			<< " preferSRGB=" << meta.preferSRGB;
		throw std::runtime_error(oss.str());
	}

	//const auto compressionElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
	//	std::chrono::steady_clock::now() - compressionStart).count();
	//spdlog::info(
	//	"TextureProcessingManager: CPU finalize complete semantic={} targetFormat={} elapsedMs={}",
	//	TextureSemanticToString(meta.processing.semantic),
	//	static_cast<uint32_t>(targetFormat),
	//	compressionElapsedMs);

	return BuildSourceDataFromScratchImage(compressedImage);
}

bool ShouldTraceTextureProcessing(std::string_view cachePath, std::string_view sourcePath) {
	char* filterValue = nullptr;
	size_t filterValueLength = 0;
	_dupenv_s(&filterValue, &filterValueLength, "SARP_TEXTURE_PROCESSING_TRACE_FILTER");
	if (filterValue == nullptr || *filterValue == '\0') {
		std::free(filterValue);
		return false;
	}

	const std::string ownedFilters(filterValue);
	std::free(filterValue);
	std::string_view filters(ownedFilters);
	while (!filters.empty()) {
		const size_t separator = filters.find(';');
		const std::string_view filter = filters.substr(0, separator);
		if (!filter.empty() &&
			(cachePath.find(filter) != std::string_view::npos || sourcePath.find(filter) != std::string_view::npos))
		{
			return true;
		}
		if (separator == std::string_view::npos) {
			break;
		}
		filters.remove_prefix(separator + 1u);
	}
	return false;
}

bool HasNonTransparentPreparedPixels(const TextureSourceData& sourceData)
{
	if (sourceData.desc.channels != 4u) {
		return false;
	}

	for (const auto& subresource : sourceData.subresources) {
		if (!subresource) {
			continue;
		}
		for (size_t byte = 3u; byte < subresource->size(); byte += 4u) {
			if ((*subresource)[byte] != 0u) {
				return true;
			}
		}
	}
	return false;
}

bool IsCanonicalTransparentBlackBc7Chain(const TextureSourceData& sourceData)
{
	const auto format = rhi::helpers::stripSrgb(sourceData.desc.format);
	if (format != rhi::Format::BC7_UNorm || sourceData.subresources.empty()) {
		return false;
	}

	bool sawBlock = false;
	for (const auto& subresource : sourceData.subresources) {
		if (!subresource || subresource->empty() || (subresource->size() % 16u) != 0u) {
			return false;
		}
		for (size_t blockOffset = 0; blockOffset < subresource->size(); blockOffset += 16u) {
			sawBlock = true;
			if ((*subresource)[blockOffset] != 0x40u) {
				return false;
			}
			for (size_t byte = 1u; byte < 16u; ++byte) {
				if ((*subresource)[blockOffset + byte] != 0u) {
					return false;
				}
			}
		}
	}
	return sawBlock;
}

PreparedTextureProcessingData ProcessTextureSourceData(
	const std::shared_ptr<TextureSourceData>& sourceData,
	const TextureFileMeta& meta)
{
	PreparedTextureProcessingData prepared{};
	prepared.preparedSourceData = PrepareTextureSourceDataForBackend(sourceData, meta);
	if (prepared.preparedSourceData && !TextureProcessingManager::GetInstance().NeedsProcessing(*prepared.preparedSourceData, meta)) {
		prepared.finalResult = prepared.preparedSourceData;
		return prepared;
	}
	prepared.requiresGpuCompression = prepared.preparedSourceData && ShouldUseGpuBc7Backend(*prepared.preparedSourceData, meta);
	if (!prepared.requiresGpuCompression) {
		prepared.finalResult = FinalizeTextureSourceDataOnCpu(prepared.preparedSourceData, meta);
	}
	return prepared;
}
}

TextureProcessingManager& TextureProcessingManager::GetInstance() {
	static TextureProcessingManager instance;
	return instance;
}

bool TextureProcessingManager::ShouldProcess(const TextureFileMeta& meta) const {
	return meta.processing.isParticipatingMaterialTexture &&
		(meta.processing.requestMipChain || meta.processing.requestBlockCompression || meta.processing.maxMipLevels != 0u);
}

bool TextureProcessingManager::NeedsProcessing(const TextureSourceData& sourceData, const TextureFileMeta& meta) const {
	if (!ShouldProcess(meta)) {
		return false;
	}

	if (meta.isProcessingCacheArtifact) {
		return false;
	}

	const uint32_t mipLevelCount = GetTextureMipLevelCount(sourceData);
	const uint32_t requestedMipLevels = sourceData.desc.imageDimensions.empty()
		? mipLevelCount
		: ResolveRequestedMipLevelCount(
			meta.processing,
			sourceData.desc.imageDimensions[0].width,
			sourceData.desc.imageDimensions[0].height);
	const bool needMipChain = meta.processing.requestMipChain && mipLevelCount < requestedMipLevels;
	const bool needMipClamp = meta.processing.maxMipLevels != 0u && mipLevelCount > requestedMipLevels;
	const bool sourceIsBlockCompressed = IsSourceBlockCompressed(sourceData);
	const bool needCompression = meta.processing.requestBlockCompression && !sourceIsBlockCompressed;
	const bool needDecompression = !meta.processing.requestBlockCompression && sourceIsBlockCompressed;
	const bool needNormalConventionConversion = NeedsNormalConventionConversion(meta);
	return needMipChain || needMipClamp || needCompression || needDecompression || needNormalConventionConversion;
}

std::wstring TextureProcessingManager::GetExistingCachePathForFile(const TextureFileMeta& meta) const {
	if (!ShouldProcess(meta) || meta.filePath.empty()) {
		return {};
	}

	const std::string key = BuildProcessingCacheKey(meta);
	const std::wstring conditionedCachePath = BuildProcessingConditionedCachePath(key);
	std::error_code ec;
	if (std::filesystem::exists(std::filesystem::path(conditionedCachePath), ec) && !ec) {
		return conditionedCachePath;
	}

	const std::wstring cachePath = BuildProcessingCachePath(key);
	if (!std::filesystem::exists(std::filesystem::path(cachePath), ec) || ec) {
		return {};
	}

	if (auto cachedSourceData = TryLoadTextureSourceDataFromCache(key)) {
		const std::wstring backfilledConditionedCachePath = TryWriteTextureSourceDataToCache(key, *cachedSourceData);
		if (!backfilledConditionedCachePath.empty()) {
			spdlog::debug(
				"TextureProcessingManager: backfilled conditioned cache '{}' from legacy DDS cache '{}'",
				ws2s(backfilledConditionedCachePath),
				ws2s(cachePath));
			return backfilledConditionedCachePath;
		}
	}

	return {};
}

StochasticTextureArtifactResult TextureProcessingManager::RequestStochasticArtifactsBlocking(
	const std::shared_ptr<TextureSourceData>& sourceData,
	const TextureFileMeta& meta,
	const StochasticTextureArtifactSettings& settings)
{
	StochasticTextureArtifactResult result{};
	if (!sourceData) {
		result.failureReason = "source data is null";
		return result;
	}
	if (sourceData->desc.isArray || sourceData->desc.isCubemap || sourceData->desc.imageDimensions.empty()) {
		result.failureReason = "only non-array 2D textures are supported";
		return result;
	}

	const bool isNormal = settings.semantic == TextureSemantic::Normal;
	const bool isScalar = settings.semantic == TextureSemantic::Height ||
		settings.semantic == TextureSemantic::OpenPBRScalar;
	const bool isDiffuse = settings.semantic == TextureSemantic::BaseColor ||
		settings.semantic == TextureSemantic::OpenPBRColor ||
		settings.semantic == TextureSemantic::Emissive;
	if (!isNormal && !isDiffuse && !isScalar) {
		result.failureReason = "semantic is not active for stochastic terrain sampling";
		return result;
	}

	const std::string identity = settings.sourceIdentity.empty()
		? ResolveProcessingIdentity(meta)
		: NormalizeCacheSourcePath(settings.sourceIdentity);
	const uint32_t lutWidth = (std::max)(16u, settings.lutWidth);
	TextureFileMeta versionMeta = meta;
	if (!settings.sourceIdentity.empty()) {
		versionMeta.processing.sourceIdentity = settings.sourceIdentity;
	}
	const std::string versionTag = TryGetSourceVersionTag(versionMeta);
	const std::string contentHash = HashTextureSourceBaseSubresource(*sourceData);
	std::ostringstream keyBuilder;
	keyBuilder
		<< "terrain-stochastic-v" << settings.algorithmVersion
		<< "|identity:" << identity
		<< "|version:" << versionTag
		<< "|baseHash:" << contentHash
		<< "|semantic:" << TextureSemanticToString(settings.semantic)
		<< "|srgb:" << (settings.preferSRGB ? 1 : 0)
		<< "|normalConv:" << static_cast<uint32_t>(settings.normalConvention)
		<< "|lut:" << lutWidth;
	const std::string key = keyBuilder.str();
	result.gaussianCachePath = BuildStochasticCachePath(key, L"_gaussian.dstexcache");
	result.inverseLutCachePath = BuildStochasticCachePath(key, L"_invlut.dstexcache");
	const std::wstring metaPath = BuildStochasticCachePath(key, L".stochmeta");

	{
		std::error_code ec;
		if (std::filesystem::exists(result.gaussianCachePath, ec) && !ec &&
			std::filesystem::exists(result.inverseLutCachePath, ec) && !ec &&
			std::filesystem::exists(metaPath, ec) && !ec &&
			TryReadStochasticMetadata(metaPath, result)) {
			result.ready = true;
			result.loadedFromCache = true;
			return result;
		}
	}

	std::scoped_lock cacheWriteLock(GetCacheWriteMutexForKey(key));
	{
		std::error_code ec;
		if (std::filesystem::exists(result.gaussianCachePath, ec) && !ec &&
			std::filesystem::exists(result.inverseLutCachePath, ec) && !ec &&
			std::filesystem::exists(metaPath, ec) && !ec &&
			TryReadStochasticMetadata(metaPath, result)) {
			result.ready = true;
			result.loadedFromCache = true;
			return result;
		}
	}

	try {
		ScratchImage sourceScratch;
		HRESULT hr = InitializeScratchImageFromSource(*sourceData, sourceScratch);
		if (FAILED(hr)) {
			throw std::runtime_error("failed to initialize source scratch image");
		}
		if (IsSourceBlockCompressed(*sourceData)) {
			ScratchImage decompressed;
			hr = Decompress(sourceScratch.GetImages(), sourceScratch.GetImageCount(), sourceScratch.GetMetadata(), DXGI_FORMAT_UNKNOWN, decompressed);
			if (FAILED(hr)) {
				throw std::runtime_error("DirectXTex decompress failed");
			}
			sourceScratch = std::move(decompressed);
		}
		ScratchImage floatScratch;
		hr = Convert(
			sourceScratch.GetImages(),
			sourceScratch.GetImageCount(),
			sourceScratch.GetMetadata(),
			DXGI_FORMAT_R32G32B32A32_FLOAT,
			TEX_FILTER_DEFAULT,
			TEX_THRESHOLD_DEFAULT,
			floatScratch);
		if (FAILED(hr)) {
			throw std::runtime_error("DirectXTex float conversion failed");
		}

		const Image* image = floatScratch.GetImage(0, 0, 0);
		if (!image || !image->pixels || image->width == 0 || image->height == 0) {
			throw std::runtime_error("invalid converted source image");
		}

		const uint32_t width = static_cast<uint32_t>(image->width);
		const uint32_t height = static_cast<uint32_t>(image->height);
		const size_t pixelCount = static_cast<size_t>(width) * height;
		std::vector<std::array<float, 4>> sourcePixels(pixelCount);
		for (uint32_t y = 0; y < height; ++y) {
			const auto* row = reinterpret_cast<const float*>(image->pixels + static_cast<size_t>(y) * image->rowPitch);
			for (uint32_t x = 0; x < width; ++x) {
				const size_t index = static_cast<size_t>(y) * width + x;
				std::array<float, 4> value = {
					row[x * 4u + 0u],
					row[x * 4u + 1u],
					row[x * 4u + 2u],
					row[x * 4u + 3u]
				};
				if (isNormal && settings.normalConvention == NormalMapConvention::OpenGL) {
					value[1] = 1.0f - value[1];
				}
				sourcePixels[index] = value;
			}
		}

		if (isDiffuse) {
			ApplyDiffuseDecorrelation(sourcePixels, result);
		}

		const uint32_t channels = isNormal ? 2u : (isScalar ? 1u : 4u);
		const rhi::Format gaussianFormat = isNormal
			? rhi::Format::R8G8_UNorm
			: (isScalar ? rhi::Format::R8_UNorm : rhi::Format::R8G8B8A8_UNorm);
		const rhi::Format lutFormat = gaussianFormat;
		const uint32_t lutHeight = CalcMipCount(width, height);
		auto gaussianizedBasePixels = BuildGaussianizedPixels(sourcePixels, channels);
		auto gaussian = BuildMipmappedUnormSourceData(
			gaussianizedBasePixels,
			width,
			height,
			channels,
			gaussianFormat);
		auto inverseLut = BuildInverseLutSourceData(
			sourcePixels,
			channels,
			lutWidth,
			lutHeight,
			lutFormat,
			gaussianizedBasePixels,
			width,
			height);
		if (!TryWriteConditionedTextureCache(result.gaussianCachePath, *gaussian)) {
			throw std::runtime_error("failed to write Gaussian texture cache");
		}
		if (!TryWriteConditionedTextureCache(result.inverseLutCachePath, *inverseLut)) {
			throw std::runtime_error("failed to write inverse LUT cache");
		}

		result.ready = true;
		result.loadedFromCache = false;
		result.lutWidth = lutWidth;
		result.lutHeight = lutHeight;
		result.transformMode = isNormal
			? StochasticTextureTransformMode::NormalXY
			: (isScalar ? StochasticTextureTransformMode::Scalar : StochasticTextureTransformMode::DecorrelatedColor);
		if (!TryWriteStochasticMetadata(metaPath, result)) {
			throw std::runtime_error("failed to write stochastic metadata");
		}
		spdlog::info(
			"TextureProcessingManager: built stochastic terrain artifacts identity='{}' semantic={} dims={}x{} lut={}x{}",
			identity,
			TextureSemanticToString(settings.semantic),
			width,
			height,
			lutWidth,
			lutHeight);
	}
	catch (const std::exception& ex) {
		result.ready = false;
		result.failureReason = ex.what();
		spdlog::warn("TextureProcessingManager: stochastic artifact build failed for '{}': {}", identity, ex.what());
	}
	return result;
}

std::string TextureProcessingManager::BuildProcessingCacheKey(
	const TextureFileMeta& meta) const
{
	const std::string normalizedIdentity = ResolveProcessingIdentity(meta);
	const std::string sourceVersionTag = TryGetSourceVersionTag(meta);

	size_t seed = 0;
	boost::hash_combine(seed, normalizedIdentity);
	boost::hash_combine(seed, sourceVersionTag);
	boost::hash_combine(seed, static_cast<uint32_t>(meta.processing.semantic));
	boost::hash_combine(seed, meta.processing.requestMipChain);
	boost::hash_combine(seed, meta.processing.requestBlockCompression);
	boost::hash_combine(seed, meta.processing.preferSRGB);
	boost::hash_combine(seed, meta.processing.preservePackedChannels);
	boost::hash_combine(seed, static_cast<uint32_t>(meta.processing.normalConvention));
	boost::hash_combine(seed, meta.processing.maxMipLevels);
	boost::hash_combine(seed, meta.alphaIsAllOpaque);
	// Any change that can alter processed pixels, formats, or the exported mip
	// chain must increment this version. Version 16 invalidates artifacts built
	// while concurrent graph compilation could race the BC7 pass job queues.
	boost::hash_combine(seed, kTextureProcessingCacheVersion);
	return normalizedIdentity + "#" + TextureSemanticToString(meta.processing.semantic) + "#" + std::to_string(seed);
}

std::string TextureProcessingManager::BuildProcessingJobKey(
	const std::shared_ptr<TextureSourceData>& sourceData,
	const TextureFileMeta& meta) const
{
	const std::string cacheKey = BuildProcessingCacheKey(meta);
	if (!sourceData) {
		return cacheKey;
	}

	size_t seed = 0;
	boost::hash_combine(seed, cacheKey);
	const uint32_t mipLevelCount = GetTextureMipLevelCount(*sourceData);
	const uint32_t baseWidth = sourceData->desc.imageDimensions.empty() ? 0u : sourceData->desc.imageDimensions[0].width;
	const uint32_t baseHeight = sourceData->desc.imageDimensions.empty() ? 0u : sourceData->desc.imageDimensions[0].height;
	boost::hash_combine(seed, static_cast<uint32_t>(sourceData->desc.format));
	boost::hash_combine(seed, baseWidth);
	boost::hash_combine(seed, baseHeight);
	boost::hash_combine(seed, mipLevelCount);
	boost::hash_combine(seed, static_cast<uint32_t>(sourceData->subresources.size()));
	boost::hash_combine(seed, sourceData->hasFullMipChain);
	boost::hash_combine(seed, IsSourceBlockCompressed(*sourceData));
	return cacheKey + "#job#" + std::to_string(seed);
}

std::shared_ptr<TextureProcessingJobHandle> TextureProcessingManager::RequestProcessing(
	const std::shared_ptr<TextureSourceData>& sourceData,
	const TextureFileMeta& meta)
{
	if (!ShouldProcess(meta) || !sourceData) {
		return {};
	}

	const std::string cacheKey = BuildProcessingCacheKey(meta);
	const std::string key = BuildProcessingJobKey(sourceData, meta);
	const std::string traceCachePath = ws2s(BuildProcessingCachePath(cacheKey));
	if (ShouldTraceTextureProcessing(traceCachePath, meta.filePath)) {
		spdlog::info(
			"TextureProcessingManager trace: cache='{}' source='{}' identity='{}' semantic={} sourceHash={} format={} dims={}x{} subresources={}",
			traceCachePath,
			meta.filePath,
			meta.processing.sourceIdentity,
			TextureSemanticToString(meta.processing.semantic),
			HashTextureSourceBaseSubresource(*sourceData),
			static_cast<uint32_t>(sourceData->desc.format),
			sourceData->desc.imageDimensions.empty() ? 0u : sourceData->desc.imageDimensions[0].width,
			sourceData->desc.imageDimensions.empty() ? 0u : sourceData->desc.imageDimensions[0].height,
			sourceData->subresources.size());
	}

	auto handle = std::make_shared<TextureProcessingJobHandle>();
	handle->requestMeta = meta;
	handle->processingKey = key;
	handle->cacheKey = cacheKey;
	handle->state.store(TextureProcessingJobState::Queued, std::memory_order_release);

	{
		std::scoped_lock lock(m_mutex);
		auto [it, inserted] = m_jobsByKey.try_emplace(key, handle);
		if (!inserted) {
			return it->second;
		}
	}

	TaskSchedulerManager::GetInstance().RunBackgroundTask("TextureProcessingManager::RequestProcessing", [handle, sourceData, meta, key, cacheKey]() {
		handle->state.store(TextureProcessingJobState::CpuPreparing, std::memory_order_release);
		try {
			const std::wstring conditionedCachePath = BuildProcessingConditionedCachePath(cacheKey);
			std::error_code cacheEc;
			if (std::filesystem::exists(std::filesystem::path(conditionedCachePath), cacheEc) && !cacheEc) {
				spdlog::debug(
					"TextureProcessingManager: conditioned cache hit for '{}' file='{}' semantic={} path='{}'",
					cacheKey,
					meta.filePath,
					TextureSemanticToString(meta.processing.semantic),
					ws2s(conditionedCachePath));
				{
					std::scoped_lock lock(handle->mutex);
					handle->conditionedCachePath = ws2s(conditionedCachePath);
					handle->preparedSourceData.reset();
					handle->result.reset();
					handle->uploadedImage.reset();
					handle->loadedFromCache = true;
					handle->requiresGpuCompression = false;
					handle->completedOnGpu = false;
					handle->error.clear();
				}
				handle->state.store(TextureProcessingJobState::Ready, std::memory_order_release);
				return;
			}

			spdlog::debug(
				"TextureProcessingManager: begin processing '{}' semantic={} srcFormat={} blockCompressed={} fullMipChain={} subresources={} dims={}x{} preservePackedChannels={}",
				key,
				TextureSemanticToString(meta.processing.semantic),
				sourceData ? static_cast<uint32_t>(sourceData->desc.format) : 0u,
				sourceData ? sourceData->isBlockCompressed : false,
				sourceData ? sourceData->hasFullMipChain : false,
				sourceData ? sourceData->subresources.size() : 0u,
				(sourceData && !sourceData->desc.imageDimensions.empty()) ? sourceData->desc.imageDimensions[0].width : 0u,
				(sourceData && !sourceData->desc.imageDimensions.empty()) ? sourceData->desc.imageDimensions[0].height : 0u,
				meta.processing.preservePackedChannels);

			if (auto cachedResult = TryLoadTextureSourceDataFromCache(cacheKey)) {
				const std::wstring backfilledConditionedCachePath = TryWriteTextureSourceDataToCache(cacheKey, *cachedResult);
				spdlog::debug(
					"TextureProcessingManager: cache hit for request='{}' cache='{}' file='{}' semantic={} bc={} mips={} fmt={} subresources={} dims={}x{}",
					key,
					cacheKey,
					meta.filePath,
					TextureSemanticToString(meta.processing.semantic),
					meta.processing.requestBlockCompression,
					meta.processing.requestMipChain,
					static_cast<uint32_t>(cachedResult->desc.format),
					cachedResult->subresources.size(),
					cachedResult->desc.imageDimensions.empty() ? 0u : cachedResult->desc.imageDimensions[0].width,
					cachedResult->desc.imageDimensions.empty() ? 0u : cachedResult->desc.imageDimensions[0].height);
				{
					std::scoped_lock lock(handle->mutex);
					handle->conditionedCachePath = ws2s(backfilledConditionedCachePath);
					handle->preparedSourceData.reset();
					handle->result = std::move(cachedResult);
					handle->uploadedImage.reset();
					handle->loadedFromCache = true;
					handle->requiresGpuCompression = false;
					handle->completedOnGpu = false;
					handle->error.clear();
				}
				handle->state.store(TextureProcessingJobState::Ready, std::memory_order_release);
				return;
			}

			auto prepared = ProcessTextureSourceData(sourceData, meta);
			if (prepared.preparedSourceData) {
				spdlog::debug(
					"TextureProcessingManager: prepared '{}' semantic={} fmt={} blockCompressed={} fullMipChain={} subresources={} dims={}x{} requiresGpuCompression={}",
					key,
					TextureSemanticToString(meta.processing.semantic),
					static_cast<uint32_t>(prepared.preparedSourceData->desc.format),
					prepared.preparedSourceData->isBlockCompressed,
					prepared.preparedSourceData->hasFullMipChain,
					prepared.preparedSourceData->subresources.size(),
					prepared.preparedSourceData->desc.imageDimensions.empty() ? 0u : prepared.preparedSourceData->desc.imageDimensions[0].width,
					prepared.preparedSourceData->desc.imageDimensions.empty() ? 0u : prepared.preparedSourceData->desc.imageDimensions[0].height,
					prepared.requiresGpuCompression);
			}
			if (prepared.requiresGpuCompression) {
				{
					std::scoped_lock lock(handle->mutex);
					handle->preparedSourceData = std::move(prepared.preparedSourceData);
					handle->result.reset();
					handle->uploadedImage.reset();
					handle->loadedFromCache = false;
					handle->requiresGpuCompression = true;
					handle->completedOnGpu = false;
					handle->error.clear();
				}
				handle->state.store(TextureProcessingJobState::GpuReadyToSubmit, std::memory_order_release);
				spdlog::debug(
					"TextureProcessingManager: prepared texture '{}' semantic={} for GPU BC7 submission",
					key,
					TextureSemanticToString(meta.processing.semantic));
				return;
			}

			auto result = std::move(prepared.finalResult);
			std::wstring writtenConditionedCachePath;
			if (result) {
				writtenConditionedCachePath = TryWriteTextureSourceDataToCache(cacheKey, *result);
			}
			{
				std::scoped_lock lock(handle->mutex);
				handle->conditionedCachePath = ws2s(writtenConditionedCachePath);
				handle->preparedSourceData.reset();
				handle->result = std::move(result);
				handle->uploadedImage.reset();
				handle->loadedFromCache = false;
				handle->requiresGpuCompression = false;
				handle->completedOnGpu = false;
				handle->error.clear();
			}
			handle->state.store(TextureProcessingJobState::Ready, std::memory_order_release);
			spdlog::debug(
				"TextureProcessingManager: processed texture '{}' semantic={} bc={} mips={}",
				key,
				TextureSemanticToString(meta.processing.semantic),
				meta.processing.requestBlockCompression,
				meta.processing.requestMipChain);
		}
		catch (const std::exception& ex) {
			{
				std::scoped_lock lock(handle->mutex);
				handle->error = ex.what();
			}
			handle->state.store(TextureProcessingJobState::Failed, std::memory_order_release);
			spdlog::error("TextureProcessingManager: processing failed for '{}': {}", key, ex.what());
		}
	});

	return handle;
}

void TextureProcessingManager::MarkGpuJobSubmitted(const std::shared_ptr<TextureProcessingJobHandle>& handle) {
	if (!handle) {
		return;
	}

	handle->state.store(TextureProcessingJobState::GpuSubmitted, std::memory_order_release);
}

void TextureProcessingManager::MarkGpuJobReadbackPending(const std::shared_ptr<TextureProcessingJobHandle>& handle) {
	if (!handle) {
		return;
	}

	handle->state.store(TextureProcessingJobState::ReadbackPending, std::memory_order_release);
}

void TextureProcessingManager::CompleteGpuProcessing(
	const std::shared_ptr<TextureProcessingJobHandle>& handle,
	std::shared_ptr<TextureSourceData> result,
	std::shared_ptr<PixelBuffer> uploadedImage,
	bool writeCacheArtifact)
{
	if (!handle) {
		return;
	}

	std::shared_ptr<TextureSourceData> preparedSourceData;
	TextureFileMeta requestMeta;
	std::string cacheKey;
	std::string processingKey;
	{
		std::scoped_lock lock(handle->mutex);
		preparedSourceData = handle->preparedSourceData;
		requestMeta = handle->requestMeta;
		cacheKey = handle->cacheKey;
		processingKey = handle->processingKey;
	}

	// Mode 6 encodes transparent black as 40 00 ... 00. A complete chain made
	// only of that block cannot be correct when the uploaded RGBA source has
	// visible pixels. Never persist such an artifact: retry just this texture on
	// the CPU so an ordering regression cannot poison the durable cache again.
	if (result && preparedSourceData &&
		HasNonTransparentPreparedPixels(*preparedSourceData) &&
		IsCanonicalTransparentBlackBc7Chain(*result))
	{
		spdlog::error(
			"TextureProcessingManager: rejected unexpected transparent-black GPU BC7 output for '{}'; retrying this texture on CPU",
			processingKey);
		handle->state.store(TextureProcessingJobState::CpuPreparing, std::memory_order_release);
		TaskSchedulerManager::GetInstance().RunBackgroundTask(
			"TextureProcessingManager::GpuBc7ValidationFallback",
			[handle, preparedSourceData, requestMeta, cacheKey, processingKey]() {
				try {
					auto cpuResult = FinalizeTextureSourceDataOnCpu(preparedSourceData, requestMeta);
					std::wstring cpuCachePath;
					if (cpuResult && !cacheKey.empty()) {
						cpuCachePath = TryWriteTextureSourceDataToCache(cacheKey, *cpuResult);
					}
					{
						std::scoped_lock lock(handle->mutex);
						handle->conditionedCachePath = ws2s(cpuCachePath);
						handle->preparedSourceData.reset();
						handle->result = std::move(cpuResult);
						handle->uploadedImage.reset();
						handle->loadedFromCache = false;
						handle->requiresGpuCompression = false;
						handle->completedOnGpu = false;
						handle->error.clear();
					}
					handle->state.store(TextureProcessingJobState::Ready, std::memory_order_release);
					spdlog::info(
						"TextureProcessingManager: CPU fallback completed for rejected GPU BC7 output '{}'",
						processingKey);
				}
				catch (const std::exception& ex) {
					TextureProcessingManager::GetInstance().FailProcessing(handle, ex.what());
				}
			});
		return;
	}

	std::wstring conditionedCachePath;
	if (writeCacheArtifact && result && !handle->cacheKey.empty()) {
		conditionedCachePath = TryWriteTextureSourceDataToCache(handle->cacheKey, *result);
	}
	if (handle->requestMeta.processing.isParticipatingMaterialTexture && !conditionedCachePath.empty()) {
		// The full-resolution GPU output was needed to build the conditioned cache,
		// but material residency must be created from the requested mip window.
		uploadedImage.reset();
	}

	{
		std::scoped_lock lock(handle->mutex);
		handle->conditionedCachePath = ws2s(conditionedCachePath);
		handle->preparedSourceData.reset();
		handle->result = std::move(result);
		handle->uploadedImage = std::move(uploadedImage);
		handle->loadedFromCache = false;
		handle->requiresGpuCompression = false;
		handle->completedOnGpu = true;
		handle->error.clear();
	}

	handle->state.store(TextureProcessingJobState::Ready, std::memory_order_release);
}

void TextureProcessingManager::FailProcessing(const std::shared_ptr<TextureProcessingJobHandle>& handle, std::string error) {
	if (!handle) {
		return;
	}

	std::string key;
	{
		std::scoped_lock lock(handle->mutex);
		key = handle->processingKey;
	}

	{
		std::scoped_lock lock(handle->mutex);
		handle->error = std::move(error);
	}

	spdlog::error(
		"TextureProcessingManager: async processing failed for '{}': {}",
		key,
		handle->error);

	handle->state.store(TextureProcessingJobState::Failed, std::memory_order_release);
}
