#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <DirectXMath.h>
#include "ThirdParty/meshoptimizer/clusterlod.h"
#include "Mesh/ClusterLODShaderTypes.h"

struct ClippingPlane {
	DirectX::XMFLOAT4 plane;
};

struct CameraInfo {
    DirectX::XMFLOAT4 positionWorldSpace;
    DirectX::XMMATRIX view;
    DirectX::XMMATRIX viewInverse;
    DirectX::XMMATRIX jitteredProjection;
    DirectX::XMMATRIX projectionInverse;
	DirectX::XMMATRIX viewProjection;

    DirectX::XMMATRIX prevView;
	DirectX::XMMATRIX prevJitteredProjection;
    DirectX::XMMATRIX prevUnjitteredProjection;

    DirectX::XMMATRIX unjitteredProjection;

	ClippingPlane clippingPlanes[6];

	float fov;
	float aspectRatio;
	float zNear;
	float zFar;

    int depthBufferArrayIndex = -1;
    unsigned int depthResX;
	unsigned int depthResY;
    uint32_t numDepthMips;

    unsigned int isOrtho = 0; // bool
	DirectX::XMFLOAT2 uvScaleToNextPowerOfTwo = { 1.0f, 1.0f }; // Scale to next power of two, for linear depth buffer
    unsigned int pad[1];
};

struct CullingCameraInfo {
    DirectX::XMFLOAT4 positionWorldSpace;
    float projX = 0.0f;
    float projY = 0.0f;
    float zNear = 0.0f;
    float errorOverDistanceThreshold = 0.0f; // Threshold for (error * scale) / distance metric
    unsigned int isOrtho = 0;
    float pad[3] = {};
    DirectX::XMFLOAT4 viewRightWorld;
    DirectX::XMFLOAT4 viewUpWorld;
    DirectX::XMFLOAT4 viewForwardWorld;
    DirectX::XMMATRIX viewProjection;
    DirectX::XMFLOAT4 viewZ;
    DirectX::XMMATRIX viewInverse;
    DirectX::XMMATRIX projectionInverse;
};

struct PerFrameCB {
    DirectX::XMVECTOR ambientLighting;
    DirectX::XMVECTOR shadowCascadeSplits;

	unsigned int mainCameraIndex;
	//unsigned int activeLightIndicesBufferIndex;
    //unsigned int lightBufferIndex;
    unsigned int numLights;

    //unsigned int pointLightCubemapBufferIndex;
    //unsigned int spotLightMatrixBufferIndex;
    //unsigned int directionalLightCascadeBufferIndex;
    unsigned int numDirectionalClipmaps;

    unsigned int activeEnvironmentIndex;
    //unsigned int environmentBufferDescriptorIndex;
	
    //unsigned int environmentBRDFLUTIndex;
	//unsigned int environmentBRDFLUTSamplerIndex;

    unsigned int outputType;
    unsigned int screenResX;
    unsigned int screenResY;
    unsigned int lightClusterGridSizeX;

    unsigned int lightClusterGridSizeY;
	unsigned int lightClusterGridSizeZ;
    unsigned int nearClusterCount; // how many uniform slices up close
    float clusterZSplitDepth; // view-space depth to switch to log

    unsigned int frameIndex; // 0 to 63
    unsigned int shadowVirtualSmrtDirectionalCountsPacked = 0u;
    float shadowVirtualSmrtMaxRayAngleFromLightDegrees = 0.0f;
    float shadowVirtualSmrtRayLengthScaleDirectional = 0.0f;
    float shadowVirtualSmrtMaxTraceDistanceWorld = 0.0f;
    unsigned int terrainStochasticSamplingEnabled = 1u;
    unsigned int terrainStochasticDiffuseEnabled = 1u;
    unsigned int terrainStochasticNormalEnabled = 1u;
    unsigned int terrainStochasticDerivativeNormalsEnabled = 1u;
    float terrainStochasticBlendCurve = 0.65f;
    unsigned int terrainGaussianStochasticEnabled = 0u;
    unsigned int terrainStochasticRegisterPad = 0u;
    DirectX::XMUINT3 terrainStochasticPad{};
    unsigned int parallaxOcclusionMappingEnabled = 1u;
    unsigned int terrainParallaxOcclusionMappingEnabled = 1u;
    float terrainParallaxHeightScale = 0.03f;
    unsigned int terrainParallaxMaxSteps = 16u;
};

static_assert(offsetof(PerFrameCB, terrainStochasticRegisterPad) == 124, "PerFrameCB layout mismatch.");
static_assert(offsetof(PerFrameCB, terrainStochasticPad) == 128, "PerFrameCB layout mismatch.");
static_assert(offsetof(PerFrameCB, parallaxOcclusionMappingEnabled) == 140, "PerFrameCB layout mismatch.");
static_assert(offsetof(PerFrameCB, terrainParallaxOcclusionMappingEnabled) == 144, "PerFrameCB layout mismatch.");
static_assert(offsetof(PerFrameCB, terrainParallaxHeightScale) == 148, "PerFrameCB layout mismatch.");
static_assert(offsetof(PerFrameCB, terrainParallaxMaxSteps) == 152, "PerFrameCB layout mismatch.");
static_assert(sizeof(PerFrameCB) == 160, "PerFrameCB layout mismatch.");

// Object flags (shared with HLSL OBJECT_FLAG_* defines)
static constexpr unsigned int OBJECT_FLAG_REVERSE_WINDING = 1u << 0;

struct PerObjectCB {
    DirectX::XMMATRIX modelMatrix;
    DirectX::XMMATRIX prevModelMatrix;
    DirectX::XMMATRIX modelInverseMatrix;
    unsigned int normalMatrixBufferIndex;
    unsigned int objectFlags;
    unsigned int pad[2];
};

struct PerMeshCB {
    unsigned int materialDataIndex;
    unsigned int materialEvalCompileFlagsID;
    unsigned int materialReyesEvalCompileFlagsID;
    unsigned int rasterBucketIndex;
    unsigned int vertexFlags;
	unsigned int vertexByteSize;
    unsigned int skinningVertexByteSize;

	BoundingSphere boundingSphere;

    unsigned int clodMeshletBufferOffset;
    unsigned int clodMeshletVerticesBufferOffset;
    unsigned int clodMeshletTrianglesBufferOffset;
    unsigned int clodNumMeshlets;

    unsigned int vertexBufferOffset;
    unsigned int numVertices;
    unsigned int numMeshlets;
};

struct PerMeshInstanceCB {
    unsigned int perMeshBufferIndex;
    unsigned int perObjectBufferIndex;
    unsigned int skinningInstanceSlot;
    float skinnedBoundsScale = 1.0f;
    BoundingSphere boundingSphere = {};
};

using PerInstanceTransformCB = PerObjectCB;

struct InstanceDrawRecordCB {
    unsigned int meshTemplateIndex;
    unsigned int instanceTransformIndex;
    unsigned int clodOffsetIndex;
    unsigned int flags;
};

struct PerMaterialCB {
    unsigned int materialFlags;
    unsigned int baseColorTextureIndex;
    unsigned int baseColorSamplerIndex;
    unsigned int normalTextureIndex;

    unsigned int normalSamplerIndex;
    unsigned int metallicTextureIndex;
    unsigned int metallicSamplerIndex;
	unsigned int roughnessTextureIndex;

	unsigned int roughnessSamplerIndex;
    unsigned int emissiveTextureIndex;
    unsigned int emissiveSamplerIndex;
    unsigned int aoMapIndex;

    unsigned int aoSamplerIndex;
    unsigned int heightMapIndex;
    unsigned int heightSamplerIndex;
	unsigned int opacityTextureIndex;
	
    unsigned int opacitySamplerIndex;
    float metallicFactor;
    float roughnessFactor;
    float ambientStrength;
    
    float specularStrength;
    float textureScale;
    float heightMapScale;
    float alphaCutoff;
	float geometricDisplacementMin;
	float geometricDisplacementMax;
    unsigned int geometricDisplacementEnabled;
    unsigned int terrainSetIndex;

    DirectX::XMFLOAT4 baseColorFactor;
    DirectX::XMFLOAT4 emissiveFactor;
    DirectX::XMUINT4 baseColorChannels;

    DirectX::XMUINT3 normalChannels;
    unsigned int compileFlagsID;

	unsigned int aoChannel;
    unsigned int heightChannel;
    unsigned int metallicChannel;
    unsigned int roughnessChannel;
    
    DirectX::XMUINT3 emissiveChannels;
	unsigned int rasterBuckedIndex;

	unsigned int baseColorUvSetIndex;
	unsigned int normalUvSetIndex;
	unsigned int metallicUvSetIndex;
	unsigned int roughnessUvSetIndex;

	unsigned int emissiveUvSetIndex;
	unsigned int aoUvSetIndex;
	unsigned int heightUvSetIndex;
	unsigned int opacityUvSetIndex;

	unsigned int openPBRMaterialDataIndex;
    unsigned int baseColorStreamingTextureID;
    unsigned int normalStreamingTextureID;
    unsigned int metallicStreamingTextureID;
    unsigned int roughnessStreamingTextureID;
    unsigned int emissiveStreamingTextureID;
    unsigned int aoStreamingTextureID;
    unsigned int heightStreamingTextureID;
    unsigned int opacityStreamingTextureID;
};

struct PerMaterialEvalCB {
    unsigned int materialFlags;
    unsigned int baseColorTextureIndex;
    unsigned int baseColorSamplerIndex;
    unsigned int normalTextureIndex;

    unsigned int normalSamplerIndex;
    unsigned int metallicTextureIndex;
    unsigned int metallicSamplerIndex;
    unsigned int roughnessTextureIndex;

    unsigned int roughnessSamplerIndex;
    unsigned int emissiveTextureIndex;
    unsigned int emissiveSamplerIndex;
    unsigned int aoMapIndex;

    unsigned int aoSamplerIndex;
    unsigned int heightMapIndex;
    unsigned int heightSamplerIndex;
    unsigned int opacityTextureIndex;

    unsigned int opacitySamplerIndex;
    float metallicFactor;
    float roughnessFactor;
    float heightMapScale;

    float alphaCutoff;
    float geometricDisplacementMin;
    float geometricDisplacementMax;
    unsigned int geometricDisplacementEnabled;

    DirectX::XMFLOAT4 baseColorFactor;
    DirectX::XMFLOAT4 emissiveFactor;
    DirectX::XMUINT4 baseColorChannels;

    DirectX::XMUINT3 normalChannels;
    unsigned int terrainSetIndex;

    unsigned int aoChannel;
    unsigned int heightChannel;
    unsigned int metallicChannel;
    unsigned int roughnessChannel;

    DirectX::XMUINT3 emissiveChannels;
    unsigned int openPBRMaterialDataIndex;

    unsigned int baseColorUvSetIndex;
    unsigned int normalUvSetIndex;
    unsigned int metallicUvSetIndex;
    unsigned int roughnessUvSetIndex;

    unsigned int emissiveUvSetIndex;
    unsigned int aoUvSetIndex;
    unsigned int heightUvSetIndex;
    unsigned int opacityUvSetIndex;
    unsigned int baseColorStreamingTextureID;
    unsigned int normalStreamingTextureID;
    unsigned int metallicStreamingTextureID;
    unsigned int roughnessStreamingTextureID;
    unsigned int emissiveStreamingTextureID;
    unsigned int aoStreamingTextureID;
    unsigned int heightStreamingTextureID;
    unsigned int opacityStreamingTextureID;
};

static_assert(sizeof(PerMaterialEvalCB) == 256, "PerMaterialEvalCB must match HLSL MaterialEvalInfo stride.");
static_assert(offsetof(PerMaterialEvalCB, geometricDisplacementEnabled) == 92, "PerMaterialEvalCB layout mismatch.");
static_assert(offsetof(PerMaterialEvalCB, baseColorFactor) == 96, "PerMaterialEvalCB layout mismatch.");
static_assert(offsetof(PerMaterialEvalCB, emissiveStreamingTextureID) == 240, "PerMaterialEvalCB layout mismatch.");

struct TerrainLayerGPU {
    unsigned int diffuseTextureIndex;
    unsigned int diffuseSamplerIndex;
    unsigned int normalTextureIndex;
    unsigned int normalSamplerIndex;
    unsigned int heightTextureIndex;
    unsigned int heightSamplerIndex;
    unsigned int diffuseStreamingTextureID;
    unsigned int normalStreamingTextureID;
    unsigned int heightStreamingTextureID;
    unsigned int pad0;
    DirectX::XMUINT3 normalChannels;
    unsigned int flags;
    DirectX::XMFLOAT4 fallbackColor;
    float uvScale;
    unsigned int stochasticLayerIndex;
    float heightScale;
    float pad1;
};

struct TerrainStochasticLayerGPU {
    unsigned int diffuseGaussianTextureIndex;
    unsigned int diffuseInverseLutTextureIndex;
    unsigned int diffuseInverseLutSamplerIndex;
    unsigned int diffuseFlags;
    unsigned int normalGaussianTextureIndex;
    unsigned int normalInverseLutTextureIndex;
    unsigned int normalInverseLutSamplerIndex;
    unsigned int normalFlags;
    float stochasticScale;
    float diffuseLutHeight;
    float normalLutHeight;
    float heightLutHeight;
    DirectX::XMFLOAT4 diffuseColorSpaceOrigin;
    DirectX::XMFLOAT4 diffuseColorSpaceVector0;
    DirectX::XMFLOAT4 diffuseColorSpaceVector1;
    DirectX::XMFLOAT4 diffuseColorSpaceVector2;
    unsigned int heightGaussianTextureIndex;
    unsigned int heightInverseLutTextureIndex;
    unsigned int heightInverseLutSamplerIndex;
    unsigned int heightFlags;
};

struct TerrainLayerRefGPU {
    unsigned int layerIndex;
    unsigned int pad0;
    unsigned int pad1;
    unsigned int pad2;
};

struct TerrainRegionGPU {
    int regionX;
    int regionY;
    unsigned int layerRefStart;
    unsigned int layerRefCount;
    unsigned int weightBlockStart;
    unsigned int weightSampleSide;
    unsigned int pad0;
    unsigned int pad1;
};

struct TerrainSetGPU {
    int minRegionX;
    int minRegionY;
    unsigned int regionCountX;
    unsigned int regionCountY;
    unsigned int regionBase;
    unsigned int regionCount;
    unsigned int layerBase;
    unsigned int layerCount;
    unsigned int layerRefBase;
    unsigned int layerRefCount;
    unsigned int weightBlockBase;
    unsigned int weightBlockCount;
    float regionSizeWorld;
    float pad0;
    float pad1;
    float pad2;
};

struct PerMaterialOpenPBRCB {
    float baseWeight;
    DirectX::XMFLOAT3 baseColor;
    float baseDiffuseRoughness;
    float baseMetalness;
    float subsurfaceWeight;
    float subsurfaceRadius;

    DirectX::XMFLOAT3 subsurfaceColor;
    float subsurfaceScatterAnisotropy;
    DirectX::XMFLOAT3 subsurfaceRadiusScale;
    float specularWeight;

    DirectX::XMFLOAT3 specularColor;
    float specularRoughness;
    float specularRoughnessAnisotropy;
    float specularIor;
    DirectX::XMFLOAT2 specularAnisotropyRotationCosSin;

    float coatWeight;
    DirectX::XMFLOAT3 coatColor;
    float coatRoughness;
    float coatRoughnessAnisotropy;
    float coatIor;
    float coatDarkening;
    DirectX::XMFLOAT2 coatAnisotropyRotationCosSin;

    float fuzzWeight;
    DirectX::XMFLOAT3 fuzzColor;
    float fuzzRoughness;
    float transmissionWeight;
    DirectX::XMFLOAT3 transmissionColor;
    float transmissionDepth;

    DirectX::XMFLOAT3 transmissionScatter;
    float transmissionScatterAnisotropy;
    float transmissionDispersionScale;
    float transmissionDispersionAbbeNumber;
    float thinFilmWeight;
    float thinFilmThickness;
    float thinFilmIor;
    float emissionLuminance;

    DirectX::XMFLOAT3 emissionColor;
    float geometryOpacity;
    unsigned int geometryThinWalled;
    unsigned int pad0;
    unsigned int pad1;
    unsigned int pad2;

    unsigned int coatColorTextureIndex;
    unsigned int coatColorSamplerIndex;
    unsigned int coatWeightTextureIndex;
    unsigned int coatWeightSamplerIndex;

    unsigned int coatRoughnessTextureIndex;
    unsigned int coatRoughnessSamplerIndex;
    unsigned int fuzzColorTextureIndex;
    unsigned int fuzzColorSamplerIndex;

    unsigned int fuzzWeightTextureIndex;
    unsigned int fuzzWeightSamplerIndex;
    unsigned int fuzzRoughnessTextureIndex;
    unsigned int fuzzRoughnessSamplerIndex;

    DirectX::XMUINT4 coatColorChannels;
    unsigned int coatWeightChannel;
    unsigned int coatRoughnessChannel;
    unsigned int coatTexturePad0;

    DirectX::XMUINT4 fuzzColorChannels;
    unsigned int fuzzWeightChannel;
    unsigned int fuzzRoughnessChannel;
    unsigned int fuzzTexturePad0;

    unsigned int coatColorUvSetIndex;
    unsigned int coatWeightUvSetIndex;
    unsigned int coatRoughnessUvSetIndex;
    unsigned int fuzzColorUvSetIndex;

    unsigned int fuzzWeightUvSetIndex;
    unsigned int fuzzRoughnessUvSetIndex;
    unsigned int coatColorStreamingTextureID;
    unsigned int coatWeightStreamingTextureID;
    unsigned int coatRoughnessStreamingTextureID;
    unsigned int fuzzColorStreamingTextureID;
    unsigned int fuzzWeightStreamingTextureID;
    unsigned int fuzzRoughnessStreamingTextureID;
};

struct TextureStreamingGPUInfo {
    unsigned int flags;
    unsigned int totalMipCount;
    unsigned int residentTopMip;
    unsigned int residentMipCount;

    unsigned int fullWidth;
    unsigned int fullHeight;

    unsigned int requestedTopMip;
    unsigned int pendingTopMip;
    unsigned int bindingRevisionLo;
    unsigned int bindingRevisionHi;
};

struct LightInfo {
    // Light attributes: x=type (0=point, 1=spot, 2=directional)
    // x=point -> w = shadow caster
    // x=spot -> y= inner cone angle, z= outer cone angle, w= shadow caster
    // x=directional => w= shadow caster
    unsigned int type;
    float innerConeAngle;
    float outerConeAngle;
    int shadowViewInfoIndex;

    DirectX::XMVECTOR posWorldSpace; // Position of the lights
    DirectX::XMVECTOR dirWorldSpace; // Direction of the lights
    DirectX::XMVECTOR attenuation; // x,y,z = constant, linear, quadratic attenuation
    DirectX::XMVECTOR color; // Color of the lights

    float nearPlane;
    float farPlane;
	int shadowMapIndex = -1;
    int shadowSamplerIndex = -1;

    bool shadowCaster;
	BoundingSphere boundingSphere;
    float maxRange;
    float shadowSourceRadius = 0.0f;
    float shadowSourceAngleDegrees = 0.0f;
};

#define LIGHTS_PER_PAGE 12
struct LightPage {
    unsigned int ptrNextPage;
    unsigned int numLightsInPage;
    unsigned int lightIndices[LIGHTS_PER_PAGE];
};

struct Cluster {
    DirectX::XMVECTOR minPoint;
    DirectX::XMVECTOR maxPoint;
    unsigned int numLights;
    unsigned int ptrFirstPage;
    unsigned int pad[2];
};

typedef unsigned int uint;

namespace XeGTAO {
    struct GTAOConstants {
        DirectX::XMUINT2 ViewportSize;
        DirectX::XMFLOAT2 ViewportPixelSize; // .zw == 1.0 / ViewportSize.xy

        DirectX::XMFLOAT2 DepthUnpackConsts; // UNUSED if source depth is linear view depth.
        DirectX::XMFLOAT2 CameraTanHalfFOV;

        DirectX::XMFLOAT2 NDCToViewMul;
        DirectX::XMFLOAT2 NDCToViewAdd;

        DirectX::XMFLOAT2 NDCToViewMul_x_PixelSize;
        float EffectRadius; // world (viewspace) maximum size of the shadow
        float EffectFalloffRange;

        float RadiusMultiplier;
        DirectX::XMFLOAT2 SourceDepthUVScale; // Scale UVs when sampling source depth if texture is padded (e.g. to next power-of-two).
        float Padding0;
        float FinalValuePower;
        float DenoiseBlurBeta;

        float SampleDistributionPower;
        float ThinOccluderCompensation;
        float DepthMIPSamplingOffset;
        int NoiseIndex; // frameIndex % 64 if using TAA or 0 otherwise
    };
}

struct GTAOInfo {
    XeGTAO::GTAOConstants g_GTAOConstants;
};

struct EnvironmentInfo {
	unsigned int cubeMapDescriptorIndex;
    unsigned int prefilteredCubemapDescriptorIndex;
    float sphericalHarmonicsScale;
    int sphericalHarmonics[27]; // Floats scaled by SH_FLOAT_SCALE
    uint pad[2];
};

struct LPMConstants {
    unsigned int u_ctl[24 * 4];
    unsigned int shoulder;
    unsigned int con;
    unsigned int soft;
    unsigned int con2;
    unsigned int clip;
    unsigned int scaleOnly;
    uint displayMode;
    uint pad;
    DirectX::XMMATRIX inputToOutputMatrix;
};

struct VisibleClusterInfo {
    DirectX::XMUINT2 drawcallIndexAndMeshletIndex; // .x = drawcall index, .y = meshlet local index
    unsigned int pad[2];
};

struct SkinningInstanceGPUInfo {
    uint32_t transformOffsetMatrices = 0;
    uint32_t invBindOffsetMatrices = 0;
    uint32_t inverseSkinOffsetMatrices = 0;
    uint32_t boneCount = 0;
    uint32_t flags = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};

constexpr uint32_t kSkinningInstanceFlagRowVectorSkinMatrix = 1u << 0;

struct MeshInstanceClodOffsets
{
    uint clodMeshMetadataIndex;
};

struct CLodMeshMetadata
{
    uint groupsBase;
    uint segmentsBase;
    uint lodNodesBase;
    uint rootNode; // node index (relative to lodNodesBase) to start traversal from
    uint groupChunkTableBase;
    uint groupChunkTableCount;
    uint pageMapBase; // global offset into GroupPageMap buffer for this mesh
    uint lodLevelInfoBase;
    uint lodLevelCount;
    uint maxDepth;
};

struct CLodHierarchyLevelInfo
{
    uint32_t rootNode = 0;
    uint32_t nodeRangeOffset = 0;
    uint32_t nodeRangeCount = 0;
    uint32_t pad0 = 0;
};

// GPU-visible page table entry - maps a virtual page ID to a slab + byte offset.
struct PageTableEntry
{
    uint32_t slabIndex      = 0; // Which slab ByteAddressBuffer this page lives in.
    uint32_t slabByteOffset = 0; // Byte offset of the page start within that slab.
};

struct CLodStreamingRequest
{
    uint32_t groupGlobalIndex = 0;
    uint32_t meshInstanceIndex = 0;
    uint32_t meshBufferIndex = 0;
    uint32_t viewId = 0; // low 16 bits: viewId, high 16 bits: quantized priority
};

struct CLodStreamingRuntimeState
{
    uint32_t activeGroupScanCount = 0;
    uint32_t unloadAfterFrames = 120;
    uint32_t activeGroupsBitsetWordCount = 0;
    uint32_t pad2 = 0;
};

struct CLodReplayBufferState {
    uint32_t nodeWriteCount = 0;
    uint32_t meshletWriteCount = 0;
    uint32_t nodeDropped = 0;
    uint32_t meshletDropped = 0;
    uint32_t visibleClusterCombinedCount = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};

struct CLodViewDepthSRVIndex {
    uint32_t cameraBufferIndex = 0;
    uint32_t linearDepthSRVIndex = 0;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};

struct CLodNodeGpuInput {
    uint32_t entrypointIndex = 0;
    uint32_t numRecords = 0;
    uint64_t recordsAddress = 0;
    uint64_t recordStride = 0;
};

struct CLodMultiNodeGpuInput {
    uint32_t numNodeInputs = 0;
    uint32_t pad0 = 0;
    uint64_t nodeInputsAddress = 0;
    uint64_t nodeInputStride = 0;
};

struct VisibleCluster {
    unsigned int viewID;
    unsigned int instanceID;
    unsigned int localMeshletIndex;       // page-local meshlet index
    unsigned int groupID;
    unsigned int pageSlabDescriptorIndex; // pre-resolved page slab descriptor
    unsigned int pageSlabByteOffset;      // pre-resolved page slab byte offset
    unsigned int shadowClipmapIndex;      // Virtual shadow clipmap index, or 0xFFFFFFFF when not applicable
    unsigned int virtualShadowPayload;
    bool hasVirtualShadowBlockData;
    bool virtualShadowBlockOverflowed;
    unsigned int virtualShadowBlockCoordX;
    unsigned int virtualShadowBlockCoordY;
    unsigned int virtualShadowActiveMinPageX;
    unsigned int virtualShadowActiveMinPageY;
    unsigned int virtualShadowActiveMaxPageX;
    unsigned int virtualShadowActiveMaxPageY;
};

inline constexpr uint32_t PackedVisibleClusterViewBits = 8u;
inline constexpr uint32_t PackedVisibleClusterInstanceBits = 24u;
inline constexpr uint32_t PackedVisibleClusterLocalMeshletBits = 14u;
inline constexpr uint32_t PackedVisibleClusterGroupBits = 20u;
inline constexpr uint32_t PackedVisibleClusterPageDescriptorBits = 20u;
inline constexpr uint32_t PackedVisibleClusterPageIndexBits = 10u;
inline constexpr uint32_t PackedVisibleClusterPageShift = 18u;
inline constexpr uint32_t PackedVisibleClusterPageSizeBytes = 1u << PackedVisibleClusterPageShift;
inline constexpr uint32_t PackedVisibleClusterInvalidShadowClipmapIndex = 0xFFFFFFFFu;
inline constexpr uint32_t PackedVisibleClusterStrideBytes = 16u;
inline constexpr uint32_t PackedVisibleClusterVsmClipmapBits = 5u;
inline constexpr uint32_t PackedVisibleClusterVsmBlockCoordBits = 5u;
inline constexpr uint32_t PackedVisibleClusterVsmLocalPageBits = 2u;
inline constexpr uint32_t PackedVisibleClusterVsmClipmapMask = (1u << PackedVisibleClusterVsmClipmapBits) - 1u;
inline constexpr uint32_t PackedVisibleClusterVsmInvalidClipmapBits = PackedVisibleClusterVsmClipmapMask;
inline constexpr uint32_t PackedVisibleClusterVsmClipmapShift = 0u;
inline constexpr uint32_t PackedVisibleClusterVsmBlockXShift = PackedVisibleClusterVsmClipmapShift + PackedVisibleClusterVsmClipmapBits;
inline constexpr uint32_t PackedVisibleClusterVsmBlockYShift = PackedVisibleClusterVsmBlockXShift + PackedVisibleClusterVsmBlockCoordBits;
inline constexpr uint32_t PackedVisibleClusterVsmRectMinXShift = PackedVisibleClusterVsmBlockYShift + PackedVisibleClusterVsmBlockCoordBits;
inline constexpr uint32_t PackedVisibleClusterVsmRectMinYShift = PackedVisibleClusterVsmRectMinXShift + PackedVisibleClusterVsmLocalPageBits;
inline constexpr uint32_t PackedVisibleClusterVsmRectMaxXShift = PackedVisibleClusterVsmRectMinYShift + PackedVisibleClusterVsmLocalPageBits;
inline constexpr uint32_t PackedVisibleClusterVsmRectMaxYShift = PackedVisibleClusterVsmRectMaxXShift + PackedVisibleClusterVsmLocalPageBits;
inline constexpr uint32_t PackedVisibleClusterVsmOverflowShift = PackedVisibleClusterVsmRectMaxYShift + PackedVisibleClusterVsmLocalPageBits;
inline constexpr uint32_t PackedVisibleClusterVsmHasBlockDataShift = PackedVisibleClusterVsmOverflowShift + 1u;

inline VisibleCluster DecodePackedVisibleCluster(const std::byte* data)
{
    uint32_t word0 = 0;
    uint32_t word1 = 0;
    uint32_t word2 = 0;
    uint32_t word3 = 0;
    std::memcpy(&word0, data + 0, sizeof(uint32_t));
    std::memcpy(&word1, data + 4, sizeof(uint32_t));
    std::memcpy(&word2, data + 8, sizeof(uint32_t));
    std::memcpy(&word3, data + 12, sizeof(uint32_t));

    VisibleCluster cluster{};
    cluster.viewID = word0 & 0xFFu;
    cluster.instanceID = (word0 >> PackedVisibleClusterViewBits) & 0xFFFFFFu;
    cluster.localMeshletIndex = word1 & 0x3FFFu;
    cluster.groupID = ((word1 >> PackedVisibleClusterLocalMeshletBits) & 0x3FFFFu) | ((word2 & 0x3u) << 18u);
    cluster.pageSlabDescriptorIndex = (word2 >> 2u) & 0xFFFFFu;
    cluster.pageSlabByteOffset = ((word2 >> 22u) & 0x3FFu) << PackedVisibleClusterPageShift;
    cluster.virtualShadowPayload = word3;

    const uint32_t encodedClipmapIndex =
        (word3 >> PackedVisibleClusterVsmClipmapShift) & PackedVisibleClusterVsmClipmapMask;
    cluster.shadowClipmapIndex = encodedClipmapIndex == PackedVisibleClusterVsmInvalidClipmapBits
        ? PackedVisibleClusterInvalidShadowClipmapIndex
        : encodedClipmapIndex;
    cluster.hasVirtualShadowBlockData = ((word3 >> PackedVisibleClusterVsmHasBlockDataShift) & 0x1u) != 0u;
    cluster.virtualShadowBlockOverflowed = ((word3 >> PackedVisibleClusterVsmOverflowShift) & 0x1u) != 0u;
    cluster.virtualShadowBlockCoordX = (word3 >> PackedVisibleClusterVsmBlockXShift) & 0x1Fu;
    cluster.virtualShadowBlockCoordY = (word3 >> PackedVisibleClusterVsmBlockYShift) & 0x1Fu;
    cluster.virtualShadowActiveMinPageX = (word3 >> PackedVisibleClusterVsmRectMinXShift) & 0x3u;
    cluster.virtualShadowActiveMinPageY = (word3 >> PackedVisibleClusterVsmRectMinYShift) & 0x3u;
    cluster.virtualShadowActiveMaxPageX = (word3 >> PackedVisibleClusterVsmRectMaxXShift) & 0x3u;
    cluster.virtualShadowActiveMaxPageY = (word3 >> PackedVisibleClusterVsmRectMaxYShift) & 0x3u;
    return cluster;
}


enum RootSignatureLayout {
    // RHI/DX12 root-parameter indices for the compact push-constant layout.
    // These are not HLSL register bindings; see the *RootSignatureIndex
    // constants below for the shader-visible binding numbers.
    MiscUintRootParameterIndex = 0,
    ResourceDescriptorIndicesRootParameterIndex = 1,
	IndirectCommandSignatureRootSignatureIndex = 2,
	NumRootSignatureParameters
};

inline constexpr uint32_t MiscUintRootSignatureIndex = 4;
inline constexpr uint32_t ResourceDescriptorIndicesRootSignatureIndex = 5;

enum MiscUintRootConstants { // Used for pass-specific one-off constants, including float payloads bit-cast on the shader side via asfloat()
    UintRootConstant0,
    UintRootConstant1,
    UintRootConstant2,
    UintRootConstant3,
	UintRootConstant4,
	UintRootConstant5,
	UintRootConstant6,
	UintRootConstant7,
	UintRootConstant8,
	UintRootConstant9,
	UintRootConstant10, 
    UintRootConstant11,
    UintRootConstant12,
    UintRootConstant13,
	UintRootConstant14,
    UintRootConstant15,
    UintRootConstant16,
    UintRootConstant17,
    UintRootConstant18,
    UintRootConstant19,
    UintRootConstant20,
    UintRootConstant21,
    UintRootConstant22,
    UintRootConstant23,
    UintRootConstant24,
    UintRootConstant25,
    UintRootConstant26,
    UintRootConstant27,
    MiscPerObjectBufferIndex = UintRootConstant19,
    MiscPerMeshBufferIndex = UintRootConstant20,
    MiscPerMeshInstanceBufferIndex = UintRootConstant21,
    MiscCurrentLightID = UintRootConstant22,
    MiscLightViewIndex = UintRootConstant23,
    MiscEnableShadows = UintRootConstant24,
    MiscEnablePunctualLights = UintRootConstant25,
    MiscEnableGTAO = UintRootConstant26,
	NumMiscUintRootConstants = UintRootConstant27 + 1
};

enum ResourceDescriptorIndicesRootConstants { // Auto-assigned, do not set manually
    ResourceDescriptorIndex0,
    ResourceDescriptorIndex1,
	ResourceDescriptorIndex2,
	ResourceDescriptorIndex3,
	ResourceDescriptorIndex4,
	ResourceDescriptorIndex5,
	ResourceDescriptorIndex6,
	ResourceDescriptorIndex7,
	ResourceDescriptorIndex8,
	ResourceDescriptorIndex9,
	ResourceDescriptorIndex10,
	ResourceDescriptorIndex11,
	ResourceDescriptorIndex12,
	ResourceDescriptorIndex13,
	ResourceDescriptorIndex14,
	ResourceDescriptorIndex15,
	ResourceDescriptorIndex16,
	ResourceDescriptorIndex17,
	ResourceDescriptorIndex18,
	ResourceDescriptorIndex19,
	ResourceDescriptorIndex20,
	ResourceDescriptorIndex21,
	ResourceDescriptorIndex22,
	ResourceDescriptorIndex23,
	ResourceDescriptorIndex24,
	ResourceDescriptorIndex25,
	ResourceDescriptorIndex26,
	ResourceDescriptorIndex27,
	ResourceDescriptorIndex28,
	ResourceDescriptorIndex29,
	ResourceDescriptorIndex30,
    NumResourceDescriptorIndicesRootConstants
};

// Used for root constants in indirect command signatures.
// You can technically use these as regular root constants as well
enum IndirectCommandSignatureRootConstants {
    IndirectCommandSignatureRootConstant0,
    IndirectCommandSignatureRootConstant1,
    IndirectCommandSignatureRootConstant2,
    IndirectCommandSignatureRootConstant3,
    NumIndirectCommandSignatureRootConstants
};
