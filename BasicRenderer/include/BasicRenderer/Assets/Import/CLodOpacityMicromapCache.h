#pragma once

// Opacity micromaps baked offline for a CLod mesh's alpha-tested clusters: a sidecar to the mesh's .clodbin, one per
// material they were baked against, named <container stem>.<identity>.clodomm. The path tracer's cluster BVH builds
// each group's micromap array from it (CLodRayTracingSystem); a mesh drawn with another material finds no file of its
// identity and leaves the alpha test to any-hit.
//
// A group's blob is laid out for upload as it is: the micromaps' descriptors (D3D12_RAYTRACING_OPACITY_MICROMAP_DESC),
// then one index unit per cluster (kOpacityMicromapIndexUnitBytes: an int32 per triangle, a micromap of the group's
// array or a special index -1 to -4), then the micromaps' data, which the descriptors' byte offsets point into.
// Clusters are in the order the BVH gives a group's CLAS: its segments, each segment's meshlets.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CLodCache {

// 2: the exact bilinear range over each micro-triangle, and a level per triangle by its texel area.
inline constexpr uint32_t kOpacityMicromapCacheVersion = 2;
// Bytes of indices per cluster: 128 triangles of int32. The BVH's index pool unit (CLOD_RT_OMM_INDEX_UNIT_BYTES).
inline constexpr uint32_t kOpacityMicromapIndexUnitBytes = 512;
inline constexpr uint32_t kOpacityMicromapMaxTrianglesPerCluster = kOpacityMicromapIndexUnitBytes / 4u;
// D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE.
inline constexpr uint16_t kOpacityMicromapFormat4State = 2;

// D3D12_RAYTRACING_OPACITY_MICROMAP_DESC, as the array build reads it.
struct OpacityMicromapDesc {
	uint32_t byteOffset = 0; // Into the group's data.
	uint16_t subdivisionLevel = 0;
	uint16_t format = kOpacityMicromapFormat4State;
};
static_assert(sizeof(OpacityMicromapDesc) == 8);

// What a material's alpha test is, for the bake and for finding its bake again: the alpha textures' processed-cache
// keys (TextureAsset::PipelineCacheKey), the cutoff and the alpha factor. Sampler address modes aren't part of it: every
// static material texture samples with wrap (AssetBridge), and the bake assumes it. Where clamping would differ, the
// conservative bounds already read both, so only an unknown micro-triangle's opaque or transparent subtype could.
struct OpacityMicromapMaterialKey {
	std::string_view baseColorTexture;  // Empty when the material has none.
	std::string_view opacityTexture;
	uint32_t baseColorUvSet = 0;
	uint32_t opacityUvSet = 0;
	float alphaCutoff = 0.5f;
	float alphaFactor = 1.0f;           // baseColorFactor.a
};

// A nonzero 31-bit hash of the key and kOpacityMicromapCacheVersion. The subdivision level is the file's: the BVH builds
// whatever level was baked.
uint32_t OpacityMicromapIdentity(const OpacityMicromapMaterialKey& key);

// <container stem>.<identity, 8 hex digits>.clodomm, beside the container.
std::wstring OpacityMicromapPath(const std::wstring& containerPath, uint32_t identity);
// The container's size as a reader would see it: its cache pack entry, else the loose file. 0 when there's neither.
uint64_t OpacityMicromapContainerBytes(const std::wstring& containerPath);

struct OpacityMicromapFileInfo {
	uint32_t identity = 0;
	uint32_t subdivisionLevel = 0;
	uint16_t format = kOpacityMicromapFormat4State;
	uint64_t containerBytes = 0; // The .clodbin's size when baked: a rebuilt container makes the file stale.
	uint32_t groupCount = 0;     // The mesh's groups; those without micromaps have no blob.
};

// One group's micromaps, for writing.
struct OpacityMicromapGroupInput {
	uint32_t groupLocalIndex = 0;
	uint32_t clusterCount = 0;
	std::vector<OpacityMicromapDesc> descs;
	std::vector<int32_t> indices; // clusterCount * kOpacityMicromapMaxTrianglesPerCluster.
	std::vector<std::byte> data;
};

// Writes atomically (a temporary renamed into place), and tells the cache pack (CachePack::NoteFileChanged). False with
// a reason on failure.
bool SaveOpacityMicromaps(const std::wstring& path, const OpacityMicromapFileInfo& info, std::span<const OpacityMicromapGroupInput> groups,
	std::string* error = nullptr);

// A group's micromaps as stored: views into the mapped file.
struct OpacityMicromapGroupView {
	uint32_t clusterCount = 0;
	std::span<const OpacityMicromapDesc> descs;
	std::span<const std::byte> indices; // clusterCount index units.
	std::span<const std::byte> data;
	std::span<const std::byte> blob;    // descs, then indices, then data: what uploads.
	uint32_t descsOffset = 0;           // In blob.
	uint32_t indicesOffset = 0;
	uint32_t dataOffset = 0;
	// How many micromaps of each subdivision level (0 to 12), for the array's prebuild.
	std::array<uint32_t, 13> levelCounts{};
};

// A .clodomm, read from the cache pack when it holds it and mapped from the loose file otherwise, and checked whole on
// opening: every group's blob in the file, every index in range, every micromap's data inside its group's. Thread
// safe once open.
class OpacityMicromapFile {
public:
	~OpacityMicromapFile();
	OpacityMicromapFile(const OpacityMicromapFile&) = delete;
	OpacityMicromapFile& operator=(const OpacityMicromapFile&) = delete;

	// Null with a reason when there is no such file, it's stale (another identity or container size) or malformed.
	static std::shared_ptr<const OpacityMicromapFile> Open(const std::wstring& path, uint32_t expectedIdentity, uint64_t expectedContainerBytes,
		std::string* error = nullptr);

	const OpacityMicromapFileInfo& Info() const { return m_info; }
	// False when the group has no micromaps (not alpha tested, or nothing there any-hit couldn't decide).
	bool Group(uint32_t groupLocalIndex, OpacityMicromapGroupView& out) const;

private:
	struct Mapping;
	OpacityMicromapFile() = default;

	std::unique_ptr<Mapping> m_mapping;
	OpacityMicromapFileInfo m_info{};
	std::vector<OpacityMicromapGroupView> m_groups;
};

}
