#include <BasicRenderer/Assets/CLodCacheStore.h>
#include <BasicRenderer/Assets/CachePack.h>

#include <BasicRenderer/Assets/Import/CLodCache.h>
#include "Assets/Cache/Skeleton/SkeletonArtifactValidation.h"
#include <BasicRenderer/Assets/DefaultCLodSettings.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <BasicRenderer/Assets/CachePaths.h>

namespace CLodCacheLoader {

namespace {
	constexpr const char* kFullMeshSubsetSentinel = "__clod_full_mesh__";

	const char* ToCoveragePreservationModeString(ClusterLODCoveragePreservationMode mode)
	{
		switch (mode)
		{
		case ClusterLODCoveragePreservationMode::None:
			return "none";
		case ClusterLODCoveragePreservationMode::PrioritizeEdges:
			return "prioritizeEdges";
		case ClusterLODCoveragePreservationMode::Voxel:
			return "voxel";
		default:
			return "unknown";
		}
	}

		std::mutex& GetCacheSaveMutexForIdentity(const MeshCacheIdentity& identity)
		{
			static std::mutex cacheMutexTableGuard;
			static std::unordered_map<std::string, std::unique_ptr<std::mutex>> cacheMutexTable;

			const std::string key = identity.sourceIdentifier + "|" + identity.primPath + "|" + identity.subsetName +
				(identity.doubleSidedVoxelSourceNormals ? "|double-sided-voxel-normals" : "");

			std::lock_guard<std::mutex> lock(cacheMutexTableGuard);
			auto& mutexPtr = cacheMutexTable[key];
			if (!mutexPtr) {
				mutexPtr = std::make_unique<std::mutex>();
			}
			return *mutexPtr;
		}

	std::string NormalizeSubsetName(const std::string& subsetName)
	{
		return subsetName.empty() ? std::string(kFullMeshSubsetSentinel) : subsetName;
	}

	CLodCache::CacheKey ToCacheKey(const MeshCacheIdentity& identity)
	{
		CLodCache::CacheKey key{};
		key.sourceIdentifier = identity.sourceIdentifier;
		key.primPath = identity.primPath;
		key.subsetName = NormalizeSubsetName(identity.subsetName);
		if (identity.doubleSidedVoxelSourceNormals) {
			key.subsetName += "|double-sided-voxel-normals";
		}
		return key;
	}

	void LogBuildConfigOnce(uint64_t buildHash, std::string_view assetIdentifier)
	{
		static std::mutex loggedHashesMutex;
		static std::unordered_set<uint64_t> loggedHashes;
		{
			std::lock_guard lock(loggedHashesMutex);
			if (!loggedHashes.insert(buildHash).second) {
				return;
			}
		}
		const ClusterLODBuilderSettings effectiveSettings = GetDefaultBuilderSettings(assetIdentifier);
		spdlog::info(
			"CLod cache build config: source='{}' hash=0x{:016X} asset_settings_hash=0x{:016X} sloppy_fallback={} sloppy_error_factor={} coverage_preservation_mode='{}' voxel_grid={} voxel_rays={} voxel_scale={} voxel_opacity_threshold={} env_sloppy_disable='{}' env_sloppy_factor='{}' env_mode='{}' env_grid='{}' env_rays='{}' env_scale='{}' env_opacity_threshold='{}'",
			assetIdentifier,
			buildHash,
			GetCLodAssetSettingsHash(assetIdentifier),
			!effectiveSettings.disableSloppyFallback,
			effectiveSettings.sloppyFallbackErrorFactor,
			ToCoveragePreservationModeString(effectiveSettings.coveragePreservationMode),
			effectiveSettings.voxelGridBaseResolution,
			effectiveSettings.voxelRaysPerCell,
			effectiveSettings.voxelFallbackScalingFactor,
			effectiveSettings.voxelFallbackOpacityThreshold,
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK"),
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_SLOPPY_ERROR_FACTOR"),
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_COVERAGE_PRESERVATION_MODE"),
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_GRID"),
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_RAYS"),
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_SCALE"),
			GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_OPACITY_THRESHOLD"));
	}
}

std::optional<ClusterLODPrebuiltData> TryLoadPrebuilt(const MeshCacheIdentity& identity)
{
	ZoneScopedN("CLodCacheLoader::TryLoadPrebuilt");
	const auto cacheKey = ToCacheKey(identity);
	const uint64_t buildHash = CLodCache::ComputeBuildConfigHash(identity.sourceIdentifier);
	const auto lookup = CLodCache::BuildCacheLookup(cacheKey, buildHash);
	LogBuildConfigOnce(buildHash, identity.sourceIdentifier);
	spdlog::debug("CLodCacheLoader::TryLoadPrebuilt  src='{}' prim='{}' subset='{}' hash=0x{:X} metadata='{}' container='{}'",
		identity.sourceIdentifier,
		identity.primPath,
		identity.subsetName,
		buildHash,
		ws2s(lookup.metadataPath),
		ws2s(lookup.containerPath));
	auto cached = CLodCache::TryLoad(cacheKey, buildHash);
	std::string skeletonArtifactError;
	if (cached && cached->prebuiltData.assemblySkeletonArtifact.jointCount != 0u &&
		!SkeletonArtifactValidation::Validate(cached->prebuiltData.assemblySkeletonArtifact, &skeletonArtifactError)) {
		spdlog::info(
			"CLod cache STALE src='{}' prim='{}' subset='{}': skeleton artifact {} is unavailable or invalid ({})",
			identity.sourceIdentifier,
			identity.primPath,
			identity.subsetName,
			cached->prebuiltData.assemblySkeletonArtifact.id.ToString(),
			skeletonArtifactError);
		cached.reset();
		if (CLodCache::IsCacheOnlyMode()) {
			// Leave the entry in place: the caller treats this as a miss, and the cache belongs to whoever built it.
			return std::nullopt;
		}
		std::error_code metadataError;
		std::error_code containerError;
		CachePack::NoteFileChanged(lookup.metadataPath);
		CachePack::NoteFileChanged(lookup.containerPath);
		std::filesystem::remove(lookup.metadataPath, metadataError);
		std::filesystem::remove(lookup.containerPath, containerError);
		if (metadataError || containerError) {
			spdlog::warn(
				"CLod cache stale-entry removal reported errors: metadata='{}' ({}) container='{}' ({})",
				ws2s(lookup.metadataPath),
				metadataError.message(),
				ws2s(lookup.containerPath),
				containerError.message());
		}
	}
	TracyPlot("CLOD.Cache.TryLoadPrebuilt.Hit", cached.has_value() ? int64_t{ 1 } : int64_t{ 0 });
	if (!cached.has_value()) {
		static std::atomic<uint32_t> loggedMisses{ 0u };
		const uint32_t logIndex = loggedMisses.fetch_add(1u, std::memory_order_relaxed);
		if (logIndex < 128u) {
			std::error_code metadataEc;
			std::error_code containerEc;
			const bool metadataExists = std::filesystem::exists(lookup.metadataPath, metadataEc);
			const bool containerExists = std::filesystem::exists(lookup.containerPath, containerEc);
			spdlog::info(
				"CLod cache MISS src='{}' prim='{}' subset='{}' hash=0x{:016X} metadata='{}' metadataExists={} container='{}' containerExists={}",
				identity.sourceIdentifier,
				identity.primPath,
				identity.subsetName,
				buildHash,
				ws2s(lookup.metadataPath),
				metadataExists,
				ws2s(lookup.containerPath),
				containerExists);
		}
		spdlog::debug("  -> cache not found on disk.");
		return std::nullopt;
	}

	{
		static std::atomic<uint32_t> loggedHits{ 0u };
		const uint32_t logIndex = loggedHits.fetch_add(1u, std::memory_order_relaxed);
		if (logIndex < 32u) {
			spdlog::info(
				"CLod cache HIT src='{}' prim='{}' subset='{}' hash=0x{:016X} metadata='{}'",
				identity.sourceIdentifier,
				identity.primPath,
				identity.subsetName,
				buildHash,
				ws2s(lookup.metadataPath));
		}
	}

	spdlog::debug("  -> cache HIT (groups={}).", cached->prebuiltData.groups.size());
	return std::move(cached->prebuiltData);
}

bool SavePrebuilt(const MeshCacheIdentity& identity, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload)
{
	ZoneScopedN("CLodCacheLoader::SavePrebuilt");
	const auto cacheKey = ToCacheKey(identity);
	const uint64_t buildHash = CLodCache::ComputeBuildConfigHash(identity.sourceIdentifier);
	LogBuildConfigOnce(buildHash, identity.sourceIdentifier);
	return CLodCache::Save(cacheKey, buildHash, prebuiltData, payload);
}

bool SavePrebuilt(const MeshCacheIdentity& identity, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload, ClusterLODPrebuiltData* outSavedPrebuiltData)
{
	ZoneScopedN("CLodCacheLoader::SavePrebuilt::WithSavedData");
	const auto cacheKey = ToCacheKey(identity);
	const uint64_t buildHash = CLodCache::ComputeBuildConfigHash(identity.sourceIdentifier);
	LogBuildConfigOnce(buildHash, identity.sourceIdentifier);
	return CLodCache::Save(cacheKey, buildHash, prebuiltData, payload, outSavedPrebuiltData);
}

bool SavePrebuiltLocked(const MeshCacheIdentity& identity, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload)
{
	ZoneScopedN("CLodCacheLoader::SavePrebuiltLocked");
	spdlog::debug("CLodCacheLoader::SavePrebuiltLocked  src='{}' prim='{}' subset='{}'",
		identity.sourceIdentifier, identity.primPath, identity.subsetName);
	std::lock_guard<std::mutex> saveLock(GetCacheSaveMutexForIdentity(identity));
	if (TryLoadPrebuilt(identity).has_value()) {
		spdlog::debug("  -> already cached (race-condition guard).");
		return true;
	}

	bool ok = SavePrebuilt(identity, prebuiltData, payload);
	if (ok)
		spdlog::debug("  -> SavePrebuilt succeeded.");
	else
		spdlog::warn("  -> SavePrebuilt FAILED.");
	return ok;
}

bool SavePrebuiltLocked(const MeshCacheIdentity& identity, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload, ClusterLODPrebuiltData* outSavedPrebuiltData)
{
	ZoneScopedN("CLodCacheLoader::SavePrebuiltLocked::WithSavedData");
	spdlog::debug("CLodCacheLoader::SavePrebuiltLocked  src='{}' prim='{}' subset='{}'",
		identity.sourceIdentifier, identity.primPath, identity.subsetName);
	std::lock_guard<std::mutex> saveLock(GetCacheSaveMutexForIdentity(identity));
	if (auto cached = TryLoadPrebuilt(identity)) {
		spdlog::debug("  -> already cached (race-condition guard).");
		if (outSavedPrebuiltData != nullptr) {
			*outSavedPrebuiltData = std::move(*cached);
		}
		return true;
	}

	bool ok = SavePrebuilt(identity, prebuiltData, payload, outSavedPrebuiltData);
	if (ok)
		spdlog::debug("  -> SavePrebuilt succeeded.");
	else
		spdlog::warn("  -> SavePrebuilt FAILED.");
	return ok;
}

}
