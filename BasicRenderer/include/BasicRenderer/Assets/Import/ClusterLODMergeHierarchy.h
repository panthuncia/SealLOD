#pragma once

// Merge hierarchies (docs/merge_hierarchies.md): many placed parts become one hierarchy root.
//
// Nearby items merge into nodes whose geometry is their children's coarsest representations, simplified together;
// nodes merge again, up to the root. A node's clusters refine into all of its children at once. A leaf is a part
// portal: when the cut passes it, traversal opens the part (a registered mesh, never copied) through the item's
// transform, and the part's own hierarchy takes over. The builder knows nothing about where items come from or how
// they were grouped into a region.
//
// Voxel forks: an item whose part forks (CLOD_GROUP_FLAG_BRANCH_*) has a second representation for raster views with
// voxel output. The hierarchy is then built twice, into one root: a triangle branch from every item's triangles, and a
// voxel branch in which such items are voxels. A voxel-branch node voxelizes its voxel children together (beside its
// triangle group for the others), so a stand of foliage becomes one voxel group at distance.
//
// Nesting: an item may be a merged mesh (one this builder made). A portal cannot open it, because a portal replaces the
// traversal's assembly transform instead of composing with it, so the root embeds it instead (flattens it): its groups,
// nodes and pages are copied into the root and reached through a non-portal instance under the item's transform, its
// portals' transforms are composed with that transform, its palette is appended to the root's, and the parts it opens
// join the root's external parts. Every portal still opens a leaf part, one level deep, and the forked branches of the
// embedded mesh are copied as they are.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <directxmath.h>

#include <BasicRenderer/Assets/ClusterLODTypes.h>

// An item's coarsest representation, in its part's space.
struct ClusterLODMergeGeometry
{
	std::vector<DirectX::XMFLOAT3> positions;
	std::vector<DirectX::XMFLOAT3> normals;
	std::vector<DirectX::XMFLOAT2> uvs;    // UV set 0; empty means zeros
	std::vector<DirectX::XMFLOAT3> colors; // empty means white
	std::vector<uint32_t> indices;
	std::vector<uint16_t> triangleSlots;   // part-local material slot per triangle; empty means slot 0
	float error = 0.0f;                    // these triangles' error against the part's finest, in part space
};

struct ClusterLODMergeItem
{
	uint32_t geometry = 0;           // into the geometries span
	uint32_t part = 0;               // the root's part ordinal (Mesh::SetPortalParts)
	uint32_t partNode = 0;           // the part-local node the portal opens: its root, or an inner part root
	uint32_t partTraversalDepth = 0; // traversal levels below partNode
	DirectX::XMFLOAT4X4 partToHierarchy = {}; // row-vector: p_hierarchy = p_part * M
	DirectX::XMFLOAT4 partLodSphere = {};      // part-space LOD sphere of what the portal opens
	uint32_t materialBase = 0;       // root palette index of the part's slot 0
	uint32_t flags = 0;              // added to the portal's ClusterLODAssemblyInstance::flags
	// A forked part's voxel branch: the triangles its voxels were made from (into the geometries span), and the cut at
	// which the part switches from them to voxels, in part space. UINT32_MAX: the part has no voxel branch.
	uint32_t voxelGeometry = UINT32_MAX;
	float voxelError = 0.0f;
	// Into ClusterLODMergeSettings::embeddedMeshes: the item is that merged mesh, embedded rather than opened. Its
	// geometry is the merged mesh's coarsest (ExtractClusterLODMergePartGeometry); `part` is unused, and the builder
	// assigns materialBase (where the merged mesh's palette lands in the root's).
	uint32_t embeddedMesh = UINT32_MAX;
};

// A merged mesh items embed: its cache data, with its external parts and palette, and every one of its pages.
struct ClusterLODMergeEmbeddedMesh
{
	ClusterLODPrebuiltData data;
	std::vector<std::vector<std::byte>> meshPageBlobs; // in mesh page order
};

struct ClusterLODMergeSettings
{
	uint32_t fanout = 8;              // children per node
	uint32_t nodeTriangleBudget = 1024;
	uint32_t rootMaxGroups = 1;       // the root may hold this many nodes' worth of triangles
	uint32_t maxLevels = 16;
	float errorGrowth = 2.0f;         // a level merges what is within this factor of its smallest error
	float weldRelativeTolerance = 0.25f; // weld positions within this fraction of a node's target error
	uint32_t materialSlotCount = 1;   // the root palette's size
	// What the merged mesh references, copied into its metadata (ClusterLODPrebuiltData::externalParts, paletteParts):
	// the part each item ordinal names, and the part whose material each palette slot takes. When given, every item's
	// part must be in range and the palette must have materialSlotCount slots. Embedded meshes append to both, so the
	// root's tables (and its materialSlotCount) can be larger than these; with only embedded items, both may be empty.
	std::vector<ClusterLODExternalPart> externalParts;
	std::vector<uint32_t> paletteParts;
	// The merged meshes items embed (ClusterLODMergeItem::embeddedMesh). Not copied: they must outlive the build.
	std::span<const ClusterLODMergeEmbeddedMesh> embeddedMeshes;
	uint32_t traversalNodeWidth = 8;
	// No node is built with an error over this: the coarsest cut a renderer will ask of the hierarchy. What cannot merge
	// under it stays at the top: nodes that draw from afar, and portals that are open at every distance. Infinity
	// merges up to one root, at rootMaxGroups times the budget.
	float maxNodeError = std::numeric_limits<float>::infinity();
	// A node the budget would take over maxNodeError may spend up to this many budgets of triangles to stay under it.
	uint32_t capBudgetFactor = 4;
	// A node draws while E(N) <= cut < E(parent). A child node whose parent's error is less than this factor above its
	// own would hardly ever draw, so it is dissolved: its children move up to the parent and it is not built.
	float minNodeDrawWindow = 1.2f;
	// A level takes at least a full fanout of the smallest entities, but never ones over errorGrowth^this times the
	// smallest: siblings switch together, so the finest is opened early by that factor.
	float maxSiblingSpreadLevels = 3.0f;
	// Voxel branch: whether nodes voxelize their voxel children (false: voxel items stay portals there), the most cubes
	// a node's voxels may take, and the coverage rays per cell.
	bool voxelNodes = true;
	uint32_t nodeVoxelCubeBudget = 512;
	uint32_t voxelRaysPerCell = 8;
};

struct ClusterLODMergeStats
{
	uint32_t items = 0;
	uint32_t nodes = 0;
	uint32_t levels = 0;
	uint64_t inputTriangles = 0;  // items' coarsest representations
	uint64_t nodeTriangles = 0;   // all nodes' geometry
	uint32_t rootTriangles = 0;
	float rootError = 0.0f;
	uint32_t sloppyNodes = 0;     // nodes that needed the sloppy fallback to reach their budget
	uint32_t topNodes = 0;        // nodes with no parent: drawn from afar
	uint32_t topPortals = 0;      // portals with no parent: open at every distance
	uint32_t foldedNodes = 0;     // nodes that keep their children's exact triangles, with no portals under them
	uint32_t foldedItems = 0;     // items inside folded nodes: no portal
	uint32_t rejectedNodes = 0;   // groupings whose node would have been over maxNodeError
	uint32_t dissolvedNodes = 0;  // nodes whose parent switched too close to them (minNodeDrawWindow)
	// Voxel forks. The counts above are the triangle branch's; these are the voxel branch's.
	uint32_t voxelItems = 0;            // items with a voxel branch
	uint32_t voxelBranchNodes = 0;      // nodes of the voxel branch
	uint32_t voxelNodes = 0;            // voxel-branch nodes with voxel groups
	uint64_t voxelCubes = 0;
	uint64_t voxelBranchNodeTriangles = 0;
	uint32_t voxelBranchTopPortals = 0;
	// Nesting: items that embed a merged mesh (and still draw through it: an embedded item inside a folded node is not
	// copied), and the groups those copies added.
	uint32_t embeddedItems = 0;
	uint32_t embeddedGroups = 0;
};

// An item whose part has no coarser representation than its own triangles (error 0) needs no portal when it merges
// with other such items while their union fits the budget: the node keeps their exact triangles (a folded node).
//
// Throws on invalid input. Needs at least two items. Returns empty artifacts, with stats.nodes 0, when no two items can
// merge under maxNodeError: the caller should keep them as they are.
ClusterLODPrebuildArtifacts BuildClusterLODMergeHierarchy(
	std::span<const ClusterLODMergeGeometry> geometries,
	std::span<const ClusterLODMergeItem> items,
	const ClusterLODMergeSettings& settings,
	ClusterLODMergeStats* stats = nullptr);
