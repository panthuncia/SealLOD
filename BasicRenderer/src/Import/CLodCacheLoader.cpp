#include "Import/CLodCacheLoader.h"

#include "Import/CLodCache.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/usd/attribute.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Utilities/CachePathUtilities.h"

namespace CLodCacheLoader {

namespace {
	constexpr const char* kFullMeshSubsetSentinel = "__clod_full_mesh__";

	const char* ToVoxelFallbackModeString(ClusterLODVoxelFallbackMode mode)
	{
		switch (mode)
		{
		case ClusterLODVoxelFallbackMode::Auto:
			return "auto";
		case ClusterLODVoxelFallbackMode::MeshOnly:
			return "mesh-only";
		case ClusterLODVoxelFallbackMode::VoxelOnly:
			return "voxel-only";
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

	struct LayerPrimIdentity {
		std::string sourceIdentifier;
		std::string primPath;
	};

	std::string GetLayerCacheSourceIdentifier(const pxr::SdfLayerHandle& layer)
	{
		if (!layer) {
			return {};
		}

		const std::string& realPath = layer->GetRealPath();
		return NormalizeCacheSourcePath(realPath.empty() ? layer->GetIdentifier() : realPath);
	}

	std::optional<LayerPrimIdentity> GetStrongestAuthoredValueIdentity(
		const pxr::UsdAttribute& attr,
		pxr::UsdTimeCode geomTimeCode)
	{
		if (!attr) {
			return std::nullopt;
		}

		for (const auto& spec : attr.GetPropertyStack(geomTimeCode)) {
			if (!spec || !spec->GetLayer()) {
				continue;
			}

			const bool hasTimeSamples =
				!spec->GetLayer()->ListTimeSamplesForPath(spec->GetPath()).empty();
			if (!spec->HasDefaultValue() && !hasTimeSamples) {
				continue;
			}

			LayerPrimIdentity identity{};
			identity.sourceIdentifier = GetLayerCacheSourceIdentifier(spec->GetLayer());
			identity.primPath = spec->GetPath().GetPrimPath().GetString();
			if (!identity.sourceIdentifier.empty() && !identity.primPath.empty()) {
				return identity;
			}
		}

		return std::nullopt;
	}

	std::optional<LayerPrimIdentity> GetReferencedGeometryIdentity(
		const pxr::UsdGeomMesh& mesh,
		pxr::UsdTimeCode geomTimeCode)
	{
		const std::optional<LayerPrimIdentity> pointsIdentity =
			GetStrongestAuthoredValueIdentity(mesh.GetPointsAttr(), geomTimeCode);
		const std::optional<LayerPrimIdentity> countsIdentity =
			GetStrongestAuthoredValueIdentity(mesh.GetFaceVertexCountsAttr(), geomTimeCode);
		const std::optional<LayerPrimIdentity> indicesIdentity =
			GetStrongestAuthoredValueIdentity(mesh.GetFaceVertexIndicesAttr(), geomTimeCode);

		if (!pointsIdentity || !countsIdentity || !indicesIdentity) {
			return std::nullopt;
		}

		const auto matchesPoints = [&pointsIdentity](const LayerPrimIdentity& other) {
			return other.sourceIdentifier == pointsIdentity->sourceIdentifier &&
				other.primPath == pointsIdentity->primPath;
		};

		if (!matchesPoints(*countsIdentity) || !matchesPoints(*indicesIdentity)) {
			return std::nullopt;
		}

		return pointsIdentity;
	}

	void LogBuildConfigOnce(uint64_t buildHash)
	{
		static std::once_flag logOnce;
		std::call_once(logOnce, [buildHash]() {
			const ClusterLODBuilderSettings effectiveSettings = ApplyClusterLODBuilderEnvironmentOverrides({});
			spdlog::info(
				"CLod cache build config: hash=0x{:016X} sloppy_fallback={} sloppy_error_factor={} voxel_enabled={} voxel_mode='{}' voxel_grid={} voxel_rays={} voxel_scale={} voxel_opacity_threshold={} env_sloppy_disable='{}' env_sloppy_factor='{}' env_mode='{}' env_grid='{}' env_rays='{}' env_scale='{}' env_opacity_threshold='{}'",
				buildHash,
				!effectiveSettings.disableSloppyFallback,
				effectiveSettings.sloppyFallbackErrorFactor,
				effectiveSettings.enableVoxelFallback,
				ToVoxelFallbackModeString(effectiveSettings.voxelFallbackMode),
				effectiveSettings.voxelGridBaseResolution,
				effectiveSettings.voxelRaysPerCell,
				effectiveSettings.voxelFallbackScalingFactor,
				effectiveSettings.voxelFallbackOpacityThreshold,
				GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK"),
				GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_SLOPPY_ERROR_FACTOR"),
				GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_MODE"),
				GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_GRID"),
				GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_RAYS"),
				GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_SCALE"),
				GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_OPACITY_THRESHOLD"));
		});
	}
}

MeshCacheIdentity BuildIdentity(
	const pxr::UsdGeomMesh& mesh,
	const pxr::UsdStageRefPtr& stage,
	const std::string& subsetName,
	pxr::UsdTimeCode geomTimeCode)
{
	MeshCacheIdentity identity{};
	if (stage && stage->GetRootLayer()) {
		identity.sourceIdentifier = NormalizeCacheSourcePath(
			stage->GetRootLayer()->GetIdentifier());
	}
	identity.primPath = mesh.GetPrim().GetPath().GetString();
	identity.subsetName = subsetName;

	if (auto referencedGeometryIdentity = GetReferencedGeometryIdentity(mesh, geomTimeCode)) {
		identity.sourceIdentifier = std::move(referencedGeometryIdentity->sourceIdentifier);
		identity.primPath = std::move(referencedGeometryIdentity->primPath);
	}

	return identity;
}

MeshCacheIdentity BuildIdentity(
	const pxr::UsdGeomMesh& mesh,
	const pxr::UsdStageRefPtr& stage,
	const std::string& subsetName,
	pxr::UsdTimeCode geomTimeCode,
	const std::string& sourceIdentifierOverride)
{
	MeshCacheIdentity identity = BuildIdentity(mesh, stage, subsetName, geomTimeCode);
	if (!sourceIdentifierOverride.empty()) {
		identity.sourceIdentifier = NormalizeCacheSourcePath(sourceIdentifierOverride);
	}
	return identity;
}

std::optional<ClusterLODPrebuiltData> TryLoadPrebuilt(const MeshCacheIdentity& identity)
{
	ZoneScopedN("CLodCacheLoader::TryLoadPrebuilt");
	const auto cacheKey = ToCacheKey(identity);
	const uint64_t buildHash = CLodCache::ComputeBuildConfigHash();
	const auto lookup = CLodCache::BuildCacheLookup(cacheKey, buildHash);
	LogBuildConfigOnce(buildHash);
	spdlog::debug("CLodCacheLoader::TryLoadPrebuilt  src='{}' prim='{}' subset='{}' hash=0x{:X} metadata='{}' container='{}'",
		identity.sourceIdentifier,
		identity.primPath,
		identity.subsetName,
		buildHash,
		ws2s(lookup.metadataPath),
		ws2s(lookup.containerPath));
	auto cached = CLodCache::TryLoad(cacheKey, buildHash);
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
	const uint64_t buildHash = CLodCache::ComputeBuildConfigHash();
	LogBuildConfigOnce(buildHash);
	return CLodCache::Save(cacheKey, buildHash, prebuiltData, payload);
}

bool SavePrebuilt(const MeshCacheIdentity& identity, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload, ClusterLODPrebuiltData* outSavedPrebuiltData)
{
	ZoneScopedN("CLodCacheLoader::SavePrebuilt::WithSavedData");
	const auto cacheKey = ToCacheKey(identity);
	const uint64_t buildHash = CLodCache::ComputeBuildConfigHash();
	LogBuildConfigOnce(buildHash);
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
