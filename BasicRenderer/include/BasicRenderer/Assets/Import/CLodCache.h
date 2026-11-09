#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <BasicRenderer/Assets/ClusterLODTypes.h>

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

inline constexpr uint32_t kSchemaVersion = 81; // 81: forked triangle/voxel DAGs (ClusterLODGroup::branchGroupId, branch flags).

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
// Where an entry's metadata is within a cache tree (relative to the cache root, as "clod\..."), for tools that open a
// tree other than this process's cache root.
std::filesystem::path RelativeMetadataPath(const CacheKey& key, uint64_t buildConfigHash);

std::optional<CacheData> TryLoad(const CacheKey& key, uint64_t expectedBuildConfigHash);

// Merged meshes (docs/merge_hierarchies.md): CLod meshes whose part portals open parts other meshes hold, named by a
// host asset key and kept together in cache/clod/merged. Their metadata carries the parts (externalParts) and palette.
inline constexpr std::string_view kMergedMeshPrimPath = "/MergedMesh";
CacheKey MergedMeshCacheKey(std::string_view assetKey);
uint64_t MergedMeshBuildConfigHash(std::string_view assetKey);
std::optional<CacheData> TryLoadMergedMesh(std::string_view assetKey);
// Replaces whatever an earlier build saved under the key.
bool SaveMergedMesh(std::string_view assetKey, const ClusterLODPrebuiltData& prebuiltData, const ClusterLODCacheBuildPayload& payload);
// Deletes the merged mesh's files, and stops the cache pack serving them.
void RemoveMergedMesh(std::string_view assetKey);
// The asset keys of the merged meshes on disk (loose or in the pack) whose keys start with the prefix, sorted.
std::vector<std::string> ListMergedMeshes(std::string_view assetKeyPrefix);
// Whether a .clodmeta file is what TryLoad would read for its own key today: the current schema, and the build config
// hash ComputeBuildConfigHash gives its source now (which its file name also encodes). False for older builds left
// behind by a schema or settings change, and for anything unreadable. Reads the pack, when this process reads it, or
// the loose file.
bool IsCurrentMetadataFile(const std::filesystem::path& metadataPath);
// The same for a metadata file's bytes held elsewhere (the cache pack), named fileName.
bool IsCurrentMetadata(std::span<const std::byte> fileBytes, const std::wstring& fileName);
// Changes whenever IsCurrentMetadataFile could judge any file differently: the schema, the build settings and the asset
// settings document. A caller may keep verdicts for unchanged files under it.
uint64_t CurrentSettingsFingerprint();
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
// The container's page count from its header, read from the cache pack when it holds the container. OpenContainerFile
// reads only the loose file: once the pack build deletes loose files, use this or the mapped readers instead.
bool ReadContainerPageCount(const ClusterLODCacheSource& cacheSource, uint32_t& outPageCount);
bool OpenContainerFile(const ClusterLODCacheSource& cacheSource,
	std::ifstream& outFile,
	uint32_t& outPageCount);
bool OpenContainerFile(const std::filesystem::path& containerPath,
	std::ifstream& outFile,
	uint32_t& outPageCount);

// Opening a cache by file, for tools that inspect a cache without knowing its key or build hash.
enum class OpenMetadataStatus {
	Ok,
	NotFound,
	BadHeader,
	SchemaMismatch,
	Corrupt,
};

struct OpenedMetadata {
	OpenMetadataStatus status = OpenMetadataStatus::NotFound;
	uint32_t foundSchemaVersion = 0;  // valid from SchemaMismatch and Ok
	CacheData data;
	std::filesystem::path containerPath;  // beside the metadata, named by the metadata
};

OpenedMetadata OpenMetadataFile(const std::filesystem::path& metadataPath);

// Cache-only mode makes a cache miss an error instead of a silent rebuild, so a tool can be sure
// it is rendering the cache it was given. Builder entry points call ThrowIfCacheOnly. Importers
// may catch that exception and carry on without the mesh, so the count lets the caller check
// afterwards that nothing was asked to build.
void SetCacheOnlyMode(bool enabled);
bool IsCacheOnlyMode();
void ThrowIfCacheOnly(std::string_view builder);
uint64_t GetCacheOnlyViolationCount();

}
