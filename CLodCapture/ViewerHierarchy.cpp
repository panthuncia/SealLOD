#include "ViewerHierarchy.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <unordered_map>

#include <BasicRenderer/Assets/CLodPageDecoder.h>

namespace clodcapture {
	namespace {
		using clodeval::IsMergeRoot;
		bool IsVoxel(const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u; }
		bool HasMeasuredError(const ClusterLODGroup& group) { return std::isfinite(group.bounds.error) && group.bounds.error > 0.0f && group.bounds.error < 1.0e19f; }

		std::string Identity(const std::string& source, const std::string& prim, const std::string& subset)
		{
			return source + "|" + prim + "|" + subset;
		}

		void MeasureVoxelGroup(const clodeval::CachedMesh& mesh, uint32_t groupIndex, GroupInfo& info)
		{
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
			const ClusterLODGroup& group = prebuilt.groups[groupIndex];
			for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
				const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
				uint32_t meshPage = 0;
				CLodCache::VoxelPage page;
				if (segment.meshletCount == 0u || !CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage) || !CLodCache::VoxelPage::Parse(mesh.Page(meshPage), page)) {
					continue;
				}
				for (uint32_t cluster = segment.firstMeshletInPage; cluster < segment.firstMeshletInPage + segment.meshletCount && cluster < page.ClusterCount(); ++cluster) {
					const CLodVoxelClusterRecord record = page.Cluster(cluster);
					info.voxelWidth = std::max(info.voxelWidth, record.aabbMinAndVoxelWidth.w);
					info.cubes += record.cubeCount;
					for (uint32_t cube = record.firstCube; cube < record.firstCube + record.cubeCount && cube < page.CubeCount(); ++cube) {
						info.cells += static_cast<uint32_t>(std::popcount(page.Cube(cube).occupancyMask));
					}
				}
			}
		}

		void BuildMesh(MeshInfo& info)
		{
			const clodeval::CachedMesh& mesh = *info.cache;
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
			const size_t groupCount = prebuilt.groups.size();
			info.name = mesh.Name();
			info.assembly = prebuilt.cacheSource.sourceIdentifier.find("usd_clod_asset_assembly") != std::string::npos;
			if (info.assembly) {
				// Assemblies are told apart only by the skin and material domains in their identifier.
				const auto hashAfter = [&](std::string_view key) {
					const std::string& identifier = prebuilt.cacheSource.sourceIdentifier;
					const size_t at = identifier.find(key);
					return at != std::string::npos ? identifier.substr(at + key.size(), 6u) : std::string("?");
				};
				info.shortName = std::format("assembly skin {} material {}", hashAfter("skin_domain="), hashAfter("material_domain="));
			}
			else {
				std::string_view prim = prebuilt.cacheSource.primPath;
				prim.remove_prefix(prim.find_last_of('/') == std::string_view::npos ? 0u : prim.find_last_of('/') + 1u);
				std::string_view subset = prebuilt.cacheSource.subsetName;
				subset = subset.substr(0u, subset.find('|'));
				info.shortName = subset.empty() || subset.starts_with("__") ? std::string(prim) : std::format("{} {}", prim, subset);
			}
			info.groups.assign(groupCount, GroupInfo{});
			if (IsMergeRoot(prebuilt)) {
				std::unique_ptr<clodeval::MergeHierarchy> merge = clodeval::ReadMergeHierarchy(mesh, true);
				info.mergeReport = clodeval::FormatMergeReport(*merge, mesh);
				info.mergeLevels = static_cast<int32_t>(merge->levels);
				info.shortName = std::format("merge root {} ({} nodes, {} portals)", prebuilt.cacheSource.sourceIdentifier, merge->nodes.size(), merge->portals.size());
				for (uint32_t n = 0; n < merge->nodes.size(); ++n) {
					info.groups[merge->nodes[n].group].mergeNode = static_cast<int32_t>(n);
					if (!merge->nodes[n].folded) {
						info.groups[merge->nodes[n].childrenGroup].mergeNode = static_cast<int32_t>(n);
						info.groups[merge->nodes[n].childrenGroup].mergeChildren = true;
					}
				}
				for (uint32_t p = 0; p < merge->portals.size(); ++p) {
					info.groups[merge->portals[p].group].mergePortal = static_cast<int32_t>(p);
				}
				info.merge = std::move(merge);
			}

			std::vector<std::vector<uint32_t>> children(groupCount), parents(groupCount);
			for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				GroupInfo& out = info.groups[groupIndex];
				out.depth = group.depth;
				out.voxel = IsVoxel(group);
				out.assemblyVoxel = (group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u;
				out.leaveError = HasMeasuredError(group) ? group.bounds.error : kNoError;
				float enter = kNoError;
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
					if (segment.refinedGroup < 0 || static_cast<size_t>(segment.refinedGroup) >= groupCount) {
						out.terminal = true;
						enter = 0.0f;
						if (!out.voxel) {
							mesh.VisitSegmentMeshlets(groupIndex, segment, [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) { info.leafTriangles += meshlet.triangleCount; });
						}
						continue;
					}
					const auto refined = static_cast<uint32_t>(segment.refinedGroup);
					if (std::ranges::find(children[groupIndex], refined) == children[groupIndex].end()) {
						children[groupIndex].push_back(refined);
						parents[refined].push_back(groupIndex);
					}
					const ClusterLODGroup& child = prebuilt.groups[refined];
					enter = std::min(enter, HasMeasuredError(child) ? child.bounds.error : 0.0f);
				}
				out.enterError = std::isfinite(enter) ? enter : 0.0f;
				if (out.voxel) {
					MeasureVoxelGroup(mesh, groupIndex, out);
					++info.voxelGroups;
				}
				else {
					for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
						mesh.VisitSegmentMeshlets(groupIndex, prebuilt.segments[segmentIndex], [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) { out.triangles += meshlet.triangleCount; });
					}
					++info.triangleGroups;
				}
				info.maxDepth = std::max(info.maxDepth, out.depth);
			}

			if (info.merge) {
				// A node's children are its child nodes' geometry and its portals, reached through its children group.
				for (auto& list : children) list.clear();
				for (auto& list : parents) list.clear();
				const clodeval::MergeHierarchy& merge = *info.merge;
				for (const clodeval::MergeNode& node : merge.nodes) {
					for (const uint32_t c : node.childNodes) {
						children[node.group].push_back(merge.nodes[c].group);
						parents[merge.nodes[c].group].push_back(node.group);
					}
					for (const uint32_t p : node.childPortals) {
						children[node.group].push_back(merge.portals[p].group);
						parents[merge.portals[p].group].push_back(node.group);
					}
				}
			}
			for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
				GroupInfo& out = info.groups[groupIndex];
				out.firstChild = static_cast<uint32_t>(info.links.size());
				out.childCount = static_cast<uint32_t>(children[groupIndex].size());
				info.links.insert(info.links.end(), children[groupIndex].begin(), children[groupIndex].end());
				out.firstParent = static_cast<uint32_t>(info.links.size());
				out.parentCount = static_cast<uint32_t>(parents[groupIndex].size());
				info.links.insert(info.links.end(), parents[groupIndex].begin(), parents[groupIndex].end());
			}

			// Walks children to find where each voxel chain starts. Children always have a smaller depth, or
			// are at least visited once thanks to the state array, so a malformed cycle cannot recurse for ever.
			std::vector<uint8_t> voxelState(groupCount, 0u);
			const auto visitVoxel = [&](auto&& self, uint32_t groupIndex) -> void {
				if (voxelState[groupIndex] != 0u) {
					return;
				}
				voxelState[groupIndex] = 1u;
				GroupInfo& out = info.groups[groupIndex];
				if (!out.voxel) {
					return;
				}
				int32_t level = -1;
				float boundary = 0.0f;
				uint32_t replaced = 0u;
				bool hasTriangleChild = false;
				for (const uint32_t child : children[groupIndex]) {
					self(self, child);
					const GroupInfo& childInfo = info.groups[child];
					if (childInfo.voxel) {
						level = std::max(level, childInfo.voxelLevel);
						if (std::isfinite(childInfo.firstVoxelError)) {
							boundary = std::max(boundary, childInfo.firstVoxelError);
						}
					}
					else {
						hasTriangleChild = true;
						replaced += childInfo.triangles;
						if (std::isfinite(childInfo.leaveError)) {
							boundary = std::max(boundary, childInfo.leaveError);
						}
					}
				}
				out.voxelLevel = level + 1;
				out.firstVoxelError = boundary > 0.0f ? boundary : kNoError;
				info.maxVoxelLevel = std::max(info.maxVoxelLevel, out.voxelLevel);
				if (hasTriangleChild && boundary > 0.0f) {
					info.handOvers.push_back({ groupIndex, boundary, replaced });
				}
			};
			for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
				visitVoxel(visitVoxel, groupIndex);
			}

			// A triangle group hands over where its nearest voxel ancestor takes it. Parents are coarser, so this
			// visits them first and takes the earliest hand-over any of them offers.
			std::vector<uint8_t> triangleState(groupCount, 0u);
			const auto visitTriangle = [&](auto&& self, uint32_t groupIndex) -> float {
				GroupInfo& out = info.groups[groupIndex];
				if (out.voxel || triangleState[groupIndex] != 0u) {
					return out.firstVoxelError;
				}
				triangleState[groupIndex] = 1u;
				float best = kNoError;
				for (const uint32_t parent : parents[groupIndex]) {
					const GroupInfo& parentInfo = info.groups[parent];
					// Directly under a voxel group, the hand-over is the cut at which this group leaves.
					best = std::min(best, parentInfo.voxel ? out.leaveError : self(self, parent));
				}
				out.firstVoxelError = best;
				return best;
			};
			for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
				(void)visitTriangle(visitTriangle, groupIndex);
			}

			std::map<int32_t, LevelInfo> levels;
			for (const GroupInfo& out : info.groups) {
				if (out.mergePortal >= 0 || out.mergeChildren) {
					continue;  // a merge root's levels are its nodes'
				}
				LevelInfo& level = levels[out.depth];
				level.depth = out.depth;
				(out.voxel ? level.voxelGroups : level.triangleGroups) += 1u;
				level.triangles += out.triangles;
				level.cells += out.cells;
				if (std::isfinite(out.leaveError)) {
					level.minError = std::min(level.minError, out.leaveError);
					level.maxError = std::max(level.maxError, out.leaveError);
				}
				if (out.voxel && out.voxelWidth > 0.0f) {
					level.minWidth = std::min(level.minWidth, out.voxelWidth);
					level.maxWidth = std::max(level.maxWidth, out.voxelWidth);
				}
			}
			for (const auto& [depth, level] : levels) {
				info.levels.push_back(level);
			}
		}
	}

	std::vector<uint32_t> MeshInfo::Children(uint32_t group) const
	{
		const GroupInfo& info = groups[group];
		return { links.begin() + info.firstChild, links.begin() + info.firstChild + info.childCount };
	}

	std::vector<uint32_t> MeshInfo::Parents(uint32_t group) const
	{
		const GroupInfo& info = groups[group];
		return { links.begin() + info.firstParent, links.begin() + info.firstParent + info.parentCount };
	}

	HierarchyIndex::~HierarchyIndex()
	{
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}

	void HierarchyIndex::StartLoading(std::filesystem::path cacheRoot)
	{
		m_thread = std::thread([this, root = std::move(cacheRoot)]() { Load(clodeval::FindCacheMetadata(root)); });
	}

	void HierarchyIndex::StartLoading(std::vector<std::string> metadataFiles)
	{
		m_thread = std::thread([this, files = std::move(metadataFiles)]() { Load({ files.begin(), files.end() }); });
	}

	std::string HierarchyIndex::Error() const
	{
		std::scoped_lock lock(m_errorMutex);
		return m_error;
	}

	void HierarchyIndex::Load(std::vector<std::filesystem::path> paths)
	{
		m_total.store(static_cast<uint32_t>(paths.size()));
		std::vector<MeshInfo> meshes(paths.size());
		std::atomic<size_t> next{ 0u };
		std::vector<std::thread> workers;
		const uint32_t workerCount = std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 8u);
		for (uint32_t worker = 0; worker < workerCount; ++worker) {
			workers.emplace_back([&]() {
				for (size_t index = next.fetch_add(1u); index < paths.size(); index = next.fetch_add(1u)) {
					std::string error;
					meshes[index].cache = clodeval::CachedMesh::Open(paths[index], error);
					if (meshes[index].cache) {
						BuildMesh(meshes[index]);
					}
					else {
						std::scoped_lock lock(m_errorMutex);
						m_error = std::format("{}: {}", paths[index].string(), error);
					}
					m_loaded.fetch_add(1u);
				}
			});
		}
		for (std::thread& worker : workers) {
			worker.join();
		}
		std::erase_if(meshes, [](const MeshInfo& mesh) { return !mesh.cache; });
		std::ranges::sort(meshes, [](const MeshInfo& a, const MeshInfo& b) {
			return a.assembly != b.assembly ? a.assembly : a.name < b.name;
		});
		m_meshes = std::move(meshes);
		for (int32_t index = 0; index < static_cast<int32_t>(m_meshes.size()); ++index) {
			const MeshInfo& mesh = m_meshes[index];
			const ClusterLODCacheSource& source = mesh.cache->Prebuilt().cacheSource;
			m_byIdentity.emplace(Identity(source.sourceIdentifier, source.primPath, source.subsetName), index);
			m_maxDepth = std::max(m_maxDepth, mesh.maxDepth);
			m_maxMergeLevel = std::max(m_maxMergeLevel, mesh.mergeLevels);
			m_maxVoxelLevel = std::max(m_maxVoxelLevel, mesh.maxVoxelLevel);
			for (const HandOver& handOver : mesh.handOvers) {
				m_minHandOver = std::min(m_minHandOver, handOver.error);
				m_maxHandOver = std::max(m_maxHandOver, handOver.error);
			}
		}
		// Which roots open each part, so a part's pixels can say they came through a portal.
		std::unordered_map<std::string, int32_t> byPath;
		for (int32_t index = 0; index < static_cast<int32_t>(m_meshes.size()); ++index) {
			std::error_code error;
			byPath.emplace(std::filesystem::weakly_canonical(m_meshes[index].cache->MetadataPath(), error).generic_string(), index);
		}
		for (const MeshInfo& root : m_meshes) {
			if (!root.merge) {
				continue;
			}
			for (const clodeval::MergePart& part : root.merge->parts) {
				std::error_code error;
				const auto found = byPath.find(std::filesystem::weakly_canonical(part.metadataPath, error).generic_string());
				if (found == byPath.end()) {
					continue;
				}
				MeshInfo& target = m_meshes[found->second];
				target.portalUse += std::format("{}{} portals of {}", target.portalUse.empty() ? "" : "; ", part.portals, root.cache->Prebuilt().cacheSource.sourceIdentifier);
				if (!part.label.empty()) {
					target.shortName = part.label;
				}
			}
		}
		m_ready.store(true);
	}

	int32_t HierarchyIndex::Find(const std::string& sourceIdentifier, const std::string& primPath, const std::string& subset) const
	{
		// No looser match: every assembly of an asset has the same prim and subset, so one would quietly name the wrong mesh.
		const auto exact = m_byIdentity.find(Identity(sourceIdentifier, primPath, subset));
		return exact != m_byIdentity.end() ? exact->second : -1;
	}

	std::optional<CubeInfo> HierarchyIndex::FindCube(int32_t mesh, uint32_t group, uint32_t pageIndex, uint32_t pageCluster, uint32_t cube) const
	{
		if (mesh < 0 || static_cast<size_t>(mesh) >= m_meshes.size()) {
			return std::nullopt;
		}
		const clodeval::CachedMesh& cache = *m_meshes[mesh].cache;
		const ClusterLODPrebuiltData& prebuilt = cache.Prebuilt();
		if (group >= prebuilt.groups.size()) {
			return std::nullopt;
		}
		const ClusterLODGroup& record = prebuilt.groups[group];
		for (uint32_t segmentIndex = record.firstSegment; segmentIndex < record.firstSegment + record.segmentCount; ++segmentIndex) {
			const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
			if (segment.pageIndex != pageIndex || pageCluster < segment.firstMeshletInPage || pageCluster >= segment.firstMeshletInPage + segment.meshletCount) {
				continue;
			}
			uint32_t meshPage = 0;
			CLodCache::VoxelPage page;
			if (!CLodCache::ResolveSegmentMeshPage(prebuilt, group, segment, meshPage) || !CLodCache::VoxelPage::Parse(cache.Page(meshPage), page) || pageCluster >= page.ClusterCount()) {
				return std::nullopt;
			}
			const CLodVoxelClusterRecord cluster = page.Cluster(pageCluster);
			if (cube >= cluster.cubeCount || cluster.firstCube + cube >= page.CubeCount()) {
				return std::nullopt;
			}
			const CLodVoxelCubeRecord cubeRecord = page.Cube(cluster.firstCube + cube);
			CubeInfo out;
			out.coord[0] = cubeRecord.cubeCoord & 0x3FFu;
			out.coord[1] = (cubeRecord.cubeCoord >> 10u) & 0x3FFu;
			out.coord[2] = (cubeRecord.cubeCoord >> 20u) & 0x3FFu;
			out.occupancy = cubeRecord.occupancyMask;
			out.opacitySum = cubeRecord.opacitySum;
			out.refinedGroup = cubeRecord.refinedGroup;
			out.clusterCubes = cluster.cubeCount;
			out.voxelWidth = cluster.aabbMinAndVoxelWidth.w;
			out.resolution = cluster.resolution;
			out.aabbMin[0] = cluster.aabbMinAndVoxelWidth.x;
			out.aabbMin[1] = cluster.aabbMinAndVoxelWidth.y;
			out.aabbMin[2] = cluster.aabbMinAndVoxelWidth.z;
			return out;
		}
		return std::nullopt;
	}

}
