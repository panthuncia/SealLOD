#ifndef CLOD_RESOLVE_TYPES_HLSLI
#define CLOD_RESOLVE_TYPES_HLSLI

// The samples the CLod resolve paths produce, from full debug detail down to colour only.

struct BarycentricDeriv
{
    float3 m_lambda;
    float3 m_ddx;
    float3 m_ddy;
};

struct ClodResolvedSample
{
    uint2 pixelCoords;
    float linearDepth;
    uint clusterIndex;
    uint meshletTriangleIndex;
    uint meshletIndex;
    float3 positionWS;
    float3 positionVS;
    float3 normalWSBase;
    float3 normalOS;
    float3 vertexColor;
    float3 dpdxWS;
    float3 dpdyWS;
    float2 motionVector;
    MaterialInfo materialInfo;
    uint materialFlags;
    MaterialInputs materialInputs;
};

struct ClodShadingSample
{
    float linearDepth;
    float3 positionWS;
    float3 positionVS;
    uint materialFlags;
    MaterialInputs materialInputs;
};

struct ClodGBufferColorSample
{
    float2 motionVector;
    MaterialInputs materialInputs;
};

struct ClodGBufferDebugSample
{
    uint meshletIndex;
    uint geometryGroupIndex;
    bool isVoxelPath;
    float3 normalOS;
    float2 materialDebugUv;
    float materialDebugUvValid;
    float2 materialDebugUvDerivative;
    float2 voxelDebugUvDensity;
    float3 reyesDebugSourceBarycentrics;
    uint materialDebugFlags;
    float2 motionVector;
    MaterialInputs materialInputs;
};

struct ClodResolvedGBufferSample
{
    uint meshletIndex;
    uint geometryGroupIndex;
    bool isVoxelPath;
    float3 normalOS;
    float2 materialDebugUv;
    float materialDebugUvValid;
    float2 materialDebugUvDerivative;
    float2 voxelDebugUvDensity;
    float3 reyesDebugSourceBarycentrics;
    uint materialDebugFlags;
    float2 motionVector;
    MaterialInputs materialInputs;
};

struct ClodResolvedCommonSample
{
    float linearDepth;
    uint clusterIndex;
    uint meshletTriangleIndex;
    uint meshletIndex;
    uint geometryGroupIndex;
    bool isVoxelPath;
    float3 positionWS;
    float3 positionVS;
    float3 normalWSBase;
    float3 normalOS;
    float3 vertexColor;
    float2 materialDebugUv;
    float materialDebugUvValid;
    float2 materialDebugUvDerivative;
    float2 voxelDebugUvDensity;
    float3 reyesDebugSourceBarycentrics;
    float3 dpdxWS;
    float3 dpdyWS;
    float2 motionVector;
    MaterialInfo materialInfo;
    uint materialFlags;
    MaterialInputs materialInputs;
};

#endif // CLOD_RESOLVE_TYPES_HLSLI
