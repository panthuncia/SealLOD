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
//
// With voxel items (forked parts) the levels are built twice into one root, a triangle pass and a voxel pass, whose
// groups are tagged CLOD_GROUP_FLAG_BRANCH_TRIANGLE and _VOXEL. In the voxel pass a voxel item has no triangles: its
// part's voxel source is voxelized, together with its voxel siblings', by the node above it, into voxel groups (one per
// material slot) that refine into P_N like the node's triangle group.

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
		bool folded = false;    // a node holding its children's exact triangles: no children group, no portals below
		bool absorbed = false;  // not built: a folded node inside a folded node, or a node dissolved into its parent
		uint32_t rejected = 0;  // groupings of it that would have been over maxNodeError
		// Groups, once built.
		uint32_t ownGroup = UINT32_MAX;      // G_N (nodes): the triangle group, or the first voxel group
		uint32_t childrenGroup = UINT32_MAX; // P_N (nodes)
		uint32_t proxyGroup = UINT32_MAX;    // the portal proxy (leaves)
		// Voxel pass.
		bool voxel = false;          // a leaf drawn as voxels; its part's voxel source is voxelSource
		Representation voxelSource;  // the source triangles of every voxel item below (leaves: the item's own)
		std::vector<std::pair<uint32_t, clod_detail::MergeVoxelization>> voxels; // a node's voxels, by palette slot
		uint32_t ownGroupCount = 0;  // G_N and its voxel groups, consecutive from ownGroup
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
		const float maxError = settings.maxNodeError;
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
		// Exact children that fit together: the union is exact, so the node stands for them at every distance.
		if (childError <= 0.0f && rep.indices.size() <= static_cast<size_t>(triangleBudget) * 3u) {
			rep.error = 0.0f;
			return rep;
		}
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
		if (std::isfinite(maxError) && (std::max)(floorError, resultError) > maxError && floorError <= maxError) {
			// The budget would take the node over the cap, where it could not stand for its children at all. Spend
			// triangles instead: the fewest within the cap, and at worst the union, which is what the children draw when
			// open at that cut anyway; but no more than capBudgetFactor budgets, past which the node saves instances
			// and nothing else, so it is rejected and its children stay as they are.
			float achieved = 0.0f;
			const float allowed = (std::max)(maxError * 0.999f - 1e-7f, floorError);
			size_t capped = meshopt_simplifyWithAttributes(
				attemptIndices.data(), rep.indices.data(), rep.indices.size(),
				&rep.vertices[0].position.x, rep.vertices.size(), sizeof(Vertex),
				attributes.data(), sizeof(float) * 3u, weights, 3u, nullptr,
				0u, allowed, meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune, &achieved);
			if (capped == 0u || (std::max)(floorError, achieved) > maxError) {
				std::copy(rep.indices.begin(), rep.indices.end(), attemptIndices.begin());
				capped = rep.indices.size();
				achieved = 0.0f;
			}
			if (capped <= static_cast<size_t>(triangleBudget) * 3u * (std::max)(settings.capBudgetFactor, 1u)) {
				std::copy(attemptIndices.begin(), attemptIndices.begin() + capped, simplified.begin());
				count = capped;
				resultError = achieved;
				usedSloppy = false;
			}
		}
		simplified.resize(count);

		// Compact the vertices the result uses.
		Representation out;
		out.sphere = rep.sphere;
		// Exact children simplified without loss (the budget pass removed nothing) are still exact: the node folds.
		out.error = childError <= 0.0f && resultError <= 0.0f && count == rep.indices.size() ? 0.0f : (std::max)(floorError, resultError);
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

	std::vector<std::byte> VoxelSourceBytes(const Representation& source, const std::vector<uint32_t>& indices)
	{
		constexpr uint32_t stride = MeshVertexLayout::VertexSize(kVertexFlags);
		std::vector<std::byte> bytes(source.vertices.size() * stride);
		for (size_t i = 0; i < source.vertices.size(); ++i) {
			std::byte* dst = bytes.data() + i * stride;
			const Vertex& v = source.vertices[i];
			std::memcpy(dst + MeshVertexLayout::PositionOffset, &v.position, sizeof(XMFLOAT3));
			std::memcpy(dst + MeshVertexLayout::NormalOffset, &v.normal, sizeof(XMFLOAT3));
			std::memcpy(dst + MeshVertexLayout::TexcoordOffset(kVertexFlags), &v.uv, sizeof(XMFLOAT2));
			std::memcpy(dst + MeshVertexLayout::ColorOffset(kVertexFlags), &v.color, sizeof(XMFLOAT3));
		}
		(void)indices;
		return bytes;
	}

	// A voxel-pass node over voxel children: their sources merged and voxelized per material slot, at the coarsest width
	// above the children's errors that keeps the node within its cube budget. False if that would be over the cap.
	bool VoxelizeNode(const std::vector<Entity>& entities, Entity& node, const ClusterLODMergeSettings& settings)
	{
		float childError = 0.0f;
		for (const uint32_t child : node.children) {
			const Entity& c = entities[child];
			childError = (std::max)(childError, c.rep.error);
			if (c.voxelSource.indices.empty()) continue;
			const uint32_t base = static_cast<uint32_t>(node.voxelSource.vertices.size());
			node.voxelSource.vertices.insert(node.voxelSource.vertices.end(), c.voxelSource.vertices.begin(), c.voxelSource.vertices.end());
			for (const uint32_t index : c.voxelSource.indices) node.voxelSource.indices.push_back(base + index);
		}
		if (node.voxelSource.indices.empty()) return true;
		node.voxelSource.sphere = SphereOfVertices(node.voxelSource.vertices);
		node.rep.sphere = MergeSpheres(node.rep.sphere, node.voxelSource.sphere);

		std::unordered_map<uint32_t, std::vector<uint32_t>> bySlot;
		for (size_t t = 0; t + 2u < node.voxelSource.indices.size(); t += 3u) {
			auto& list = bySlot[node.voxelSource.vertices[node.voxelSource.indices[t]].slot];
			list.insert(list.end(), { node.voxelSource.indices[t], node.voxelSource.indices[t + 1u], node.voxelSource.indices[t + 2u] });
		}
		const std::vector<std::byte> bytes = VoxelSourceBytes(node.voxelSource, node.voxelSource.indices);
		const uint32_t stride = MeshVertexLayout::VertexSize(kVertexFlags);
		const float floorError = childError * 1.0001f + 1e-7f;
		float width = (std::max)({ floorError, childError * settings.errorGrowth, node.voxelSource.sphere.w * 1e-4f });
		const float cap = settings.maxNodeError;
		for (uint32_t attempt = 0; attempt < 12u; ++attempt) {
			if (std::isfinite(cap) && width > cap) break;
			std::vector<std::pair<uint32_t, clod_detail::MergeVoxelization>> voxels;
			uint32_t cubes = 0;
			bool ok = true;
			for (const auto& [slot, indices] : bySlot) {
				clod_detail::MergeVoxelization voxelization;
				if (!clod_detail::VoxelizeMergeTriangles(bytes, stride, indices, width, settings.voxelRaysPerCell, voxelization)) {
					// Nothing covered at this width (sub-cell geometry) is fine; a grid too large is not.
					if (voxelization.cubes == 0u && indices.size() > 0u) {
						const float extent = node.voxelSource.sphere.w * 2.0f + width;
						ok = extent / width <= 4096.0f;
						if (!ok) break;
					}
					continue;
				}
				cubes += voxelization.cubes;
				voxels.emplace_back(slot, std::move(voxelization));
			}
			if (ok && !voxels.empty() && cubes <= (std::max)(settings.nodeVoxelCubeBudget, 1u)) {
				std::sort(voxels.begin(), voxels.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				node.voxels = std::move(voxels);
				node.rep.error = (std::max)(node.rep.error, width);
				return true;
			}
			width *= 1.5f;
		}
		return false;
	}

	// Size-aware, spatial: each level merges the entities whose error is within errorGrowth of the smallest, in
	// spatial clusters of `fanout`. Large items wait until the level of their own error; single leftovers pass up.
	// Nothing merges over maxNodeError: what cannot merge under it is left at the top, as `active`.
	void BuildLevels(std::vector<Entity>& entities, std::vector<uint32_t>& active, const ClusterLODMergeSettings& settings,
		uint32_t& levelsOut, uint32_t& sloppyNodes, uint32_t& rejectedNodes, uint32_t& dissolvedNodes)
	{
		const float cap = settings.maxNodeError;
		const bool capped = std::isfinite(cap);
		const uint32_t fanout = (std::max)(settings.fanout, 2u);
		// Retried groupings change as levels widen, so an entity gets two chances before it is left at the top.
		constexpr uint32_t kMaxRejections = 2u;
		std::vector<uint32_t> top;
		uint32_t level = 0;
		const auto mergeable = [&](uint32_t e) {
			return entities[e].rejected < kMaxRejections && (!capped || entities[e].rep.error * 1.0001f + 1e-7f <= cap);
		};
		const auto makeNode = [&](std::vector<uint32_t> children, uint32_t budget) -> uint32_t {
			float childError = 0.0f;
			for (const uint32_t child : children) childError = (std::max)(childError, entities[child].rep.error);
			const auto reject = [&]() {
				++rejectedNodes;
				for (const uint32_t child : children) ++entities[child].rejected;
				return UINT32_MAX;
			};
			if (capped && childError * 1.0001f + 1e-7f > cap) return reject();
			Entity node;
			node.leaf = false;
			node.level = level;
			node.children = std::move(children);
			bool sloppy = false;
			node.rep = BuildNodeRepresentation(entities, node.children, budget, settings, sloppy);
			if (!VoxelizeNode(entities, node, settings)) {
				children = std::move(node.children);
				return reject();
			}
			if (capped && node.rep.error > cap) {
				children = std::move(node.children);
				return reject();
			}
			sloppyNodes += sloppy ? 1u : 0u;
			// Children that would draw over too narrow a range of cut dissolve into this node, recursively.
			{
				std::vector<uint32_t> kept, pending = node.children;
				while (!pending.empty()) {
					const uint32_t child = pending.back();
					pending.pop_back();
					Entity& c = entities[child];
					if (!c.leaf && !c.folded && c.rep.error > 0.0f && node.rep.error < c.rep.error * settings.minNodeDrawWindow) {
						c.absorbed = true;
						++dissolvedNodes;
						pending.insert(pending.end(), c.children.begin(), c.children.end());
						continue;
					}
					kept.push_back(child);
				}
				node.children = std::move(kept);
			}
			bool exactChildren = true;
			for (const uint32_t child : node.children) {
				const Entity& c = entities[child];
				exactChildren &= c.leaf ? c.rep.error <= 0.0f : c.folded;
			}
			node.folded = exactChildren && node.rep.error <= 0.0f;
			if (node.folded) {
				for (const uint32_t child : node.children) entities[child].absorbed = !entities[child].leaf;
			}
			const uint32_t index = static_cast<uint32_t>(entities.size());
			for (const uint32_t child : node.children) entities[child].parent = index;
			entities.push_back(std::move(node));
			return index;
		};

		while (level < settings.maxLevels) {
			std::erase_if(active, [&](uint32_t e) {
				if (mergeable(e)) return false;
				top.push_back(e);
				return true;
			});
			if (active.size() < 2u) break;
			++level;
			std::sort(active.begin(), active.end(), [&](uint32_t a, uint32_t b) { return entities[a].rep.error < entities[b].rep.error; });
			const float smallest = (std::max)(entities[active[0]].rep.error, 1e-6f);
			float threshold = smallest * settings.errorGrowth;
			// At least a full node's worth, so near-zero errors do not climb one doubling per level; but no further than
			// the sibling spread allows, so small items merge among themselves before they meet large ones.
			const float fullNode = entities[active[(std::min)(static_cast<size_t>(fanout), active.size()) - 1u]].rep.error;
			threshold = (std::max)(threshold, (std::min)(fullNode, smallest * std::pow(settings.errorGrowth, settings.maxSiblingSpreadLevels)));
			// And wide enough to reach the top (the cap, or the largest error) within the levels left: a big region
			// banded at errorGrowth runs out of levels and leaves what has not merged at the top.
			{
				const float target = capped ? cap : entities[active.back()].rep.error;
				const uint32_t remaining = settings.maxLevels - level + 1u;
				if (target > smallest && remaining > 0u) {
					threshold = (std::max)(threshold, smallest * std::pow(target / smallest, 1.0f / static_cast<float>(remaining)));
				}
			}
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
				std::vector<uint32_t> children;
				for (size_t i = first; i < end; ++i) children.push_back(candidates[order[i]]);
				const bool root = lastLevel && waiting.empty() && top.empty() && order.size() <= fanout;
				const uint32_t budget = settings.nodeTriangleBudget * (root ? (std::max)(settings.rootMaxGroups, 1u) : 1u);
				const uint32_t node = makeNode(children, budget);
				if (node == UINT32_MAX) {
					next.insert(next.end(), children.begin(), children.end());
					continue;
				}
				next.push_back(node);
				++created;
			}
			if (created == 0u) {
				// Nothing within range had a neighbour: merge the two smallest so every level makes progress. A rejection
				// counts against them, so this ends.
				const uint32_t node = makeNode({ active[0], active[1] }, settings.nodeTriangleBudget);
				if (node != UINT32_MAX) {
					next.assign(active.begin() + 2, active.end());
					next.push_back(node);
				}
				else {
					next = active;
				}
			}
			active = std::move(next);
		}
		if (!capped && active.size() > 1u && top.empty()) {
			// Out of levels: one root over what is left, at the root's budget.
			++level;
			const uint32_t node = makeNode(active, settings.nodeTriangleBudget * (std::max)(settings.rootMaxGroups, 1u));
			if (node != UINT32_MAX) active = { node };
		}
		active.insert(active.end(), top.begin(), top.end());
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
	for (const ClusterLODMergeItem& item : items) {
		if (item.geometry >= geometries.size()) throw std::runtime_error("merge hierarchy: item geometry out of range");
		if (item.voxelGeometry != UINT32_MAX && item.voxelGeometry >= geometries.size()) {
			throw std::runtime_error("merge hierarchy: item voxel geometry out of range");
		}
		localStats.voxelItems += item.voxelGeometry != UINT32_MAX ? 1u : 0u;
	}

	// One pass per branch: the triangle pass alone without voxel items (its groups untagged).
	struct Pass
	{
		bool voxel = false;
		uint32_t branchFlags = 0u;
		std::vector<Entity> entities;
		uint32_t levels = 0u;
		bool anyNode = false;
	};
	std::vector<Pass> passes(localStats.voxelItems != 0u ? 2u : 1u);
	passes[0].branchFlags = passes.size() > 1u ? CLOD_GROUP_FLAG_BRANCH_TRIANGLE : 0u;
	if (passes.size() > 1u) {
		passes[1].voxel = true;
		passes[1].branchFlags = CLOD_GROUP_FLAG_BRANCH_VOXEL;
	}
	for (Pass& pass : passes) {
		pass.entities.reserve(items.size() * 2u);
		std::vector<uint32_t> active;
		for (uint32_t i = 0; i < items.size(); ++i) {
			const ClusterLODMergeItem& item = items[i];
			Entity leaf;
			leaf.item = i;
			if (pass.voxel && item.voxelGeometry != UINT32_MAX) {
				// No triangles: the node above voxelizes the part's voxel source with its voxel siblings'.
				leaf.voxel = true;
				leaf.voxelSource = LeafRepresentation(geometries[item.voxelGeometry], item);
				leaf.rep.error = (std::max)(item.voxelError, 0.0f) * MaxScale(item.partToHierarchy);
				leaf.rep.sphere = leaf.voxelSource.sphere;
				if (!settings.voxelNodes) leaf.rejected = UINT32_MAX; // stays a portal
			} else {
				leaf.rep = LeafRepresentation(geometries[item.geometry], item);
				if (!pass.voxel) localStats.inputTriangles += leaf.rep.indices.size() / 3u;
			}
			active.push_back(static_cast<uint32_t>(pass.entities.size()));
			pass.entities.push_back(std::move(leaf));
		}
		uint32_t sloppy = 0u, rejected = 0u, dissolved = 0u;
		BuildLevels(pass.entities, active, settings, pass.levels, sloppy, rejected, dissolved);
		if (!pass.voxel) {
			localStats.sloppyNodes = sloppy;
			localStats.rejectedNodes = rejected;
			localStats.dissolvedNodes = dissolved;
		}
		pass.anyNode = std::ranges::any_of(pass.entities, [](const Entity& e) { return !e.leaf; });
	}
	if (!std::ranges::any_of(passes, [](const Pass& pass) { return pass.anyNode; })) {
		// Nothing merges under the cap: the items are better left as they are.
		if (stats) *stats = localStats;
		return {};
	}

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
	std::vector<std::byte> vertexBytes;
	const uint32_t stride = MeshVertexLayout::VertexSize(kVertexFlags);
	uint32_t cumulativeMeshlets = 0u;
	uint32_t cumulativeGroupVertices = 0u;

	for (Pass& pass : passes) {
		std::vector<Entity>& entities = pass.entities;
		// An item inside a folded node has no portal: the node's exact triangles stand for it at every distance.
		const auto hasPortal = [&](const Entity& e) {
			return e.leaf && (e.parent == UINT32_MAX || !entities[e.parent].folded);
		};
		// Levels where every grouping was rejected built nothing; group depths must be contiguous for the traversal
		// hierarchy, so number the levels that hold nodes densely, from 1 over portal proxies (depth 0) or from 0 without.
		{
			std::vector<uint32_t> levels;
			for (const Entity& e : entities) {
				if (!e.leaf && !e.absorbed) levels.push_back(e.level);
			}
			std::sort(levels.begin(), levels.end());
			levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
			const bool anyPortal = std::ranges::any_of(entities, hasPortal);
			for (Entity& e : entities) {
				if (!e.leaf && !e.absorbed) {
					e.level = static_cast<uint32_t>(std::lower_bound(levels.begin(), levels.end(), e.level) - levels.begin()) + (anyPortal ? 1u : 0u);
				}
			}
			if (!pass.voxel) localStats.levels = static_cast<uint32_t>(levels.size());
		}
		auto parentBounds = [&](const Entity& e) {
			if (e.parent == UINT32_MAX) return MakeBounds(e.rep.sphere, (std::numeric_limits<float>::max)());
			const Entity& p = entities[e.parent];
			return MakeBounds(p.rep.sphere, p.rep.error);
		};

		// Children groups first, so node clusters can name them. A folded node has none: its clusters are terminal.
		for (Entity& e : entities) {
			if (e.leaf || e.folded || e.absorbed) continue;
			ClusterLODGroup children{};
			children.bounds = MakeBounds(e.rep.sphere, e.rep.error);
			children.depth = static_cast<int32_t>(e.level);
			children.flags = pass.branchFlags;
			e.childrenGroup = appendGroup(children, false);
		}
		for (Entity& e : entities) {
			if (!hasPortal(e)) {
				if (!pass.voxel) localStats.foldedItems += e.leaf ? 1u : 0u;
				continue;
			}
			if (e.parent == UINT32_MAX) {
				(pass.voxel ? localStats.voxelBranchTopPortals : localStats.topPortals) += 1u;
			}
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
			proxy.flags = CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY | pass.branchFlags;
			proxy.firstMeshlet = static_cast<uint32_t>(instances.size());
			instances.push_back(instance);
			e.proxyGroup = appendGroup(proxy, true);
		}

		// Node clusters: a triangle group per node, clusters single-material and sorted by slot, then its voxel groups.
		for (Entity& e : entities) {
			if (e.leaf || e.absorbed) continue;
			if (pass.voxel) {
				localStats.voxelBranchNodes++;
				localStats.voxelBranchNodeTriangles += e.rep.indices.size() / 3u;
				localStats.voxelNodes += e.voxels.empty() ? 0u : 1u;
			} else {
				localStats.nodes++;
				localStats.foldedNodes += e.folded ? 1u : 0u;
				localStats.topNodes += e.parent == UINT32_MAX ? 1u : 0u;
				localStats.nodeTriangles += e.rep.indices.size() / 3u;
			}
			if (!e.rep.indices.empty()) {
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
						cluster.refinedGroup = e.folded ? -1 : static_cast<int32_t>(e.childrenGroup);
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
				group.flags = pass.branchFlags;
				cumulativeMeshlets += group.meshletCount;
				cumulativeGroupVertices += group.groupVertexCount;
				state.segments.insert(state.segments.end(), built.segments.begin(), built.segments.end());
				state.segmentBounds.insert(state.segmentBounds.end(), built.segmentBounds.begin(), built.segmentBounds.end());
				e.ownGroup = appendGroup(group, true);
				e.ownGroupCount = 1u;
				state.groupPageBlobs[e.ownGroup] = std::move(built.pageBlobs);
				state.groupChunks[e.ownGroup] = built.groupChunk;
			}
			for (const auto& [slot, voxelization] : e.voxels) {
				const uint32_t groupIndex = static_cast<uint32_t>(state.groups.size());
				clod_detail::MergeGroupOutput built =
					clod_detail::AppendMergeVoxelGroup(state, voxelization, groupIndex, static_cast<int32_t>(e.childrenGroup));
				ClusterLODGroup group = built.group;
				group.bounds = parentBounds(e);
				group.depth = static_cast<int32_t>(e.level);
				group.firstSegment = static_cast<uint32_t>(state.segments.size());
				group.flags |= pass.branchFlags | (slot << CLOD_GROUP_VOXEL_MATERIAL_SLOT_SHIFT);
				localStats.voxelCubes += voxelization.cubes;
				state.segments.insert(state.segments.end(), built.segments.begin(), built.segments.end());
				state.segmentBounds.insert(state.segmentBounds.end(), built.segmentBounds.begin(), built.segmentBounds.end());
				const uint32_t index = appendGroup(group, true);
				state.groupPageBlobs[index] = std::move(built.pageBlobs);
				if (e.ownGroup == UINT32_MAX) e.ownGroup = index;
				++e.ownGroupCount;
			}
			if (e.ownGroup == UINT32_MAX) throw std::runtime_error("merge hierarchy: a node has neither triangles nor voxels");
		}
		// The top: every node with no parent (one root, uncapped).
		if (!pass.voxel) {
			for (const Entity& e : entities) {
				if (e.leaf || e.absorbed || e.parent != UINT32_MAX) continue;
				localStats.rootTriangles += static_cast<uint32_t>(e.rep.indices.size() / 3u);
				localStats.rootError = (std::max)(localStats.rootError, e.rep.error);
			}
		}
	}

	state.voxelGroupMapping.groupToPayloadIndex.resize(state.groups.size(), -1);
	state.voxelGroupMapping.groupToPackedMetadataIndex.resize(state.groups.size(), -1);
	clod_detail::BuildClusterLODTraversalHierarchy(state, settings.traversalNodeWidth);
	// The traversal hierarchy derives parents from refinement only. A child (node group or proxy) is reached through
	// its parent's children group, so name the parent's own group: a proxy without a parent counts as structural and
	// always opens, and a missing parent opens its children.
	for (const Pass& pass : passes) {
		const std::vector<Entity>& entities = pass.entities;
		const auto hasPortal = [&](const Entity& e) {
			return e.leaf && (e.parent == UINT32_MAX || !entities[e.parent].folded);
		};
		for (const Entity& e : entities) {
			if (e.parent == UINT32_MAX || e.absorbed || (e.leaf && !hasPortal(e))) continue;
			const Entity& parent = entities[e.parent];
			std::vector<uint32_t> childGroups;
			if (e.leaf) {
				childGroups.push_back(e.proxyGroup);
			} else {
				// A node's own groups: its triangle group and each of its voxel groups.
				for (uint32_t g = e.ownGroup; g < e.ownGroup + e.ownGroupCount; ++g) childGroups.push_back(g);
			}
			for (const uint32_t childGroup : childGroups) {
				state.groups[childGroup].parentGroupId = static_cast<int32_t>(parent.ownGroup);
				state.groups[childGroup].flags |= CLOD_GROUP_FLAG_MERGE_CHILD;
				state.groups[childGroup].maxParentError = parent.rep.error;
			}
		}
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
	out.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	out.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);
	std::string representationError;
	if (!ValidateClusterLODPageRepresentations(data, &out.cacheBuildData.meshPageBlobs, &representationError)) {
		throw std::runtime_error("merge hierarchy page validation failed: " + representationError);
	}
	if (stats) *stats = localStats;
	return out;
}
