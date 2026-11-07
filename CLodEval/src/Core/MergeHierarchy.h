#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <nlohmann/json.hpp>

#include "Core/CachedMesh.h"

// A merge-hierarchy root (docs/merge_hierarchies.md in SARP) read back from its cache: the merge tree, its portals, and
// when the root has a portal parts file, the parts the portals open. Everything here comes from the cache, so it
// describes what the builder wrote and not what any frame drew.
//
// The layout the builder writes, which this reverses:
//   - A node N owns a geometry group G_N and a page-less children group P_N. Every segment of G_N refines into P_N.
//     P_N carries (sphere(N), E(N)); G_N carries its parent's (sphere, error), or FLT_MAX at the root.
//   - Every child of N (a node's G_M, or a portal proxy) names G_N as its parentGroupId and carries P_N's bounds.
//   - A proxy is a CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY group whose firstMeshlet is its assembly instance, and the
//     instance's stackDepth is the part ordinal.
namespace clodeval {

// What one cut costs: a cut T draws the node groups with E(N) <= T < E(parent), and opens the portals of every node
// with E(N) > T. An opened part is drawn at T over the portal's scale.
struct MergeCutCost {
	float cut = 0.0f;
	uint32_t nodes = 0;
	uint64_t nodeTriangles = 0;
	uint64_t nodeClusters = 0;
	uint32_t portals = 0;  // opened: each is an instance for ray tracing and a part traversal for culling
	uint32_t coarsestPortals = 0;  // opened, but the part draws nothing finer than the triangles the node had merged
	uint64_t partTriangles = 0;
	uint64_t partClusters = 0;
	uint64_t partVoxelClusters = 0;
	bool partsKnown = false;  // part numbers are only counted when every part's cache was opened

	uint64_t Triangles() const { return nodeTriangles + partTriangles; }
	uint64_t Clusters() const { return nodeClusters + partClusters + partVoxelClusters; }
};

struct MergePart {
	std::string label;
	std::filesystem::path metadataPath;
	std::shared_ptr<const CachedMesh> cache;
	std::string error;
	uint32_t portals = 0;
	uint32_t rootNode = 0;  // part-local node the portals open
	DirectX::XMFLOAT4 lodSphere{};  // part space
	uint64_t coarseTriangles = 0;  // the top triangle groups: what the builder merged
	uint64_t leafTriangles = 0;
	float coarseError = 0.0f;  // the error of those triangles, part space

	// One segment of the part: it draws while enter <= cut < leave.
	struct Span {
		float enter = 0.0f;
		float leave = 0.0f;
		uint32_t triangles = 0;
		uint32_t clusters = 0;
		bool voxel = false;
		bool coarse = false;  // a top triangle group's, or a voxel group above them: no finer than what the builder merged
	};
	std::vector<Span> spans;
};

struct MergeNode {
	uint32_t group = 0;  // G_N
	uint32_t childrenGroup = UINT32_MAX;  // P_N; none for a folded node
	bool folded = false;  // its clusters are terminal: its children's exact triangles, with no portals under it
	int32_t parent = -1;  // node index, -1 at the root
	uint32_t level = 0;  // the builder's level: 1 for nodes over leaves
	uint32_t depth = 0;  // nodes above it
	float error = 0.0f;  // E(N)
	DirectX::XMFLOAT4 sphere{};  // sphere(N)
	uint32_t triangles = 0;
	uint32_t clusters = 0;
	uint64_t pageBytes = 0;
	std::vector<uint32_t> childNodes;
	std::vector<uint32_t> childPortals;
};

struct MergePortal {
	uint32_t group = 0;  // its proxy
	uint32_t instance = 0;
	uint32_t part = 0;
	uint32_t transform = 0;
	uint32_t materialBase = 0;
	int32_t parent = -1;  // node
	uint32_t depth = 0;  // nodes above it
	float scale = 1.0f;  // largest axis scale of the part-to-root transform
	DirectX::XMFLOAT3 position{};  // the transform's translation
	DirectX::XMFLOAT4 sphere{};  // the part's LOD sphere in root space, when its cache is open
};

struct MergeHierarchy {
	std::vector<MergeNode> nodes;
	std::vector<MergePortal> portals;
	std::vector<MergePart> parts;
	std::vector<uint32_t> paletteParts;
	int32_t root = -1;  // the top node with the largest error
	std::vector<uint32_t> tops;  // nodes with no parent: one, or with an error cap, several
	uint32_t topPortals = 0;  // portals with no parent: open at every distance
	uint32_t levels = 0;
	uint32_t materialSlots = 0;
	bool partsFile = false;  // the root had a portal parts file
	bool partsOpen = false;  // and every part in it opened
	std::vector<int32_t> nodeOfGroup;  // G_N or P_N -> node, else -1
	std::vector<int32_t> portalOfGroup;  // proxy -> portal, else -1
	std::vector<std::string> problems;  // where the cache breaks the rules above

	uint64_t NodeTriangles() const;
	// Every portal's part at its coarsest: what the builder was given. Needs the parts.
	uint64_t InputTriangles() const;
	MergeCutCost CostAt(float cut) const;
	// Cuts from the root's error down by factors of two, until every part draws its finest.
	std::vector<MergeCutCost> Sweep(uint32_t maxSteps = 40) const;
};

// A root is a mesh whose assembly instances are part portals.
bool IsMergeRoot(const ClusterLODPrebuiltData& prebuilt);

// Parts are shared by many roots (a region's walls, every region's rocks), so a pass over many roots opens and measures
// each once. Safe to use from many threads; a part being measured blocks only the threads that want that part.
class MergePartLibrary {
public:
	// The part's cache and measurements. Its label, portals, rootNode and lodSphere are per root and left empty. A forked
	// part is measured in `branch` (CLOD_BRANCH_*), the branch of the root that opens it.
	std::shared_ptr<const MergePart> Get(const std::filesystem::path& metadataPath, uint32_t branch = 3u);
	size_t Size() const;

private:
	struct Slot {
		std::once_flag once;
		std::shared_ptr<const MergePart> part;
	};
	mutable std::mutex m_mutex;
	std::unordered_map<std::string, std::shared_ptr<Slot>> m_slots;
};

// Null when the mesh is not a root. Parts are opened from the root's portal parts file when loadParts is set and it has
// one, through `library` when given; a part that cannot be opened is recorded with its error and leaves partsOpen false.
std::unique_ptr<MergeHierarchy> ReadMergeHierarchy(const CachedMesh& root, bool loadParts, MergePartLibrary* library = nullptr);

// What keeps a root from paying for itself, judged at `reachCut`: the coarsest cut the renderer ever asks of it (the
// world size of a threshold's worth of pixels at the farthest distance a region is drawn from). The census sorts and
// totals these over every root.
struct MergeIssues {
	float reachCut = 0.0f;
	// Nodes whose geometry would only draw at cuts over reachCut, so they never draw: traversal and pages for nothing.
	uint32_t unreachableNodes = 0;
	uint64_t unreachableTriangles = 0;
	uint32_t unreachableLevels = 0;  // tree depth (from the root) that is always opened past
	bool rootDraws = false;  // the root node draws at reachCut: the region is ever a single draw
	// Portals under a node that is always open: the part is a separate instance at every distance.
	uint32_t alwaysOpenPortals = 0;
	uint32_t alwaysOpenCoarsestPortals = 0;  // and at reachCut its part still draws only its coarsest
	// Portals under a node with a sentinel error (its children had none): they never open, and the node holds a copy.
	uint32_t deadPortals = 0;
	uint64_t deadPortalTriangles = 0;
	// Nodes drawn over less than kHardlyDrawn of cut: a level that does not earn its pages.
	uint32_t hardlyDrawnNodes = 0;
	uint64_t hardlyDrawnTriangles = 0;
	// Siblings switch together, at the coarsest child's error. Spread: E(N) over the smallest child error, so the
	// factor by which the finest sibling is opened too early. Geometric mean over nodes, and the worst.
	double siblingSpreadMean = 1.0;
	double siblingSpreadMax = 1.0;
	uint32_t rootPortals = 0;  // portals at the top, with no parent: open at every distance
	double portalDepthMean = 0.0;
	uint32_t portalDepthMax = 0;
	MergeCutCost atReach;  // the region from its farthest distance
};
MergeIssues FindMergeIssues(const MergeHierarchy& hierarchy, float reachCut);

// Every number the merge report prints, for scripts.
nlohmann::json MergeReportJson(const MergeHierarchy& hierarchy, const CachedMesh& root);
std::string FormatMergeReport(const MergeHierarchy& hierarchy, const CachedMesh& root);

}
