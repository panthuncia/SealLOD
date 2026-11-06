// Merge hierarchy invariants (docs/merge_hierarchies.md) on a synthetic field of placed parts:
// - every child of a node (node groups and portal proxies) carries exactly its parent's children-group bounds,
// - errors grow toward the root, which never refines away,
// - at any camera distance every item is drawn exactly once: by its open portal or by one drawn ancestor node,
// - node clusters name material slots inside the palette.

#include <BasicRenderer/Assets/Import/ClusterLODMergeHierarchy.h>
#include <BasicRenderer/Assets/ClusterLODShaderTypes.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
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

void TestHierarchy()
{
	std::vector<ClusterLODMergeGeometry> geometries;
	geometries.push_back(MakeBlob(8, 0.02f, 0));
	geometries.push_back(MakeBlob(6, 0.03f, 1));
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
			items.push_back(item);
		}
	}
	ClusterLODMergeSettings settings;
	settings.fanout = 4;
	settings.nodeTriangleBudget = 256;
	settings.materialSlotCount = 3;
	ClusterLODMergeStats stats{};
	const ClusterLODPrebuildArtifacts built = BuildClusterLODMergeHierarchy(geometries, items, settings, &stats);
	const ClusterLODPrebuiltData& data = built.prebuiltData;
	std::fprintf(stdout, "merge hierarchy: items=%u nodes=%u levels=%u input=%llu node=%llu root=%u rootError=%g sloppy=%u groups=%zu nodes=%zu pages=%zu\n",
		stats.items, stats.nodes, stats.levels, static_cast<unsigned long long>(stats.inputTriangles),
		static_cast<unsigned long long>(stats.nodeTriangles), stats.rootTriangles, stats.rootError, stats.sloppyNodes,
		data.groups.size(), data.nodes.size(), built.cacheBuildData.meshPageBlobs.size());

	CHECK(stats.levels >= 2u);
	CHECK(data.assemblyInstances.size() == items.size());
	CHECK(data.assemblyTransforms.size() == items.size());
	CHECK(data.assemblyTransformMaterialBases.size() == items.size());
	CHECK(data.materialSlotCount == 3u);
	CHECK(data.maxTraversalDepth >= 3u + 2u);
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
		if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u) {
			proxies.push_back(g);
			CHECK(group.parentGroupId >= 0); // a proxy without a parent would always open
			continue;
		}
		if (group.segmentCount == 0u) continue; // children group
		const int32_t refined = data.segments[group.firstSegment].refinedGroup;
		CHECK(refined >= 0);
		for (uint32_t s = 0; s < group.segmentCount; ++s) CHECK(data.segments[group.firstSegment + s].refinedGroup == refined);
		const ClusterLODGroup& children = data.groups[static_cast<uint32_t>(refined)];
		CHECK(children.segmentCount == 0u && children.pageCount == 0u);
		CHECK(group.bounds.error >= children.bounds.error);
		nodes.push_back({ g, static_cast<uint32_t>(refined) });
		if (group.bounds.error >= (std::numeric_limits<float>::max)()) ++rootGroups;
	}
	CHECK(rootGroups == 1u);
	CHECK(proxies.size() == items.size());

	// Every child carries its parent's children-group bounds.
	auto parentChildren = [&](uint32_t childGroup) -> const ClusterLODGroup* {
		const int32_t parentOwn = data.groups[childGroup].parentGroupId;
		for (const NodeInfo& n : nodes) if (static_cast<int32_t>(n.own) == parentOwn) return &data.groups[n.children];
		return nullptr;
	};
	for (const uint32_t p : proxies) {
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
		CHECK(!ancestors[i].empty());
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
	CHECK(opened != 0u && merged != 0u); // the sweep saw both

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
}

int main()
{
	try {
		TestHierarchy();
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
