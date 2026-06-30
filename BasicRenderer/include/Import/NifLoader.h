#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <filesystem>
#include <string>

#include "Import/USDLoader.h"

class Scene;

namespace NifLoader {

struct LoadTimingStats {
	bool cacheHit = false;
	bool payloadCacheHit = false;
	bool assetCacheWritten = false;
	std::uint64_t cacheProbeMs = 0;
	std::uint64_t brniflyMs = 0;
	std::uint64_t brniflyDescribeMs = 0;
	std::uint64_t brniflyConvertMs = 0;
	std::uint64_t brniflyClientPersistentStartMs = 0;
	std::uint64_t brniflyClientWriteRequestMs = 0;
	std::uint64_t brniflyClientWaitResponseMs = 0;
	std::uint64_t brniflyClientWaitFirstByteMs = 0;
	std::uint64_t brniflyClientWaitMoreResponseMs = 0;
	std::uint64_t brniflyClientReadResponseMs = 0;
	std::uint64_t brniflyClientResponseBytes = 0;
	std::uint64_t brniflyClientResponseChunks = 0;
	std::uint64_t brniflyClientSharedMemoryReadMs = 0;
	std::uint64_t brniflyClientParseJsonMs = 0;
	std::uint64_t brniflyChildLoadNiflyApiMs = 0;
	std::uint64_t brniflyChildNiflyLoadMs = 0;
	std::uint64_t brniflyChildGetGameNameMs = 0;
	std::uint64_t brniflyChildReadNodesMs = 0;
	std::uint64_t brniflyChildReadShapesMs = 0;
	std::uint64_t brniflyChildReadExtraDataMs = 0;
	std::uint64_t brniflyChildDestroyNifMs = 0;
	std::uint64_t brniflyChildConvertShapesToUsdMs = 0;
	std::uint64_t brniflyChildUsdExportToStringMs = 0;
	std::uint64_t brniflyChildHashAndResponseMs = 0;
	std::uint64_t brniflyChildSharedMemoryCreateMs = 0;
	std::uint64_t brniflyChildJsonDumpMs = 0;
	std::uint64_t assetWriteMs = 0;
	std::uint64_t usdLoadMs = 0;
	std::uint64_t usdOpenMs = 0;
	std::uint64_t usdExtractMs = 0;
	std::uint64_t meshBuildMs = 0;
	std::filesystem::path cachePath;
	std::string sourceIdentifier;
	std::string contentHash;
	std::string importFailureReason;
};

struct PreprocessResult {
	bool success = false;
	bool skipped = false;
	bool cacheHit = false;
	bool payloadCacheHit = false;
	bool assetCacheWritten = false;
	std::filesystem::path assetCachePath;
	std::string sourceIdentifier;
	std::string contentHash;
	std::string failureReason;
	std::uint64_t submeshes = 0;
	std::uint64_t clodCacheHits = 0;
	std::uint64_t clodCacheMisses = 0;
	std::uint64_t clodBuildMs = 0;
	std::uint64_t clodSaveMs = 0;
	std::uint64_t clodReloadMs = 0;
};

std::optional<USDLoader::ImportedAssetPayload> TryLoadCachedImportedAsset(std::string cacheKey, const USDLoader::ImportSettings& settings = {}, LoadTimingStats* stats = nullptr);
std::optional<USDLoader::ImportedAssetPayload> LoadImportedAssetWithCacheKey(std::string filePath, std::string cacheKey, const USDLoader::ImportSettings& settings = {}, LoadTimingStats* stats = nullptr);
PreprocessResult PreprocessNifWithCacheKey(std::string filePath, std::string cacheKey, const USDLoader::ImportSettings& settings = {}, LoadTimingStats* stats = nullptr);
std::shared_ptr<Scene> LoadModelWithCacheKey(std::string filePath, std::string cacheKey, const USDLoader::ImportSettings& settings = {}, LoadTimingStats* stats = nullptr);
std::shared_ptr<Scene> LoadModel(std::string filePath, const USDLoader::ImportSettings& settings = {});

} // namespace NifLoader
