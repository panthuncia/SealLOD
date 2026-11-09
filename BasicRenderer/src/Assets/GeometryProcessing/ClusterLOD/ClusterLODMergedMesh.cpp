#include <BasicRenderer/Assets/Import/ClusterLODMergedMesh.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include <BasicRenderer/Assets/CLodPageDecoder.h>
#include <BasicRenderer/Assets/Geometry/Mesh.h>
#include <BasicRenderer/Assets/Geometry/VertexLayout.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Scene/VertexFlags.h>

namespace
{
	constexpr unsigned int kMergedMeshVertexFlags = VertexFlags::VERTEX_NORMALS | VertexFlags::VERTEX_TEXCOORDS | VertexFlags::VERTEX_COLORS;

	// Row-vector matrix of a column-vector 3x4.
	DirectX::XMMATRIX ToMatrix(const ClusterLODAssemblyTransform& t)
	{
		return DirectX::XMMatrixSet(
			t.row0.x, t.row1.x, t.row2.x, 0.0f,
			t.row0.y, t.row1.y, t.row2.y, 0.0f,
			t.row0.z, t.row1.z, t.row2.z, 0.0f,
			t.row0.w, t.row1.w, t.row2.w, 1.0f);
	}

	float MaxScale(const ClusterLODAssemblyTransform& t)
	{
		const auto length = [](const DirectX::XMFLOAT4& r) { return std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z); };
		return (std::max)(length(t.row0), (std::max)(length(t.row1), length(t.row2)));
	}

	// Moves the geometry appended from `firstVertex` and `firstTriangle` on by the transform (none: as it is) and offsets
	// its slots by the base.
	void PlaceAppended(ClusterLODMergeGeometry& g, size_t firstVertex, size_t firstTriangle, const ClusterLODAssemblyTransform* transform,
		uint32_t slotBase)
	{
		if (transform) {
			const DirectX::XMMATRIX m = ToMatrix(*transform);
			const DirectX::XMMATRIX n = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, m));
			for (size_t v = firstVertex; v < g.positions.size(); ++v) {
				DirectX::XMStoreFloat3(&g.positions[v], DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&g.positions[v]), m));
				if (v < g.normals.size()) {
					DirectX::XMStoreFloat3(&g.normals[v], DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&g.normals[v]), n)));
				}
			}
		}
		for (size_t t = firstTriangle; t < g.triangleSlots.size(); ++t) g.triangleSlots[t] = static_cast<uint16_t>(g.triangleSlots[t] + slotBase);
	}

	// Appends a part's coarse geometry (its own space) to g.
	void AppendGeometry(ClusterLODMergeGeometry& g, const ClusterLODMergeGeometry& part, const ClusterLODAssemblyTransform& transform, uint32_t slotBase)
	{
		const size_t firstVertex = g.positions.size(), firstTriangle = g.triangleSlots.size();
		const auto base = static_cast<uint32_t>(firstVertex);
		const size_t count = part.positions.size();
		g.positions.insert(g.positions.end(), part.positions.begin(), part.positions.end());
		for (size_t v = 0; v < count; ++v) {
			g.normals.push_back(v < part.normals.size() ? part.normals[v] : DirectX::XMFLOAT3(0, 0, 1));
			g.uvs.push_back(v < part.uvs.size() ? part.uvs[v] : DirectX::XMFLOAT2(0, 0));
			g.colors.push_back(v < part.colors.size() ? part.colors[v] : DirectX::XMFLOAT3(1, 1, 1));
		}
		for (const uint32_t index : part.indices) g.indices.push_back(base + index);
		const size_t triangles = part.indices.size() / 3u;
		for (size_t t = 0; t < triangles; ++t) g.triangleSlots.push_back(t < part.triangleSlots.size() ? part.triangleSlots[t] : uint16_t{ 0 });
		PlaceAppended(g, firstVertex, firstTriangle, &transform, slotBase);
	}
}

ClusterLODMergePartGeometry ExtractClusterLODMergePartGeometry(const Mesh& mesh, const ClusterLODPartGeometryResolver& resolvePart)
{
	ClusterLODMergePartGeometry result;
	if (mesh.HasBaseSkin()) {
		result.reason = "skinned";
		return result;
	}
	// A part-library assembly copies its parts into its own cache; a merged mesh names its parts, and may embed others.
	const bool merged = !mesh.GetCLodExternalParts().empty();
	if (!mesh.GetCLodAssemblyInstances().empty() && !merged) {
		result.reason = "part-library assembly";
		return result;
	}
	const ClusterLODPrebuiltData data = mesh.GetClusterLODPrebuiltData();
	if (data.groups.empty() || data.nodes.empty()) {
		result.reason = "no hierarchy";
		return result;
	}
	// The coarsest triangles: groups nothing coarser (in triangles) replaces. A forked part's are in its triangle branch,
	// whose parents the groups' own records name.
	const auto isTopGroup = [&](uint32_t g) {
		const ClusterLODGroup& group = data.groups[g];
		if ((group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY | CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL)) != 0u) return false;
		if ((ClusterLODGroupBranches(group.flags) & CLOD_BRANCH_TRIANGLE) == 0u) return false;
		const bool parentIsTriangles = group.parentGroupId >= 0 &&
			(data.groups[static_cast<uint32_t>(group.parentGroupId)].flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u;
		return !parentIsTriangles;
	};
	// A merged mesh's own groups are those its traversal reaches from a root without passing an instance; the instances
	// at its top (portals, or embedded meshes) are open at every distance, so what they draw at their coarsest belongs to
	// the mesh's coarsest too.
	struct Subtree
	{
		std::vector<uint32_t> topGroups;
		std::vector<uint32_t> topInstances;
	};
	const auto subtree = [&](uint32_t rootNode) {
		Subtree out;
		std::vector<uint8_t> visited(data.nodes.size(), 0u);
		std::vector<uint32_t> stack{ rootNode };
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			if (n >= data.nodes.size() || visited[n]) continue;
			visited[n] = 1u;
			const ClusterLODNode& node = data.nodes[n];
			if (node.range.isGroup == CLOD_NODE_INTERNAL) {
				for (uint32_t c = 0; c <= node.range.countMinusOne; ++c) stack.push_back(node.range.indexOrOffset + c);
				continue;
			}
			const uint32_t owner = node.range.ownerGroupId;
			if (owner >= data.groups.size()) continue;
			if (node.range.isGroup == CLOD_NODE_INSTANCE_ROOT) {
				const ClusterLODGroup& proxy = data.groups[owner];
				if (proxy.parentGroupId < 0 && (ClusterLODGroupBranches(proxy.flags) & CLOD_BRANCH_TRIANGLE) != 0u) {
					out.topInstances.push_back(node.range.indexOrOffset);
				}
			} else if (isTopGroup(owner)) {
				out.topGroups.push_back(owner);
			}
		}
		std::ranges::sort(out.topGroups);
		out.topGroups.erase(std::unique(out.topGroups.begin(), out.topGroups.end()), out.topGroups.end());
		std::ranges::sort(out.topInstances);
		out.topInstances.erase(std::unique(out.topInstances.begin(), out.topInstances.end()), out.topInstances.end());
		return out;
	};
	Subtree top;
	if (merged) {
		top = subtree(mesh.GetCLodRootNodeIndex());
	} else {
		for (uint32_t g = 0; g < data.groups.size(); ++g) {
			if (isTopGroup(g)) top.topGroups.push_back(g);
		}
	}
	if (top.topGroups.empty() && top.topInstances.empty()) {
		result.reason = "no triangle groups";
		return result;
	}

	// The mapped reader: the pack holds the container once the pack build has deleted loose files.
	uint32_t pageCount = 0;
	if (!CLodCache::ReadContainerPageCount(data.cacheSource, pageCount)) {
		result.reason = "container unavailable";
		return result;
	}
	const std::wstring containerPath = CLodCache::ResolveContainerPath(data.cacheSource);
	std::unordered_map<uint32_t, std::vector<std::byte>> pages;
	auto pageBlob = [&](uint32_t meshPage) -> const std::vector<std::byte>* {
		if (const auto it = pages.find(meshPage); it != pages.end()) return &it->second;
		CLodCache::LoadedGroupPayload payload;
		const uint32_t index[1] = { meshPage };
		if (!CLodCache::LoadMeshPagesSelectiveMapped(containerPath, data.pageDiskLocators, std::span<const uint32_t>(index, 1),
				std::vector<bool>(1, true), payload) || payload.pageBlobs.empty()) {
			return nullptr;
		}
		return &pages.emplace(meshPage, std::move(payload.pageBlobs.front())).first->second;
	};

	const auto appendTriangles = [&](ClusterLODMergeGeometry& g, uint32_t groupIndex) {
		const ClusterLODGroup& group = data.groups[groupIndex];
		for (uint32_t s = 0; s < group.segmentCount; ++s) {
			const ClusterLODGroupSegment& segment = data.segments[group.firstSegment + s];
			uint32_t meshPage = 0;
			if (!CLodCache::ResolveSegmentMeshPage(data, groupIndex, segment, meshPage)) continue;
			const auto* blob = pageBlob(meshPage);
			CLodCache::TrianglePage page;
			if (!blob || !CLodCache::TrianglePage::Parse(*blob, page)) continue;
			for (uint32_t m = 0; m < segment.meshletCount; ++m) {
				const uint32_t meshletIndex = segment.firstMeshletInPage + m;
				if (meshletIndex >= page.MeshletCount()) break;
				const CLodCache::TriangleMeshlet meshlet = page.Meshlet(meshletIndex);
				const auto base = static_cast<uint32_t>(g.positions.size());
				for (uint32_t v = 0; v < meshlet.vertexCount; ++v) {
					g.positions.push_back(page.Position(meshlet, v));
					g.normals.push_back(page.HasNormals() ? page.Normal(meshlet, v) : DirectX::XMFLOAT3(0, 0, 1));
					g.uvs.push_back(page.Uv(meshletIndex, meshlet, 0u, v));
					g.colors.push_back(page.Color(meshlet, v));
				}
				// A multi-material mesh's clusters name their palette slot in descriptor word 15 (ClusterLODClustering): its
				// triangles keep it, as the part-local slot a merge offsets by the item's material base.
				const uint16_t slot = mesh.GetCLodMaterialSlotCount() > 1u
					? static_cast<uint16_t>(std::bit_cast<uint32_t>(meshlet.descriptor.terrainRvtLocalSkyrimXYRadius))
					: uint16_t{ 0 };
				for (uint32_t t = 0; t < meshlet.triangleCount; ++t) {
					const auto tri = page.Triangle(meshlet, t);
					g.indices.insert(g.indices.end(), { base + tri[0], base + tri[1], base + tri[2] });
					g.triangleSlots.push_back(slot);
				}
			}
		}
	};
	ClusterLODMergeGeometry& g = result.geometry;
	uint64_t topMeshlets = 0;
	uint64_t ownTriangleCount = 0;         // read from this mesh's own pages
	uint64_t instancedFinestTriangles = 0; // what the top portals open, at full detail
	// Instance transforms and material bases are the mesh's own (an embedded mesh's are already composed), so a
	// subtree's geometry is placed by the instance that reached it, never by a product down the chain.
	const std::function<bool(const Subtree&, const ClusterLODAssemblyTransform*, uint32_t, uint32_t)> appendSubtree =
		[&](const Subtree& tree, const ClusterLODAssemblyTransform* transform, uint32_t slotBase, uint32_t depth) -> bool {
		if (depth > CLOD_ASSEMBLY_MAX_STACK_DEPTH + 8u) {
			result.reason = "embedded meshes nest too deeply";
			return false;
		}
		const float scale = transform ? MaxScale(*transform) : 1.0f;
		for (const uint32_t groupIndex : tree.topGroups) {
			const ClusterLODGroup& group = data.groups[groupIndex];
			for (uint32_t s = 0; s < group.segmentCount; ++s) {
				const ClusterLODGroupSegment& segment = data.segments[group.firstSegment + s];
				if (segment.refinedGroup >= 0) {
					const float error = data.groups[static_cast<uint32_t>(segment.refinedGroup)].bounds.error;
					if (error < (std::numeric_limits<float>::max)()) g.error = (std::max)(g.error, error * scale);
				}
				topMeshlets += segment.meshletCount;
			}
			const size_t firstVertex = g.positions.size(), firstTriangle = g.triangleSlots.size();
			appendTriangles(g, groupIndex);
			PlaceAppended(g, firstVertex, firstTriangle, transform, slotBase);
			ownTriangleCount += g.triangleSlots.size() - firstTriangle;
		}
		for (const uint32_t instanceIndex : tree.topInstances) {
			if (instanceIndex >= data.assemblyInstances.size()) continue;
			const ClusterLODAssemblyInstance& instance = data.assemblyInstances[instanceIndex];
			const bool own = instance.transformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL && instance.transformIndex < data.assemblyTransforms.size();
			const ClusterLODAssemblyTransform* placed = own ? &data.assemblyTransforms[instance.transformIndex] : transform;
			const uint32_t base = own && instance.transformIndex < data.assemblyTransformMaterialBases.size()
				? data.assemblyTransformMaterialBases[instance.transformIndex]
				: slotBase;
			if ((instance.flags & CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL) == 0u) {
				if (!appendSubtree(subtree(instance.targetRootNode), placed, base, depth + 1u)) return false;
				continue;
			}
			const auto& parts = mesh.GetPortalParts();
			const Mesh* part = instance.stackDepth < parts.size() ? parts[instance.stackDepth].get() : nullptr;
			if (!part) {
				result.reason = std::format("top portal {} has no part mesh", instance.stackDepth);
				return false;
			}
			std::shared_ptr<const ClusterLODMergePartGeometry> partGeometry = resolvePart
				? resolvePart(*part)
				: std::make_shared<const ClusterLODMergePartGeometry>(ExtractClusterLODMergePartGeometry(*part));
			if (!partGeometry || !partGeometry->valid) {
				result.reason = std::format("top portal {}: {}", instance.stackDepth, partGeometry ? partGeometry->reason : std::string("no geometry"));
				return false;
			}
			const ClusterLODAssemblyTransform identity{ { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
			const ClusterLODAssemblyTransform& partTransform = placed ? *placed : identity;
			AppendGeometry(g, partGeometry->geometry, partTransform, base);
			g.error = (std::max)(g.error, partGeometry->geometry.error * MaxScale(partTransform));
			instancedFinestTriangles += partGeometry->finestTriangles;
		}
		return true;
	};
	if (!appendSubtree(top, nullptr, 0u, 0u)) return result;
	{
		uint64_t finestMeshlets = 0;
		for (const ClusterLODGroup& group : data.groups) {
			if (group.depth != 0 || (group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY |
				CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL | CLOD_GROUP_FLAG_BRANCH_SHADOW)) != 0u) {
				continue;
			}
			for (uint32_t s = 0; s < group.segmentCount; ++s) finestMeshlets += data.segments[group.firstSegment + s].meshletCount;
		}
		const uint64_t ownTriangles = ownTriangleCount;
		const double perMeshlet = topMeshlets != 0u ? static_cast<double>(ownTriangles) / static_cast<double>(topMeshlets) : 0.0;
		result.finestTriangles = (std::max)(static_cast<uint64_t>(static_cast<double>(finestMeshlets) * perMeshlet + 0.5), ownTriangles) +
			instancedFinestTriangles;
	}
	// A forked part's voxel branch: its voxels stand for the shared groups its voxel groups refine into. A merged mesh's
	// voxels stand for its items, which an embedding copies with it.
	if (!merged) {
		// The source's error is where the part switches to voxels: its boundary groups' voxel-branch boundary, as a
		// triangle item's is its top groups' boundary. Past it the part's own voxel levels take over by cut, so a node
		// voxelizing the source only has to be coarser than that, not than the part's coarsest voxels.
		std::vector<uint8_t> boundary(data.groups.size(), 0u);
		float coarsestVoxels = 0.0f;
		for (const ClusterLODGroup& group : data.groups) {
			if ((group.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) == 0u || (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u) continue;
			for (uint32_t s = 0; s < group.segmentCount; ++s) {
				const int32_t child = data.segments[group.firstSegment + s].refinedGroup;
				if (child >= 0 && ClusterLODGroupBranches(data.groups[static_cast<uint32_t>(child)].flags) == CLOD_BRANCH_ALL) {
					boundary[static_cast<uint32_t>(child)] = 1u;
				}
			}
		}
		for (uint32_t groupIndex = 0; groupIndex < data.groups.size(); ++groupIndex) {
			if (boundary[groupIndex] == 0u) continue;
			appendTriangles(result.voxelSource, groupIndex);
			const ClusterLODGroup& group = data.groups[groupIndex];
			const int32_t shadow = group.branchGroupId;
			const float voxelBoundary = shadow >= 0 && static_cast<uint32_t>(shadow) < data.groups.size()
				? data.groups[static_cast<uint32_t>(shadow)].bounds.error
				: group.bounds.error;
			if (voxelBoundary < (std::numeric_limits<float>::max)() * 0.5f) coarsestVoxels = (std::max)(coarsestVoxels, voxelBoundary);
		}
		if (!result.voxelSource.indices.empty() && coarsestVoxels > 0.0f) {
			result.voxelSource.error = coarsestVoxels;
			result.hasVoxelBranch = true;
		} else {
			result.voxelSource = {};
		}
	}
	if (g.indices.empty()) {
		result.reason = "no coarse triangles";
		return result;
	}
	if (g.error >= (std::numeric_limits<float>::max)()) g.error = 0.0f;
	result.rootNode = mesh.GetCLodRootNodeIndex();
	result.traversalDepth = mesh.GetCLodMaxTraversalDepth();
	result.lodSphere = result.rootNode < data.nodes.size() ? data.nodes[result.rootNode].traversalMetric.lodBoundingSphere : DirectX::XMFLOAT4{};
	result.valid = true;
	return result;
}

ClusterLODExternalPart DescribeClusterLODExternalPart(const Mesh& part)
{
	const ClusterLODCacheSource& source = part.GetCLodCacheSource();
	ClusterLODExternalPart out;
	out.sourceIdentifier = source.sourceIdentifier;
	out.primPath = source.primPath;
	out.subsetName = source.subsetName;
	out.buildConfigHash = source.buildConfigHash;
	out.nodeCount = static_cast<uint32_t>(part.GetCLodNodes().size());
	return out;
}

std::string ClusterLODExternalPartMismatch(const ClusterLODExternalPart& expected, const Mesh& part)
{
	const ClusterLODExternalPart actual = DescribeClusterLODExternalPart(part);
	if (actual.sourceIdentifier != expected.sourceIdentifier || actual.primPath != expected.primPath || actual.subsetName != expected.subsetName) {
		return std::format("the mesh is '{}' {} '{}', not the part's cache '{}' {} '{}'", actual.sourceIdentifier, actual.primPath,
			actual.subsetName, expected.sourceIdentifier, expected.primPath, expected.subsetName);
	}
	if (actual.buildConfigHash != expected.buildConfigHash) {
		return std::format("the part was rebuilt (build hash {:016x}, the table names {:016x})", actual.buildConfigHash, expected.buildConfigHash);
	}
	// A registered mesh may have released its CPU nodes (MeshCpuDataPolicy): the cache identity then has to do.
	if (actual.nodeCount != 0u && actual.nodeCount != expected.nodeCount) {
		return std::format("the part has {} nodes, the table was built against {}", actual.nodeCount, expected.nodeCount);
	}
	if ((expected.flags & CLOD_EXTERNAL_PART_FLAG_NESTED) != 0u || !part.GetCLodExternalParts().empty()) {
		return "the part is itself a merged mesh: a portal cannot open one (merging embeds it instead)";
	}
	return {};
}

bool LoadClusterLODMergeEmbeddedMesh(ClusterLODPrebuiltData data, ClusterLODMergeEmbeddedMesh& out, std::string* outReason)
{
	const auto fail = [&](std::string reason) {
		if (outReason) *outReason = std::move(reason);
		return false;
	};
	if (data.externalParts.empty()) return fail("not a merged mesh (no external parts)");
	uint32_t pageCount = 0;
	if (!CLodCache::ReadContainerPageCount(data.cacheSource, pageCount)) return fail("container unavailable");
	if (pageCount != data.pageDiskLocators.size()) {
		return fail(std::format("the container has {} pages, the metadata locates {}", pageCount, data.pageDiskLocators.size()));
	}
	CLodCache::LoadedGroupPayload payload;
	if (!CLodCache::LoadMeshPagesSelectiveMapped(CLodCache::ResolveContainerPath(data.cacheSource), data.pageDiskLocators, 0u, pageCount, {}, payload)) {
		return fail("its pages could not be read");
	}
	out.meshPageBlobs = std::move(payload.pageBlobs);
	out.data = std::move(data);
	return true;
}

std::shared_ptr<Mesh> CreateClusterLODMergedMesh(
	ClusterLODPrebuiltData prebuiltData,
	std::span<const std::shared_ptr<Mesh>> parts,
	std::string* outReason)
{
	const auto fail = [&](std::string reason) -> std::shared_ptr<Mesh> {
		if (outReason) *outReason = std::move(reason);
		return nullptr;
	};
	if (prebuiltData.externalParts.empty() || prebuiltData.paletteParts.empty()) return fail("the cache names no external parts");
	if (parts.size() != prebuiltData.externalParts.size()) {
		return fail(std::format("{} parts given, the cache names {}", parts.size(), prebuiltData.externalParts.size()));
	}
	if (prebuiltData.paletteParts.size() != (std::max)(prebuiltData.materialSlotCount, 1u)) {
		return fail(std::format("the palette has {} slots, the clusters {}", prebuiltData.paletteParts.size(), prebuiltData.materialSlotCount));
	}
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (!parts[i]) return fail(std::format("part {} is missing", i));
		if (auto mismatch = ClusterLODExternalPartMismatch(prebuiltData.externalParts[i], *parts[i]); !mismatch.empty()) {
			return fail(std::format("part {} ('{}'): {}", i, prebuiltData.externalParts[i].assetKey, mismatch));
		}
	}
	std::vector<std::shared_ptr<Material>> palette;
	palette.reserve(prebuiltData.paletteParts.size());
	for (const uint32_t part : prebuiltData.paletteParts) {
		if (part >= parts.size() || !parts[part]->material) return fail(std::format("palette part {} has no material", part));
		palette.push_back(parts[part]->material);
	}
	MeshIngestBuilder builder(MeshVertexLayout::VertexSize(kMergedMeshVertexFlags), 0u, kMergedMeshVertexFlags);
	auto mesh = builder.Build(palette.front(), std::move(prebuiltData));
	if (!mesh) return fail("the mesh could not be built from its cache");
	mesh->SetPortalParts(std::vector<std::shared_ptr<Mesh>>(parts.begin(), parts.end()));
	mesh->SetMaterialPalette(std::move(palette));
	return mesh;
}
