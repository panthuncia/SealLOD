// Merge hierarchy invariants (docs/merge_hierarchies.md) on a synthetic field of placed parts:
// - every child of a node (node groups and portal proxies) carries exactly its parent's children-group bounds,
// - errors grow toward the root, which never refines away; with a cap, no node is over it and the top is a forest,
// - items whose triangles are exact fold into nodes that keep them, with no portal,
// - at any camera distance every item is drawn exactly once: by its open portal or by one drawn ancestor node,
// - node clusters name material slots inside the palette,
// - with voxel items (forked parts), each branch on its own holds all of the above, and voxel-branch nodes voxelize them.

#include <BasicRenderer/Assets/Import/ClusterLODMergeHierarchy.h>
#include <BasicRenderer/Assets/Import/CLodCache.h>
#include <BasicRenderer/Assets/ClusterLODShaderTypes.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <limits>
#include <vector>

namespace {
int g_failures = 0;

#define CHECK(condition)                                                          \
	do {                                                                          \
		if (!(condition)) {                                                       \
			std::fprintf(stderr, "%s:%d FAILED: %s\n", __FILE__, __LINE__, #condition); \
			++g_failures;                                                         \
		}                                                                         \
	} while (false)

// A unit sphere-ish blob: a subdivided cube, so simplification has something to do.
ClusterLODMergeGeometry MakeBlob(uint32_t subdivisions, float error, uint16_t slot)
{
	ClusterLODMergeGeometry g;
	const int n = static_cast<int>(subdivisions);
	for (int face = 0; face < 6; ++face) {
		const uint32_t base = static_cast<uint32_t>(g.positions.size());
		for (int y = 0; y <= n; ++y) {
			for (int x = 0; x <= n; ++x) {
				const float u = 2.0f * x / n - 1.0f, v = 2.0f * y / n - 1.0f;
				DirectX::XMFLOAT3 p{};
				switch (face) {
				case 0: p = { 1, u, v }; break;
				case 1: p = { -1, v, u }; break;
				case 2: p = { u, 1, v }; break;
				case 3: p = { v, -1, u }; break;
				case 4: p = { u, v, 1 }; break;
				default: p = { v, u, -1 }; break;
				}
				const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
				p = { p.x / len, p.y / len, p.z / len };
				g.positions.push_back(p);
				g.normals.push_back(p);
				g.uvs.push_back({ 0.5f * (u + 1.0f), 0.5f * (v + 1.0f) });
			}
		}
		for (int y = 0; y < n; ++y) {
			for (int x = 0; x < n; ++x) {
				const uint32_t a = base + y * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
				g.indices.insert(g.indices.end(), { a, c, b, b, c, d });
			}
		}
	}
	g.triangleSlots.assign(g.indices.size() / 3u, slot);
	g.error = error;
	return g;
}

float ProjectedError(const clodBounds& b, DirectX::XMFLOAT3 camera)
{
	if (b.error >= (std::numeric_limits<float>::max)()) return (std::numeric_limits<float>::max)();
	const float dx = b.center[0] - camera.x, dy = b.center[1] - camera.y, dz = b.center[2] - camera.z;
	const float distance = (std::max)(std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius, 0.1f);
	return b.error / distance;
}

struct Case {
	const char* name;
	uint32_t subdivisions0, subdivisions1;
	float error0, error1;
	float cap;
	bool voxelItems = false; // part 1 forks: its items have a voxel branch
};

uint32_t Branches(const ClusterLODGroup& group) { return ClusterLODGroupBranches(group.flags); }

void TestHierarchy(const Case& test)
{
	std::fprintf(stdout, "case %s\n", test.name);
	const bool capped = std::isfinite(test.cap);
	const bool exact = test.error0 <= 0.0f && test.error1 <= 0.0f;
	std::vector<ClusterLODMergeGeometry> geometries;
	geometries.push_back(MakeBlob(test.subdivisions0, test.error0, 0));
	geometries.push_back(MakeBlob(test.subdivisions1, test.error1, 1));
	std::vector<ClusterLODMergeItem> items;
	constexpr int kSide = 12;
	for (int z = 0; z < kSide; ++z) {
		for (int x = 0; x < kSide; ++x) {
			ClusterLODMergeItem item;
			item.geometry = static_cast<uint32_t>((x + z) & 1);
			item.part = item.geometry;
			item.partNode = 7;
			item.partTraversalDepth = 3;
			const float scale = 1.0f + 0.5f * ((x * 7 + z * 3) % 4); // a mix of sizes
			DirectX::XMStoreFloat4x4(&item.partToHierarchy,
				DirectX::XMMatrixScaling(scale, scale, scale) * DirectX::XMMatrixTranslation(x * 6.0f, 0.0f, z * 6.0f));
			item.partLodSphere = { 0, 0, 0, 1.0f };
			item.materialBase = item.geometry; // part 0's slots start at palette 0, part 1's at 1; part 1 uses its slot 1
			if (test.voxelItems && item.geometry == 1u) {
				item.voxelGeometry = 1u; // its triangles stand for what its voxels were made from
				item.voxelError = test.error1 * 1.5f;
			}
			items.push_back(item);
		}
	}
	ClusterLODMergeSettings settings;
	settings.fanout = 4;
	settings.nodeTriangleBudget = 256;
	settings.materialSlotCount = 3;
	settings.maxNodeError = test.cap;
	ClusterLODMergeStats stats{};
	const ClusterLODPrebuildArtifacts built = BuildClusterLODMergeHierarchy(geometries, items, settings, &stats);
	const ClusterLODPrebuiltData& data = built.prebuiltData;
	std::fprintf(stdout, "  top nodes=%u top portals=%u folded nodes=%u folded items=%u rejected=%u\n", stats.topNodes, stats.topPortals, stats.foldedNodes, stats.foldedItems, stats.rejectedNodes);
	std::fprintf(stdout, "merge hierarchy: items=%u nodes=%u levels=%u input=%llu node=%llu root=%u rootError=%g sloppy=%u groups=%zu nodes=%zu pages=%zu\n",
		stats.items, stats.nodes, stats.levels, static_cast<unsigned long long>(stats.inputTriangles),
		static_cast<unsigned long long>(stats.nodeTriangles), stats.rootTriangles, stats.rootError, stats.sloppyNodes,
		data.groups.size(), data.nodes.size(), built.cacheBuildData.meshPageBlobs.size());

	const size_t portals = items.size() - stats.foldedItems;
	const uint32_t branchCount = test.voxelItems ? 2u : 1u;
	CHECK(stats.levels >= 2u);
	CHECK(data.assemblyInstances.size() == portals * branchCount);
	CHECK(data.assemblyTransforms.size() == portals * branchCount);
	CHECK(data.assemblyTransformMaterialBases.size() == portals * branchCount);
	if (test.voxelItems) {
		std::fprintf(stdout, "  voxel items=%u voxel-branch nodes=%u voxel nodes=%u cubes=%llu voxel-branch node tris=%llu\n", stats.voxelItems,
			stats.voxelBranchNodes, stats.voxelNodes, static_cast<unsigned long long>(stats.voxelCubes),
			static_cast<unsigned long long>(stats.voxelBranchNodeTriangles));
		CHECK(stats.voxelItems == items.size() / 2u);
		CHECK(stats.voxelNodes != 0u && stats.voxelCubes != 0u);
		// Half the items are voxels there, so the voxel branch merges fewer triangles.
		CHECK(stats.voxelBranchNodeTriangles < stats.nodeTriangles);
		uint32_t voxelGroups = 0;
		for (const ClusterLODGroup& group : data.groups) {
			CHECK((group.flags & (CLOD_GROUP_FLAG_BRANCH_TRIANGLE | CLOD_GROUP_FLAG_BRANCH_VOXEL)) != 0u);
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u) continue;
			++voxelGroups;
			CHECK((group.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) != 0u);
			CHECK((group.flags >> CLOD_GROUP_VOXEL_MATERIAL_SLOT_SHIFT) == 2u); // part 1's slot 1 at its palette base 1
			CHECK(group.representationError > 0.0f && group.pageCount != 0u);
		}
		CHECK(voxelGroups >= stats.voxelNodes);
		CHECK(built.prebuiltData.voxelPageCount != 0u);
	}
	for (uint32_t branch = 1u; branch <= (test.voxelItems ? 2u : 1u); ++branch) {
	const uint32_t branchBit = test.voxelItems ? branch : CLOD_BRANCH_ALL;
	CHECK(data.materialSlotCount == 3u);
	if (!capped) CHECK(data.maxTraversalDepth >= 3u + 2u);
	if (exact) CHECK(stats.foldedNodes != 0u && stats.foldedItems != 0u);
	else CHECK(stats.foldedItems == 0u);
	if (capped) CHECK(stats.topNodes + stats.topPortals > 1u);
	for (const ClusterLODAssemblyInstance& instance : data.assemblyInstances) {
		CHECK((instance.flags & CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL) != 0u);
		CHECK(instance.targetRootNode == 7u);
		CHECK(instance.stackDepth <= 1u);
	}

	// Node groups: owned clusters refine into one page-less children group.
	struct NodeInfo { uint32_t own; uint32_t children; };
	std::vector<NodeInfo> nodes;
	std::vector<uint32_t> proxies;
	uint32_t rootGroups = 0;
	for (uint32_t g = 0; g < data.groups.size(); ++g) {
		const ClusterLODGroup& group = data.groups[g];
		if ((Branches(group) & branchBit) == 0u) continue;
		if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u) {
			proxies.push_back(g);
			// A proxy without a parent always opens: only the top of a capped hierarchy has them.
			CHECK(group.parentGroupId >= 0 || (capped && group.bounds.error >= (std::numeric_limits<float>::max)()));
			continue;
		}
		if (group.segmentCount == 0u) continue; // children group
		const int32_t refined = data.segments[group.firstSegment].refinedGroup;
		if (refined < 0) {
			// A folded node: exact triangles, no children.
			CHECK(exact);
			for (uint32_t s = 0; s < group.segmentCount; ++s) CHECK(data.segments[group.firstSegment + s].refinedGroup < 0);
			nodes.push_back({ g, UINT32_MAX });
			if (group.bounds.error >= (std::numeric_limits<float>::max)()) ++rootGroups;
			continue;
		}
		for (uint32_t s = 0; s < group.segmentCount; ++s) CHECK(data.segments[group.firstSegment + s].refinedGroup == refined);
		const ClusterLODGroup& children = data.groups[static_cast<uint32_t>(refined)];
		CHECK(children.segmentCount == 0u && children.pageCount == 0u);
		CHECK(group.bounds.error >= children.bounds.error);
		CHECK(children.bounds.error <= test.cap);
		// A voxel-branch node's voxel groups refine into the same children group as its triangle group: count nodes once.
		const bool sameNode = std::any_of(nodes.begin(), nodes.end(), [&](const NodeInfo& n) { return n.children == static_cast<uint32_t>(refined); });
		nodes.push_back({ g, static_cast<uint32_t>(refined) });
		if (group.bounds.error >= (std::numeric_limits<float>::max)() && !sameNode) ++rootGroups;
	}
	if (capped) CHECK(rootGroups > 1u || stats.topPortals != 0u);
	else CHECK(rootGroups == 1u);
	CHECK(proxies.size() == portals);

	// Every child carries its parent's children-group bounds.
	auto parentChildren = [&](uint32_t childGroup) -> const ClusterLODGroup* {
		const int32_t parentOwn = data.groups[childGroup].parentGroupId;
		for (const NodeInfo& n : nodes) if (static_cast<int32_t>(n.own) == parentOwn && n.children != UINT32_MAX) return &data.groups[n.children];
		return nullptr;
	};
	for (const uint32_t p : proxies) {
		if (data.groups[p].parentGroupId < 0) continue;
		const ClusterLODGroup* pc = parentChildren(p);
		CHECK(pc != nullptr);
		if (pc) CHECK(std::memcmp(&pc->bounds, &data.groups[p].bounds, sizeof(clodBounds)) == 0);
	}
	for (const NodeInfo& n : nodes) {
		if (data.groups[n.own].parentGroupId < 0) continue;
		const ClusterLODGroup* pc = parentChildren(n.own);
		CHECK(pc != nullptr);
		if (pc) CHECK(std::memcmp(&pc->bounds, &data.groups[n.own].bounds, sizeof(clodBounds)) == 0);
	}

	// Exactly-once coverage: walk each item's ancestor chain under the renderer's cut rules.
	std::vector<std::vector<uint32_t>> ancestors(proxies.size());
	for (size_t i = 0; i < proxies.size(); ++i) {
		int32_t parent = data.groups[proxies[i]].parentGroupId;
		while (parent >= 0) {
			ancestors[i].push_back(static_cast<uint32_t>(parent));
			parent = data.groups[static_cast<uint32_t>(parent)].parentGroupId;
		}
		CHECK(!ancestors[i].empty() || data.groups[proxies[i]].bounds.error >= (std::numeric_limits<float>::max)());
	}
	auto childrenOf = [&](uint32_t own) {
		for (const NodeInfo& n : nodes) if (n.own == own) return n.children;
		return UINT32_MAX;
	};
	uint32_t opened = 0, merged = 0;
	for (const float threshold : { 1e-5f, 1e-4f, 1e-3f, 3e-3f, 1e-2f, 3e-2f, 0.1f }) {
		for (const float distance : { 5.0f, 40.0f, 200.0f, 1000.0f, 1e5f }) {
			const DirectX::XMFLOAT3 camera{ 30.0f, 10.0f, -distance };
			for (size_t i = 0; i < proxies.size(); ++i) {
				uint32_t covers = ProjectedError(data.groups[proxies[i]].bounds, camera) > threshold ? 1u : 0u;
				opened += covers;
				for (const uint32_t own : ancestors[i]) {
					const uint32_t children = childrenOf(own);
					const bool draws = ProjectedError(data.groups[own].bounds, camera) > threshold &&
						!(ProjectedError(data.groups[children].bounds, camera) > threshold);
					covers += draws ? 1u : 0u;
					merged += draws ? 1u : 0u;
				}
				CHECK(covers == 1u);
			}
		}
	}
	CHECK(exact || (opened != 0u && merged != 0u)); // the sweep saw both (exact items all fold: nothing opens)
	}

	// Node clusters name palette slots.
	uint32_t slotsSeen = 0;
	for (const auto& blob : built.cacheBuildData.meshPageBlobs) {
		CLodPageHeader header{};
		if (blob.size() < sizeof(header)) continue;
		std::memcpy(&header, blob.data(), sizeof(header));
		if (header.formatAndKind != CLOD_TRIANGLE_PAGE_MAGIC) continue;
		for (uint32_t m = 0; m < header.meshletCount; ++m) {
			CLodMeshletDescriptor desc{};
			std::memcpy(&desc, blob.data() + header.descriptorOffset + m * sizeof(desc), sizeof(desc));
			const uint32_t slot = std::bit_cast<uint32_t>(desc.terrainRvtLocalSkyrimXYRadius);
			CHECK(slot < 3u);
			slotsSeen |= 1u << slot;
		}
	}
	CHECK(slotsSeen == 0x5u); // part 0's slot 0 (base 0) and part 1's slot 1 (base 1) reach the merged geometry
}

// A merged mesh's references survive its cache: the external parts and palette are in its metadata, it can be listed
// by key prefix, and removing it removes it. The builder rejects a palette that does not match the clusters' slots.
void TestMergedMeshCache()
{
	std::fprintf(stdout, "case merged mesh cache\n");
	std::vector<ClusterLODMergeGeometry> geometries{ MakeBlob(4, 0.02f, 0), MakeBlob(3, 0.03f, 1) };
	std::vector<ClusterLODMergeItem> items;
	for (int i = 0; i < 6; ++i) {
		ClusterLODMergeItem item;
		item.geometry = static_cast<uint32_t>(i & 1);
		item.part = item.geometry;
		item.partNode = 0;
		DirectX::XMStoreFloat4x4(&item.partToHierarchy, DirectX::XMMatrixTranslation(i * 3.0f, 0.0f, 0.0f));
		item.partLodSphere = { 0, 0, 0, 1.0f };
		item.materialBase = item.geometry;
		items.push_back(item);
	}
	ClusterLODMergeSettings settings;
	settings.fanout = 4;
	settings.nodeTriangleBudget = 128;
	settings.materialSlotCount = 3;
	for (uint32_t p = 0; p < 2; ++p) {
		ClusterLODExternalPart part;
		part.sourceIdentifier = "part_source_" + std::to_string(p);
		part.primPath = "/mesh";
		part.buildConfigHash = 0x1234u + p;
		part.assetKey = "meshes/part" + std::to_string(p) + ".nif";
		part.variantHash = p == 1u ? 0xABCDu : 0u;
		if (p == 1u) part.variant = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
		part.partIndex = p;
		part.meshIndex = 2u * p;
		part.nodeCount = 9u + p;
		settings.externalParts.push_back(part);
	}
	settings.paletteParts = { 0u, 1u, 1u };

	ClusterLODMergeSettings badPalette = settings;
	badPalette.paletteParts = { 0u, 1u };
	bool threw = false;
	try {
		(void)BuildClusterLODMergeHierarchy(geometries, items, badPalette);
	} catch (const std::exception&) {
		threw = true;
	}
	CHECK(threw);

	const ClusterLODPrebuildArtifacts built = BuildClusterLODMergeHierarchy(geometries, items, settings);
	CHECK(built.prebuiltData.externalParts == settings.externalParts);
	const std::string key = "test-merged:world_r00001";
	CHECK(CLodCache::SaveMergedMesh(key, built.prebuiltData, built.cacheBuildData.AsPayload()));
	CHECK(CLodCache::SaveMergedMesh(key, built.prebuiltData, built.cacheBuildData.AsPayload())); // replaces
	const auto loaded = CLodCache::TryLoadMergedMesh(key);
	CHECK(loaded.has_value());
	if (loaded) {
		CHECK(loaded->prebuiltData.externalParts == settings.externalParts);
		CHECK(loaded->prebuiltData.paletteParts == settings.paletteParts);
		CHECK(loaded->prebuiltData.materialSlotCount == 3u);
		CHECK(loaded->prebuiltData.assemblyInstances.size() == built.prebuiltData.assemblyInstances.size());
	}
	const auto listed = CLodCache::ListMergedMeshes("test-merged:");
	CHECK(listed.size() == 1u && listed.front() == key);
	CHECK(CLodCache::ListMergedMeshes("other-prefix:").empty());
	CLodCache::RemoveMergedMesh(key);
	CHECK(!CLodCache::TryLoadMergedMesh(key).has_value());
	CHECK(CLodCache::ListMergedMeshes("test-merged:").empty());
}

ClusterLODExternalPart TestPart(uint32_t p)
{
	ClusterLODExternalPart part;
	part.sourceIdentifier = "part_source_" + std::to_string(p);
	part.primPath = "/mesh";
	part.buildConfigHash = 0x1234u + p;
	part.assetKey = "meshes/part" + std::to_string(p) + ".nif";
	part.partIndex = p;
	part.nodeCount = 9u + p;
	return part;
}

// A capped, forked field of the two test parts, as a merged mesh: it has top portals and voxel-branch groups.
ClusterLODPrebuildArtifacts BuildFieldMesh(const std::vector<ClusterLODMergeGeometry>& geometries, float offset)
{
	std::vector<ClusterLODMergeItem> items;
	for (int z = 0; z < 5; ++z) {
		for (int x = 0; x < 5; ++x) {
			ClusterLODMergeItem item;
			item.geometry = static_cast<uint32_t>((x + z) & 1);
			item.part = item.geometry;
			item.partNode = 7;
			item.partTraversalDepth = 3;
			const float scale = 1.0f + 0.5f * ((x * 7 + z * 3) % 4);
			DirectX::XMStoreFloat4x4(&item.partToHierarchy,
				DirectX::XMMatrixScaling(scale, scale, scale) * DirectX::XMMatrixTranslation(offset + x * 6.0f, 0.0f, z * 6.0f));
			item.partLodSphere = { 0, 0, 0, 1.0f };
			item.materialBase = item.geometry;
			if (item.geometry == 1u) {
				item.voxelGeometry = 1u;
				item.voxelError = 0.045f;
			}
			items.push_back(item);
		}
	}
	ClusterLODMergeSettings settings;
	settings.fanout = 4;
	settings.nodeTriangleBudget = 256;
	settings.materialSlotCount = 3;
	settings.maxNodeError = 1.0f;
	settings.externalParts = { TestPart(0), TestPart(1) };
	settings.paletteParts = { 0u, 1u, 1u };
	return BuildClusterLODMergeHierarchy(geometries, items, settings);
}

bool Near(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b)
{
	return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f && std::abs(a.z - b.z) < 1e-4f && std::abs(a.w - b.w) < 1e-3f;
}

// Every group a page's clusters refine into is one of the mesh's.
void CheckPagesRefineInside(const ClusterLODPrebuildArtifacts& built)
{
	const auto groupCount = static_cast<int32_t>(built.prebuiltData.groups.size());
	for (const auto& blob : built.cacheBuildData.meshPageBlobs) {
		uint32_t magic = 0;
		std::memcpy(&magic, blob.data(), sizeof(magic));
		if (magic == CLOD_TRIANGLE_PAGE_MAGIC) {
			CLodPageHeader header{};
			std::memcpy(&header, blob.data(), sizeof(header));
			for (uint32_t m = 0; m < header.meshletCount; ++m) {
				CLodMeshletDescriptor desc{};
				std::memcpy(&desc, blob.data() + header.descriptorOffset + m * sizeof(desc), sizeof(desc));
				CHECK(static_cast<int32_t>(desc.triangleCountAndRefinedGroup >> 16u) - 1 < groupCount);
			}
		} else if (magic == CLOD_VOXEL_PAGE_MAGIC) {
			CLodVoxelPageHeader header{};
			std::memcpy(&header, blob.data(), sizeof(header));
			for (uint32_t c = 0; c < header.clusterCount; ++c) {
				CLodVoxelClusterRecord record{};
				std::memcpy(&record, blob.data() + header.descriptorOffset + c * header.clusterRecordStride, sizeof(record));
				CHECK(record.refinedGroup < groupCount);
			}
		}
	}
}

// Nesting: merged meshes embedded in a bigger one keep their groups (both branches), and their portals open the leaf
// parts, renamed into the root's table, under transforms composed with the item's and material bases past the
// palette block their palette landed in. A root that embeds them can itself be embedded.
void TestEmbedding()
{
	std::fprintf(stdout, "case embedding\n");
	std::vector<ClusterLODMergeGeometry> partGeometries{ MakeBlob(8, 0.02f, 0), MakeBlob(6, 0.03f, 1) };
	const ClusterLODPrebuildArtifacts m1 = BuildFieldMesh(partGeometries, 0.0f);
	const ClusterLODPrebuildArtifacts m2 = BuildFieldMesh(partGeometries, 40.0f);
	std::vector<ClusterLODMergeEmbeddedMesh> embedded(2);
	embedded[0] = { m1.prebuiltData, m1.cacheBuildData.meshPageBlobs };
	embedded[1] = { m2.prebuiltData, m2.cacheBuildData.meshPageBlobs };
	const auto voxelGroups = [](const ClusterLODPrebuiltData& d) {
		return std::count_if(d.groups.begin(), d.groups.end(), [](const ClusterLODGroup& g) { return (g.flags & CLOD_GROUP_FLAG_BRANCH_VOXEL) != 0u; });
	};
	CHECK(voxelGroups(m1.prebuiltData) != 0);

	// The embedded meshes' coarsest stand-ins (a merge only needs triangles in their space, and their error), and a leaf.
	std::vector<ClusterLODMergeGeometry> geometries{ MakeBlob(6, 1.5f, 1), MakeBlob(8, 0.02f, 0) };
	DirectX::XMMATRIX placement[3] = {
		DirectX::XMMatrixTranslation(0.0f, 0.0f, 0.0f),
		DirectX::XMMatrixScaling(2.0f, 2.0f, 2.0f) * DirectX::XMMatrixRotationY(0.5f) * DirectX::XMMatrixTranslation(100.0f, 0.0f, 10.0f),
		DirectX::XMMatrixTranslation(60.0f, 0.0f, -20.0f) };
	std::vector<ClusterLODMergeItem> items(3);
	for (uint32_t i = 0; i < 3; ++i) {
		ClusterLODMergeItem& item = items[i];
		DirectX::XMStoreFloat4x4(&item.partToHierarchy, placement[i]);
		item.partLodSphere = { 0, 0, 0, 1.0f };
		if (i < 2) {
			item.geometry = 0;
			item.embeddedMesh = i;
			item.partTraversalDepth = (i == 0 ? m1 : m2).prebuiltData.maxTraversalDepth;
		} else {
			item.geometry = 1;
			item.part = 0;
			item.partNode = 7;
			item.materialBase = 0;
		}
	}
	ClusterLODMergeSettings settings;
	settings.fanout = 4;
	settings.nodeTriangleBudget = 256;
	settings.materialSlotCount = 1;
	settings.externalParts = { TestPart(0) };
	settings.paletteParts = { 0u };
	settings.embeddedMeshes = embedded;
	ClusterLODMergeStats stats{};
	const ClusterLODPrebuildArtifacts built = BuildClusterLODMergeHierarchy(geometries, items, settings, &stats);
	const ClusterLODPrebuiltData& data = built.prebuiltData;
	std::fprintf(stdout, "  embedded items=%u groups=%u, root groups=%zu nodes=%zu instances=%zu parts=%zu slots=%u\n", stats.embeddedItems,
		stats.embeddedGroups, data.groups.size(), data.nodes.size(), data.assemblyInstances.size(), data.externalParts.size(), data.materialSlotCount);

	CHECK(stats.embeddedItems == 2u);
	CHECK(stats.embeddedGroups == m1.prebuiltData.groups.size() + m2.prebuiltData.groups.size());
	// Part 0 is shared by the leaf and both embedded meshes: one row each.
	CHECK(data.externalParts.size() == 2u && data.externalParts[0] == TestPart(0) && data.externalParts[1] == TestPart(1));
	// The leaf's palette, then each embedded mesh's, whole.
	CHECK(data.materialSlotCount == 7u);
	CHECK((data.paletteParts == std::vector<uint32_t>{ 0u, 0u, 1u, 1u, 0u, 1u, 1u }));
	CHECK(voxelGroups(data) == voxelGroups(m1.prebuiltData) + voxelGroups(m2.prebuiltData));

	// Transforms: the root's own (one per proxy), then each copy's, composed with its item's.
	const size_t copied = m1.prebuiltData.assemblyTransforms.size() + m2.prebuiltData.assemblyTransforms.size();
	CHECK(data.assemblyTransforms.size() > copied);
	const size_t own = data.assemblyTransforms.size() - copied;
	for (uint32_t k = 0; k < 2; ++k) {
		const ClusterLODPrebuiltData& source = (k == 0 ? m1 : m2).prebuiltData;
		const size_t base = own + (k == 0 ? 0u : m1.prebuiltData.assemblyTransforms.size());
		const uint32_t paletteBase = k == 0 ? 1u : 4u;
		for (size_t t = 0; t < source.assemblyTransforms.size(); ++t) {
			// Row-vector: part -> embedded mesh -> root.
			const ClusterLODAssemblyTransform& s = source.assemblyTransforms[t];
			const DirectX::XMMATRIX sm = DirectX::XMMatrixSet(s.row0.x, s.row1.x, s.row2.x, 0, s.row0.y, s.row1.y, s.row2.y, 0,
				s.row0.z, s.row1.z, s.row2.z, 0, s.row0.w, s.row1.w, s.row2.w, 1);
			DirectX::XMFLOAT4X4 expected;
			DirectX::XMStoreFloat4x4(&expected, sm * placement[k]);
			const ClusterLODAssemblyTransform& c = data.assemblyTransforms[base + t];
			CHECK(Near(c.row0, { expected._11, expected._21, expected._31, expected._41 }));
			CHECK(Near(c.row1, { expected._12, expected._22, expected._32, expected._42 }));
			CHECK(Near(c.row2, { expected._13, expected._23, expected._33, expected._43 }));
			CHECK(data.assemblyTransformMaterialBases[base + t] == paletteBase + source.assemblyTransformMaterialBases[t]);
		}
	}

	// Instances: portals open leaf parts; the two into the copies target their roots, inside the root.
	uint32_t intoCopies = 0;
	for (const ClusterLODAssemblyInstance& instance : data.assemblyInstances) {
		if ((instance.flags & CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL) != 0u) {
			CHECK(instance.stackDepth < data.externalParts.size());
			CHECK(instance.targetRootNode == 7u);
		} else {
			++intoCopies;
			CHECK(instance.targetRootNode < data.nodes.size());
		}
	}
	CHECK(intoCopies == 2u);
	for (const ClusterLODNode& node : data.nodes) {
		if (node.range.isGroup == CLOD_NODE_INTERNAL) {
			CHECK(node.range.indexOrOffset + node.range.countMinusOne < data.nodes.size());
			continue;
		}
		CHECK(node.range.ownerGroupId < data.groups.size());
		if (node.range.isGroup == CLOD_NODE_INSTANCE_ROOT) CHECK(node.range.indexOrOffset < data.assemblyInstances.size());
		if (node.range.isGroup == CLOD_NODE_SEGMENT_LEAF) CHECK(node.range.indexOrOffset < data.segments.size());
	}
	for (const ClusterLODGroup& group : data.groups) {
		CHECK(group.parentGroupId < static_cast<int32_t>(data.groups.size()));
		CHECK(group.branchGroupId < static_cast<int32_t>(data.groups.size()));
		for (uint32_t s = 0; s < group.segmentCount; ++s) {
			CHECK(data.segments[group.firstSegment + s].refinedGroup < static_cast<int32_t>(data.groups.size()));
		}
	}
	CheckPagesRefineInside(built);

	// The root's cache round-trips with the merged tables.
	const std::string key = "test-merged:world_nested";
	CHECK(CLodCache::SaveMergedMesh(key, data, built.cacheBuildData.AsPayload()));
	const auto loaded = CLodCache::TryLoadMergedMesh(key);
	CHECK(loaded.has_value());
	if (loaded) {
		CHECK(loaded->prebuiltData.externalParts == data.externalParts);
		CHECK(loaded->prebuiltData.paletteParts == data.paletteParts);
		CHECK(loaded->prebuiltData.groups.size() == data.groups.size());
	}
	CLodCache::RemoveMergedMesh(key);

	// One more level: the nested root, embedded beside one of the meshes it embeds.
	std::vector<ClusterLODMergeEmbeddedMesh> outer(2);
	outer[0] = { data, built.cacheBuildData.meshPageBlobs };
	outer[1] = { m1.prebuiltData, m1.cacheBuildData.meshPageBlobs };
	std::vector<ClusterLODMergeItem> outerItems(2);
	for (uint32_t i = 0; i < 2; ++i) {
		outerItems[i].geometry = 0;
		outerItems[i].embeddedMesh = i;
		outerItems[i].partLodSphere = { 0, 0, 0, 1.0f };
		DirectX::XMStoreFloat4x4(&outerItems[i].partToHierarchy, DirectX::XMMatrixTranslation(i * 300.0f, 0.0f, 0.0f));
	}
	ClusterLODMergeSettings outerSettings;
	outerSettings.fanout = 4;
	outerSettings.nodeTriangleBudget = 256;
	outerSettings.embeddedMeshes = outer;
	ClusterLODMergeStats outerStats{};
	const ClusterLODPrebuildArtifacts top = BuildClusterLODMergeHierarchy(geometries, outerItems, outerSettings, &outerStats);
	CHECK(outerStats.embeddedItems == 2u);
	CHECK(top.prebuiltData.externalParts.size() == 2u);
	CHECK(top.prebuiltData.materialSlotCount == 10u); // 7 + 3
	uint32_t outerIntoCopies = 0;
	for (const ClusterLODAssemblyInstance& instance : top.prebuiltData.assemblyInstances) {
		if ((instance.flags & CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL) != 0u) {
			CHECK(instance.stackDepth < 2u);
		} else {
			++outerIntoCopies;
			CHECK(instance.targetRootNode < top.prebuiltData.nodes.size());
		}
	}
	CHECK(outerIntoCopies == 2u + 2u); // its own two, and the two inside the nested root's copy
	CheckPagesRefineInside(top);
}
}

int main()
{
	// The merged-mesh cache case writes a cache: keep it out of the real one.
	const std::filesystem::path cacheRoot = std::filesystem::temp_directory_path() / "sarp_merge_hierarchy_tests";
	std::error_code ec;
	std::filesystem::remove_all(cacheRoot, ec);
	_putenv_s("SARP_CACHE_ROOT", cacheRoot.string().c_str());
	_putenv_s("SARP_CACHE_PACK", "0");
	try {
		TestMergedMeshCache();
		TestEmbedding();
		TestHierarchy({ "uncapped", 8, 6, 0.02f, 0.03f, std::numeric_limits<float>::infinity() });
		TestHierarchy({ "capped", 8, 6, 0.02f, 0.03f, 1.0f });
		TestHierarchy({ "exact", 2, 1, 0.0f, 0.0f, std::numeric_limits<float>::infinity() });
		TestHierarchy({ "voxel items", 8, 6, 0.02f, 0.03f, std::numeric_limits<float>::infinity(), true });
		TestHierarchy({ "voxel items, capped", 8, 6, 0.02f, 0.03f, 1.0f, true });
	} catch (const std::exception& e) {
		std::fprintf(stderr, "exception: %s\n", e.what());
		return 1;
	}
	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::fprintf(stdout, "MergeHierarchyTests passed\n");
	return 0;
}
