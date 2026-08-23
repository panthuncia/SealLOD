#pragma once

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Mesh/ClusterLODTypes.h"

namespace CLodCache {

class MappedContainerLease {
public:
	~MappedContainerLease();
	uint32_t GetPageCount() const;
	bool GetBlob(
		uint64_t offset,
		uint32_t size,
		std::span<const std::byte>& outBlob) const;
	bool WarmBlob(uint64_t offset, uint32_t size) const;
	bool WarmBlobs(
		std::span<const uint64_t> offsets,
		std::span<const uint32_t> sizes) const;

private:
	struct Impl;
	explicit MappedContainerLease(std::shared_ptr<const Impl> impl);
	std::shared_ptr<const Impl> m_impl;
	friend bool AcquireMappedContainer(
		const std::wstring&,
		uint32_t,
		std::shared_ptr<const MappedContainerLease>&);
};

bool AcquireMappedContainer(
	const std::wstring& containerPath,
	uint32_t expectedPageCount,
	std::shared_ptr<const MappedContainerLease>& outLease);

inline constexpr uint32_t kSchemaVersion = 79;

struct CacheKey {
	std::string sourceIdentifier;
	std::string primPath;
	std::string subsetName;
};

struct CacheData {
	uint32_t schemaVersion = kSchemaVersion;
	uint64_t buildConfigHash = 0;
	ClusterLODPrebuiltData prebuiltData;
};

struct CacheLookup {
	CacheKey key;
	uint64_t buildConfigHash = 0;
	std::wstring metadataFileName;
	std::wstring metadataPath;
	std::wstring containerFileName;
	std::wstring containerPath;
};

struct LoadedGroupPayload {
	std::optional<ClusterLODGroupChunk> groupChunkMetadata;
	std::vector<std::vector<std::byte>> pageBlobs;
};

struct PagePayloadLayoutMetadata {
	std::optional<ClusterLODGroupChunk> groupChunkMetadata;
	std::vector<uint32_t> pageBlobSizes;
	std::vector<uint64_t> pageBlobOffsets;

	bool IsValid() const {
		return pageBlobSizes.size() == pageBlobOffsets.size();
	}

	void Clear() {
		groupChunkMetadata.reset();
		pageBlobSizes.clear();
		pageBlobOffsets.clear();
	}
};

using GroupPayloadLayoutMetadata = PagePayloadLayoutMetadata;

std::wstring ResolveContainerPath(const ClusterLODCacheSource& cacheSource);

uint64_t ComputeBuildConfigHash(std::string_view assetIdentifier = {});
CacheLookup BuildCacheLookup(const CacheKey& key, uint64_t buildConfigHash);
std::wstring BuildCacheFileName(const CacheKey& key, uint64_t buildConfigHash);
std::wstring GetCacheFilePathForSource(const std::wstring& fileName, const std::string& sourceIdentifier);

std::optional<CacheData> TryLoad(const CacheKey& key, uint64_t expectedBuildConfigHash);
bool Save(const CacheKey& key, const CacheData& data);
bool Save(const CacheKey& key, uint64_t buildConfigHash, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload);
bool Save(const CacheKey& key, uint64_t buildConfigHash, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload, ClusterLODPrebuiltData* outSavedPrebuiltData);
bool LoadGroupPayload(const CacheData& cacheData, uint32_t groupLocalIndex, LoadedGroupPayload& outPayload);

bool LoadMeshPagesSelective(std::ifstream& file,
	std::span<const ClusterLODGroupDiskLocator> pageLocators,
	uint32_t firstPage,
	uint32_t pageCount,
	const std::vector<bool>& pageNeedsFetch,
	LoadedGroupPayload& outPayload);

bool LoadMeshPagesSelective(std::ifstream& file,
	std::span<const ClusterLODGroupDiskLocator> pageLocators,
	std::span<const uint32_t> meshPageIndices,
	const std::vector<bool>& pageNeedsFetch,
	LoadedGroupPayload& outPayload);

bool LoadMeshPagesSelectiveMapped(
	const std::wstring& containerPath,
	std::span<const ClusterLODGroupDiskLocator> pageLocators,
	uint32_t firstPage,
	uint32_t pageCount,
	const std::vector<bool>& pageNeedsFetch,
	LoadedGroupPayload& outPayload);

bool LoadMeshPagesSelectiveMapped(
	const std::wstring& containerPath,
	std::span<const ClusterLODGroupDiskLocator> pageLocators,
	std::span<const uint32_t> meshPageIndices,
	const std::vector<bool>& pageNeedsFetch,
	LoadedGroupPayload& outPayload);

bool GetMeshPagePayloadLayout(std::span<const ClusterLODGroupDiskLocator> pageLocators,
	uint32_t firstPage,
	uint32_t pageCount,
	PagePayloadLayoutMetadata& outLayout);

bool GetMeshPagePayloadLayout(std::span<const ClusterLODGroupDiskLocator> pageLocators,
	std::span<const uint32_t> meshPageIndices,
	PagePayloadLayoutMetadata& outLayout);

bool LoadMeshPagesSelectiveDirectStorage(
	const std::wstring& containerPath,
	std::span<const ClusterLODGroupDiskLocator> pageLocators,
	uint32_t firstPage,
	uint32_t pageCount,
	const std::vector<bool>& pageNeedsFetch,
	LoadedGroupPayload& outPayload,
	std::string* outMessage = nullptr);

bool LoadMeshPagesSelectiveDirectStorage(
	const std::wstring& containerPath,
	std::span<const ClusterLODGroupDiskLocator> pageLocators,
	std::span<const uint32_t> meshPageIndices,
	const std::vector<bool>& pageNeedsFetch,
	LoadedGroupPayload& outPayload,
	std::string* outMessage = nullptr);

// Open a container file and validate its header.  Returns true on success.
// The caller keeps the ifstream around for repeated mesh-page interval loads.
bool OpenContainerFile(const ClusterLODCacheSource& cacheSource,
	std::ifstream& outFile,
	uint32_t& outPageCount);

}
