#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Core/CachedMesh.h"
#include "Core/MergeHierarchy.h"

// What the viewer knows about each cached hierarchy, computed once from the cache on a background thread.
// Nothing here touches the GPU, so everything it reports is what the build wrote and not what happens to be resident.
namespace clodcapture {

inline constexpr float kNoError = std::numeric_limits<float>::infinity();

// World-space cut T = threshold * world size of a pixel. A segment of group G that refines into R draws
// while R.error <= T < G.error. So a group appears at the error of its finest child and leaves at its own.
struct GroupInfo {
	float enterError = 0.0f;  // the group starts drawing at this cut. 0: it holds original geometry, so it draws from the very fine end
	float leaveError = kNoError;  // and stops at this one. kNoError: it is a root
	float firstVoxelError = kNoError;  // the cut at which this part of the mesh became voxels. kNoError: it never does
	float voxelWidth = 0.0f;
	int32_t depth = 0;
	int32_t voxelLevel = -1;  // -1: triangles. 0: first voxel group above triangles, then one more per voxel step
	uint32_t triangles = 0;
	uint32_t cells = 0;
	uint32_t cubes = 0;
	uint32_t firstChild = 0, childCount = 0;
	uint32_t firstParent = 0, parentCount = 0;
	bool voxel = false;
	bool assemblyVoxel = false;
	bool terminal = false;  // has segments of original geometry
	// Merge-hierarchy roots only (docs/merge_hierarchies.md in SARP).
	int32_t mergeNode = -1;  // the node whose geometry (or children group) this is
	int32_t mergePortal = -1;  // the portal this proxy is
	bool mergeChildren = false;  // a page-less children group: never drawn
};

struct LevelInfo {
	int32_t depth = 0;
	uint32_t triangleGroups = 0, voxelGroups = 0;
	uint64_t triangles = 0, cells = 0;
	float minError = kNoError, maxError = 0.0f;
	float minWidth = kNoError, maxWidth = 0.0f;
};

// A place where triangles hand over to voxels: the first voxel group, and what it replaces.
struct HandOver {
	uint32_t group = 0;
	float error = 0.0f;
	uint32_t replacedTriangles = 0;
};

struct MeshInfo {
	std::unique_ptr<clodeval::CachedMesh> cache;
	std::string name;
	std::string shortName;  // fits a table: an assembly's prim and subset say nothing, since all of them share one
	bool assembly = false;
	std::vector<GroupInfo> groups;
	std::vector<uint32_t> links;  // children and parents, addressed by GroupInfo
	std::vector<LevelInfo> levels;
	std::vector<HandOver> handOvers;
	uint32_t triangleGroups = 0, voxelGroups = 0;
	uint64_t leafTriangles = 0;
	int32_t maxDepth = 0;
	int32_t maxVoxelLevel = -1;

	// A merge root's tree and report, or a part some root in the index opens.
	std::shared_ptr<const clodeval::MergeHierarchy> merge;
	std::string mergeReport;
	int32_t mergeLevels = 0;
	std::string portalUse;  // for a part: which roots open it, and how often

	std::vector<uint32_t> Children(uint32_t group) const;
	std::vector<uint32_t> Parents(uint32_t group) const;
};

// One voxel cube as the cache stores it.
struct CubeInfo {
	uint32_t coord[3]{};
	uint64_t occupancy = 0;
	float opacitySum = 0.0f;
	int32_t refinedGroup = -1;
	uint32_t clusterCubes = 0;
	float voxelWidth = 0.0f;
	uint32_t resolution = 0;
	float aabbMin[3]{};
};

class HierarchyIndex {
public:
	~HierarchyIndex();

	void StartLoading(std::filesystem::path cacheRoot);
	// These caches only, each a .clodmeta.
	void StartLoading(std::vector<std::string> metadataFiles);
	bool Ready() const { return m_ready.load(); }
	uint32_t Loaded() const { return m_loaded.load(); }
	uint32_t Total() const { return m_total.load(); }
	std::string Error() const;

	// Only once Ready.
	const std::vector<MeshInfo>& Meshes() const { return m_meshes; }
	int32_t Find(const std::string& sourceIdentifier, const std::string& primPath, const std::string& subset) const;
	int32_t MaxDepth() const { return m_maxDepth; }
	int32_t MaxMergeLevel() const { return m_maxMergeLevel; }
	int32_t MaxVoxelLevel() const { return m_maxVoxelLevel; }
	float MinHandOver() const { return m_minHandOver; }
	float MaxHandOver() const { return m_maxHandOver; }

	std::optional<CubeInfo> FindCube(int32_t mesh, uint32_t group, uint32_t pageIndex, uint32_t pageCluster, uint32_t cube) const;

private:
	void Load(std::vector<std::filesystem::path> paths);

	std::thread m_thread;
	std::atomic<bool> m_ready{ false };
	std::atomic<uint32_t> m_loaded{ 0u }, m_total{ 0u };
	mutable std::mutex m_errorMutex;
	std::string m_error;
	std::vector<MeshInfo> m_meshes;
	std::unordered_map<std::string, int32_t> m_byIdentity;
	int32_t m_maxDepth = 0;
	int32_t m_maxMergeLevel = 0;
	int32_t m_maxVoxelLevel = -1;
	float m_minHandOver = kNoError, m_maxHandOver = 0.0f;
};

}
