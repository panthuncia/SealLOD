#include "Core/CachedMesh.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#include <BasicRenderer/Assets/CachePaths.h>

namespace clodeval {

namespace {
	constexpr std::string_view kFullMeshSubset = "__clod_full_mesh__";

	uint64_t Mix(uint64_t value)
	{
		// SplitMix64: a triangle's hash has to be well distributed for the sum over triangles to mean anything.
		value += 0x9E3779B97F4A7C15ull;
		value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
		value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
		return value ^ (value >> 31u);
	}

	uint64_t HashPosition(const DirectX::XMFLOAT3& position)
	{
		uint32_t bits[3];
		std::memcpy(bits, &position, sizeof(bits));
		return Mix(Mix(Mix(bits[0]) ^ bits[1]) ^ bits[2]);
	}
}

std::unique_ptr<CachedMesh> CachedMesh::Open(const std::filesystem::path& metadataPath, std::string& outError)
{
	auto mesh = std::make_unique<CachedMesh>();
	mesh->m_metadataPath = metadataPath;
	mesh->m_opened = CLodCache::OpenMetadataFile(metadataPath);
	switch (mesh->m_opened.status) {
	case CLodCache::OpenMetadataStatus::Ok:
		break;
	case CLodCache::OpenMetadataStatus::NotFound:
		outError = std::format("'{}' does not exist", metadataPath.string());
		return nullptr;
	case CLodCache::OpenMetadataStatus::BadHeader:
		outError = std::format("'{}' is not a CLod cache", metadataPath.string());
		return nullptr;
	case CLodCache::OpenMetadataStatus::SchemaMismatch:
		outError = std::format(
			"'{}' has schema {} but this tool reads schema {}: rebuild the cache or rebuild this tool",
			metadataPath.string(), mesh->m_opened.foundSchemaVersion, CLodCache::kSchemaVersion);
		return nullptr;
	case CLodCache::OpenMetadataStatus::Corrupt:
		outError = std::format("'{}' is corrupt", metadataPath.string());
		return nullptr;
	}

	std::error_code sizeError;
	mesh->m_metadataBytes = std::filesystem::file_size(metadataPath, sizeError);
	const auto containerBytes = std::filesystem::file_size(mesh->m_opened.containerPath, sizeError);
	mesh->m_containerBytes = sizeError ? 0u : containerBytes;

	if (!CLodCache::AcquireMappedContainer(mesh->m_opened.containerPath.wstring(), mesh->PageCount(), mesh->m_container)) {
		outError = std::format("could not map container '{}' with {} pages", mesh->m_opened.containerPath.string(), mesh->PageCount());
		return nullptr;
	}
	return mesh;
}

std::string CachedMesh::Name() const
{
	const ClusterLODCacheSource& source = Prebuilt().cacheSource;
	// A full mesh's subset is a sentinel, possibly followed by a build suffix such as
	// "|double-sided-voxel-normals". The suffix distinguishes caches, so it stays in the name.
	std::string_view subset = source.subsetName;
	if (subset.starts_with(kFullMeshSubset)) {
		subset.remove_prefix(kFullMeshSubset.size());
		return source.primPath + std::string(subset) + m_nameSuffix;
	}
	return (subset.empty() ? source.primPath : source.primPath + ":" + std::string(subset)) + m_nameSuffix;
}

bool CachedMesh::IsForked() const
{
	return std::ranges::any_of(Prebuilt().groups, [](const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_BRANCH_FLAGS) != 0u; });
}

std::unique_ptr<CachedMesh> CachedMesh::BranchView(uint32_t branch, std::string nameSuffix) const
{
	auto view = std::make_unique<CachedMesh>();
	view->m_metadataPath = m_metadataPath;
	view->m_opened = m_opened;
	view->m_container = m_container;
	view->m_metadataBytes = m_metadataBytes;
	view->m_containerBytes = m_containerBytes;
	view->m_nameSuffix = std::move(nameSuffix);
	view->m_branch = branch;

	const ClusterLODPrebuiltData& source = Prebuilt();
	ClusterLODPrebuiltData& out = view->m_opened.data.prebuiltData;
	const uint32_t groupCount = static_cast<uint32_t>(source.groups.size());
	std::vector<int32_t> renamed(groupCount, -1);
	out.groups.clear();
	out.groupChunks.clear();
	out.groupPageReferences.clear();
	out.groupPageReferenceOffsets.assign(1u, 0u);
	for (uint32_t index = 0; index < groupCount; ++index) {
		if ((ClusterLODGroupBranches(source.groups[index].flags) & branch) != 0u) {
			renamed[index] = static_cast<int32_t>(out.groups.size());
			out.groups.push_back(source.groups[index]);
		}
	}
	const auto rename = [&](int32_t group) { return group >= 0 && static_cast<uint32_t>(group) < groupCount ? renamed[group] : -1; };
	for (uint32_t index = 0; index < groupCount; ++index) {
		if (renamed[index] < 0) continue;
		ClusterLODGroup& group = out.groups[static_cast<size_t>(renamed[index])];
		if (branch == CLOD_BRANCH_VOXEL && group.branchGroupId >= 0 && static_cast<uint32_t>(group.branchGroupId) < groupCount) {
			const ClusterLODGroup& shadow = source.groups[static_cast<size_t>(group.branchGroupId)];
			group.bounds = shadow.bounds;
			group.parentGroupId = shadow.parentGroupId;
			group.maxParentError = shadow.maxParentError;
		}
		group.parentGroupId = rename(group.parentGroupId);
		group.branchGroupId = -1;
		group.flags &= ~CLOD_GROUP_BRANCH_FLAGS;
		if (index < source.groupChunks.size()) out.groupChunks.push_back(source.groupChunks[index]);
		const uint32_t begin = source.groupPageReferenceOffsets[index];
		const uint32_t end = source.groupPageReferenceOffsets[index + 1u];
		out.groupPageReferences.insert(out.groupPageReferences.end(), source.groupPageReferences.begin() + begin, source.groupPageReferences.begin() + end);
		out.groupPageReferenceOffsets.push_back(static_cast<uint32_t>(out.groupPageReferences.size()));
	}
	// The groups' segments stay where they are (other branches' become unused); their children are in the branch.
	for (ClusterLODGroupSegment& segment : out.segments) {
		segment.refinedGroup = rename(segment.refinedGroup);
	}
	for (ClusterLODNode& node : out.nodes) {
		if (node.range.isGroup == CLOD_NODE_INTERNAL) continue;
		const int32_t owner = rename(static_cast<int32_t>(node.range.ownerGroupId));
		if (owner < 0 && node.range.isGroup != CLOD_NODE_INSTANCE_ROOT) {
			// A leaf of the other branch: an instance root names no group, so nothing walks into it.
			node.range.isGroup = CLOD_NODE_INSTANCE_ROOT;
			node.range.ownerGroupId = UINT32_MAX;
			node.range.countMinusOne = 0u;
			continue;
		}
		if (owner >= 0) node.range.ownerGroupId = static_cast<uint32_t>(owner);
		if (node.range.countMinusOne != 0u) node.range.countMinusOne = static_cast<uint32_t>(rename(static_cast<int32_t>(node.range.countMinusOne) - 1) + 1);
	}
	for (ClusterLODPartRecord& part : out.partRecords) {
		part.groupBase = 0u;
		part.groupCount = static_cast<uint32_t>(out.groups.size());
	}
	return view;
}

std::span<const std::byte> CachedMesh::Page(uint32_t meshPageIndex) const
{
	std::span<const std::byte> blob;
	if (meshPageIndex < PageCount()) {
		const ClusterLODGroupDiskLocator& locator = Prebuilt().pageDiskLocators[meshPageIndex];
		if (!m_container->GetBlob(locator.blobOffset, locator.blobSizeBytes, blob)) {
			blob = {};
		}
	}
	return blob;
}

bool CachedMesh::IsVoxelPage(uint32_t meshPageIndex) const
{
	const ClusterLODPrebuiltData& prebuilt = Prebuilt();
	return meshPageIndex >= prebuilt.voxelPageBase && meshPageIndex < prebuilt.voxelPageBase + prebuilt.voxelPageCount;
}

bool CachedMesh::VisitSegmentMeshlets(uint32_t groupIndex, const ClusterLODGroupSegment& segment, const MeshletVisitor& visitor, std::string* outError) const
{
	if (segment.meshletCount == 0u) {
		return true;
	}
	uint32_t meshPage = 0;
	if (!CLodCache::ResolveSegmentMeshPage(Prebuilt(), groupIndex, segment, meshPage)) {
		if (outError != nullptr) {
			*outError = std::format("group {} segment names page slot {} outside its page map", groupIndex, segment.pageIndex);
		}
		return false;
	}
	CLodCache::TrianglePage page;
	if (!CLodCache::TrianglePage::Parse(Page(meshPage), page, outError)) {
		return false;
	}
	if (static_cast<uint64_t>(segment.firstMeshletInPage) + segment.meshletCount > page.MeshletCount()) {
		if (outError != nullptr) {
			*outError = std::format("group {} segment meshlets exceed page {}", groupIndex, meshPage);
		}
		return false;
	}
	for (uint32_t index = 0; index < segment.meshletCount; ++index) {
		visitor(page, page.Meshlet(segment.firstMeshletInPage + index));
	}
	return true;
}

uint64_t CachedMesh::LeafGeometryHash() const
{
	if (m_leafGeometryHash) {
		return *m_leafGeometryHash;
	}
	uint64_t total = 0;
	const ClusterLODPrebuiltData& prebuilt = Prebuilt();
	for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
		const ClusterLODGroup& group = prebuilt.groups[groupIndex];
		if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
			continue;
		}
		for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
			const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
			if (segment.refinedGroup != -1) {
				continue;
			}
			VisitSegmentMeshlets(groupIndex, segment, [&](const CLodCache::TrianglePage& page, const CLodCache::TriangleMeshlet& meshlet) {
				for (uint32_t triangle = 0; triangle < meshlet.triangleCount; ++triangle) {
					const auto vertices = page.Triangle(meshlet, triangle);
					std::array<uint64_t, 3> hashes{
						HashPosition(page.Position(meshlet, vertices[0])),
						HashPosition(page.Position(meshlet, vertices[1])),
						HashPosition(page.Position(meshlet, vertices[2])),
					};
					std::sort(hashes.begin(), hashes.end());
					total += Mix(Mix(hashes[0]) ^ Mix(hashes[1] + 1u) ^ Mix(hashes[2] + 2u));
				}
			});
		}
	}
	m_leafGeometryHash = total;
	return total;
}

std::vector<std::filesystem::path> FindCacheMetadata(const std::filesystem::path& path)
{
	std::vector<std::filesystem::path> found;
	std::error_code error;
	if (std::filesystem::is_regular_file(path, error)) {
		found.push_back(path);
		return found;
	}
	for (std::filesystem::recursive_directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
		if (it->is_regular_file() && it->path().extension() == L".clodmeta") {
			found.push_back(it->path());
		}
	}
	std::sort(found.begin(), found.end());
	return found;
}

}
