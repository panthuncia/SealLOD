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
