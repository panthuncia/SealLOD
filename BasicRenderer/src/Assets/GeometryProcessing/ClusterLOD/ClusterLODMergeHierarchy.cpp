// Merge hierarchies: see include/BasicRenderer/Assets/Import/ClusterLODMergeHierarchy.h and docs/merge_hierarchies.md.
//
// Every entity (an item, or a node made of entities) has a representation: geometry in hierarchy space and the error
// of that geometry against what it stands for. A node's representation is its children's, merged and simplified, and
// its error is never below theirs. For node N with parent P, in the cut rules the renderer uses (a group's error is
// the error of whatever replaces it):
//   G_N  N's own groups:            sphere(P), error(P)   (FLT_MAX at the root)
//   P_N  a page-less "children" group every segment of G_N refines into: sphere(N), error(N)
//   and every child of N - a merge node's G_M, or an item's portal proxy - carries exactly P_N's sphere and error.
// So N draws when its parent is not good enough and it is, and all of its children switch together: no merged cluster
// ever stands for part of a child.

#include <BasicRenderer/Assets/Import/ClusterLODMergeHierarchy.h>
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

#include <meshoptimizer.h>
#include <spdlog/spdlog.h>

#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include <BasicRenderer/Scene/VertexFlags.h>
#include "../shaders/Common/defines.h"

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSkinningSidecar.h"

namespace
{
	using namespace DirectX;
	using clod_detail::ClusterLODBuildState;

	constexpr uint32_t kVertexFlags = VertexFlags::VERTEX_NORMALS | VertexFlags::VERTEX_TEXCOORDS | VertexFlags::VERTEX_COLORS;

	struct Vertex
	{
		XMFLOAT3 position;
		XMFLOAT3 normal;
		XMFLOAT2 uv;
		XMFLOAT3 color;
		uint32_t slot;
	};

	struct Representation
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		float error = 0.0f;
		XMFLOAT4 sphere{ 0, 0, 0, 0 };
	};

	struct Entity
	{
		bool leaf = true;
		uint32_t item = 0;
		std::vector<uint32_t> children;
		Representation rep;
		uint32_t level = 0;
		uint32_t parent = UINT32_MAX;
		// Groups, once built.
		uint32_t ownGroup = UINT32_MAX;      // G_N (nodes)
		uint32_t childrenGroup = UINT32_MAX; // P_N (nodes)
		uint32_t proxyGroup = UINT32_MAX;    // the portal proxy (leaves)
	};

	XMFLOAT4 MergeSpheres(XMFLOAT4 a, XMFLOAT4 b)
	{
		if (a.w <= 0.0f) return b;
		if (b.w <= 0.0f) return a;
		const XMVECTOR ca = XMVectorSet(a.x, a.y, a.z, 0), cb = XMVectorSet(b.x, b.y, b.z, 0);
		const float d = XMVectorGetX(XMVector3Length(XMVectorSubtract(cb, ca)));
		if (d + b.w <= a.w) return a;
		if (d + a.w <= b.w) return b;
		const float r = 0.5f * (d + a.w + b.w);
		const XMVECTOR c = d > 1e-12f ? XMVectorAdd(ca, XMVectorScale(XMVectorSubtract(cb, ca), (r - a.w) / d)) : ca;
		return XMFLOAT4(XMVectorGetX(c), XMVectorGetY(c), XMVectorGetZ(c), r * (1.0f + 1e-5f));
	}

	XMFLOAT4 SphereOfVertices(const std::vector<Vertex>& vertices)
	{
		if (vertices.empty()) return XMFLOAT4(0, 0, 0, 0);
		XMFLOAT3 lo = vertices[0].position, hi = lo;
		for (const Vertex& v : vertices) {
			lo = XMFLOAT3((std::min)(lo.x, v.position.x), (std::min)(lo.y, v.position.y), (std::min)(lo.z, v.position.z));
			hi = XMFLOAT3((std::max)(hi.x, v.position.x), (std::max)(hi.y, v.position.y), (std::max)(hi.z, v.position.z));
		}
		const XMFLOAT3 c(0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z));
		float r = 0.0f;
		for (const Vertex& v : vertices) {
			const float dx = v.position.x - c.x, dy = v.position.y - c.y, dz = v.position.z - c.z;
			r = (std::max)(r, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
		return XMFLOAT4(c.x, c.y, c.z, r * (1.0f + 1e-5f));
	}

	float MaxScale(const XMFLOAT4X4& m)
	{
		const float sx = std::sqrt(m._11 * m._11 + m._12 * m._12 + m._13 * m._13);
		const float sy = std::sqrt(m._21 * m._21 + m._22 * m._22 + m._23 * m._23);
		const float sz = std::sqrt(m._31 * m._31 + m._32 * m._32 + m._33 * m._33);
		return (std::max)(sx, (std::max)(sy, sz));
	}

	// Row-vector matrix to the column-vector 3x4 rows ClusterLODAssemblyTransform stores.
	ClusterLODAssemblyTransform ToAssemblyTransform(const XMFLOAT4X4& m)
	{
		ClusterLODAssemblyTransform t{};
		t.row0 = XMFLOAT4(m._11, m._21, m._31, m._41);
		t.row1 = XMFLOAT4(m._12, m._22, m._32, m._42);
		t.row2 = XMFLOAT4(m._13, m._23, m._33, m._43);
		return t;
	}

	Representation LeafRepresentation(const ClusterLODMergeGeometry& g, const ClusterLODMergeItem& item)
	{
		Representation rep;
		const XMMATRIX m = XMLoadFloat4x4(&item.partToHierarchy);
		XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, m));
		const size_t vertexCount = g.positions.size();
		// One output vertex per (source vertex, material slot): a part's mesh is one material, but a vertex shared by
		// triangles of two slots must split so every vertex has exactly one.
		std::unordered_map<uint64_t, uint32_t> outputVertex;
		outputVertex.reserve(vertexCount);
		const size_t triangleCount = g.indices.size() / 3u;
		rep.indices.reserve(g.indices.size());
		for (size_t t = 0; t < triangleCount; ++t) {
			const uint32_t slot = t < g.triangleSlots.size() ? g.triangleSlots[t] : 0u;
			for (uint32_t k = 0; k < 3u; ++k) {
				const uint32_t source = g.indices[t * 3u + k];
				if (source >= vertexCount) throw std::runtime_error("merge hierarchy: item geometry index out of range");
				const uint64_t key = (static_cast<uint64_t>(source) << 16u) | slot;
				auto [it, inserted] = outputVertex.try_emplace(key, static_cast<uint32_t>(rep.vertices.size()));
				if (inserted) {
					Vertex v{};
					XMStoreFloat3(&v.position, XMVector3TransformCoord(XMLoadFloat3(&g.positions[source]), m));
					const XMFLOAT3 n0 = source < g.normals.size() ? g.normals[source] : XMFLOAT3(0, 0, 1);
					XMStoreFloat3(&v.normal, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&n0), normalMatrix)));
					v.uv = source < g.uvs.size() ? g.uvs[source] : XMFLOAT2(0, 0);
					v.color = source < g.colors.size() ? g.colors[source] : XMFLOAT3(1, 1, 1);
					v.slot = item.materialBase + slot;
					rep.vertices.push_back(v);
				}
				rep.indices.push_back(it->second);
			}
		}
		rep.error = (std::max)(g.error, 0.0f) * MaxScale(item.partToHierarchy);
		XMFLOAT4 sphere = item.partLodSphere;
		{
			const XMVECTOR c = XMVector3TransformCoord(XMVectorSet(sphere.x, sphere.y, sphere.z, 1), m);
			sphere = XMFLOAT4(XMVectorGetX(c), XMVectorGetY(c), XMVectorGetZ(c), sphere.w * MaxScale(item.partToHierarchy));
		}
		rep.sphere = MergeSpheres(sphere, SphereOfVertices(rep.vertices));
		return rep;
	}

	// Children's representations merged and, past the budget, simplified. Material boundaries are vertex seams, which
	// the attribute-aware simplifier keeps; only the sloppy fallback may blend them.
	Representation BuildNodeRepresentation(
		const std::vector<Entity>& entities,
		const std::vector<uint32_t>& children,
		uint32_t triangleBudget,
		const ClusterLODMergeSettings& settings,
		bool& usedSloppy)
	{
		Representation rep;
		float childError = 0.0f;
		for (const uint32_t child : children) {
			const Representation& source = entities[child].rep;
			const uint32_t base = static_cast<uint32_t>(rep.vertices.size());
			rep.vertices.insert(rep.vertices.end(), source.vertices.begin(), source.vertices.end());
			for (const uint32_t index : source.indices) rep.indices.push_back(base + index);
			childError = (std::max)(childError, source.error);
			rep.sphere = MergeSpheres(rep.sphere, source.sphere);
		}
		// A node's error is strictly above its children's, even when it only unions them: the cut needs a parent to
		// switch before its children, never at the same error.
		const float floorError = childError * 1.0001f + 1e-7f;
		rep.error = floorError;
		if (rep.indices.empty()) return rep;
		// Every node simplifies toward its level's error, even under budget: an unsimplified union only duplicates its
		// children's geometry at their error.

		// Weld: coincident positions across children become exactly equal, so the simplifier sees the seams.
		const float targetError = (std::max)(childError * settings.errorGrowth, rep.sphere.w * 1e-4f);
		const float weld = (std::max)(targetError * settings.weldRelativeTolerance, 1e-5f);
		for (Vertex& v : rep.vertices) {
			v.position = XMFLOAT3(std::round(v.position.x / weld) * weld, std::round(v.position.y / weld) * weld, std::round(v.position.z / weld) * weld);
		}

		std::vector<float> attributes(rep.vertices.size() * 3u);
		for (size_t i = 0; i < rep.vertices.size(); ++i) {
			attributes[i * 3u + 0u] = rep.vertices[i].normal.x;
			attributes[i * 3u + 1u] = rep.vertices[i].normal.y;
			attributes[i * 3u + 2u] = rep.vertices[i].normal.z;
		}
		const float weights[3] = { 0.5f, 0.5f, 0.5f };
		const size_t targetIndices = static_cast<size_t>(triangleBudget) * 3u;
		std::vector<uint32_t> simplified(rep.indices.size());
		std::vector<uint32_t> attemptIndices(rep.indices.size());
		float resultError = 0.0f;
		float error = targetError;
		size_t count = 0u;
		const auto simplify = [&](float allowed, unsigned options, float& achieved) {
			return meshopt_simplifyWithAttributes(
				attemptIndices.data(), rep.indices.data(), rep.indices.size(),
				&rep.vertices[0].position.x, rep.vertices.size(), sizeof(Vertex),
				attributes.data(), sizeof(float) * 3u, weights, 3u, nullptr,
				targetIndices, allowed, meshopt_SimplifyErrorAbsolute | options, &achieved);
		};
		// Error-driven, up to the budget: each pass allows twice the error, pruning components below it. A pass that
		// prunes everything retries without pruning; one that still leaves nothing keeps the last non-empty result.
		for (uint32_t attempt = 0; attempt < 12u; ++attempt) {
			float achieved = 0.0f;
			size_t attemptCount = simplify(error, meshopt_SimplifyPrune, achieved);
			if (attemptCount == 0u) attemptCount = simplify(error, 0u, achieved);
			if (attemptCount == 0u) break;
			std::copy(attemptIndices.begin(), attemptIndices.begin() + attemptCount, simplified.begin());
			count = attemptCount;
			resultError = achieved;
			if (count <= targetIndices) break;
			error *= 2.0f;
		}
		if (count == 0u) {
			// Nothing survives even the level's own error: keep the union.
			std::copy(rep.indices.begin(), rep.indices.end(), simplified.begin());
			count = rep.indices.size();
			resultError = 0.0f;
		}
		if (count > targetIndices * 2u) {
			// Disjoint pieces that cannot collapse further: drop below the budget regardless of seams.
			float sloppyError = 0.0f;
			const size_t sloppyCount = meshopt_simplifySloppy(
				attemptIndices.data(), rep.indices.data(), rep.indices.size(),
				&rep.vertices[0].position.x, rep.vertices.size(), sizeof(Vertex), nullptr,
				targetIndices, (std::numeric_limits<float>::max)(), &sloppyError);
			// A sloppy pass that collapses everything keeps the over-budget result instead.
			if (sloppyCount > 0u) {
				usedSloppy = true;
				std::copy(attemptIndices.begin(), attemptIndices.begin() + sloppyCount, simplified.begin());
				count = sloppyCount;
				resultError = (std::max)(resultError, sloppyError);
			}
		}
		simplified.resize(count);

		// Compact the vertices the result uses.
		Representation out;
		out.sphere = rep.sphere;
		out.error = (std::max)(floorError, resultError);
		std::vector<uint32_t> remap(rep.vertices.size(), UINT32_MAX);
		out.indices.reserve(simplified.size());
		for (size_t t = 0; t + 2u < simplified.size(); t += 3u) {
			// A triangle the sloppy fallback made across materials takes its first corner's.
			const uint32_t slot = rep.vertices[simplified[t]].slot;
			for (uint32_t k = 0; k < 3u; ++k) {
				const uint32_t source = simplified[t + k];
				uint32_t& mapped = remap[source];
				if (mapped == UINT32_MAX || out.vertices[mapped].slot != slot) {
					Vertex v = rep.vertices[source];
					v.slot = slot;
					mapped = static_cast<uint32_t>(out.vertices.size());
					out.vertices.push_back(v);
				}
				out.indices.push_back(mapped);
			}
		}
		return out;
	}

	// Size-aware, spatial: each level merges the entities whose error is within errorGrowth of the smallest, in
	// spatial clusters of `fanout`. Large items wait until the level of their own error; single leftovers pass up.
	void BuildLevels(std::vector<Entity>& entities, std::vector<uint32_t>& active, const ClusterLODMergeSettings& settings,
		uint32_t& levelsOut, uint32_t& sloppyNodes)
	{
		uint32_t level = 0;
		while (active.size() > 1u && level < settings.maxLevels) {
			++level;
			std::sort(active.begin(), active.end(), [&](uint32_t a, uint32_t b) { return entities[a].rep.error < entities[b].rep.error; });
			const uint32_t fanout = (std::max)(settings.fanout, 2u);
			const float smallest = (std::max)(entities[active[0]].rep.error, 1e-6f);
			float threshold = smallest * settings.errorGrowth;
			// At least a full node's worth: near-zero errors would otherwise climb one doubling per level.
			threshold = (std::max)(threshold, entities[active[(std::min)(static_cast<size_t>(fanout), active.size()) - 1u]].rep.error);
			std::vector<uint32_t> candidates, waiting;
			for (const uint32_t e : active) (entities[e].rep.error <= threshold ? candidates : waiting).push_back(e);
			const bool lastLevel = level == settings.maxLevels || candidates.size() == active.size();

			std::vector<float> centers(candidates.size() * 3u);
			for (size_t i = 0; i < candidates.size(); ++i) {
				const XMFLOAT4& s = entities[candidates[i]].rep.sphere;
				centers[i * 3u + 0u] = s.x; centers[i * 3u + 1u] = s.y; centers[i * 3u + 2u] = s.z;
			}
			std::vector<uint32_t> order(candidates.size());
			meshopt_spatialClusterPoints(order.data(), centers.data(), candidates.size(), sizeof(float) * 3u, fanout);

			std::vector<uint32_t> next = waiting;
			uint32_t created = 0;
			for (size_t first = 0; first < order.size(); first += fanout) {
				const size_t end = (std::min)(first + fanout, order.size());
				if (end - first < 2u) {
					next.push_back(candidates[order[first]]);
					continue;
				}
				Entity node;
				node.leaf = false;
				node.level = level;
				for (size_t i = first; i < end; ++i) node.children.push_back(candidates[order[i]]);
				const bool root = lastLevel && waiting.empty() && order.size() <= fanout;
				const uint32_t budget = settings.nodeTriangleBudget * (root ? (std::max)(settings.rootMaxGroups, 1u) : 1u);
				bool sloppy = false;
				node.rep = BuildNodeRepresentation(entities, node.children, budget, settings, sloppy);
				sloppyNodes += sloppy ? 1u : 0u;
				const uint32_t index = static_cast<uint32_t>(entities.size());
				for (const uint32_t child : node.children) entities[child].parent = index;
				entities.push_back(std::move(node));
				next.push_back(index);
				++created;
			}
			if (created == 0u) {
				// Nothing within range had a neighbour: merge the two smallest so every level makes progress.
				Entity node;
				node.leaf = false;
				node.level = level;
				node.children = { active[0], active[1] };
				bool sloppy = false;
				node.rep = BuildNodeRepresentation(entities, node.children, settings.nodeTriangleBudget, settings, sloppy);
				sloppyNodes += sloppy ? 1u : 0u;
				const uint32_t index = static_cast<uint32_t>(entities.size());
				for (const uint32_t child : node.children) entities[child].parent = index;
				entities.push_back(std::move(node));
				next.assign(active.begin() + 2, active.end());
				next.push_back(index);
			}
			active = std::move(next);
		}
		if (active.size() > 1u) {
			// Out of levels: one root over what is left, at the root's budget.
			++level;
			Entity node;
			node.leaf = false;
			node.level = level;
			node.children = active;
			bool sloppy = false;
			node.rep = BuildNodeRepresentation(entities, node.children,
				settings.nodeTriangleBudget * (std::max)(settings.rootMaxGroups, 1u), settings, sloppy);
			sloppyNodes += sloppy ? 1u : 0u;
			const uint32_t index = static_cast<uint32_t>(entities.size());
			for (const uint32_t child : node.children) entities[child].parent = index;
			entities.push_back(std::move(node));
			active = { index };
		}
		levelsOut = level;
	}

	clodBounds MakeBounds(XMFLOAT4 sphere, float error)
	{
		clodBounds b{};
		b.center[0] = sphere.x; b.center[1] = sphere.y; b.center[2] = sphere.z;
		b.radius = sphere.w;
		b.error = error;
		return b;
	}
}

ClusterLODPrebuildArtifacts BuildClusterLODMergeHierarchy(
	std::span<const ClusterLODMergeGeometry> geometries,
	std::span<const ClusterLODMergeItem> items,
	const ClusterLODMergeSettings& settings,
	ClusterLODMergeStats* stats)
{
	if (items.size() < 2u) throw std::runtime_error("merge hierarchy: needs at least two items");
	ClusterLODMergeStats localStats{};
	localStats.items = static_cast<uint32_t>(items.size());

	std::vector<Entity> entities;
	entities.reserve(items.size() * 2u);
	std::vector<uint32_t> active;
	for (uint32_t i = 0; i < items.size(); ++i) {
		if (items[i].geometry >= geometries.size()) throw std::runtime_error("merge hierarchy: item geometry out of range");
		Entity leaf;
		leaf.item = i;
		leaf.rep = LeafRepresentation(geometries[items[i].geometry], items[i]);
		localStats.inputTriangles += leaf.rep.indices.size() / 3u;
		active.push_back(static_cast<uint32_t>(entities.size()));
		entities.push_back(std::move(leaf));
	}
	BuildLevels(entities, active, settings, localStats.levels, localStats.sloppyNodes);
	const uint32_t rootEntity = active.front();
	if (entities[rootEntity].leaf) throw std::runtime_error("merge hierarchy: no node was built");

	// ---- groups: nodes' clusters and children groups, items' portal proxies ----
	ClusterLODBuildState state{};
	std::vector<ClusterLODAssemblyTransform> transforms;
	std::vector<uint32_t> materialBases;
	std::vector<ClusterLODAssemblyInstance> instances;
	auto appendGroup = [&](const ClusterLODGroup& group, bool traversal) {
		const uint32_t index = static_cast<uint32_t>(state.groups.size());
		state.groups.push_back(group);
		state.groupChunks.emplace_back();
		state.groupPageBlobs.emplace_back();
		state.traversalGroupMask.push_back(traversal ? 1u : 0u);
		return index;
	};
	auto parentBounds = [&](const Entity& e) {
		if (e.parent == UINT32_MAX) return MakeBounds(e.rep.sphere, (std::numeric_limits<float>::max)());
		const Entity& p = entities[e.parent];
		return MakeBounds(p.rep.sphere, p.rep.error);
	};

	// Children groups first, so node clusters can name them.
	for (Entity& e : entities) {
		if (e.leaf) continue;
		ClusterLODGroup children{};
		children.bounds = MakeBounds(e.rep.sphere, e.rep.error);
		children.depth = static_cast<int32_t>(e.level);
		e.childrenGroup = appendGroup(children, false);
	}
	for (Entity& e : entities) {
		if (!e.leaf) continue;
		const ClusterLODMergeItem& item = items[e.item];
		const uint32_t transformIndex = static_cast<uint32_t>(transforms.size());
		transforms.push_back(ToAssemblyTransform(item.partToHierarchy));
		materialBases.push_back(item.materialBase);
		ClusterLODAssemblyInstance instance{};
		instance.targetRootNode = item.partNode;
		instance.transformIndex = transformIndex;
		instance.flags = CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL | item.flags;
		instance.stackDepth = item.part;
		ClusterLODGroup proxy{};
		proxy.bounds = parentBounds(e);
		proxy.depth = 0;
		proxy.flags = CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY;
		proxy.firstMeshlet = static_cast<uint32_t>(instances.size());
		instances.push_back(instance);
		e.proxyGroup = appendGroup(proxy, true);
	}

	// Node clusters: one group per node, clusters single-material and sorted by slot.
	std::vector<std::byte> vertexBytes;
	const uint32_t stride = MeshVertexLayout::VertexSize(kVertexFlags);
	uint32_t cumulativeMeshlets = 0u;
	uint32_t cumulativeGroupVertices = 0u;
	for (Entity& e : entities) {
		if (e.leaf) continue;
		localStats.nodes++;
		localStats.nodeTriangles += e.rep.indices.size() / 3u;
		const uint32_t vertexBase = static_cast<uint32_t>(vertexBytes.size() / stride);
		vertexBytes.resize(vertexBytes.size() + e.rep.vertices.size() * stride);
		for (size_t i = 0; i < e.rep.vertices.size(); ++i) {
			std::byte* dst = vertexBytes.data() + (static_cast<size_t>(vertexBase) + i) * stride;
			const Vertex& v = e.rep.vertices[i];
			std::memcpy(dst + MeshVertexLayout::PositionOffset, &v.position, sizeof(XMFLOAT3));
			std::memcpy(dst + MeshVertexLayout::NormalOffset, &v.normal, sizeof(XMFLOAT3));
			std::memcpy(dst + MeshVertexLayout::TexcoordOffset(kVertexFlags), &v.uv, sizeof(XMFLOAT2));
			std::memcpy(dst + MeshVertexLayout::ColorOffset(kVertexFlags), &v.color, sizeof(XMFLOAT3));
		}

		clod_detail::MergeCapturedGroup captured{};
		captured.depth = static_cast<int>(e.level);
		captured.simplified = parentBounds(e);
		captured.writeMaterialSlots = true;
		// Partition triangles by slot (each vertex has one), then cut each slot's triangles into meshlets.
		std::unordered_map<uint32_t, std::vector<uint32_t>> bySlot;
		for (size_t t = 0; t + 2u < e.rep.indices.size(); t += 3u) {
			auto& list = bySlot[e.rep.vertices[e.rep.indices[t]].slot];
			list.insert(list.end(), { e.rep.indices[t], e.rep.indices[t + 1u], e.rep.indices[t + 2u] });
		}
		std::vector<uint32_t> slots;
		for (const auto& [slot, _] : bySlot) slots.push_back(slot);
		std::sort(slots.begin(), slots.end());
		for (const uint32_t slot : slots) {
			const std::vector<uint32_t>& indices = bySlot[slot];
			const size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), MS_MESHLET_SIZE, MS_MESHLET_SIZE);
			std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
			std::vector<uint32_t> meshletVertices(maxMeshlets * MS_MESHLET_SIZE);
			std::vector<unsigned char> meshletTriangles(maxMeshlets * MS_MESHLET_SIZE * 3u);
			const size_t meshletCount = meshopt_buildMeshlets(meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
				indices.data(), indices.size(), &e.rep.vertices[0].position.x, e.rep.vertices.size(), sizeof(Vertex),
				MS_MESHLET_SIZE, MS_MESHLET_SIZE, 0.0f);
			for (size_t m = 0; m < meshletCount; ++m) {
				const meshopt_Meshlet& meshlet = meshlets[m];
				const meshopt_Bounds bounds = meshopt_computeMeshletBounds(&meshletVertices[meshlet.vertex_offset],
					&meshletTriangles[meshlet.triangle_offset], meshlet.triangle_count, &e.rep.vertices[0].position.x,
					e.rep.vertices.size(), sizeof(Vertex));
				clod_detail::MergeCapturedCluster cluster{};
				cluster.refinedGroup = static_cast<int32_t>(e.childrenGroup);
				cluster.bounds = MakeBounds(XMFLOAT4(bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius), e.rep.error);
				cluster.indicesOffset = static_cast<uint32_t>(captured.flattenedIndices.size());
				cluster.indexCount = meshlet.triangle_count * 3u;
				cluster.vertexCount = meshlet.vertex_count;
				cluster.materialSlot = slot;
				for (uint32_t t = 0; t < meshlet.triangle_count * 3u; ++t) {
					captured.flattenedIndices.push_back(vertexBase +
						meshletVertices[meshlet.vertex_offset + meshletTriangles[meshlet.triangle_offset + t]]);
				}
				captured.clusters.push_back(cluster);
			}
		}
		if (captured.clusters.empty()) throw std::runtime_error("merge hierarchy: a node simplified to nothing");
		const uint32_t groupIndex = static_cast<uint32_t>(state.groups.size());
		clod_detail::MergeGroupOutput built = clod_detail::BuildMergeGroupOutput(
			captured, groupIndex, vertexBytes, kVertexFlags, stride);
		ClusterLODGroup group = built.group;
		group.firstSegment = static_cast<uint32_t>(state.segments.size());
		group.firstMeshlet = cumulativeMeshlets;
		group.firstGroupVertex = cumulativeGroupVertices;
		group.flags = 0u;
		cumulativeMeshlets += group.meshletCount;
		cumulativeGroupVertices += group.groupVertexCount;
		state.segments.insert(state.segments.end(), built.segments.begin(), built.segments.end());
		state.segmentBounds.insert(state.segmentBounds.end(), built.segmentBounds.begin(), built.segmentBounds.end());
		e.ownGroup = appendGroup(group, true);
		state.groupPageBlobs[e.ownGroup] = std::move(built.pageBlobs);
		state.groupChunks[e.ownGroup] = built.groupChunk;
	}
	localStats.rootTriangles = static_cast<uint32_t>(entities[rootEntity].rep.indices.size() / 3u);
	localStats.rootError = entities[rootEntity].rep.error;

	clod_detail::BuildClusterLODTraversalHierarchy(state, settings.traversalNodeWidth);
	// The traversal hierarchy derives parents from refinement only. A child (node group or proxy) is reached through
	// its parent's children group, so name the parent's own group: a proxy without a parent counts as structural and
	// always opens, and a missing parent opens its children.
	for (const Entity& e : entities) {
		if (e.parent == UINT32_MAX) continue;
		const uint32_t childGroup = e.leaf ? e.proxyGroup : e.ownGroup;
		state.groups[childGroup].parentGroupId = static_cast<int32_t>(entities[e.parent].ownGroup);
		state.groups[childGroup].flags |= CLOD_GROUP_FLAG_MERGE_CHILD;
		state.groups[childGroup].maxParentError = entities[e.parent].rep.error;
	}

	uint32_t partDepth = 0u;
	for (const ClusterLODMergeItem& item : items) partDepth = (std::max)(partDepth, item.partTraversalDepth);
	state.maxTraversalDepth = clod_detail::ComputeCLodTraversalDepth(state.nodes, state.topRootNode) + partDepth;

	std::vector<ClusterLODNodeSkinningInfo> nodeSkinningInfos;
	std::vector<uint32_t> nodeBoneIndices;
	clod_detail::BuildNodeSkinningSidecar(state, CLOD_NODE_BONE_LIMIT_DEFAULT, nodeSkinningInfos, nodeBoneIndices);

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences, groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u, voxelPageBase = 0u, voxelPageCount = 0u;
	clod_detail::FinalizeMeshWidePagePacking(state, meshPageBlobs, groupPageReferences, groupPageReferenceOffsets,
		trianglePageCount, voxelPageBase, voxelPageCount);

	ClusterLODPrebuildArtifacts out{};
	auto& data = out.prebuiltData;
	data.groups = std::move(state.groups);
	data.segments = std::move(state.segments);
	data.segmentBounds = std::move(state.segmentBounds);
	data.objectBoundingSphere = clod_detail::BuildObjectBoundingSphereFromRootNode(state.nodes, state.topRootNode);
	data.groupChunks = std::move(state.groupChunks);
	data.groupPageReferences = std::move(groupPageReferences);
	data.groupPageReferenceOffsets = std::move(groupPageReferenceOffsets);
	data.trianglePageCount = trianglePageCount;
	data.voxelPageBase = voxelPageBase;
	data.voxelPageCount = voxelPageCount;
	data.nodes = std::move(state.nodes);
	data.nodeSkinningInfos = std::move(nodeSkinningInfos);
	data.nodeBoneIndices = std::move(nodeBoneIndices);
	data.nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
	data.lodNodeRanges = std::move(state.lodNodeRanges);
	data.lodLevelRoots = std::move(state.lodLevelRoots);
	data.assemblyTransforms = std::move(transforms);
	data.assemblyTransformMaterialBases = std::move(materialBases);
	data.assemblyInstances = std::move(instances);
	data.maxDepth = state.maxDepth;
	data.maxTraversalDepth = state.maxTraversalDepth;
	data.materialSlotCount = (std::max)(settings.materialSlotCount, 1u);
	clod_detail::AssignSingleRootPartRecord(data, state.topRootNode);
	out.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	out.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);
	std::string representationError;
	if (!ValidateClusterLODPageRepresentations(data, &out.cacheBuildData.meshPageBlobs, &representationError)) {
		throw std::runtime_error("merge hierarchy page validation failed: " + representationError);
	}
	if (stats) *stats = localStats;
	return out;
}
