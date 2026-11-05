#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <BasicRenderer/Assets/CLodPageDecoder.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>

namespace clodeval {

// One mesh's cache: metadata in memory and pages available on demand. Everything an analyzer learns
// about a hierarchy comes through here, so a production cache can be read the same way as an
// evaluation build, with no source asset.
class CachedMesh {
public:
	// Returns nullptr and sets outError if the metadata or container cannot be used. A schema
	// mismatch says so plainly, because the fix is rebuilding the caches or this tool.
	static std::unique_ptr<CachedMesh> Open(const std::filesystem::path& metadataPath, std::string& outError);

	const ClusterLODPrebuiltData& Prebuilt() const { return m_opened.data.prebuiltData; }
	uint64_t BuildConfigHash() const { return m_opened.data.buildConfigHash; }
	const std::filesystem::path& MetadataPath() const { return m_metadataPath; }
	const std::filesystem::path& ContainerPath() const { return m_opened.containerPath; }
	uint64_t MetadataBytes() const { return m_metadataBytes; }
	uint64_t ContainerBytes() const { return m_containerBytes; }

	// "<prim>" or "<prim>:<subset>", which is stable across builds of the same source.
	std::string Name() const;

	uint32_t PageCount() const { return static_cast<uint32_t>(Prebuilt().pageDiskLocators.size()); }
	std::span<const std::byte> Page(uint32_t meshPageIndex) const;
	bool IsVoxelPage(uint32_t meshPageIndex) const;

	// Calls back for every meshlet of a triangle group's segment. Returns false if the metadata and
	// pages disagree, in which case the mesh should be reported rather than measured.
	using MeshletVisitor = std::function<void(const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet&)>;
	bool VisitSegmentMeshlets(uint32_t groupIndex, const ClusterLODGroupSegment& segment, const MeshletVisitor& visitor, std::string* outError = nullptr) const;

	// A hash of the terminal triangles that ignores clustering and order, so two builds of one source
	// agree however they partition it. Comparing images of two builds is meaningless if this differs.
	uint64_t LeafGeometryHash() const;

private:
	std::filesystem::path m_metadataPath;
	CLodCache::OpenedMetadata m_opened;
	std::shared_ptr<const CLodCache::MappedContainerLease> m_container;
	uint64_t m_metadataBytes = 0;
	uint64_t m_containerBytes = 0;
	mutable std::optional<uint64_t> m_leafGeometryHash;
};

// Finds .clodmeta files under a directory, or returns the one file it was given.
std::vector<std::filesystem::path> FindCacheMetadata(const std::filesystem::path& path);

}
