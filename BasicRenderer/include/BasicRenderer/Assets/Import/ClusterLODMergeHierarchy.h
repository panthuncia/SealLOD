#pragma once

// Merge hierarchies (docs/merge_hierarchies.md): many placed parts become one hierarchy root.
//
// Nearby items merge into nodes whose geometry is their children's coarsest representations, simplified together;
// nodes merge again, up to the root. A node's clusters refine into all of its children at once. A leaf is a part
// portal: when the cut passes it, traversal opens the part (a registered mesh, never copied) through the item's
// transform, and the part's own hierarchy takes over. The builder knows nothing about where items come from or how
// they were grouped into a region.

#include <cstdint>
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
	uint32_t traversalNodeWidth = 8;
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
};

// Throws on invalid input. Needs at least two items.
ClusterLODPrebuildArtifacts BuildClusterLODMergeHierarchy(
	std::span<const ClusterLODMergeGeometry> geometries,
	std::span<const ClusterLODMergeItem> items,
	const ClusterLODMergeSettings& settings,
	ClusterLODMergeStats* stats = nullptr);
