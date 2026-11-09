#include "Core/MergeHierarchy.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

#include <BasicRenderer/Assets/Import/CLodCache.h>

namespace clodeval {

namespace {
	constexpr float kInfinity = std::numeric_limits<float>::infinity();
	// A node drawn over less than this factor of cut is all but invisible: a camera moving 10% passes straight through.
	constexpr double kHardlyDrawn = 1.1;

	bool IsVoxel(const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u; }
	bool IsProxy(const ClusterLODGroup& group) { return (group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u; }
	// Roots carry FLT_MAX, which is a sentinel and not an error.
	float ErrorOf(const ClusterLODGroup& group) { return group.bounds.error >= 1.0e30f || !std::isfinite(group.bounds.error) ? kInfinity : group.bounds.error; }

	DirectX::XMFLOAT4 SphereOf(const ClusterLODGroup& group)
	{
		return { group.bounds.center[0], group.bounds.center[1], group.bounds.center[2], group.bounds.radius };
	}

	float Distance(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT4& b)
	{
		const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
		return std::sqrt(x * x + y * y + z * z);
	}

	float Distance(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b) { return Distance(DirectX::XMFLOAT3(a.x, a.y, a.z), b); }

	// (distance between centres + child radius) / parent radius: over 1 means the child pokes out of its parent.
	float Containment(const DirectX::XMFLOAT4& child, const DirectX::XMFLOAT4& parent)
	{
		return parent.w > 0.0f ? (Distance(child, parent) + child.w) / parent.w : kInfinity;
	}

	void MeasurePart(MergePart& part)
	{
		const ClusterLODPrebuiltData& prebuilt = part.cache->Prebuilt();
		const auto& groups = prebuilt.groups;
		for (uint32_t g = 0; g < groups.size(); ++g) {
			const ClusterLODGroup& group = groups[g];
			if (IsProxy(group)) {
				continue;
			}
			const bool voxel = IsVoxel(group);
			// The builder merged a part's top triangle groups: ones that no coarser triangles replace.
			const bool top = !voxel && (group.parentGroupId < 0 || static_cast<size_t>(group.parentGroupId) >= groups.size() ||
				IsVoxel(groups[static_cast<uint32_t>(group.parentGroupId)]));
			for (uint32_t s = group.firstSegment; s < group.firstSegment + group.segmentCount; ++s) {
				const ClusterLODGroupSegment& segment = prebuilt.segments[s];
				MergePart::Span span;
				span.voxel = voxel;
				span.leave = ErrorOf(group);
				span.coarse = top || voxel;
				span.enter = segment.refinedGroup >= 0 && static_cast<size_t>(segment.refinedGroup) < groups.size()
					? ErrorOf(groups[static_cast<uint32_t>(segment.refinedGroup)]) : 0.0f;
				if (!std::isfinite(span.enter)) {
					span.enter = 0.0f;
				}
				span.clusters = segment.meshletCount;
				if (!voxel) {
					part.cache->VisitSegmentMeshlets(g, segment, [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) {
						span.triangles += meshlet.triangleCount;
					});
					if (segment.refinedGroup < 0) {
						part.leafTriangles += span.triangles;
					}
					if (top) {
						part.coarseTriangles += span.triangles;
						part.coarseError = std::max(part.coarseError, span.enter);
					}
				}
				part.spans.push_back(span);
			}
		}
	}

	struct Stats {
		double min = 0.0, mean = 0.0, p50 = 0.0, p95 = 0.0, max = 0.0;
		size_t count = 0;
	};

	Stats Summarize(std::vector<double> values)
	{
		Stats out;
		out.count = values.size();
		if (values.empty()) {
			return out;
		}
		std::ranges::sort(values);
		out.min = values.front();
		out.max = values.back();
		out.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
		const auto at = [&](double q) { return values[std::min(values.size() - 1u, static_cast<size_t>(q * static_cast<double>(values.size() - 1u) + 0.5))]; };
		out.p50 = at(0.5);
		out.p95 = at(0.95);
		return out;
	}

	nlohmann::json ToJson(const Stats& stats)
	{
		return { { "count", stats.count }, { "min", stats.min }, { "mean", stats.mean }, { "p50", stats.p50 }, { "p95", stats.p95 }, { "max", stats.max } };
	}

	std::string Compact(const Stats& stats, int precision = 3)
	{
		if (stats.count == 0u) {
			return "-";
		}
		return std::format("{:.{}g} / {:.{}g} / {:.{}g}", stats.min, precision, stats.mean, precision, stats.max, precision);
	}

	// Camera distance at which a pixel is `cut` world units, for a 1080-line, 60 degree view at a 1 pixel threshold.
	// Only a way to read a cut; the renderer's threshold is a setting.
	double DistanceAtOnePixel(double cut)
	{
		return cut * 1080.0 / (2.0 * std::tan(30.0 * 3.14159265358979 / 180.0));
	}

	// Per level, the numbers the report shows and the analyzer emits.
	struct LevelSummary {
		uint32_t level = 0;
		uint32_t nodes = 0;
		uint32_t nodeChildren = 0, portalChildren = 0;
		Stats fanout, triangles, error, radius, errorPerRadius, growth, containment, window;
		uint32_t hardlyDrawn = 0;  // window under kHardlyDrawn: the node's own geometry almost never draws
		uint64_t hardlyDrawnTriangles = 0;
	};

	std::vector<LevelSummary> SummarizeLevels(const MergeHierarchy& h)
	{
		std::map<uint32_t, std::vector<uint32_t>> byLevel;
		for (uint32_t n = 0; n < h.nodes.size(); ++n) {
			byLevel[h.nodes[n].level].push_back(n);
		}
		std::vector<LevelSummary> out;
		for (const auto& [level, members] : byLevel) {
			LevelSummary summary;
			summary.level = level;
			summary.nodes = static_cast<uint32_t>(members.size());
			std::vector<double> fanout, triangles, error, radius, errorPerRadius, growth, containment, window;
			for (const uint32_t n : members) {
				const MergeNode& node = h.nodes[n];
				summary.nodeChildren += static_cast<uint32_t>(node.childNodes.size());
				summary.portalChildren += static_cast<uint32_t>(node.childPortals.size());
				fanout.push_back(static_cast<double>(node.childNodes.size() + node.childPortals.size()));
				triangles.push_back(node.triangles);
				// A node's own geometry draws while E(N) <= cut < E(parent): this is how wide that range is.
				if (node.parent >= 0 && node.error > 0.0f) {
					const double w = h.nodes[static_cast<uint32_t>(node.parent)].error / node.error;
					window.push_back(w);
					if (w < kHardlyDrawn) {
						++summary.hardlyDrawn;
						summary.hardlyDrawnTriangles += node.triangles;
					}
				}
				error.push_back(node.error);
				radius.push_back(node.sphere.w);
				if (node.sphere.w > 0.0f) {
					errorPerRadius.push_back(node.error / node.sphere.w);
				}
				// How much coarser N is than its finest-to-switch child: what one level of the merge costs in error.
				float childError = 0.0f;
				bool childErrorKnown = true;
				for (const uint32_t c : node.childNodes) {
					childError = std::max(childError, h.nodes[c].error);
					containment.push_back(Containment(h.nodes[c].sphere, node.sphere));
				}
				for (const uint32_t p : node.childPortals) {
					const MergePortal& portal = h.portals[p];
					if (h.partsOpen) {
						childError = std::max(childError, h.parts[portal.part].coarseError * portal.scale);
						containment.push_back(Containment(portal.sphere, node.sphere));
					}
					else {
						childErrorKnown = false;
					}
				}
				if (childErrorKnown && childError > 0.0f) {
					growth.push_back(node.error / childError);
				}
			}
			summary.fanout = Summarize(fanout);
			summary.triangles = Summarize(triangles);
			summary.error = Summarize(error);
			summary.radius = Summarize(radius);
			summary.errorPerRadius = Summarize(errorPerRadius);
			summary.growth = Summarize(growth);
			summary.containment = Summarize(containment);
			summary.window = Summarize(window);
			out.push_back(summary);
		}
		return out;
	}
}

bool IsMergeRoot(const ClusterLODPrebuiltData& prebuilt)
{
	return std::ranges::any_of(prebuilt.assemblyInstances, [](const ClusterLODAssemblyInstance& instance) {
		return (instance.flags & CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL) != 0u;
	});
}

uint64_t MergeHierarchy::NodeTriangles() const
{
	uint64_t total = 0;
	for (const MergeNode& node : nodes) {
		total += node.triangles;
	}
	return total;
}

uint64_t MergeHierarchy::InputTriangles() const
{
	uint64_t total = 0;
	for (const MergePortal& portal : portals) {
		if (portal.part < parts.size()) {
			total += parts[portal.part].coarseTriangles;
		}
	}
	return total;
}

MergeCutCost MergeHierarchy::CostAt(float cut) const
{
	MergeCutCost cost;
	cost.cut = cut;
	cost.partsKnown = partsOpen;
	const auto open = [&](const MergePortal& portal) {
		++cost.portals;
		if (!partsOpen) {
			return;
		}
		const MergePart& part = parts[portal.part];
		const float partCut = cut / std::max(portal.scale, 1.0e-12f);
		bool finer = false;
		for (const MergePart::Span& span : part.spans) {
			if (span.enter <= partCut && partCut < span.leave) {
				if (span.voxel) {
					cost.partVoxelClusters += span.clusters;
				}
				else {
					cost.partTriangles += span.triangles;
					cost.partClusters += span.clusters;
				}
				finer |= !span.coarse;
			}
		}
		cost.coarsestPortals += finer ? 0u : 1u;
	};
	for (const MergePortal& portal : portals) {
		if (portal.parent < 0) {
			open(portal);
		}
	}
	std::vector<uint32_t> stack = tops;
	while (!stack.empty()) {
		const MergeNode& node = nodes[stack.back()];
		stack.pop_back();
		if (cut >= node.error || node.folded) {
			++cost.nodes;
			cost.nodeTriangles += node.triangles;
			cost.nodeClusters += node.clusters;
			continue;
		}
		stack.insert(stack.end(), node.childNodes.begin(), node.childNodes.end());
		for (const uint32_t p : node.childPortals) {
			open(portals[p]);
		}
	}
	return cost;
}

std::vector<MergeCutCost> MergeHierarchy::Sweep(uint32_t maxSteps) const
{
	std::vector<MergeCutCost> out;
	if (root < 0) {
		return out;
	}
	// From where the root first splits, by halves, to under the smallest error anything has: past that, every node is
	// open and every part draws its finest. Errors of zero-error leaves are sentinels and do not count.
	float floor = std::numeric_limits<float>::max();
	for (const MergeNode& node : nodes) {
		if (node.error > 1.0e-5f) {
			floor = std::min(floor, node.error);
		}
	}
	if (partsOpen) {
		for (const MergePortal& portal : portals) {
			for (const MergePart::Span& span : parts[portal.part].spans) {
				if (span.enter > 1.0e-5f) {
					floor = std::min(floor, span.enter * portal.scale);
				}
			}
		}
	}
	float cut = 0.0f;
	for (const uint32_t top : tops) {
		cut = std::max(cut, nodes[top].error);
	}
	for (uint32_t step = 0; step < maxSteps && cut > 0.0f; ++step) {
		out.push_back(CostAt(cut));
		if (cut < floor * 0.5f) {
			break;
		}
		cut *= 0.5f;
	}
	return out;
}

std::shared_ptr<const MergePart> MergePartLibrary::Get(const std::filesystem::path& metadataPath, uint32_t branch)
{
	std::shared_ptr<Slot> slot;
	{
		std::scoped_lock lock(m_mutex);
		auto& entry = m_slots[metadataPath.generic_string() + "#" + std::to_string(branch)];
		if (!entry) {
			entry = std::make_shared<Slot>();
		}
		slot = entry;
	}
	std::call_once(slot->once, [&]() {
		auto part = std::make_shared<MergePart>();
		part->metadataPath = metadataPath;
		std::unique_ptr<CachedMesh> cache = CachedMesh::Open(metadataPath, part->error);
		if (cache && cache->IsForked() && branch != CLOD_BRANCH_ALL) {
			cache = cache->BranchView(branch, "");
		}
		if (cache) {
			part->cache = std::move(cache);
			MeasurePart(*part);
		}
		slot->part = std::move(part);
	});
	return slot->part;
}

size_t MergePartLibrary::Size() const
{
	std::scoped_lock lock(m_mutex);
	return m_slots.size();
}

std::unique_ptr<MergeHierarchy> ReadMergeHierarchy(const CachedMesh& root, bool loadParts, MergePartLibrary* library)
{
	const ClusterLODPrebuiltData& prebuilt = root.Prebuilt();
	if (!IsMergeRoot(prebuilt)) {
		return nullptr;
	}
	auto h = std::make_unique<MergeHierarchy>();
	h->materialSlots = prebuilt.materialSlotCount;
	const auto& groups = prebuilt.groups;
	h->nodeOfGroup.assign(groups.size(), -1);
	h->portalOfGroup.assign(groups.size(), -1);

	// Nodes: groups whose segments refine into page-less children groups.
	for (uint32_t g = 0; g < groups.size(); ++g) {
		const ClusterLODGroup& group = groups[g];
		if (IsProxy(group) || group.segmentCount == 0u) {
			continue;
		}
		int32_t children = -1;
		uint32_t terminal = 0;
		for (uint32_t s = group.firstSegment; s < group.firstSegment + group.segmentCount; ++s) {
			terminal += prebuilt.segments[s].refinedGroup < 0 ? 1u : 0u;
		}
		const bool folded = terminal == group.segmentCount;
		for (uint32_t s = group.firstSegment; s < group.firstSegment + group.segmentCount && !folded; ++s) {
			const int32_t refined = prebuilt.segments[s].refinedGroup;
			if (refined < 0 || static_cast<size_t>(refined) >= groups.size() || groups[static_cast<uint32_t>(refined)].segmentCount != 0u) {
				children = -2;
				break;
			}
			if (children == -1) {
				children = refined;
			}
			else if (children != refined) {
				children = -2;
				break;
			}
		}
		if (!folded && children < 0) {
			h->problems.push_back(std::format("group {} has segments but does not refine into one children group", g));
			continue;
		}
		MergeNode node;
		node.group = g;
		node.folded = folded;
		if (folded) {
			// Exact triangles: no error, and drawn wherever its parent is not. Its own bounds are its parent's.
			node.error = 0.0f;
			node.sphere = SphereOf(group);
			node.level = static_cast<uint32_t>(std::max(group.depth, 0));
		}
		else {
			node.childrenGroup = static_cast<uint32_t>(children);
			const ClusterLODGroup& childrenGroup = groups[node.childrenGroup];
			node.error = ErrorOf(childrenGroup);
			node.sphere = SphereOf(childrenGroup);
			node.level = static_cast<uint32_t>(std::max(childrenGroup.depth, 0));
		}
		for (uint32_t s = group.firstSegment; s < group.firstSegment + group.segmentCount; ++s) {
			node.clusters += prebuilt.segments[s].meshletCount;
			root.VisitSegmentMeshlets(g, prebuilt.segments[s], [&](const CLodCache::TrianglePage&, const CLodCache::TriangleMeshlet& meshlet) {
				node.triangles += meshlet.triangleCount;
			});
		}
		h->nodeOfGroup[g] = static_cast<int32_t>(h->nodes.size());
		if (!folded) {
			h->nodeOfGroup[node.childrenGroup] = static_cast<int32_t>(h->nodes.size());
		}
		h->nodes.push_back(std::move(node));
	}

	// Portals: proxies, each naming its assembly instance.
	for (uint32_t g = 0; g < groups.size(); ++g) {
		const ClusterLODGroup& group = groups[g];
		if (!IsProxy(group)) {
			continue;
		}
		MergePortal portal;
		portal.group = g;
		portal.instance = group.firstMeshlet;
		if (portal.instance >= prebuilt.assemblyInstances.size()) {
			h->problems.push_back(std::format("proxy {} names instance {} of {}", g, portal.instance, prebuilt.assemblyInstances.size()));
			continue;
		}
		const ClusterLODAssemblyInstance& instance = prebuilt.assemblyInstances[portal.instance];
		if ((instance.flags & CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL) == 0u) {
			h->problems.push_back(std::format("proxy {}: instance {} is not a part portal", g, portal.instance));
			continue;
		}
		portal.part = instance.stackDepth;
		portal.transform = instance.transformIndex;
		portal.materialBase = portal.transform < prebuilt.assemblyTransformMaterialBases.size() ? prebuilt.assemblyTransformMaterialBases[portal.transform] : 0u;
		if (portal.transform < prebuilt.assemblyTransforms.size()) {
			const ClusterLODAssemblyTransform& t = prebuilt.assemblyTransforms[portal.transform];
			// Rows of the column-vector 3x4: an axis's scale is the length of its column.
			const float sx = std::sqrt(t.row0.x * t.row0.x + t.row1.x * t.row1.x + t.row2.x * t.row2.x);
			const float sy = std::sqrt(t.row0.y * t.row0.y + t.row1.y * t.row1.y + t.row2.y * t.row2.y);
			const float sz = std::sqrt(t.row0.z * t.row0.z + t.row1.z * t.row1.z + t.row2.z * t.row2.z);
			portal.scale = std::max({ sx, sy, sz });
			portal.position = { t.row0.w, t.row1.w, t.row2.w };
		}
		else {
			h->problems.push_back(std::format("proxy {}: transform {} of {}", g, portal.transform, prebuilt.assemblyTransforms.size()));
		}
		h->portalOfGroup[g] = static_cast<int32_t>(h->portals.size());
		h->portals.push_back(portal);
	}

	// Parents: a child names its parent's geometry group.
	const auto parentNodeOf = [&](uint32_t g) -> int32_t {
		const int32_t parentGroup = groups[g].parentGroupId;
		if (parentGroup < 0 || static_cast<size_t>(parentGroup) >= groups.size()) {
			return -1;
		}
		const int32_t node = h->nodeOfGroup[static_cast<uint32_t>(parentGroup)];
		return node >= 0 && h->nodes[static_cast<uint32_t>(node)].group == static_cast<uint32_t>(parentGroup) ? node : -2;
	};
	for (uint32_t n = 0; n < h->nodes.size(); ++n) {
		MergeNode& node = h->nodes[n];
		node.parent = parentNodeOf(node.group);
		if (node.parent == -2) {
			h->problems.push_back(std::format("node group {} names parent group {}, which is not a node", node.group, groups[node.group].parentGroupId));
			node.parent = -1;
		}
		if (node.parent >= 0) {
			h->nodes[static_cast<uint32_t>(node.parent)].childNodes.push_back(n);
		}
		else {
			h->tops.push_back(n);
			if (h->root < 0 || node.error > h->nodes[static_cast<uint32_t>(h->root)].error) {
				h->root = static_cast<int32_t>(n);
			}
		}
	}
	for (uint32_t p = 0; p < h->portals.size(); ++p) {
		MergePortal& portal = h->portals[p];
		portal.parent = parentNodeOf(portal.group);
		if (portal.parent < 0) {
			// At the top: open at every distance. Only a capped hierarchy has these.
			++h->topPortals;
			continue;
		}
		h->nodes[static_cast<uint32_t>(portal.parent)].childPortals.push_back(p);
	}

	// Depths from the root, and the rules children must keep.
	if (!h->tops.empty()) {
		std::vector<std::pair<uint32_t, uint32_t>> stack;
		for (const uint32_t top : h->tops) {
			stack.push_back({ top, 0u });
		}
		std::vector<bool> seen(h->nodes.size(), false);
		while (!stack.empty()) {
			const auto [n, depth] = stack.back();
			stack.pop_back();
			if (seen[n]) {
				h->problems.push_back(std::format("node group {} is reached twice", h->nodes[n].group));
				continue;
			}
			seen[n] = true;
			MergeNode& node = h->nodes[n];
			node.depth = depth;
			h->levels = std::max(h->levels, node.level);
			if (node.folded) {
				if (!node.childNodes.empty() || !node.childPortals.empty()) {
					h->problems.push_back(std::format("folded node group {} has children", node.group));
				}
				continue;
			}
			const ClusterLODGroup& childrenBounds = groups[node.childrenGroup];
			const auto sameBounds = [&](const ClusterLODGroup& child) {
				return child.bounds.error == childrenBounds.bounds.error && child.bounds.radius == childrenBounds.bounds.radius &&
					child.bounds.center[0] == childrenBounds.bounds.center[0] && child.bounds.center[1] == childrenBounds.bounds.center[1] &&
					child.bounds.center[2] == childrenBounds.bounds.center[2];
			};
			for (const uint32_t c : node.childNodes) {
				stack.push_back({ c, depth + 1u });
				if (!(h->nodes[c].error < node.error)) {
					h->problems.push_back(std::format("node group {}: error {} is not under its parent's {}", h->nodes[c].group, h->nodes[c].error, node.error));
				}
				if (!sameBounds(groups[h->nodes[c].group])) {
					h->problems.push_back(std::format("node group {} does not carry its parent's children bounds, so siblings switch apart", h->nodes[c].group));
				}
			}
			for (const uint32_t p : node.childPortals) {
				h->portals[p].depth = depth + 1u;
				if (!sameBounds(groups[h->portals[p].group])) {
					h->problems.push_back(std::format("proxy {} does not carry its parent's children bounds", h->portals[p].group));
				}
			}
		}
		for (uint32_t n = 0; n < h->nodes.size(); ++n) {
			if (!seen[n]) {
				h->problems.push_back(std::format("node group {} is not reached from the top", h->nodes[n].group));
			}
		}
	}
	else {
		h->problems.push_back("no root node");
	}

	// Parts, from the merged mesh's own table (ClusterLODPrebuiltData::externalParts), in the cache tree the root is in:
	// a merged mesh's metadata is <tree>/clod/merged/<file>.
	uint32_t partCount = 0;
	for (const MergePortal& portal : h->portals) {
		partCount = std::max(partCount, portal.part + 1u);
	}
	const std::vector<ClusterLODExternalPart>& external = prebuilt.externalParts;
	const bool hasTable = !external.empty();
	h->partsFile = hasTable;
	h->parts.resize(std::max<size_t>(external.size(), partCount));
	if (hasTable) {
		h->paletteParts = prebuilt.paletteParts;
		const std::filesystem::path tree = root.MetadataPath().parent_path().parent_path().parent_path();
		for (size_t i = 0; i < external.size(); ++i) {
			const ClusterLODExternalPart& part = external[i];
			h->parts[i].label = std::format("{} part {} mesh {}", part.assetKey, part.partIndex, part.meshIndex);
			if (part.variantHash != 0u) h->parts[i].label += std::format(" variant {:016x}", part.variantHash);
			const CLodCache::CacheKey key{ part.sourceIdentifier, part.primPath, part.subsetName };
			h->parts[i].metadataPath = tree / CLodCache::RelativeMetadataPath(key, part.buildConfigHash);
		}
		if (external.size() < partCount) {
			h->problems.push_back(std::format("portals name {} parts, the table lists {}", partCount, external.size()));
		}
	}
	for (const MergePortal& portal : h->portals) {
		++h->parts[portal.part].portals;
	}
	if (hasTable && loadParts) {
		h->partsOpen = external.size() >= partCount;
		for (MergePart& part : h->parts) {
			if (part.metadataPath.empty()) {
				h->partsOpen = false;
				continue;
			}
			// Measured parts are shared; what is per root (label, portals) is kept.
			// A forked part opens in the root's branch (the root's own view, or its triangle branch when unprojected).
			const uint32_t branch = root.Branch() == CLOD_BRANCH_ALL ? CLOD_BRANCH_TRIANGLE : root.Branch();
			const std::shared_ptr<const MergePart> measured = library ? library->Get(part.metadataPath, branch) : nullptr;
			MergePart local;
			if (!measured) {
				local.metadataPath = part.metadataPath;
				if (std::unique_ptr<CachedMesh> cache = CachedMesh::Open(part.metadataPath, local.error)) {
					if (cache->IsForked()) cache = cache->BranchView(branch, "");
					local.cache = std::move(cache);
					MeasurePart(local);
				}
			}
			const MergePart& source = measured ? *measured : local;
			part.cache = source.cache;
			part.error = source.error;
			part.spans = source.spans;
			part.coarseTriangles = source.coarseTriangles;
			part.leafTriangles = source.leafTriangles;
			part.coarseError = source.coarseError;
			if (!part.cache) {
				h->partsOpen = false;
			}
		}
		// Each portal's target node and its part's sphere, in root space.
		for (MergePortal& portal : h->portals) {
			MergePart& part = h->parts[portal.part];
			if (!part.cache) {
				continue;
			}
			const ClusterLODAssemblyInstance& instance = prebuilt.assemblyInstances[portal.instance];
			part.rootNode = instance.targetRootNode;
			const auto& nodes = part.cache->Prebuilt().nodes;
			if (instance.targetRootNode >= nodes.size()) {
				h->problems.push_back(std::format("proxy {} opens node {} of a part with {}", portal.group, instance.targetRootNode, nodes.size()));
				continue;
			}
			part.lodSphere = nodes[instance.targetRootNode].traversalMetric.lodBoundingSphere;
			if (portal.transform < prebuilt.assemblyTransforms.size()) {
				const ClusterLODAssemblyTransform& t = prebuilt.assemblyTransforms[portal.transform];
				const DirectX::XMFLOAT4& s = part.lodSphere;
				portal.sphere = {
					t.row0.x * s.x + t.row0.y * s.y + t.row0.z * s.z + t.row0.w,
					t.row1.x * s.x + t.row1.y * s.y + t.row1.z * s.z + t.row1.w,
					t.row2.x * s.x + t.row2.y * s.y + t.row2.z * s.z + t.row2.w,
					s.w * portal.scale,
				};
			}
		}
	}
	else if (!hasTable) {
		h->problems.push_back("the cache names no external parts, so the parts are unknown");
	}
	return h;
}

MergeIssues FindMergeIssues(const MergeHierarchy& h, float reachCut)
{
	MergeIssues out;
	out.reachCut = reachCut;
	if (h.root < 0) {
		return out;
	}
	out.atReach = h.CostAt(reachCut);
	out.rootDraws = h.tops.size() == 1u && h.topPortals == 0u && h.nodes[static_cast<uint32_t>(h.root)].error <= reachCut;
	constexpr float kSentinel = 1.0e-5f;
	double logSpread = 0.0;
	uint32_t spreadNodes = 0;
	for (const MergeNode& node : h.nodes) {
		if (node.error > reachCut) {
			++out.unreachableNodes;
			out.unreachableTriangles += node.triangles;
			out.unreachableLevels = std::max(out.unreachableLevels, node.depth + 1u);
		}
		else if (node.parent >= 0 && node.error > 0.0f && h.nodes[static_cast<uint32_t>(node.parent)].error / node.error < kHardlyDrawn) {
			++out.hardlyDrawnNodes;
			out.hardlyDrawnTriangles += node.triangles;
		}
		float smallest = std::numeric_limits<float>::max();
		uint32_t children = 0;
		for (const uint32_t c : node.childNodes) {
			if (h.nodes[c].error > kSentinel) {
				smallest = std::min(smallest, h.nodes[c].error);
				++children;
			}
		}
		for (const uint32_t p : node.childPortals) {
			const MergePortal& portal = h.portals[p];
			const float error = h.partsOpen ? h.parts[portal.part].coarseError * portal.scale : 0.0f;
			if (error > kSentinel) {
				smallest = std::min(smallest, error);
				++children;
			}
		}
		if (children >= 2u && node.error > kSentinel) {
			const double spread = std::max(1.0, static_cast<double>(node.error) / smallest);
			logSpread += std::log(spread);
			out.siblingSpreadMax = std::max(out.siblingSpreadMax, spread);
			++spreadNodes;
		}
	}
	out.siblingSpreadMean = spreadNodes ? std::exp(logSpread / spreadNodes) : 1.0;
	double depthSum = 0.0;
	for (const MergePortal& portal : h.portals) {
		depthSum += portal.depth;
		out.portalDepthMax = std::max(out.portalDepthMax, portal.depth);
		if (portal.parent < 0) {
			// At the top: open at every distance.
			++out.rootPortals;
			++out.alwaysOpenPortals;
			if (h.partsOpen) {
				const MergePart& part = h.parts[portal.part];
				const float partCut = reachCut / std::max(portal.scale, 1.0e-12f);
				bool finer = false;
				for (const MergePart::Span& span : part.spans) {
					finer |= span.enter <= partCut && partCut < span.leave && !span.coarse;
				}
				out.alwaysOpenCoarsestPortals += finer ? 0u : 1u;
			}
			continue;
		}
		const MergeNode& parent = h.nodes[static_cast<uint32_t>(portal.parent)];
		if (parent.error < kSentinel) {
			++out.deadPortals;
			out.deadPortalTriangles += h.partsOpen ? h.parts[portal.part].coarseTriangles : 0u;
			continue;
		}
		if (parent.error > reachCut) {
			++out.alwaysOpenPortals;
			if (h.partsOpen) {
				const MergePart& part = h.parts[portal.part];
				const float partCut = reachCut / std::max(portal.scale, 1.0e-12f);
				bool finer = false;
				for (const MergePart::Span& span : part.spans) {
					finer |= span.enter <= partCut && partCut < span.leave && !span.coarse;
				}
				out.alwaysOpenCoarsestPortals += finer ? 0u : 1u;
			}
		}
	}
	out.portalDepthMean = h.portals.empty() ? 0.0 : depthSum / static_cast<double>(h.portals.size());
	return out;
}

nlohmann::json MergeReportJson(const MergeHierarchy& h, const CachedMesh& root)
{
	nlohmann::json json;
	json["mesh"] = root.Name();
	json["source"] = root.Prebuilt().cacheSource.sourceIdentifier;
	json["metadata_bytes"] = root.MetadataBytes();
	json["container_bytes"] = root.ContainerBytes();
	json["nodes"] = h.nodes.size();
	json["portals"] = h.portals.size();
	json["parts"] = h.parts.size();
	json["palette"] = h.materialSlots;
	json["levels"] = h.levels;
	json["node_triangles"] = h.NodeTriangles();
	json["parts_open"] = h.partsOpen;
	if (h.root >= 0) {
		const MergeNode& top = h.nodes[static_cast<uint32_t>(h.root)];
		json["root"] = { { "triangles", top.triangles }, { "clusters", top.clusters }, { "error", top.error }, { "radius", top.sphere.w },
			{ "fanout", top.childNodes.size() + top.childPortals.size() } };
	}
	if (h.partsOpen) {
		json["input_triangles"] = h.InputTriangles();
		uint64_t partBytes = 0, leafTriangles = 0;
		for (const MergePart& part : h.parts) {
			partBytes += part.cache ? part.cache->ContainerBytes() : 0u;
		}
		for (const MergePortal& portal : h.portals) {
			leafTriangles += h.parts[portal.part].leafTriangles;
		}
		json["part_container_bytes"] = partBytes;
		json["opened_leaf_triangles"] = leafTriangles;
	}
	nlohmann::json levels = nlohmann::json::array();
	for (const LevelSummary& level : SummarizeLevels(h)) {
		levels.push_back({
			{ "level", level.level }, { "nodes", level.nodes }, { "node_children", level.nodeChildren }, { "portal_children", level.portalChildren },
			{ "fanout", ToJson(level.fanout) }, { "triangles", ToJson(level.triangles) }, { "error", ToJson(level.error) },
			{ "radius", ToJson(level.radius) }, { "error_per_radius", ToJson(level.errorPerRadius) }, { "growth", ToJson(level.growth) },
			{ "containment", ToJson(level.containment) }, { "draw_window", ToJson(level.window) },
			{ "hardly_drawn", level.hardlyDrawn }, { "hardly_drawn_triangles", level.hardlyDrawnTriangles },
		});
	}
	json["by_level"] = std::move(levels);
	std::vector<double> portalDepths, scales;
	for (const MergePortal& portal : h.portals) {
		portalDepths.push_back(portal.depth);
		scales.push_back(portal.scale);
	}
	json["portal_depth"] = ToJson(Summarize(portalDepths));
	json["portal_scale"] = ToJson(Summarize(scales));
	nlohmann::json parts = nlohmann::json::array();
	for (uint32_t i = 0; i < h.parts.size(); ++i) {
		const MergePart& part = h.parts[i];
		parts.push_back({ { "part", i }, { "label", part.label }, { "portals", part.portals }, { "coarse_triangles", part.coarseTriangles },
			{ "leaf_triangles", part.leafTriangles }, { "coarse_error", part.coarseError }, { "radius", part.lodSphere.w }, { "error", part.error } });
	}
	json["part_list"] = std::move(parts);
	nlohmann::json sweep = nlohmann::json::array();
	for (const MergeCutCost& cost : h.Sweep()) {
		sweep.push_back({ { "cut", cost.cut }, { "distance_1px_1080p_60deg", DistanceAtOnePixel(cost.cut) }, { "nodes", cost.nodes },
			{ "node_triangles", cost.nodeTriangles }, { "node_clusters", cost.nodeClusters }, { "portals", cost.portals },
			{ "coarsest_portals", cost.coarsestPortals }, { "part_triangles", cost.partTriangles }, { "part_clusters", cost.partClusters },
			{ "part_voxel_clusters", cost.partVoxelClusters } });
	}
	json["cut_sweep"] = std::move(sweep);
	json["problems"] = h.problems;
	return json;
}

std::string FormatMergeReport(const MergeHierarchy& h, const CachedMesh& root)
{
	std::string out;
	const auto line = [&](std::string text) { out += text; out += '\n'; };
	line(std::format("merge root {}  ({})", root.Prebuilt().cacheSource.sourceIdentifier, root.MetadataPath().string()));
	line(std::format("  {} nodes over {} levels, {} portals into {} parts, palette {}; metadata {} KiB, pages {} KiB",
		h.nodes.size(), h.levels, h.portals.size(), h.parts.size(), h.materialSlots, root.MetadataBytes() / 1024u, root.ContainerBytes() / 1024u));
	const uint64_t nodeTriangles = h.NodeTriangles();
	if (h.partsOpen) {
		const uint64_t input = h.InputTriangles();
		uint64_t partBytes = 0, leaf = 0;
		for (const MergePart& part : h.parts) {
			partBytes += part.cache ? part.cache->ContainerBytes() : 0u;
		}
		for (const MergePortal& portal : h.portals) {
			leaf += h.parts[portal.part].leafTriangles;
		}
		line(std::format("  node triangles {} for {} merged input triangles ({:.2f}x); fully open, the portals draw {} triangles; part pages {} KiB",
			nodeTriangles, input, input ? static_cast<double>(nodeTriangles) / static_cast<double>(input) : 0.0, leaf, partBytes / 1024u));
	}
	else {
		line(std::format("  node triangles {}; parts not opened, so part numbers are missing", nodeTriangles));
	}
	if (h.root >= 0) {
		const MergeNode& top = h.nodes[static_cast<uint32_t>(h.root)];
		if (h.tops.size() != 1u || h.topPortals != 0u) {
			uint64_t topTriangles = 0;
			for (const uint32_t t : h.tops) {
				topTriangles += h.nodes[t].triangles;
			}
			line(std::format("  top: {} nodes ({} triangles) and {} portals open at every distance; the largest:", h.tops.size(), topTriangles, h.topPortals));
		}
		line(std::format("  root: {} triangles in {} clusters, E={:.4g}, radius {:.4g}, {} children ({} nodes, {} portals); splits from about {:.0f} units at 1 px",
			top.triangles, top.clusters, top.error, top.sphere.w, top.childNodes.size() + top.childPortals.size(), top.childNodes.size(),
			top.childPortals.size(), DistanceAtOnePixel(top.error)));
	}
	line("");
	line("  by level (min / mean / max)");
	line(std::format("  {:>5} {:>6} {:>7} {:>7} {:>16} {:>22} {:>24} {:>22} {:>18} {:>18} {:>18} {:>22} {:>6}", "level", "nodes", "c.node", "c.portal", "fanout", "triangles",
		"error E(N)", "radius", "E/radius", "E/max child E", "child fit", "draw window", "hardly"));
	for (const LevelSummary& level : SummarizeLevels(h)) {
		line(std::format("  {:>5} {:>6} {:>7} {:>7} {:>16} {:>22} {:>24} {:>22} {:>18} {:>18} {:>18} {:>22} {:>6}", level.level, level.nodes, level.nodeChildren, level.portalChildren,
			Compact(level.fanout), Compact(level.triangles, 4), Compact(level.error), Compact(level.radius), Compact(level.errorPerRadius),
			Compact(level.growth), Compact(level.containment), Compact(level.window), level.hardlyDrawn));
	}
	line("  child fit: (centre distance + child radius) / parent radius; over 1 pokes out. E/max child E: the error one merge step adds.");
	line(std::format("  draw window: E(parent) / E(N), the range of cuts a node's own geometry draws in. hardly: nodes under {:g}x.", kHardlyDrawn));
	{
		uint32_t hardly = 0;
		uint64_t hardlyTriangles = 0;
		for (const LevelSummary& level : SummarizeLevels(h)) {
			hardly += level.hardlyDrawn;
			hardlyTriangles += level.hardlyDrawnTriangles;
		}
		line(std::format("  {} of {} nodes hardly ever draw their own geometry ({} triangles, {:.1f}% of node triangles)", hardly, h.nodes.size(),
			hardlyTriangles, nodeTriangles ? 100.0 * static_cast<double>(hardlyTriangles) / static_cast<double>(nodeTriangles) : 0.0));
	}
	std::vector<double> portalDepths;
	for (const MergePortal& portal : h.portals) {
		portalDepths.push_back(portal.depth);
	}
	const Stats depth = Summarize(portalDepths);
	std::map<uint32_t, uint32_t> depthHistogram;
	for (const MergePortal& portal : h.portals) {
		++depthHistogram[portal.depth];
	}
	std::string histogram;
	for (const auto& [d, count] : depthHistogram) {
		histogram += std::format(" {}:{}", d, count);
	}
	line(std::format("  portal depth (nodes above): mean {:.2f}, max {:.0f};{}", depth.mean, depth.max, histogram));

	if (!h.parts.empty()) {
		std::vector<uint32_t> order(h.parts.size());
		std::iota(order.begin(), order.end(), 0u);
		std::ranges::sort(order, [&](uint32_t a, uint32_t b) { return h.parts[a].portals > h.parts[b].portals; });
		const uint32_t shared = static_cast<uint32_t>(std::ranges::count_if(h.parts, [](const MergePart& part) { return part.portals > 1u; }));
		line(std::format("  parts: {} used once, {} shared; most used:", h.parts.size() - shared, shared));
		for (size_t i = 0; i < std::min<size_t>(order.size(), 8u); ++i) {
			const MergePart& part = h.parts[order[i]];
			line(std::format("    {:>4}x  {:>6} coarse / {:>7} leaf triangles, coarse E {:.3g}  {}", part.portals, part.coarseTriangles, part.leafTriangles,
				part.coarseError, part.label.empty() ? std::format("part {}", order[i]) : part.label));
		}
	}

	line("");
	line("  cut sweep: the whole region at one world cut (telephoto). distance: where 1 px is that cut at 1080p, 60 degrees");
	line(std::format("  {:>10} {:>9} {:>6} {:>9} {:>8} {:>8} {:>9} {:>10} {:>8} {:>8} {:>10}", "cut", "distance", "nodes", "node tri", "node cl", "portals",
		"coarsest", "part tri", "part cl", "part vx", "total tri"));
	for (const MergeCutCost& cost : h.Sweep()) {
		line(std::format("  {:>10.4g} {:>9.0f} {:>6} {:>9} {:>8} {:>8} {:>9} {:>10} {:>8} {:>8} {:>10}", cost.cut, DistanceAtOnePixel(cost.cut), cost.nodes,
			cost.nodeTriangles, cost.nodeClusters, cost.portals, h.partsOpen ? std::to_string(cost.coarsestPortals) : "-",
			h.partsOpen ? std::to_string(cost.partTriangles) : "-", h.partsOpen ? std::to_string(cost.partClusters) : "-",
			h.partsOpen ? std::to_string(cost.partVoxelClusters) : "-", h.partsOpen ? std::to_string(cost.Triangles()) : "-"));
	}
	line("  coarsest: opened portals whose part draws nothing finer than what the node had merged, so opening it bought nothing yet.");
	if (!h.problems.empty()) {
		line(std::format("  {} problems:", h.problems.size()));
		for (size_t i = 0; i < std::min<size_t>(h.problems.size(), 20u); ++i) {
			line("    " + h.problems[i]);
		}
	}
	return out;
}

}
