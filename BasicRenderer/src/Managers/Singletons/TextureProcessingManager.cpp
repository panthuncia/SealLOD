#include "Managers/Singletons/TextureProcessingManager.h"

#include <algorithm>
#include <chrono>
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
	for (const std::string* candidate : { &meta.filePath, &meta.processing.sourceIdentity }) {
		if (!candidate || candidate->empty()) {
			continue;
		}

		std::error_code ec;
		const std::filesystem::path path(*candidate);
		if (!std::filesystem::exists(path, ec) || ec) {
			continue;
		}

		auto lastWriteTime = std::filesystem::last_write_time(path, ec);
		if (ec) {
			continue;
		}

		return std::to_string(lastWriteTime.time_since_epoch().count());
	}

	return {};
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
	if (sourceData.isBlockCompressed) {
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

			std::memcpy(const_cast<uint8_t*>(dstImage->pixels), bytes->data(), static_cast<size_t>(dims.slicePitch));
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

	if (!meta.processing.requestBlockCompression || sourceData.isBlockCompressed) {
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

	const bool needMipChain = meta.processing.requestMipChain && !sourceData->hasFullMipChain;
	const bool needCompression = meta.processing.requestBlockCompression && !sourceData->isBlockCompressed;
	const bool needDecompression = !meta.processing.requestBlockCompression && sourceData->isBlockCompressed;
	const bool needNormalConventionConversion = NeedsNormalConventionConversion(meta);
	const bool needGpuAlphaMipChain = needMipChain && ShouldPreserveAlphaCoverage(meta, *sourceData);
	const bool needCpuMipChain = needMipChain && !needGpuAlphaMipChain;

	if (!needMipChain && !needCompression && !needDecompression && !needNormalConventionConversion) {
		return sourceData;
	}

	ScratchImage workingImage;
	HRESULT hr = InitializeScratchImageFromSource(*sourceData, workingImage);
	if (FAILED(hr)) {
		throw std::runtime_error("TextureProcessingManager: failed to initialize working scratch image");
	}

	ScratchImage linearImage;
	if (sourceData->isBlockCompressed) {
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
			0,
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
		throw std::runtime_error("TextureProcessingManager: DirectXTex Compress failed");
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

PreparedTextureProcessingData ProcessTextureSourceData(
	const std::shared_ptr<TextureSourceData>& sourceData,
	const TextureFileMeta& meta)
{
	PreparedTextureProcessingData prepared{};
	prepared.preparedSourceData = PrepareTextureSourceDataForBackend(sourceData, meta);
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
		(meta.processing.requestMipChain || meta.processing.requestBlockCompression);
}

bool TextureProcessingManager::NeedsProcessing(const TextureSourceData& sourceData, const TextureFileMeta& meta) const {
	if (!ShouldProcess(meta)) {
		return false;
	}

	if (meta.isProcessingCacheArtifact) {
		return false;
	}

	const uint32_t mipLevelCount = GetTextureMipLevelCount(sourceData);
	const bool hasResidentMipChain = mipLevelCount > 1u;
	const bool needMipChain = meta.processing.requestMipChain && !sourceData.hasFullMipChain && !hasResidentMipChain;
	const bool needCompression = meta.processing.requestBlockCompression && !sourceData.isBlockCompressed;
	const bool needDecompression = !meta.processing.requestBlockCompression && sourceData.isBlockCompressed;
	const bool needNormalConventionConversion = NeedsNormalConventionConversion(meta);
	return needMipChain || needCompression || needDecompression || needNormalConventionConversion;
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
	boost::hash_combine(seed, meta.alphaIsAllOpaque);
	boost::hash_combine(seed, 5u); // texture processing algorithm/cache version
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
	boost::hash_combine(seed, sourceData->isBlockCompressed);
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

	std::wstring conditionedCachePath;
	if (writeCacheArtifact && result && !handle->cacheKey.empty()) {
		conditionedCachePath = TryWriteTextureSourceDataToCache(handle->cacheKey, *result);
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
