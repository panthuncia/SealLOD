#pragma once

// Standalone header containing all ClusterLOD data types and the
// MeshIngestBuilder class.  No GPU / RHI / DeletionManager / BufferView
// dependencies - only standard library, DirectXMath, meshoptimizer, and the
// CLod shader types header.

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <directxmath.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <meshoptimizer.h>

#include "BasicRenderer/Assets/ClusterLODShaderTypes.h"
#include <BasicRenderer/Scene/VertexFlags.h>
#include <BasicRenderer/Assets/MeshData.h>
#include "BasicRenderer/Scene/Animation/DynamicWindMetadata.h"
#include "BasicRenderer/Scene/Animation/SkeletonArtifact.h"

// Forward declarations for GPU-side types only needed by MeshIngestBuilder::Build()
class Material;
class Mesh;

enum class MeshCpuDataPolicy {
	Retain,
	ReleaseAfterUpload,
};

struct VoxelCoverageHit
{
	uint32_t triangleIndex = 0;
	DirectX::XMFLOAT3 position{};
	DirectX::XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT2 uv{};
	DirectX::XMFLOAT3 trianglePositions[3]{};
	DirectX::XMFLOAT2 triangleUvs[3]{};
	float barycentrics[3]{};
};

struct VoxelCoverageMaterialSample
{
	bool accepted = true;
	bool overrideNormal = false;
	DirectX::XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
	float weight = 1.0f;
};

using VoxelCoverageMaterialSampler = std::function<VoxelCoverageMaterialSample(const VoxelCoverageHit&)>;

namespace clod_texture { struct Texture; }

// Stage 6 (docs/clod-appearance-stage6.md): the base colour a primitive's material draws, for the colour-weighted texture term
// (lodErrorChannelsTextureColor). Importers that can describe their materials fill it and hand it to the builder beside the
// geometry (MeshIngestBuilder::SetMaterialAppearance); the builder never decodes images or knows the importer. Without it (or
// when it cannot decode) the texture term stays binary.
struct ClusterLODMaterialAppearance
{
	std::array<float, 4> baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f }; // linear RGBA
	bool hasBaseColorTexture = false;
	uint32_t baseColorTexCoord = 0;  // the UV set the texture reads; only UV set 0 is weighted (others fall back to binary)
	// The texture's UV transform (glTF KHR_texture_transform) as a 2x3 affine matrix, row-major: u' = m0 u + m1 v + m2,
	// v' = m3 u + m4 v + m5. Identity without one.
	bool hasBaseColorUvTransform = false;
	std::array<float, 6> baseColorUvTransform{ 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
	bool hasVertexColors = false;    // vertex colours multiply the base colour, which the term does not model (falls back to binary)
	bool alphaMasked = false;
	float alphaCutoff = 0.5f;
	// Hash of all of the above and the image's content. Importers put it in the mesh's cache identity
	// (MeshCacheIdentity::appearanceHash), so a changed texture rebuilds what was weighted by it.
	uint64_t contentHash = 0;
	// Decodes the base colour image as RGBA8 sRGB with its mip chain (clod_texture::BuildMips); null when it cannot. Called at most
	// once per build, from the building thread.
	std::function<std::shared_ptr<const clod_texture::Texture>()> decodeBaseColor;
};

// Traversal types

struct ClusterLODTraversalMetric
{
	DirectX::XMFLOAT4 cullingSphere = {};
	DirectX::XMFLOAT4 lodBoundingSphere = {};
	float maxQuadricError = 0;
	// Error channels (stage 3, docs/clod-simplification-validation.md), in what were padding words: a segment leaf holds its
	// owning group's measured geometric and appearance errors (mesh units), an internal node the largest of its subtree's, with
	// CLOD_NODE_CHANNELS_PRESENT set. Without channels all three are 0; the upload then fills the GPU copy so that every
	// selection rule reduces to maxQuadricError (MeshManager::AddMesh).
	float maxMeasuredGeometricError = 0;
	float maxAppearanceError = 0;
	uint32_t channelFlags = 0;
};
static_assert(sizeof(ClusterLODTraversalMetric) == 48, "ClusterLODTraversalMetric layout is shared with workGraphCulling.hlsl");
static constexpr uint32_t CLOD_NODE_CHANNELS_PRESENT = 1u << 0;

struct ClusterLODNodeRange
{
	uint32_t isGroup = 0;        // 0=internal, 1=voxel-group-leaf, 2=segment-leaf
	uint32_t indexOrOffset = 0;  // segment-leaf: mesh-local segment index; internal: child offset
	uint32_t countMinusOne = 0;  // internal: childCount-1; leaf: unused
	uint32_t ownerGroupId = 0;   // segment-leaf: mesh-local group index (for page resolution + streaming)
};

struct ClusterLODNode
{
	ClusterLODNodeRange        range{};
	ClusterLODTraversalMetric  traversalMetric{};
};

struct ClusterLODNodeSkinningInfo
{
	uint32_t boneListOffset = 0;
	uint16_t boneCount = 0;
	uint16_t flags = 0;
};
static_assert(sizeof(ClusterLODNodeSkinningInfo) == 8, "ClusterLODNodeSkinningInfo must be 8 bytes");

static constexpr uint16_t CLOD_NODE_SKINNING_FLAG_OVERFLOW = 1u << 0;
static constexpr uint16_t CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK = 1u << 1;
static constexpr uint32_t CLOD_NODE_BONE_LIMIT_DEFAULT = 16u;
static constexpr uint32_t CLOD_NODE_BONE_LIMIT_HARD_MAX = 64u;

struct ClusterLODNodeRangeAlloc
{
	uint32_t offset = 0;
	uint32_t count = 0;
};

// Disk / Cache types

struct ClusterLODDiskChunkSpan
{
	uint64_t offset = 0;
	uint64_t sizeBytes = 0;
};

struct ClusterLODGroupDiskLocator
{
	uint64_t blobOffset = 0;
	uint32_t blobSizeBytes = 0;
	uint32_t reserved = 0;
};

struct ClusterLODCacheSource
{
	std::string sourceIdentifier;
	std::string primPath;
	std::string subsetName;
	uint64_t buildConfigHash = 0;
	std::wstring containerFileName;
};

struct ClusterLODAssemblySkeletonData
{
	std::vector<std::string> jointNames;
	std::vector<int32_t> parentIndices;
	std::vector<DirectX::XMFLOAT4X4> inverseBindMatrices;
	std::vector<DirectX::XMFLOAT4X4> restLocalMatrices;
	std::vector<DirectX::XMFLOAT4X4> bindGlobalMatrices;
	std::vector<uint32_t> windSimulationGroupIndices;
	std::string windProfileIdentity;
	DynamicWindMetadata dynamicWindMetadata;

	bool Empty() const
	{
		return jointNames.empty();
	}

	void Clear()
	{
		jointNames.clear();
		parentIndices.clear();
		inverseBindMatrices.clear();
		restLocalMatrices.clear();
		bindGlobalMatrices.clear();
		windSimulationGroupIndices.clear();
		windProfileIdentity.clear();
		dynamicWindMetadata = {};
	}
};

// Voxel group data model

struct VoxelCell
{
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t z = 0;
	float    opacity = 0.0f;
	DirectX::XMFLOAT4 sggxAxisAndSigmas = { 0.0f, 0.0f, 1.0e-4f, 0.5f };
	DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
	uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	int32_t refinedGroup = -1;
};

struct VoxelGroupPayload
{
	uint32_t resolution = 0;
	DirectX::XMFLOAT3 aabbMin{};
	DirectX::XMFLOAT3 aabbMax{};
	float voxelWidth = 0.0f;
	DirectX::XMFLOAT2 uvDensity = { 0.0f, 0.0f };
	std::vector<VoxelCell> activeCells;
};

struct VoxelGroupPackedMetadata
{
	DirectX::XMFLOAT4 aabbMinAndVoxelWidth{}; // xyz=min, w=voxel width
	DirectX::XMFLOAT4 aabbMaxAndError{};      // xyz=max, w=accepted voxel error
	uint32_t firstCluster = 0;
	uint32_t clusterCount = 0;
	uint32_t firstCube = 0;
	uint32_t cubeCount = 0;
	uint32_t resolution = 0;
	uint32_t flags = 0;
	DirectX::XMFLOAT2 uvDensity = { 0.0f, 0.0f };
};

struct VoxelGroupMapping
{
	std::vector<int32_t> groupToPayloadIndex;
	std::vector<VoxelGroupPayload> payloads;
	std::vector<int32_t> groupToPackedMetadataIndex;
	std::vector<VoxelGroupPackedMetadata> packedGroupMetadata;
	std::vector<CLodVoxelClusterRecord> packedClusterRecords;
	std::vector<CLodVoxelCubeRecord> packedCubeRecords;
	std::vector<CLodVoxelAttributeSample> packedAttributeSamples;
};

// Prebuilt / Cache payload types

struct ClusterLODPartRecord
{
	uint32_t groupBase = 0;
	uint32_t groupCount = 0;
	uint32_t nodeBase = 0;
	uint32_t nodeCount = 0;
	uint32_t transformBase = 0;
	uint32_t transformCount = 0;
	uint32_t instanceBase = 0;
	uint32_t instanceCount = 0;
	uint32_t rootNode = 0;
	uint32_t flags = 0;
};

static constexpr uint32_t CLOD_PART_RECORD_FLAG_ROOT = 1u << 0;

// Reserved: a part that has external parts of its own, opened through a GPU-composed nested portal. Merging flattens
// such parts instead (docs/merge_hierarchies.md), so registration rejects the flag for now.
static constexpr uint32_t CLOD_EXTERNAL_PART_FLAG_NESTED = 1u << 0;

// A part another CLod mesh holds, which this mesh's part portals open (CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL, whose
// stackDepth is the part's index in ClusterLODPrebuiltData::externalParts). Parts are shared between the meshes that
// open them, so they stay in their own caches. The host address is what the static asset source resolves (asset key,
// material variant, part and mesh of the asset); the cache identity is what the portal's node numbering was built
// against, so a rebuilt part that no longer matches is caught.
struct ClusterLODExternalPart
{
	std::string sourceIdentifier;
	std::string primPath;
	std::string subsetName;
	uint64_t buildConfigHash = 0;
	std::string assetKey;
	uint64_t variantHash = 0;
	std::vector<std::byte> variant; // the host's encoding of the material variant; empty for the asset's own materials
	uint32_t partIndex = 0;
	uint32_t meshIndex = 0;
	uint32_t nodeCount = 0;
	uint32_t flags = 0;

	bool operator==(const ClusterLODExternalPart&) const = default;
};

struct ClusterLODPrebuiltData
{
	std::vector<ClusterLODGroup> groups;
	std::vector<ClusterLODGroupSegment> segments;
	std::vector<BoundingSphere> segmentBounds;
	BoundingSphere objectBoundingSphere{};
	std::vector<ClusterLODGroupChunk> groupChunks;
	std::vector<ClusterLODGroupDiskLocator> groupDiskLocators;
	std::vector<ClusterLODGroupDiskLocator> pageDiskLocators;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0;
	uint32_t voxelPageBase = 0;
	uint32_t voxelPageCount = 0;
	ClusterLODCacheSource cacheSource;
	std::vector<ClusterLODNode> nodes;
	std::vector<ClusterLODNodeSkinningInfo> nodeSkinningInfos;
	std::vector<uint32_t> nodeBoneIndices;
	uint32_t nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
	std::vector<ClusterLODNodeRangeAlloc> lodNodeRanges;
	std::vector<uint32_t> lodLevelRoots;
	std::vector<ClusterLODAssemblyTransform> assemblyTransforms;
	std::vector<ClusterLODAssemblyInstance> assemblyInstances;
	std::vector<ClusterLODAssemblyBoneRemap> assemblyBoneRemaps;
	std::vector<uint32_t> assemblyBoneRemapIndices;
	SkeletonArtifactReference assemblySkeletonArtifact;
	std::vector<ClusterLODPartRecord> partRecords;
	uint32_t rootPartIndex = 0;
	uint32_t maxDepth = 0;
	uint32_t maxTraversalDepth = 0;
	// Material slots (docs/merge_hierarchies.md). Each cluster names a part-local slot in its page descriptor (word 15,
	// CLodMeshletDescriptorMaterialSlot); a hierarchy root's palette supplies the material for slot + the material
	// base of the portal that reached the part, which is per assembly transform.
	uint32_t materialSlotCount = 1;
	std::vector<uint32_t> assemblyTransformMaterialBases;
	// A merged mesh's references (docs/merge_hierarchies.md): the parts its portals open, and for each palette slot the
	// external part whose material it takes. Empty for every other mesh.
	std::vector<ClusterLODExternalPart> externalParts;
	std::vector<uint32_t> paletteParts;
	// Stage 6 appearance blocks in the triangle pages (docs/clod-appearance-stage6.md): how many clusters carry a UV remap block
	// (step 2, CLOD_PAGE_ATTRIBUTE_UV_REMAP) or a normal spread block (step 3, CLOD_PAGE_ATTRIBUTE_NORMAL_SPREAD), and their bytes.
	// Always stored, zero for a mesh without them; readers find each cluster's block through its page, not through these.
	uint32_t uvRemapClusters = 0;
	uint64_t uvRemapBytes = 0;
	uint32_t normalSpreadClusters = 0;
	uint64_t normalSpreadBytes = 0;
};

struct ClusterLODCacheBuildPayload
{
	const std::vector<std::vector<std::vector<std::byte>>>* groupPageBlobs = nullptr;
	const std::vector<std::vector<std::byte>>* meshPageBlobs = nullptr;
};

struct ClusterLODCacheBuildOwnedData
{
	std::vector<std::vector<std::vector<std::byte>>> groupPageBlobs;
	std::vector<std::vector<std::byte>> meshPageBlobs;
	VoxelGroupMapping voxelGroupMapping;

	ClusterLODCacheBuildPayload AsPayload() const {
		ClusterLODCacheBuildPayload payload{};
		payload.groupPageBlobs = &groupPageBlobs;
		payload.meshPageBlobs = &meshPageBlobs;
		return payload;
	}
};

struct ClusterLODPrebuildArtifacts
{
	ClusterLODPrebuiltData prebuiltData;
	ClusterLODCacheBuildOwnedData cacheBuildData;
};

struct ClusterLODVoxelGridOverride
{
	DirectX::XMFLOAT3 aabbMin{};
	DirectX::XMFLOAT3 aabbMax{};
	float voxelWidth = 0.0f;
	uint32_t resolution = 0u;
};

// Builder settings

enum class ClusterLODCoveragePreservationMode : uint8_t
{
	None,
	PrioritizeEdges,
	Voxel, // voxel groups replace the triangle groups above the switch: no triangle fallback (grass, voxel-only builds)
};

// Whether a triangle build (PrioritizeEdges or None) also gets a voxel branch above the levels that voxelize well, so
// that raster views with voxel output take voxels there while every other view keeps triangles (CLOD_GROUP_FLAG_BRANCH_*).
enum class ClusterLODVoxelForkMode : uint8_t
{
	Off,
	Auto,   // fork where voxels are shown to win: the seed test, a stalled triangle reduction and the surface test
	Forced, // fork wherever the seed test passes
};

// The defaults are the recommended build (docs/clod-appearance-stage6.md, "Defaults"): no sloppy fallback except as the last resort
// for groups that would otherwise stay stuck (sloppyLastResort), replacement vertices with
// UV transfer, error channels with the texture term, and topology changes across normal and UV seams guarded by the colour-weighted
// chart test. UV remap blocks, normal spread, validation, optimal placement and measured error stay off. Every field can be set
// back through the asset settings document or its BASICRENDERER_CLOD_* variable (ApplyClusterLODBuilderEnvironmentOverrides);
// the cache key covers the effective values (CLodCache::ComputeBuildConfigHash).
struct ClusterLODBuilderSettings
{
	bool disableSloppyFallback = true;
	// With disableSloppyFallback, a group every other candidate leaves stuck (it would stay terminal: parentless, error FLT_MAX)
	// is simplified by the sloppy fallback as a last resort, with its real error, so every group reaches a parent.
	bool sloppyLastResort = true;
	// The last resort also takes groups whose stage 5 topology candidate got under the threshold but was rejected by validation;
	// off, it takes only groups no candidate got there (the stuck ones).
	bool sloppyLastResortRejected = false;
	// Last-resort groups' normals are fitted to the original as stage 2 refits unmoved vertices (also on skinned meshes, which
	// otherwise build without replacement vertices: a replacement copies its parent's skinning).
	bool sloppyLastResortFit = true;
	// The last resort's error factor; 0 uses sloppyFallbackErrorFactor.
	float sloppyLastResortErrorFactor = 0.0f;
	float sloppyFallbackErrorFactor = 2.0f;
	float lodErrorMergePrevious = 1.5f;
	float lodErrorMergeAdditive = 0.0f;
	uint32_t partitionSizeFloor = 8u;
	uint32_t nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
	bool preserveImportedNormals = true;
	bool enableNormalAttributeSimplification = true;
	float normalAttributeWeight = 1.0f;
	// meshopt_SimplifyPermissive for regular simplification (clodConfig::simplify_permissive): attribute seams without a
	// protect bit may collapse. Off keeps every attribute seam, as non-permissive meshoptimizer does.
	bool simplifyPermissive = true;
	float simplifyTangentWeight = 0.01f;
	float simplifyTangentSignWeight = 0.5f;
	// Validation of every simplification result against the original surface before it is accepted, with retries and finer
	// geometry on rejection; sloppy output is accepted only when it validates (docs/clod-simplification-validation.md). The
	// thresholds are experimental. Off by default.
	bool simplifyValidation = false;
	uint32_t simplifyValidationRetries = 2u;          // less aggressive targets tried after a rejection, up to simplify_threshold
	float simplifyValidationFoldAngle = 90.0f;        // degrees between a face and the matched original face beyond which it is folded
	float simplifyValidationFoldArea = 0.01f;         // new folded area allowed, fraction of the result's area
	float simplifyValidationShadingAngle = 75.0f;     // degrees between interpolated and original normals beyond which shading fails
	float simplifyValidationShadingArea = 0.02f;      // new severe-shading area allowed, fraction of the result's area
	float simplifyValidationRadiusScale = 1.0f;       // match/coverage radius as a multiple of the group's error
	float simplifyValidationCoverageArea = 0.01f;     // uncovered input area and unmatched result area allowed, fractions
	// Replacement vertices (stage 2, docs/clod-simplification-validation.md): each group first tries simplifying a private copy
	// of its vertices with meshopt_simplifyWithUpdate, with normals fitted from the original surface and UVs solved with the
	// positions; accepted new vertices are appended to a builder-owned vertex store. Meshes without normals, with skinning or
	// without preserveImportedNormals are built without it. On by default.
	bool simplifyUpdate = true;
	bool simplifyUpdatePositions = true;             // off: normals are fitted but no vertex moves (attribution experiments)
	float simplifyUpdateRefitAngle = 5.0f;           // degrees; an unmoved vertex is replaced when its fitted normal differs more
	bool simplifyUpdateMeasuredError = false;        // report max(own error, measured deviation from the original) for every candidate
	// Follow-up to stages 2 and 5: every update candidate's moved vertices take each solved UV stream (the record texcoord and the
	// solved UV sets) from the original at their side-aware match, where it lies in their own chart neighbourhood, instead of
	// meshoptimizer's solve, which drifts on curved surfaces. Without it only topology candidates transfer, and only the record
	// texcoord. On by default.
	bool simplifyUpdateUvTransfer = true;
	// Build cost (docs/clod-appearance-stage6.md, "Build cost"): the side-aware match against the original that the normal fit, the
	// error channels and validation share (SimplifyTriangleBvh::SetMatchShell). Candidates lie within simplifyMatchReach times the
	// nearest distance; simplifyMatchCandidates (at most 32) keeps only that many of the nearest, so a sample over dense or
	// overlapping source stops looking once it holds them. 0 (default) keeps every candidate; facing bins below replace it.
	float simplifyMatchReach = 1.1f;
	uint32_t simplifyMatchCandidates = 0u;
	// Instead: the exact match while it holds at most 32 candidates, past that the nearest of each facing direction, with subtrees
	// that cannot add a direction or a nearer candidate skipped (SimplifyTriangleBvh::SetMatchShell). The value is the octahedral
	// grid of directions: 8 (default; 64 directions, also for 1) or 16 (256); 0 is the exact match, whose overlapping or dense
	// source can gather thousands of candidates per sample.
	uint32_t simplifyMatchFacingBins = 8u;
	// The fit's sample points on each triangle around a vertex, as bits of its five (1 the vertex, 2 towards it from the centre,
	// 4 and 8 the edge midpoints, 16 the centre; default 31, all of them). Neighbours share the vertex, the midpoints and the centre.
	uint32_t simplifyUpdateFitSamples = 31u;
	// Degrees: an unmoved vertex whose original normal at its own position (averaged over its triangles' faces) stays within this
	// of its current normal is taken as unchanged without the full fit. 0 (default): every vertex fitted.
	float simplifyUpdateRefitCheck = 0.0f;
	// Area samples per triangle (1, 2 or 4) of the error channels' measurement and its texture term (default 1), and of validation
	// and the topology candidates' texture check (default 4, all). Fewer take one or two of the four, rotating with the triangle.
	uint32_t lodErrorChannelsSamples = 1u;
	uint32_t simplifyValidationSamples = 4u;
	// Optimal placement (stage 4, docs/clod-simplification-validation.md): each group first tries a collapse loop that solves
	// replacement positions and wedge attributes while ranking collapses and validates each collapse at its solved position,
	// including orientation against the source sheet; it uses the replacement vertices of simplifyUpdate (turned on with it, under
	// the same restrictions), whose update candidate and the rest follow as fallbacks. Off by default.
	bool simplifyOptimal = false;
	float simplifyOptimalSheetAngle = 60.0f;         // degrees a face may turn from the source surface it represents (unless inherited)
	float simplifyOptimalFlipAngle = 75.5f;          // degrees a face may rotate in one collapse (meshoptimizer's own limit)
	bool simplifyOptimalPreserveFolds = false;       // meshopt_SimplifyPreserveFolds' quadrics in the optimal loop (comparison)
	uint32_t simplifyOptimalMoves = 7u;              // placements besides the endpoints, bits: 1 border/seam along the edge, 2 interior
	                                                 // along the edge, 4 interior free solve
	// Error channels (stage 3, docs/clod-simplification-validation.md): after the build, every group's simplified output is measured
	// against the original surface as a two-sided geometric deviation and an appearance channel (the root of its integrated squared
	// shading-normal chord), made monotone through the DAG and stored in the traversal nodes' spare words. The runtime selects with
	// them only when asked (clodLodGeometricChannel, clodLodAppearanceWeight). Meshes with voxel groups keep none. On by default.
	bool lodErrorChannels = true;
	// Stage 6 (docs/clod-appearance-stage6.md): with lodErrorChannels, the appearance channel also counts texture damage: the area of
	// each group's simplified output whose texture comes from the wrong place (UV set 0 interpolated across UV charts, matched to
	// another chart, or sliding farther than lodErrorChannelsTextureSlide times the group's error from the original's texcoord at
	// the side-aware match), weighted by this and added to Q_N before the square root (default 1; meshes without texcoords measure
	// nothing). 0 measures nothing and stores exactly what lodErrorChannels alone does. The runtime selects with it through
	// clodLodAppearanceWeight; nothing else changes.
	float lodErrorChannelsTexture = 1.0f;
	float lodErrorChannelsTextureSlide = 8.0f;
	// Stage 6 step 1: with lodErrorChannelsTexture, each displaced sample counts by the base colour difference it draws instead of
	// as 1: min(1, dE / this), dE the CIE76 difference between the material's base colour (factor times texture, linear light)
	// at the simplified UV and at the original's, both filtered at the mip where the group's error is one texel. So crossings over
	// uniform or tiled texture cost little and wrong-coloured ones cost what they did. Needs the importer's material
	// (ClusterLODMaterialAppearance); without it a mesh keeps the binary term. 0 (default): binary, nothing read or hashed.
	float lodErrorChannelsTextureColor = 0.0f;
	// The filter footprint of that comparison in group errors: 1 (default) is a pixel of a one-pixel LOD threshold; a runtime
	// threshold of t pixels draws the group where a pixel is 1/t of its error, which 1/t matches.
	float lodErrorChannelsTextureColorFootprint = 1.0f;
	// Weigh every sample of the group's output by its colour difference, not only those the displacement tests flag (texture that
	// slides less than the tolerance, or is interpolated non-linearly, also draws the wrong colour). Off by default.
	bool lodErrorChannelsTextureColorAll = false;
	// Stage 6 step 2 (docs/clod-appearance-stage6.md): with lodErrorChannelsTexture, a group whose simplified output has at least this
	// share of its measured area displaced gets UV remap blocks (ClusterLODUvRemap.h) for its damaged triangles: source UVs on a
	// barycentric grid of each triangle, matched to the original along the triangle's normal, which material evaluation samples
	// instead of the interpolated corner UVs. The group's Q_T is then the damage left through the remap. Needs the importer's
	// material, not alpha-masked (raster alpha tests read the corners' UVs). 0 (default): nothing remapped, nothing written.
	float lodErrorChannelsTextureRemap = 0.0f;
	float lodErrorChannelsTextureRemapTexel = 4.0f;      // grid spacing in group errors (never finer than a source texel)
	uint32_t lodErrorChannelsTextureRemapMaxNodes = 17u; // nodes per triangle edge at most (2..64)
	// Stage 6 step 2 refinement: 1 (default) the step 2 grid; 2 chart regions (exact per-region affine maps with a line per grid cell
	// between two of them, matched by the frontmost crossing along the normal, the grid only where regions do not fit). Either way the
	// blocks have the one remap layout (clod_uv_remap::kVersion); 2 only adds region triangles to them.
	uint32_t lodErrorChannelsTextureRemapLayout = 1u;
	// Layout 2: the coarsest cell grid whose lines give another region than the correspondence over at most this share of a
	// triangle is taken (finer grids cost bytes).
	float lodErrorChannelsTextureRemapCellMiss = 0.01f;
	// Bytes per simplified triangle that page binning leaves free for UV remap blocks (0: none, pages as without remaps). Remap blocks
	// live in their cluster's page, so a page that binning filled has no room for them.
	uint32_t lodErrorChannelsTextureRemapReserve = 0u;
	// Stage 6 step 3 (docs/clod-appearance-stage6.md): every simplified cluster gets a normal spread block (ClusterLODNormalSpread.h):
	// per vertex, the hat-weighted area mean of |n_coarse - n_source|^2 over its triangles, measured against the original's normals
	// (the error channels' measurement, which runs for it even without lodErrorChannels). Material evaluation (raster and path-traced
	// hit shading) widens the material's roughness by it, alpha'^2 = alpha^2 + v, so coarse levels that stand for curved, bumpy or
	// folded source keep the leaves' specular energy and highlight width. Needs normals and a UV set. Off by default: nothing measured
	// or written.
	bool lodNormalSpread = false;
	// The source each sample's spread is averaged over: a disc of this times half the group's error (a pixel where the cluster starts
	// to draw at a one-pixel threshold; a runtime threshold of t pixels draws it where a pixel is 1/t of that). 0: the point alone.
	float lodNormalSpreadFootprint = 1.0f;
	// Each footprint point's |n_coarse - n_source|^2 is capped at this before averaging (0: no cap). Measured on Bistro: uncapped, the
	// few points that land on another sheet or a crease's far face make the stored spread several times what a single widened lobe
	// can stand for (docs/clod-appearance-stage6.md, step 3).
	float lodNormalSpreadSampleCap = 0.1f;
	// Controlled topology changes (stage 5, docs/clod-simplification-validation.md): a group that would otherwise be terminal
	// (stuck above the simplify threshold, or every candidate rejected) tries update candidates whose seams lose their protect
	// bits, so meshoptimizer may collapse across normal hard edges, with normals refitted from the original. UV chart seams stay
	// unless simplifyTopologyRelaxUv. Each candidate is validated with the simplifyValidation* thresholds (also when
	// simplifyValidation is off); on rejection the group stays terminal. Uses replacement vertices (turned on with it, under
	// their restrictions). On by default.
	bool simplifyTopology = true;
	uint32_t simplifyTopologyRetries = 2u;           // less aggressive targets after the configured one, up to the threshold
	bool simplifyTopologyRelaxUv = true;             // relax UV chart seams too, where the texture check below allows it (default on)
	// Follow-up: with simplifyOptimal, a topology candidate the optimal loop leaves rejected or stuck is followed by stage 2's
	// (meshoptimizer's loop, same relaxed seams) at the same target. Off: optimal-placement topology candidates only.
	bool simplifyTopologyUpdateFallback = false;
	// Texture check of topology candidates: area fraction on which the texture may slide along the surface farther than the
	// validation radius (measured against the original's texcoords; meshes without texcoords skip it). 1 or more turns it off.
	float simplifyTopologyTextureArea = 0.02f;
	float simplifyTopologyTextureSlide = 8.0f;       // how far the texture may slide before it counts, as a multiple of the radius
	// Follow-ups to the texture check, each also counting a sample as displaced. Charts (on by default): the sample's triangle
	// mixes UV charts, or its chart is not the matched original's (charts: the source's UV-connected components). Texels (off): the
	// texture slides farther than this many texels of a simplifyTopologyTextureResolution texture (0: off). The builder is given no
	// texture sizes, so the resolution is nominal: the tolerance is a UV-space distance.
	bool simplifyTopologyTextureCharts = true;
	float simplifyTopologyTextureTexels = 0.0f;
	uint32_t simplifyTopologyTextureResolution = 512u;
	// Stage 6 step 4 (texture-aware validation, docs/clod-appearance-stage6.md): with the chart test, each displaced sample counts
	// min(1, dE / this) of its area instead of all of it, dE the CIE76 difference of the importer's base colour (factor times texture,
	// filtered at the candidate's error) drawn at the candidate's texcoord and at the original's (masked materials: a cutout flip counts
	// 1), so a crossing between charts that look alike passes. Needs the importer's material; without it the test stays binary.
	// Default 10; 0: off (binary).
	float simplifyTopologyTextureColor = 10.0f;

	ClusterLODCoveragePreservationMode coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
	uint32_t voxelGridBaseResolution = 32u;
	uint32_t voxelMinResolution = 0u;
	uint32_t voxelRaysPerCell = 64u;
	float voxelFallbackScalingFactor = 0.75f;
	uint32_t voxelFallbackMaxRetryCount = 1000u;
	float voxelFallbackGrowthFactor = 1.1f;
	float voxelFallbackAcceptanceBias = 1.0f;
	float voxelFallbackOpacityThreshold = 0.0f;
	uint32_t voxelTailMaxLevels = 4u;
	float voxelTailGrowthFactor = 1.5f;
	// Most of a group's traced surface a proposed voxelization may lose before the group stays triangles. See VoxelSurfaceTrace.h.
	// 0 turns the test off.
	float voxelSurfaceLossLimit = 0.0f;
	bool doubleSidedVoxelSourceNormals = false;

	ClusterLODVoxelForkMode voxelFork = ClusterLODVoxelForkMode::Off;
	// Auto: a seed's triangle parents must keep more than this share of its triangles (the reduction stalls), or it must
	// have no parent (reduction stopped)...
	float voxelForkStallRatio = 0.6f;
	// ...while it still has this many triangles: a stall at the top of a mesh that reduced well saves too little to fork.
	uint32_t voxelForkMinTriangles = 512u;
	// Auto: only double-sided surfaces (doubleSidedVoxelSourceNormals), which is what foliage cards are. Solid surfaces
	// (architecture, rocks) pass the tests above wherever reduction stops at a large top, but read as blocks voxelized.
	bool voxelForkDoubleSidedOnly = true;
	// Auto: the surface-loss limit for fork seeds (voxelSurfaceLossLimit for this decision). 0 turns the test off.
	float voxelForkSurfaceLoss = 0.008f;
};

inline std::string GetClusterLODEnvironmentVariable(const char* name)
{
#if defined(_WIN32)
	char* value = nullptr;
	size_t valueLength = 0;
	if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
	{
		return {};
	}
	std::string result(value);
	std::free(value);
	return result;
#else
	const char* value = std::getenv(name);
	return value != nullptr ? std::string(value) : std::string();
#endif
}

// BASICRENDERER_CLOD_FORCE_BRANCH=triangle|voxel makes raster views take that branch of forked DAGs, for A/B runs (the
// ray tracing view always takes triangles). 0: each view takes its own (CLOD_GROUP_FLAG_BRANCH_*).
inline uint32_t GetClusterLODForcedViewBranch()
{
	const std::string value = GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_FORCE_BRANCH");
	return value == "triangle" ? 1u : value == "voxel" ? 2u : 0u;
}

inline ClusterLODBuilderSettings ApplyClusterLODBuilderEnvironmentOverrides(ClusterLODBuilderSettings settings)
{
	const std::string modeString = GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_COVERAGE_PRESERVATION_MODE");
	if (!modeString.empty())
	{
		if (modeString == "none")
		{
			settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::None;
		}
		else if (modeString == "prioritizeEdges" || modeString == "prioritize-edges")
		{
			settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
		}
		else if (modeString == "voxel")
		{
			settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::Voxel;
		}
	}

	auto readUint = [](const char* name, uint32_t& outValue)
	{
		const std::string text = GetClusterLODEnvironmentVariable(name);
		if (!text.empty())
		{
			char* end = nullptr;
			const unsigned long value = std::strtoul(text.c_str(), &end, 10);
			if (end != text.c_str())
			{
				outValue = static_cast<uint32_t>(value);
			}
		}
	};

	auto readFloat = [](const char* name, float& outValue)
	{
		const std::string text = GetClusterLODEnvironmentVariable(name);
		if (!text.empty())
		{
			char* end = nullptr;
			const float value = std::strtof(text.c_str(), &end);
			if (end != text.c_str() && std::isfinite(value))
			{
				outValue = value;
			}
		}
	};

	auto readBool = [](const char* name, bool& outValue)
	{
		const std::string text = GetClusterLODEnvironmentVariable(name);
		if (text.empty())
		{
			return;
		}

		outValue = text == "1" || text == "true" || text == "on" || text == "yes";
	};

	readUint("BASICRENDERER_CLOD_VOXEL_GRID", settings.voxelGridBaseResolution);
	readUint("BASICRENDERER_CLOD_VOXEL_MIN_RES", settings.voxelMinResolution);
	readUint("BASICRENDERER_CLOD_VOXEL_RAYS", settings.voxelRaysPerCell);
	readUint("BASICRENDERER_CLOD_VOXEL_RETRIES", settings.voxelFallbackMaxRetryCount);
	readFloat("BASICRENDERER_CLOD_VOXEL_SCALE", settings.voxelFallbackScalingFactor);
	readFloat("BASICRENDERER_CLOD_VOXEL_GROWTH", settings.voxelFallbackGrowthFactor);
	readFloat("BASICRENDERER_CLOD_VOXEL_ACCEPTANCE_BIAS", settings.voxelFallbackAcceptanceBias);
	readFloat("BASICRENDERER_CLOD_VOXEL_OPACITY_THRESHOLD", settings.voxelFallbackOpacityThreshold);
	readUint("BASICRENDERER_CLOD_VOXEL_TAIL_LEVELS", settings.voxelTailMaxLevels);
	readUint("BASICRENDERER_CLOD_NODE_BONE_LIMIT", settings.nodeBoneLimit);
	readFloat("BASICRENDERER_CLOD_VOXEL_TAIL_GROWTH", settings.voxelTailGrowthFactor);
	readFloat("BASICRENDERER_CLOD_VOXEL_SURFACE_LOSS", settings.voxelSurfaceLossLimit);
	const std::string forkString = GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_FORK");
	if (forkString == "0" || forkString == "off")
	{
		settings.voxelFork = ClusterLODVoxelForkMode::Off;
	}
	else if (forkString == "auto")
	{
		settings.voxelFork = ClusterLODVoxelForkMode::Auto;
	}
	else if (forkString == "forced" || forkString == "1")
	{
		settings.voxelFork = ClusterLODVoxelForkMode::Forced;
	}
	readFloat("BASICRENDERER_CLOD_VOXEL_FORK_STALL", settings.voxelForkStallRatio);
	readUint("BASICRENDERER_CLOD_VOXEL_FORK_MIN_TRIANGLES", settings.voxelForkMinTriangles);
	readBool("BASICRENDERER_CLOD_VOXEL_FORK_DOUBLE_SIDED_ONLY", settings.voxelForkDoubleSidedOnly);
	readFloat("BASICRENDERER_CLOD_VOXEL_FORK_SURFACE_LOSS", settings.voxelForkSurfaceLoss);
	readBool("BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK", settings.disableSloppyFallback);
	readBool("BASICRENDERER_CLOD_SLOPPY_LAST_RESORT", settings.sloppyLastResort);
	readBool("BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_REJECTED", settings.sloppyLastResortRejected);
	readBool("BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_FIT", settings.sloppyLastResortFit);
	readFloat("BASICRENDERER_CLOD_SLOPPY_LAST_RESORT_ERROR_FACTOR", settings.sloppyLastResortErrorFactor);
	readFloat("BASICRENDERER_CLOD_SLOPPY_ERROR_FACTOR", settings.sloppyFallbackErrorFactor);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_PERMISSIVE", settings.simplifyPermissive);
	readFloat("BASICRENDERER_CLOD_NORMAL_WEIGHT", settings.normalAttributeWeight);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION", settings.simplifyValidation);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_RETRIES", settings.simplifyValidationRetries);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_FOLD_ANGLE", settings.simplifyValidationFoldAngle);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_FOLD_AREA", settings.simplifyValidationFoldArea);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_SHADING_ANGLE", settings.simplifyValidationShadingAngle);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_SHADING_AREA", settings.simplifyValidationShadingArea);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_RADIUS_SCALE", settings.simplifyValidationRadiusScale);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_COVERAGE_AREA", settings.simplifyValidationCoverageArea);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_UPDATE", settings.simplifyUpdate);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_UPDATE_POSITIONS", settings.simplifyUpdatePositions);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_UPDATE_REFIT_ANGLE", settings.simplifyUpdateRefitAngle);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_UPDATE_MEASURED_ERROR", settings.simplifyUpdateMeasuredError);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_UPDATE_UV_TRANSFER", settings.simplifyUpdateUvTransfer);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_MATCH_REACH", settings.simplifyMatchReach);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_MATCH_CANDIDATES", settings.simplifyMatchCandidates);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_MATCH_FACING_BINS", settings.simplifyMatchFacingBins);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_UPDATE_FIT_SAMPLES", settings.simplifyUpdateFitSamples);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_UPDATE_REFIT_CHECK", settings.simplifyUpdateRefitCheck);
	readUint("BASICRENDERER_CLOD_ERROR_CHANNELS_SAMPLES", settings.lodErrorChannelsSamples);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_SAMPLES", settings.simplifyValidationSamples);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL", settings.simplifyOptimal);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_SHEET_ANGLE", settings.simplifyOptimalSheetAngle);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_FLIP_ANGLE", settings.simplifyOptimalFlipAngle);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_PRESERVE_FOLDS", settings.simplifyOptimalPreserveFolds);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_MOVES", settings.simplifyOptimalMoves);
	readBool("BASICRENDERER_CLOD_ERROR_CHANNELS", settings.lodErrorChannels);
	readFloat("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE", settings.lodErrorChannelsTexture);
	readFloat("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_SLIDE", settings.lodErrorChannelsTextureSlide);
	readFloat("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_COLOR", settings.lodErrorChannelsTextureColor);
	readFloat("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_COLOR_FOOTPRINT", settings.lodErrorChannelsTextureColorFootprint);
	readBool("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_COLOR_ALL", settings.lodErrorChannelsTextureColorAll);
	readFloat("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP", settings.lodErrorChannelsTextureRemap);
	readFloat("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_TEXEL", settings.lodErrorChannelsTextureRemapTexel);
	readUint("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_MAX_NODES", settings.lodErrorChannelsTextureRemapMaxNodes);
	readUint("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_LAYOUT", settings.lodErrorChannelsTextureRemapLayout);
	readFloat("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_CELL_MISS", settings.lodErrorChannelsTextureRemapCellMiss);
	readUint("BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP_RESERVE", settings.lodErrorChannelsTextureRemapReserve);
	readBool("BASICRENDERER_CLOD_NORMAL_SPREAD", settings.lodNormalSpread);
	readFloat("BASICRENDERER_CLOD_NORMAL_SPREAD_FOOTPRINT", settings.lodNormalSpreadFootprint);
	readFloat("BASICRENDERER_CLOD_NORMAL_SPREAD_SAMPLE_CAP", settings.lodNormalSpreadSampleCap);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY", settings.simplifyTopology);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_RETRIES", settings.simplifyTopologyRetries);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_RELAX_UV", settings.simplifyTopologyRelaxUv);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_UPDATE_FALLBACK", settings.simplifyTopologyUpdateFallback);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_AREA", settings.simplifyTopologyTextureArea);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_SLIDE", settings.simplifyTopologyTextureSlide);
	readBool("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_CHARTS", settings.simplifyTopologyTextureCharts);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_TEXELS", settings.simplifyTopologyTextureTexels);
	readUint("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_RESOLUTION", settings.simplifyTopologyTextureResolution);
	readFloat("BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_COLOR", settings.simplifyTopologyTextureColor);

	return settings;
}

// Stage 6 step 1: whether a build with these (effective) settings weights texture damage by colour, and so whether importers
// describe materials and hash their images into the cache identity.
inline bool ClusterLODTextureColorActive(const ClusterLODBuilderSettings& settings)
{
	return settings.lodErrorChannels && settings.lodErrorChannelsTexture > 0.0f && settings.lodErrorChannelsTextureColor > 0.0f;
}

// Stage 6 step 2: whether a build with these (effective) settings writes UV remap blocks.
inline bool ClusterLODUvRemapActive(const ClusterLODBuilderSettings& settings)
{
	return settings.lodErrorChannels && settings.lodErrorChannelsTexture > 0.0f && settings.lodErrorChannelsTextureRemap > 0.0f;
}

// The facing-bin grid simplifyMatchFacingBins asks for: 0 (off), 4, 8 (also for 1, 2 and 3) or 16 (anything above 8).
inline uint32_t ClusterLODMatchFacingGrid(const ClusterLODBuilderSettings& settings)
{
	const uint32_t requested = settings.simplifyMatchFacingBins;
	return requested == 0u ? 0u : requested < 4u ? 8u : requested == 4u ? 4u : requested <= 8u ? 8u : 16u;
}

// Stage 6 step 3: whether a build with these (effective) settings writes normal spread blocks.
inline bool ClusterLODNormalSpreadActive(const ClusterLODBuilderSettings& settings)
{
	return settings.lodNormalSpread;
}

// Stage 6 step 4: whether topology candidates' chart test weighs its displaced samples by colour.
inline bool ClusterLODTopologyTextureColorActive(const ClusterLODBuilderSettings& settings)
{
	return settings.simplifyTopology && settings.simplifyTopologyTextureCharts && settings.simplifyTopologyTextureArea < 1.0f &&
		settings.simplifyTopologyTextureColor > 0.0f;
}

// Whether importers describe materials to the builder (and hash them into the cache identity): the colour weight, the remap or the
// colour-weighted chart test.
inline bool ClusterLODMaterialAppearanceWanted(const ClusterLODBuilderSettings& settings)
{
	return ClusterLODTextureColorActive(settings) || ClusterLODUvRemapActive(settings) || ClusterLODTopologyTextureColorActive(settings);
}

// Runtime summary

struct ClusterLODRuntimeSummary
{
	struct GroupChunkHint
	{
		uint32_t groupVertexCount = 0;
		uint32_t meshletCount = 0;
		uint32_t meshletTrianglesByteCount = 0;
		uint32_t segmentCount = 0; // number of segments (= pages before re-binning) for per-segment paging
		uint32_t pageCount = 0;    // number of physical pages after bin-packing
	};

	struct GroupRange
	{
		uint32_t firstGroup = 0;
		uint32_t groupCount = 0;
		uint32_t branches = 3u; // the CLOD_BRANCH_* in which these groups are coarsest: pinned while one of them is active
	};

	std::vector<GroupChunkHint> groupChunkHints;
	std::vector<int32_t> parentGroupByLocal;
	// A forked DAG: each group's streaming parent while the voxel branch is active (parentGroupByLocal otherwise; the two
	// differ only for shared groups at the fork). Empty for an unforked mesh.
	std::vector<int32_t> voxelParentGroupByLocal;
	std::vector<float> groupErrorByLocal;
	std::vector<uint32_t> firstGroupVertexByLocal;
	std::vector<GroupRange> coarsestRanges;
};

// MeshIngestBuilder

// Accumulates raw vertex/index data and produces ClusterLOD artifacts
// or GPU Mesh objects (renderer-side).  The Build() method is only implemented
// in the renderer; BuildClusterLODArtifacts() has no GPU dependencies.
class MeshIngestBuilder {
public:
	MeshIngestBuilder(
		unsigned int vertexSize,
		unsigned int skinningVertexSize,
		unsigned int flags,
		ClusterLODBuilderSettings clusterLODBuilderSettings = {})
		: m_vertexSize(vertexSize),
		  m_skinningVertexSize(skinningVertexSize),
		  m_flags(flags),
		  m_clusterLODBuilderSettings(ApplyClusterLODBuilderEnvironmentOverrides(std::move(clusterLODBuilderSettings))) {}

	void ReserveVertices(size_t vertexCount) {
		m_vertices.reserve(vertexCount * static_cast<size_t>(m_vertexSize));
	}

	void ReserveIndices(size_t indexCount) {
		m_indices.reserve(indexCount);
	}

	void AppendVertexBytes(const std::byte* data, size_t byteCount) {
		if (byteCount != m_vertexSize) {
			throw std::runtime_error("MeshIngestBuilder vertex byte size mismatch");
		}
		m_vertices.insert(m_vertices.end(), data, data + byteCount);
	}

	void AppendSkinningVertexBytes(const std::byte* data, size_t byteCount) {
		if (m_skinningVertexSize == 0) {
			throw std::runtime_error("MeshIngestBuilder has no skinning vertex format");
		}
		if (byteCount != m_skinningVertexSize) {
			throw std::runtime_error("MeshIngestBuilder skinning vertex byte size mismatch");
		}
		constexpr size_t kMaxDebugVertices = 4u;
		constexpr size_t kMaxSkinInfluences = 8u;
		if (m_skinningDebugPositions.size() / 3u < kMaxDebugVertices) {
			const size_t jointsOffset = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3);
			const size_t weightsOffset = jointsOffset + sizeof(uint32_t) * kMaxSkinInfluences;
			if (byteCount >= weightsOffset + sizeof(float) * kMaxSkinInfluences) {
				const auto oldJointCount = m_skinningDebugJoints.size();
				const auto oldPositionCount = m_skinningDebugPositions.size();
				m_skinningDebugJoints.resize(oldJointCount + kMaxSkinInfluences);
				m_skinningDebugWeights.resize(oldJointCount + kMaxSkinInfluences);
				m_skinningDebugPositions.resize(oldPositionCount + 3u);
				m_skinningDebugNormals.resize(oldPositionCount + 3u);
				std::memcpy(m_skinningDebugPositions.data() + oldPositionCount, data, sizeof(float) * 3u);
				std::memcpy(m_skinningDebugNormals.data() + oldPositionCount, data + sizeof(DirectX::XMFLOAT3), sizeof(float) * 3u);
				std::memcpy(m_skinningDebugJoints.data() + oldJointCount, data + jointsOffset, sizeof(uint32_t) * kMaxSkinInfluences);
				std::memcpy(m_skinningDebugWeights.data() + oldJointCount, data + weightsOffset, sizeof(float) * kMaxSkinInfluences);
			}
		}
		m_skinningVertices.insert(m_skinningVertices.end(), data, data + byteCount);
	}

	void AppendIndex(uint32_t index) {
		m_indices.push_back(index);
	}

	void AppendIndices(const uint32_t* data, size_t count) {
		m_indices.insert(m_indices.end(), data, data + count);
	}

	void SetUvSets(std::vector<MeshUvSetData> uvSets) {
		m_uvSets = std::move(uvSets);
	}

	// Stage 6: the material's base colour, for the colour-weighted texture term (only read when it is enabled).
	void SetMaterialAppearance(std::shared_ptr<const ClusterLODMaterialAppearance> appearance) {
		m_materialAppearance = std::move(appearance);
	}

	const std::shared_ptr<const ClusterLODMaterialAppearance>& GetMaterialAppearance() const {
		return m_materialAppearance;
	}

	const std::vector<MeshUvSetData>& GetUvSets() const {
		return m_uvSets;
	}

	const std::vector<std::byte>& GetVertices() const {
		return m_vertices;
	}

	const std::vector<std::byte>& GetSkinningVertices() const {
		return m_skinningVertices;
	}

	const std::vector<uint32_t>& GetIndices() const {
		return m_indices;
	}

	unsigned int GetVertexSize() const {
		return m_vertexSize;
	}

	unsigned int GetSkinningVertexSize() const {
		return m_skinningVertexSize;
	}

	unsigned int GetFlags() const {
		return m_flags;
	}

	// GPU-side: creates a Mesh object with buffer views.
	// Only implemented in the renderer (Mesh.cpp); not available in headless builds.
	std::shared_ptr<Mesh> Build(
		const std::shared_ptr<Material>& material,
		std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD = std::nullopt,
		MeshCpuDataPolicy cpuDataPolicy = MeshCpuDataPolicy::Retain);

	// Headless: runs the full ClusterLOD build pipeline (CPU-only).
	ClusterLODPrebuildArtifacts BuildClusterLODArtifacts(
		const VoxelCoverageMaterialSampler* coverageMaterialSampler = nullptr) const;
	ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifacts(
		uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER) const;
	ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifacts(
		const ClusterLODVoxelGridOverride& grid,
		uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER) const;
	VoxelGroupPayload BuildVoxelOnlyPayload(const ClusterLODVoxelGridOverride& grid) const;
	VoxelGroupPayload BuildVoxelOnlyPayload(
		const ClusterLODVoxelGridOverride& grid,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler) const;
	static ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromPayload(
		const VoxelGroupPayload& payload,
		const ClusterLODBuilderSettings& settings,
		uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);

	void SetClusterLODBuilderSettings(const ClusterLODBuilderSettings& settings) {
		m_clusterLODBuilderSettings = settings;
	}

	const ClusterLODBuilderSettings& GetClusterLODBuilderSettings() const {
		return m_clusterLODBuilderSettings;
	}

private:
	unsigned int m_vertexSize = 0;
	unsigned int m_skinningVertexSize = 0;
	unsigned int m_flags = 0;
	std::vector<std::byte> m_vertices;
	std::vector<std::byte> m_skinningVertices;
	std::vector<uint32_t> m_skinningDebugJoints;
	std::vector<float> m_skinningDebugWeights;
	std::vector<float> m_skinningDebugPositions;
	std::vector<float> m_skinningDebugNormals;
	std::vector<uint32_t> m_indices;
	std::vector<MeshUvSetData> m_uvSets;
	std::shared_ptr<const ClusterLODMaterialAppearance> m_materialAppearance;
	ClusterLODBuilderSettings m_clusterLODBuilderSettings{};
};
