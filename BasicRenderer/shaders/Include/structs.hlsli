#ifndef __STRUCTS_HLSL__
#define __STRUCTS_HLSL__

struct PSInput {
    float4 position : SV_POSITION; // Screen-space position, required for rasterization
    float4 clipPosition : TEXCOORD0;
    float4 prevClipPosition : TEXCOORD1; // Previous frame position for motion vectors
    float4 positionWorldSpace : TEXCOORD2; // For world-space lighting
    float4 positionViewSpace : TEXCOORD3; // For cascaded shadows
    float3 normalWorldSpace : TEXCOORD4; // For world-space lighting
    float2 texcoord : TEXCOORD5;
    float3 color : TEXCOORD6; // For models with vertex colors
    float3 normalModelSpace : TEXCOORD7; // For debug view
    uint meshletIndex : TEXCOORD8; // For meshlet debug view
    float4 tangentWorldSpace : TEXCOORD9;
};

struct VisBufferPSInput
{
    float4 position : SV_POSITION; // Screen-space position, required for rasterization
    float linearDepth : TEXCOORD0;
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    float3 positionWorldSpace : TEXCOORD1;
    float3 normalWorldSpace : TEXCOORD2;
    float3 color : TEXCOORD3;
    float4 uvSet01 : TEXCOORD4;
    float4 uvSet23 : TEXCOORD5;
    float4 uvSet45 : TEXCOORD6;
    float4 uvSet67 : TEXCOORD7;
    nointerpolation uint materialDataIndex : TEXCOORD8;
#if defined (PSO_ALPHA_TEST)
    float2 texcoord : TEXCOORD9;
#endif
    nointerpolation uint visibleClusterIndex : TEXCOORD10;
    nointerpolation uint viewID : TEXCOORD11;
    nointerpolation uint shadowClipmapIndex : TEXCOORD12;
#else
#if defined (PSO_ALPHA_TEST)
    float2 texcoord : TEXCOORD1;
    nointerpolation uint materialDataIndex : TEXCOORD2;
    nointerpolation uint visibleClusterIndex : TEXCOORD3;
    nointerpolation uint viewID : TEXCOORD4;
#if defined(CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW) && CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    nointerpolation uint shadowClipmapIndex : TEXCOORD5;
#endif
#else
    nointerpolation uint visibleClusterIndex : TEXCOORD1;
    nointerpolation uint viewID : TEXCOORD2;
#if defined(CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW) && CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    nointerpolation uint shadowClipmapIndex : TEXCOORD3;
#endif
#endif
#endif
};

struct VisibilityPerPrimitive
{
    uint triangleIndex : SV_PrimitiveID;
};

struct ClodViewRasterInfo
{
    uint visibilityUAVDescriptorIndex;
    uint opaqueVisibilitySRVDescriptorIndex;
    uint deepVisibilityHeadPointerUAVDescriptorIndex;
    uint scissorMinX;
    uint scissorMinY;
    uint scissorMaxX;
    uint scissorMaxY;
    float viewportScaleX;
    float viewportScaleY;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct CLodDeepVisibilityNode
{
    uint64_t visKey;
    uint next;
    uint flags;
};

struct CLodDeepVisibilityStats
{
    uint truncatedPixelCount;
    uint truncatedNodeCount;
    uint totalResolvedSamples;
    uint maxRawNodeCount;
    uint maxResolvedSamples;
    uint pad0;
    uint pad1;
    uint pad2;
};

static const uint CLOD_AVBOIT_VBOIT_DEFAULT_SLICE_COUNT = 16u;
static const uint CLOD_AVBOIT_VBOIT_DEFAULT_VIRTUAL_SLICE_COUNT = 32u;
static const uint CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_RESOLUTION = 8192u;
static const uint CLOD_AVBOIT_VBOIT_DEFAULT_DOWNSAMPLE_FACTOR = 4u;
static const float CLOD_AVBOIT_VBOIT_EXTINCTION_QUANTIZATION_SCALE = 4096.0f;
static const float CLOD_AVBOIT_VBOIT_DEFAULT_DEPTH_DISTRIBUTION_EXPONENT = 1.0f;
static const float CLOD_AVBOIT_VBOIT_DEFAULT_LOOKUP_DEPTH_BIAS_IN_SLICES = 2.0f;
static const float CLOD_AVBOIT_VBOIT_DEFAULT_ZERO_TRANSMITTANCE_THRESHOLD = 1.0e-3f;
static const float CLOD_AVBOIT_VBOIT_DEFAULT_RESOLUTION_SCALE = 0.25f;
static const float CLOD_AVBOIT_VBOIT_MIN_DEPTH_DISTRIBUTION_EXPONENT = 0.5f;
static const float CLOD_AVBOIT_VBOIT_MAX_DEPTH_DISTRIBUTION_EXPONENT = 2.0f;

struct CLodAVBOITConfig
{
    uint occupancyUAVDescriptorIndex;
    uint coverageUAVDescriptorIndex;
    uint occupancySliceMaskUAVDescriptorIndex;
    uint depthWarpLUTSRVDescriptorIndex;
    uint scalarExtinctionUAVDescriptorIndex;
    uint chromaticExtinctionUAVDescriptorIndex;
    uint integratedTransmittanceUAVDescriptorIndex;
    uint shadingTransmittanceSRVDescriptorIndex;
    uint zeroTransmittanceSliceUAVDescriptorIndex;
    uint sliceCount;
    uint virtualSliceCount;
    uint lowResolutionWidth;
    uint lowResolutionHeight;
    float viewNearDepth;
    float viewFarDepth;
    float depthDistributionExponent;
    float lookupDepthBiasInSlices;
    float zeroTransmittanceThreshold;
    float pad0;
};

static const uint CLOD_AVBOIT_VBOIT_DEPTH_WARP_FLAG_FILTER_TO_NEXT = 1u;

struct CLodAVBOITDepthWarpLUTEntry
{
    float warpedSliceCoordinate;
    uint flags;
};

struct CLodAVBOITFitState
{
    uint fittedVirtualSliceCount;
    uint occupiedVirtualSliceCount;
    float fittedDepthDistributionExponent;
    uint pad1;
};

struct CLodAVBOITEarlyDepthTileIndirectCommand
{
    uint lowResolutionPixelX;
    uint lowResolutionPixelY;
    uint zeroTransmittanceSlice;
    uint vertexCountPerInstance;
    uint instanceCount;
    uint startVertexLocation;
    uint startInstanceLocation;
};

struct ClippingPlane {
    float4 plane;
};

struct Camera {
    float4 positionWorldSpace;
    row_major matrix view;
    row_major matrix viewInverse;
    row_major matrix projection;
    row_major matrix projectionInverse;
    row_major matrix viewProjection;
    
    row_major matrix prevView;
    row_major matrix prevJitteredProjection;
    row_major matrix prevUnjitteredProjection;
    
    row_major matrix unjitteredProjection;

    ClippingPlane clippingPlanes[6];
    
    float fov;
    float aspectRatio;
    float zNear;
    float zFar;

    int depthBufferArrayIndex;
    uint depthResX;
    uint depthResY;
    uint numDepthMips;
    
    bool isOrtho;
    float2 UVScaleToNextPowerOf2;
    uint lodResY;
};

struct CullingCameraInfo
{
    float4 positionWorldSpace;
    float projX;
    float projY;
    float zNear;
    float errorOverDistanceThreshold; // Threshold for (error * scale) / distance metric
    uint isOrtho;
    float viewportWidth;
    float viewportHeight;
    float reyesDiceRatePixels;
    float4 viewRightWorld;
    float4 viewUpWorld;
    float4 viewForwardWorld;
    row_major matrix viewProjection;
    float4 viewZ;
    row_major matrix viewInverse;
    row_major matrix projectionInverse;
};

struct PerFrameBuffer {
    float4 ambientLighting;
    float4 shadowCascadeSplits;
    
    uint mainCameraIndex;
    uint numLights;
    uint numDirectionalClipmaps;
    
    unsigned int activeEnvironmentIndex;
    
    uint outputType;
    uint screenResX;
    uint screenResY;
    uint lightClusterGridSizeX;
    
    uint lightClusterGridSizeY;
    uint lightClusterGridSizeZ;
    uint nearClusterCount; // how many uniform slices up close
    float clusterZSplitDepth; // view-space depth to switch to log
    
    uint frameIndex; // 0 to 64
    uint shadowVirtualSmrtDirectionalCountsPacked;
    float shadowVirtualSmrtMaxRayAngleFromLightDegrees;
    float shadowVirtualSmrtRayLengthScaleDirectional;
    float shadowVirtualSmrtMaxTraceDistanceWorld;
    uint terrainStochasticSamplingEnabled;
    uint terrainStochasticDiffuseEnabled;
    uint terrainStochasticNormalEnabled;
    uint terrainStochasticDerivativeNormalsEnabled;
    float terrainStochasticBlendCurve;
    uint terrainGaussianStochasticEnabled;
    uint3 terrainStochasticPad;
    uint parallaxOcclusionMappingEnabled;
    uint terrainParallaxOcclusionMappingEnabled;
    float terrainParallaxHeightScale;
    uint terrainParallaxMaxSteps;
    float heightFadeStartDistance;
    float heightFadeEndDistance;
    uint terrainRvtEnabled;
    uint terrainRvtForceDirectFallback;
    uint terrainRvtDebugView;
    uint terrainRvtTelemetryEnabled;
    float terrainReyesDisplacementScale;
    float objectReyesDisplacementScale;
    float objectParallaxHeightScale;
};

static const uint TERRAIN_RVT_CONTENT_HEIGHT = 1u << 0;
static const uint TERRAIN_RVT_CONTENT_MATERIAL = 1u << 1;
static const uint TERRAIN_RVT_PAGE_VALID = 1u << 31;
static const uint TERRAIN_RVT_PAGE_VISITED = 1u << 30;
static const uint TERRAIN_RVT_PAGE_CONTENT_SHIFT = 28u;
static const uint TERRAIN_RVT_PAGE_CONTENT_MASK = 0x3u << TERRAIN_RVT_PAGE_CONTENT_SHIFT;
static const uint TERRAIN_RVT_PAGE_PHYSICAL_MASK = 0x00FFFFFFu;
static const uint TERRAIN_RVT_INFO_INITIALIZED = 1u << 0;
static const uint TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT = 1u << 0;

struct TerrainRvtInfo
{
    uint pageSize;
    uint borderTexels;
    uint physicalTileTexelSide;
    uint physicalAtlasPagesWide;
    uint physicalAtlasPagesHigh;
    uint maxPhysicalPages;
    uint maxVirtualPageTableEntries;
    uint maxRequests;
    uint maxGenerationEntries;
    uint mipCount;
    uint pageTableResolution;
    uint flags;
    float basePageWorldSize;
    uint physicalAtlasPoolCount;
    uint maxTerrainSets;
    uint maxClipLevels;
    uint maxGeneratedPagesPerFrame;
    float mipOffset;
};

struct TerrainRvtClipInfo
{
    uint terrainSetIndex;
    uint clipLevel;
    uint tableBaseSlot;
    uint tableResolution;
    uint2 originPage;
    uint2 terrainPageCount;
    float pageWorldSize;
    float invPageWorldSize;
    uint valid;
    uint terrainClipCount;
    int2 clearDelta;
};

struct TerrainRvtPageTag
{
    uint terrainSetIndex;
    uint clipLevel;
    uint pageX;
    uint pageY;
};

struct TerrainRvtPageRequest
{
    uint pageTableIndex;
    uint terrainSetIndex;
    uint clipLevel;
    uint contentMask;
    uint pageX;
    uint pageY;
    uint pad0;
    uint pad1;
};

struct TerrainRvtGenerationRequest
{
    uint pageTableIndex;
    uint physicalPageIndex;
    uint contentMask;
    uint terrainSetIndex;
    uint clipLevel;
    uint pageX;
    uint pageY;
    uint pad0;
};

struct TerrainRvtPhysicalPageAtlasInfo
{
    float2 atlasBaseUv;
    float2 pageUvScale;
    float poolIndex;
    float3 pad0;
};

struct TerrainRvtHeightResidentCacheEntry
{
    uint status;
    uint requestedTerrainSetIndex;
    uint requestedClipLevel;
    uint requestedPageX;
    uint requestedPageY;
    uint residentClipLevel;
    uint residentPageTableIndex;
    uint physicalPageIndex;
    uint residentPageX;
    uint residentPageY;
    uint pad0;
    uint pad1;
};

struct TerrainRvtStats
{
    uint heightRequests;
    uint materialRequests;
    uint requestOverflows;
    uint generatedPages;
    uint allocationFailures;
    uint heightFallbacks;
    uint materialFallbacks;
    uint residentHits;
    uint heightSampleAttempts;
    uint materialSampleAttempts;
    uint heightSampleHits;
    uint materialSampleHits;
    uint heightPageTableMisses;
    uint materialPageTableMisses;
    uint heightComputePageFailures;
    uint materialComputePageFailures;
    uint heightDisabledFallbacks;
    uint materialDisabledFallbacks;
    uint heightForcedFallbacks;
    uint materialForcedFallbacks;
    uint markComputePageFailures;
    uint markWorldRectCalls;
    uint markWorldRectPages;
    uint resolveResidentPages;
    uint generationHeightPages;
    uint generationMaterialPages;
    uint generationCombinedPages;
    uint generationTexels;
    uint materialSampleRequestedPageXor;
    uint materialSampleResidentPageXor;
    uint materialSamplePhysicalPageXor;
    uint materialSampleRequestedPageMin;
    uint materialSampleRequestedPageMax;
    uint materialSampleResidentPageMin;
    uint materialSampleResidentPageMax;
    uint materialSamplePhysicalPageMin;
    uint materialSamplePhysicalPageMax;
    uint materialSampleCoarserResidentHits;
    uint materialSampleAtlasPoolMask;
    uint heightOwnerMismatches;
    uint materialOwnerMismatches;
    uint requestPageTableXor;
    uint requestPageTableMin;
    uint requestPageTableMax;
    uint generationPageTableMin;
    uint generationPageTableMax;
    uint materialSampleAttemptedPageXor;
    uint materialSampleAttemptedPageMin;
    uint materialSampleAttemptedPageMax;
    uint materialSamplePageMissRequestedPageXor;
    uint materialSamplePageMissRequestedPageMin;
    uint materialSamplePageMissRequestedPageMax;
    uint heightSampleAttemptedPageXor;
    uint heightSampleAttemptedPageMin;
    uint heightSampleAttemptedPageMax;
    uint heightSamplePageMissRequestedPageXor;
    uint heightSamplePageMissRequestedPageMin;
    uint heightSamplePageMissRequestedPageMax;
    uint heightFastSampleAttempts;
    uint heightFastSampleHits;
    uint heightFastPageMissRequests;
    uint heightFullSampleAttempts;
    uint heightFullSampleHits;
    uint generationPageTableXor;
    uint generationPhysicalPageXor;
    uint physicalPageOwnerCollisions;
    uint heightRequestMipHistogram[16];
    uint materialRequestMipHistogram[16];
    uint heightSampleMipHistogram[16];
    uint materialSampleMipHistogram[16];
    uint generationMipHistogram[16];
};

struct BoundingSphere {
    float4 sphere;
};

struct LightInfo {
    uint type;
    float innerConeAngle;
    float outerConeAngle;
    int shadowViewInfoIndex; // -1 if no shadow map
    
    float4 posWorldSpace; // Position of the light
    float4 dirWorldSpace; // Direction of the light
    float4 attenuation; // x,y,z = constant, linear, quadratic attenuation, w= max range
    float4 color; // Color of the light
    
    float nearPlane;
    float farPlane;
    int shadowMapIndex;
    int shadowSamplerIndex;
    
    bool shadowCaster;
    BoundingSphere boundingSphere;
    float maxRange;
    float shadowSourceRadius;
    float shadowSourceAngleDegrees;
};

struct MaterialInfo {
    uint materialFlags;
    uint baseColorTextureIndex;
    uint baseColorSamplerIndex;
    uint normalTextureIndex;
    
    uint normalSamplerIndex;
    uint metallicTextureIndex;
    uint metallicSamplerIndex;
    uint roughnessTextureIndex;
    
    uint roughnessSamplerIndex;
    uint emissiveTextureIndex;
    uint emissiveSamplerIndex;
    uint aoMapIndex;
    
    uint aoSamplerIndex;
    uint heightMapIndex;
    uint heightSamplerIndex;
    uint opacityTextureIndex;
    
    uint opacitySamplerIndex;
    float metallicFactor;
    float roughnessFactor;
    float ambientStrength;
    
    float specularStrength;
    float textureScale;
    float heightMapScale;
    float alphaCutoff;

    float geometricDisplacementMin;
    float geometricDisplacementMax;
    uint geometricDisplacementEnabled;
    uint terrainSetIndex;
    
    float4 baseColorFactor;
    float4 emissiveFactor;
    
    uint4 baseColorChannels;
    
    uint3 normalChannels;
    uint compileFlagsID;
    
    uint aoChannel;
    uint heightChannel;
    uint metallicChannel;
    uint roughnessChannel;

    uint3 emissiveChannels;
    uint rasterBucketIndex;

    uint baseColorUvSetIndex;
    uint normalUvSetIndex;
    uint metallicUvSetIndex;
    uint roughnessUvSetIndex;

    uint emissiveUvSetIndex;
    uint aoUvSetIndex;
    uint heightUvSetIndex;
    uint opacityUvSetIndex;

    uint openPBRMaterialDataIndex;
    uint baseColorStreamingTextureID;
    uint normalStreamingTextureID;
    uint metallicStreamingTextureID;
    uint roughnessStreamingTextureID;
    uint emissiveStreamingTextureID;
    uint aoStreamingTextureID;
    uint heightStreamingTextureID;
    uint opacityStreamingTextureID;
    float2 reyesUvDensity;
    float objectSurfaceTexelDensity;
    uint objectSurfaceSamplingMode;
    uint4 padObjectSurface;
    float4 glintParameters;
    uint glintEnabled;
    uint3 padGlint;
};

struct MaterialEvalInfo {
    uint materialFlags;
    uint baseColorTextureIndex;
    uint baseColorSamplerIndex;
    uint normalTextureIndex;

    uint normalSamplerIndex;
    uint metallicTextureIndex;
    uint metallicSamplerIndex;
    uint roughnessTextureIndex;

    uint roughnessSamplerIndex;
    uint emissiveTextureIndex;
    uint emissiveSamplerIndex;
    uint aoMapIndex;

    uint aoSamplerIndex;
    uint heightMapIndex;
    uint heightSamplerIndex;
    uint opacityTextureIndex;

    uint opacitySamplerIndex;
    float metallicFactor;
    float roughnessFactor;
    float heightMapScale;

    float alphaCutoff;
    float geometricDisplacementMin;
    float geometricDisplacementMax;
    uint geometricDisplacementEnabled;

    float4 baseColorFactor;
    float4 emissiveFactor;
    uint4 baseColorChannels;

    uint3 normalChannels;
    uint terrainSetIndex;

    uint aoChannel;
    uint heightChannel;
    uint metallicChannel;
    uint roughnessChannel;

    uint3 emissiveChannels;
    uint openPBRMaterialDataIndex;

    uint baseColorUvSetIndex;
    uint normalUvSetIndex;
    uint metallicUvSetIndex;
    uint roughnessUvSetIndex;

    uint emissiveUvSetIndex;
    uint aoUvSetIndex;
    uint heightUvSetIndex;
    uint opacityUvSetIndex;

    uint baseColorStreamingTextureID;
    uint normalStreamingTextureID;
    uint metallicStreamingTextureID;
    uint roughnessStreamingTextureID;
    uint emissiveStreamingTextureID;
    uint aoStreamingTextureID;
    uint heightStreamingTextureID;
    uint opacityStreamingTextureID;
    float2 reyesUvDensity;
    float objectSurfaceTexelDensity;
    uint objectSurfaceSamplingMode;
    uint4 padObjectSurface;
    float4 glintParameters;
    uint glintEnabled;
    uint3 padGlint;
};

struct TerrainLayerInfo {
    uint diffuseTextureIndex;
    uint diffuseSamplerIndex;
    uint normalTextureIndex;
    uint normalSamplerIndex;
    uint heightTextureIndex;
    uint heightSamplerIndex;
    uint rmaosTextureIndex;
    uint rmaosSamplerIndex;
    uint diffuseStreamingTextureID;
    uint normalStreamingTextureID;
    uint heightStreamingTextureID;
    uint rmaosStreamingTextureID;
    uint3 normalChannels;
    uint flags;
    float4 fallbackColor;
    float uvScale;
    uint stochasticLayerIndex;
    float heightScale;
    float roughnessScale;
    float specularLevel;
    float4 glintParameters;
    float4 farOverlayParams;
};

struct TerrainStochasticLayerInfo {
    uint diffuseGaussianTextureIndex;
    uint diffuseInverseLutTextureIndex;
    uint diffuseInverseLutSamplerIndex;
    uint diffuseFlags;
    uint normalGaussianTextureIndex;
    uint normalInverseLutTextureIndex;
    uint normalInverseLutSamplerIndex;
    uint normalFlags;
    float stochasticScale;
    float diffuseLutHeight;
    float normalLutHeight;
    float heightLutHeight;
    float4 diffuseColorSpaceOrigin;
    float4 diffuseColorSpaceVector0;
    float4 diffuseColorSpaceVector1;
    float4 diffuseColorSpaceVector2;
    uint heightGaussianTextureIndex;
    uint heightInverseLutTextureIndex;
    uint heightInverseLutSamplerIndex;
    uint heightFlags;
};

struct TerrainLayerRefInfo {
    uint layerIndex;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct TerrainRegionInfo {
    int regionX;
    int regionY;
    uint layerRefStart;
    uint layerRefCount;
    uint weightBlockStart;
    uint weightSampleSide;
    uint weightSamplesPerLayer;
    uint pad1;
};

struct TerrainSetInfo {
    int minRegionX;
    int minRegionY;
    uint regionCountX;
    uint regionCountY;
    uint regionBase;
    uint regionCount;
    uint layerBase;
    uint layerCount;
    uint layerRefBase;
    uint layerRefCount;
    uint weightBlockBase;
    uint weightBlockCount;
    float regionSizeWorld;
    float pad0;
    float pad1;
    float pad2;
};

struct OpenPBRMaterialInfo {
    float baseWeight;
    float3 baseColor;
    float baseDiffuseRoughness;
    float baseMetalness;
    float subsurfaceWeight;
    float subsurfaceRadius;

    float3 subsurfaceColor;
    float subsurfaceScatterAnisotropy;
    float3 subsurfaceRadiusScale;
    float specularWeight;

    float3 specularColor;
    float specularRoughness;
    float specularRoughnessAnisotropy;
    float specularIor;
    float2 specularAnisotropyRotationCosSin;

    float coatWeight;
    float3 coatColor;
    float coatRoughness;
    float coatRoughnessAnisotropy;
    float coatIor;
    float coatDarkening;
    float2 coatAnisotropyRotationCosSin;

    float fuzzWeight;
    float3 fuzzColor;
    float fuzzRoughness;
    float transmissionWeight;
    float3 transmissionColor;
    float transmissionDepth;

    float3 transmissionScatter;
    float transmissionScatterAnisotropy;
    float transmissionDispersionScale;
    float transmissionDispersionAbbeNumber;
    float thinFilmWeight;
    float thinFilmThickness;
    float thinFilmIor;
    float emissionLuminance;

    float3 emissionColor;
    float geometryOpacity;
    uint geometryThinWalled;
    uint pad0;
    uint pad1;
    uint pad2;

    uint coatColorTextureIndex;
    uint coatColorSamplerIndex;
    uint coatWeightTextureIndex;
    uint coatWeightSamplerIndex;

    uint coatRoughnessTextureIndex;
    uint coatRoughnessSamplerIndex;
    uint fuzzColorTextureIndex;
    uint fuzzColorSamplerIndex;

    uint fuzzWeightTextureIndex;
    uint fuzzWeightSamplerIndex;
    uint fuzzRoughnessTextureIndex;
    uint fuzzRoughnessSamplerIndex;

    uint4 coatColorChannels;
    uint coatWeightChannel;
    uint coatRoughnessChannel;
    uint coatTexturePad0;

    uint4 fuzzColorChannels;
    uint fuzzWeightChannel;
    uint fuzzRoughnessChannel;
    uint fuzzTexturePad0;

    uint coatColorUvSetIndex;
    uint coatWeightUvSetIndex;
    uint coatRoughnessUvSetIndex;
    uint fuzzColorUvSetIndex;

    uint fuzzWeightUvSetIndex;
    uint fuzzRoughnessUvSetIndex;
    uint coatColorStreamingTextureID;
    uint coatWeightStreamingTextureID;
    uint coatRoughnessStreamingTextureID;
    uint fuzzColorStreamingTextureID;
    uint fuzzWeightStreamingTextureID;
    uint fuzzRoughnessStreamingTextureID;
};

struct TextureStreamingGPUInfo {
    uint flags;
    uint totalMipCount;
    uint residentTopMip;
    uint residentMipCount;

    uint fullWidth;
    uint fullHeight;

    uint requestedTopMip;
    uint pendingTopMip;
    uint bindingRevisionLo;
    uint bindingRevisionHi;
};

struct SingleMatrix {
    row_major matrix value;
};

struct PerObjectBuffer {
    row_major matrix model;
    row_major matrix prevModel;
    row_major matrix modelInverse;
    uint normalMatrixBufferIndex;
    uint objectFlags;
    uint pad[2];
};

struct PerMeshBuffer {
    uint materialDataIndex;
    uint materialEvalCompileFlagsID;
    uint materialReyesEvalCompileFlagsID;
    uint rasterBucketIndex;
    uint vertexFlags;
    uint vertexByteSize;
    uint skinningVertexByteSize;

    BoundingSphere boundingSphere;
    
    uint clodMeshletBufferOffset;
    uint clodMeshletVerticesBufferOffset;
    uint clodMeshletTrianglesBufferOffset;
    uint clodNumMeshlets;
    
    uint vertexBufferOffset;
    uint numVertices;
    uint numMeshlets;
};

struct PerMeshInstanceBuffer {
    uint perMeshBufferIndex;
    uint perObjectBufferIndex;
    uint skinningInstanceSlot;
    float skinnedBoundsScale;
    BoundingSphere boundingSphere;
};

struct PerInstanceTransformBuffer {
    row_major matrix model;
    row_major matrix prevModel;
    row_major matrix modelInverse;
    uint normalMatrixBufferIndex;
    uint objectFlags;
    uint pad[2];
};

struct InstanceDrawRecordBuffer {
    uint meshTemplateIndex;
    uint instanceTransformIndex;
    uint clodOffsetIndex;
    uint skinnedAssemblyPlacementIndex;
    uint skinningTypeSlot;
};

struct SkinnedAssemblyPlacementBuffer {
    uint instanceTransformIndex;
    uint skinningTypeSlot;
    uint stableSceneId;
    uint generation;
    float4 localBoundingSphere;
    float boundsScale;
    uint3 pad;
};

#define LIGHTS_PER_PAGE 12
#define LIGHT_PAGE_ADDRESS_NULL 0xFFFFFFFF
struct LightPage {
    uint ptrNextPage;
    uint numLightsInPage;
    uint lightIndices[LIGHTS_PER_PAGE];
};

struct Cluster {
    float4 minPoint;
    float4 maxPoint;
    uint numLights;
    uint ptrFirstPage;
    uint pad[2];
};

struct GTAOConstants {
    uint2 ViewportSize;
    float2 ViewportPixelSize; // .zw == 1.0 / ViewportSize.xy

    float2 DepthUnpackConsts;
    float2 CameraTanHalfFOV;

    float2 NDCToViewMul;
    float2 NDCToViewAdd;

    float2 NDCToViewMul_x_PixelSize;
    float EffectRadius; // world (viewspace) maximum size of the shadow
    float EffectFalloffRange;

    float RadiusMultiplier;
    float2 SourceDepthUVScale;
    float Padding0;
    float FinalValuePower;
    float DenoiseBlurBeta;

    float SampleDistributionPower;
    float ThinOccluderCompensation;
    float DepthMIPSamplingOffset;
    int NoiseIndex; // frameIndex % 64 if using TAA or 0 otherwise
};

struct GTAOInfo {
    GTAOConstants g_GTAOConstants;
};

struct FragmentInfo {
    float2 pixelCoords;
    float3 fragPosWorldSpace;
    float3 fragPosViewSpace;
    uint openPBRMaterialDataIndex;
    float3 normalWS;
    float3 diffuseColor;
    float baseDiffuseRoughness;
    float specularAlpha;
    float weightedSpecularIor;
    float dielectricSpecularWeight;
    float3 dielectricSpecularF0;
    float metalSpecularWeight;
    float3 metalSpecularF0;
    float3 metalAverageFresnel;
    float3 albedo;
    float coatWeight;
    float3 coatColor;
    float coatPerceptualRoughness;
    float coatRoughness;
    float3 coatF0;
    float coatIor;
    float coatDarkening;
    float fuzzWeight;
    float3 fuzzColor;
    float fuzzRoughness;
    float alpha;
    float diffuseAmbientOcclusion;
    float metallic;
    float perceptualRoughnessUnclamped;
    float perceptualRoughness;
    float roughness;
    float roughnessUnclamped;
    float3 emissive;
    //float2 DFG; // Replaced by MaterialX quadratic fit
    float3 viewWS;
    float NdotV;
    float reflectance;
    float dielectricF0;
    float3 F0;
    float3 reflectedWS;
    uint heightMapIndex;
    uint heightMapSamplerIndex;
    uint materialFlags;
    uint selectedMaterialMipLevel;
    uint selectedMaterialMipMaxLevel;
    uint parallaxApplied;
    uint glintEnabled;
    float geometricHeightDebug;
    float4 glintParameters;
};

struct EnvironmentInfo {
    uint cubeMapDescriptorIndex;
    uint prefilteredCubemapDescriptorIndex;
    float sphericalHarmonicsScale;
    int sphericalHarmonics[27]; // floats scaled by SH_FLOAT_SCALE
    uint pad[2];
};

struct LPMConstants
{
    uint u_ctl[24 * 4];
    uint shoulder;
    uint con;
    uint soft;
    uint con2;
    uint clip;
    uint scaleOnly;
    uint displayMode;
    uint pad;
    float4x4 inputToOutputMatrix;
};

static const uint MATERIAL_DEBUG_INVALID_MIP_LEVEL = 0xffffffffu;

struct MaterialInputs
{
    float3 albedo;
    float3 normalWS;
    float3 emissive;
    float3 coatColor;
    float metallic;
    float roughness;
    float coatWeight;
    float coatRoughness;
    float3 fuzzColor;
    float fuzzWeight;
    float fuzzRoughness;
    float opacity;
    float ambientOcclusion;
    uint openPBRMaterialDataIndex;
    uint selectedMaterialMipLevel;
    uint selectedMaterialMipMaxLevel;
    uint parallaxApplied;
    uint terrainRvtDebugFlags;
    uint terrainRvtRequestedMip;
    uint terrainRvtResidentMip;
    uint terrainRvtPageTableIndex;
    uint terrainRvtPhysicalPageIndex;
    uint terrainRvtAtlasPoolIndex;
    uint terrainRvtOwnerPageTableIndex;
    uint terrainRvtFallbackReason;
    uint2 terrainRvtPageCoord;
    float2 terrainRvtPageUv;
    float3 terrainRvtAtlasUv;
    float2 terrainRvtPhysicalTileUv;
    float3 terrainRvtSampleAlbedo;
    float3 terrainRvtSampleAlbedoPoint;
    float3 terrainRvtSampleNormal;
    float3 terrainRvtSampleMaterial;
    float terrainRvtHeightScale;
    float2 terrainRvtLocal;
    uint terrainRvtTerrainClipCount;
    float geometricHeightDebug;
    uint glintEnabled;
    float4 glintParameters;
};

struct SkinningInstanceGPUInfo
{
    // Offset into Builtin::SkeletonResources::BoneTransforms, which stores
    // row-vector inverseBind * animatedGlobal skin matrices.
    uint transformOffsetMatrices;
    // Kept for CPU/debug compatibility; forward skinning no longer reads this in shaders.
    uint invBindOffsetMatrices;
    uint inverseSkinOffsetMatrices;
    uint boneCount;
    uint flags;
    uint pad0;
    uint previousTransformOffsetMatrices;
    uint stableSceneId;
	uint boneRemapDescriptor;
	uint boneRemapOffset;
	uint sourceBoneCount;
	uint skeletonLodVariant;
};

// Legacy/cache compatibility bit. Palette orientation is now canonical and does
// not vary per instance.
static const uint SkinningInstanceFlag_RowVectorSkinMatrix = 1u << 0;

// TODO: packing?
/*
struct ClusterCandidateNode
{
    uint viewIndex;

    uint perMeshBufferIndex;
    uint perMeshInstanceBufferIndex;
    uint perObjectBufferIndex;

    uint rootGroupGlobal; // absolute group index in global groups buffer
    uint flags; // bits: fullyInside, skipFrustum, wasVisibleLastFrame, etc.
};*/

struct VisibleCluster
{
    unsigned int viewID;
    unsigned int instanceID;
    unsigned int localMeshletIndex;       // page-local meshlet index
    unsigned int groupID;
    unsigned int pageSlabDescriptorIndex; // pre-resolved page slab descriptor
    unsigned int pageSlabByteOffset;      // pre-resolved page slab byte offset
    unsigned int shadowClipmapIndex;
};

struct CLodReyesFullClusterOutput
{
    uint visibleClusterIndex;
    uint instanceID;
    uint materialIndex;
    uint flags;
};

struct CLodReyesOwnedClusterEntry
{
    uint visibleClusterIndex;
    uint instanceID;
    uint materialIndex;
    uint flags;
};

struct CLodReyesTessTableConfigEntry
{
    uint firstTriangle;
    uint firstVertex;
    uint numTriangles;
    uint numVertices;
};

struct CLodReyesSplitQueueEntry
{
    uint visibleClusterIndex;
    uint instanceID;
    uint localMeshletIndex;
    uint materialIndex;
    uint viewID;
    uint splitLevel;
    uint quantizedTessFactor;
    uint flags;
    uint sourcePrimitiveAndSplitConfig;
    float2 domainVertex0UV;
    float2 domainVertex1UV;
    float2 domainVertex2UV;
};

struct CLodReyesDiceQueueEntry
{
    uint visibleClusterIndex;
    uint instanceID;
    uint localMeshletIndex;
    uint materialIndex;
    uint viewID;
    uint splitLevel;
    uint quantizedTessFactor;
    uint flags;
    uint sourcePrimitiveAndSplitConfig;
    float2 domainVertex0UV;
    float2 domainVertex1UV;
    float2 domainVertex2UV;
    uint tessTableConfigIndex;
    uint reserved;
};

static const uint CLOD_REYES_FLAG_SKINNED = 1u << 0;
static const uint CLOD_REYES_FLAG_DISPLACEMENT_ENABLED = 1u << 1;
static const uint CLOD_REYES_FLAG_COARSE_DIRTY_ONLY_LEAF = 1u << 2;
static const uint CLOD_REYES_FLAG_ROUTE_SHIFT = 8u;
static const uint CLOD_REYES_FLAG_ROUTE_MASK = 0x3u << CLOD_REYES_FLAG_ROUTE_SHIFT;
static const uint CLOD_REYES_ROUTE_VISIBILITY = 0u;
static const uint CLOD_REYES_ROUTE_FINE_MICROPOLY_VSM = 1u;
static const uint CLOD_REYES_ROUTE_COARSE_HARDWARE_VSM = 2u;

bool CLodReyesIsCoarseDirtyOnlyLeaf(uint flags)
{
    return (flags & CLOD_REYES_FLAG_COARSE_DIRTY_ONLY_LEAF) != 0u;
}

uint CLodReyesDecodeRouteKind(uint flags)
{
    return (flags & CLOD_REYES_FLAG_ROUTE_MASK) >> CLOD_REYES_FLAG_ROUTE_SHIFT;
}

struct CLodReyesDispatchIndirectCommand
{
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

struct CLodReyesRasterWorkEntry
{
    uint diceQueueIndex;
    uint microTriangleOffset;
    uint microTriangleCount;
    uint rasterBucketIndex;
};

struct CLodReyesPackedRasterWorkGroupEntry
{
    uint firstCompactedRasterWorkIndex;
    uint rasterWorkEntryCount;
    uint requestedMicroTriangleCount;
    uint reserved;
};

struct CLodReyesTelemetry
{
    uint visibleClusterInputCount;
    uint fullClusterOutputCount;
    uint ownedClusterOutputCount;
    uint immediateDiceQueueEntryCount;
    uint finalDiceQueueEntryCount;
    uint phaseIndex;
    uint deepestSplitLevelReached;
    uint configuredMaxSplitPassCount;
    uint patchRasterizedPatchCount;
    uint dicedPatchCount;
    uint dicedTriangleEstimateCount;
    uint dicedVertexEstimateCount;
    uint patchRasterizedMicroTriangleCount;
    uint rasterWorkEntryCount;
    uint hardwareRasterMeshGroupCount;
    uint hardwareRasterMicroTriangleCount;
    uint hardwareRasterRequestedMicroTriangleCount;
    uint hardwareRasterPackedWorkEntryCount;
    uint splitInputCounts[5];
    uint splitChildOutputCounts[5];
    uint splitDiceOutputCounts[5];
    uint splitQueueOverflowCounts[5];
    uint diceQueueOverflowCounts[5];
    uint invalidSplitPatchDomainCount;
    uint invalidDicePatchDomainCount;
    uint splitCollapseFallbackDiceCount;
    uint rasterWorkOverflowPatchCount;
    uint rasterWorkOverflowBatchCount;
    uint canonicalFactorTieCount;
    uint flippedTessTableConfigCount;
    uint splitConfigTieCount;
    uint splitFrustumCullCount;
    uint splitShadowDirtyCullCount;
    uint splitChildCullCount;
    uint splitCoarseOnlyDirtyEligibleCount;
    uint splitCoarseOnlyDirtyRejectedCount;
    uint splitCoarseOnlyDirtyLeafOutputCount;
    uint splitConfigSelectionCounts[4];
    uint canonicalRotationCounts[3];
    uint siblingSharedEdgeCheckCount;
    uint siblingSharedEdgeMismatchCount;
    uint rasterClipCullCount;
    uint rasterPreAreaCullCount;
    uint rasterWindingSwapCount;
    uint rasterPostSwapNonNegativeAreaCount;
    uint rasterEmptyBoundsCullCount;
    uint rasterZeroMicroTriangleCount;
    uint rasterMicroTriangleOverflowCount;
    uint rasterNearPlaneClippedQuadCount;
    uint rasterTinyTriangleFallbackCount;
    uint splitOcclusionTestCount;
    uint splitOcclusionDeferCount;
    uint splitOcclusionDropCount;
    uint diceOcclusionTestCount;
    uint diceOcclusionDeferCount;
    uint diceOcclusionDropCount;
    uint replaySplitQueueOverflowCount;
    uint replayDiceQueueOverflowCount;
    uint replaySplitMergeCount;
    uint replayDiceMergeCount;
};

#endif // __STRUCTS_HLSL__
