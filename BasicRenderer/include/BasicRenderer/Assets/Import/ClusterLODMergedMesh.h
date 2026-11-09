#pragma once

// Merged meshes (docs/merge_hierarchies.md in SARP): CLod meshes whose part portals open parts that other meshes hold.
//
// A merged mesh is an ordinary CLod mesh. Its cache names what its portals open (ClusterLODPrebuiltData::externalParts)
// and which part's material each palette slot takes (paletteParts). Whoever resolves the host addresses in that table
// to registered meshes builds it here; this file also extracts what a mesh contributes to a merge.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include <directxmath.h>

#include <BasicRenderer/Assets/ClusterLODTypes.h>
#include <BasicRenderer/Assets/Import/ClusterLODMergeHierarchy.h>

class Mesh;

// What a part mesh contributes to a merge: its coarsest triangles (its top triangle groups), in part space.
struct ClusterLODMergePartGeometry
{
	bool valid = false;
	std::string reason;
	ClusterLODMergeGeometry geometry;
	// A forked part (CLOD_GROUP_FLAG_BRANCH_*): what its voxels were made from, the shared triangles at the fork, with the
	// error at which the part switches to voxels (voxelSource.error). Empty when the part has no voxel branch.
	ClusterLODMergeGeometry voxelSource;
	bool hasVoxelBranch = false;
	uint32_t rootNode = 0;
	uint32_t traversalDepth = 0;
	DirectX::XMFLOAT4 lodSphere{ 0, 0, 0, 0 };
	// The part's full-detail triangles, estimated: its finest (depth 0) triangle meshlets times the triangles per meshlet
	// of the coarse groups read above, so that only the coarse pages are read. Exact when the finest groups are the top.
	uint64_t finestTriangles = 0;
};

// A part's geometry, from wherever the caller keeps it (null for none).
using ClusterLODPartGeometryResolver = std::function<std::shared_ptr<const ClusterLODMergePartGeometry>(const Mesh&)>;

// Reads the mesh's coarse pages from its CLod cache (or the cache pack). Not valid for skinned meshes and part-library
// assemblies. A merged mesh (one that names external parts) is valid too, as an item to embed: its coarsest is its top
// groups and, at its top, what its portals open at their coarsest (the parts' own geometry, from `resolvePart` when
// given, else extracted here; its portal parts must be set) and what embedded meshes draw at theirs.
[[nodiscard]] ClusterLODMergePartGeometry ExtractClusterLODMergePartGeometry(const Mesh& mesh, const ClusterLODPartGeometryResolver& resolvePart = {});

// The cache identity and node count of a part as a merged mesh's table records it. The host address (asset key,
// variant, part and mesh) is the caller's to fill in.
[[nodiscard]] ClusterLODExternalPart DescribeClusterLODExternalPart(const Mesh& part);

// Empty when the registered mesh is the part the table names; otherwise why not (it was rebuilt, or is another mesh).
[[nodiscard]] std::string ClusterLODExternalPartMismatch(const ClusterLODExternalPart& expected, const Mesh& part);

// A merged mesh's cache data with all of its pages read (from the cache or the pack), for a merge that embeds it.
[[nodiscard]] bool LoadClusterLODMergeEmbeddedMesh(ClusterLODPrebuiltData data, ClusterLODMergeEmbeddedMesh& out, std::string* outReason = nullptr);

// The merged mesh from its cache data, opening `parts` (in externalParts order) and drawing with the palette their
// materials make. Null, with the reason, when the parts do not match the table.
[[nodiscard]] std::shared_ptr<Mesh> CreateClusterLODMergedMesh(
	ClusterLODPrebuiltData prebuiltData,
	std::span<const std::shared_ptr<Mesh>> parts,
	std::string* outReason = nullptr);
