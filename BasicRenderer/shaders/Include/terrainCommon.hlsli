#ifndef __TERRAIN_COMMON_HLSLI__
#define __TERRAIN_COMMON_HLSLI__

#include "structs.hlsli"
#include "materialFlags.hlsli"

static const float TERRAIN_CELL_SIZE = 4096.0f;
static const float TERRAIN_QUADRANT_SIZE = 2048.0f;
static const uint TERRAIN_MAX_LAYERS = 6u;
static const uint TERRAIN_INVALID_DESCRIPTOR = 0xffffffffu;
static const uint TERRAIN_LAYER_FLAG_SNOW = 1u << 0;

float TerrainDynamicSwizzle(float4 value, uint channel)
{
    if (channel == 0u) return value.x;
    if (channel == 1u) return value.y;
    if (channel == 2u) return value.z;
    if (channel == 3u) return value.w;
    if (channel == 4u) return sqrt(saturate(1.0f - value.x * value.x - value.y * value.y));
    return 1.0f;
}

float3 TerrainDecodeNormal(float4 encoded, uint3 channels)
{
    float3 n;
    n.x = TerrainDynamicSwizzle(encoded, channels.x) * 2.0f - 1.0f;
    n.y = TerrainDynamicSwizzle(encoded, channels.y) * 2.0f - 1.0f;
    n.z = channels.z == 4u
        ? sqrt(saturate(1.0f - n.x * n.x - n.y * n.y))
        : TerrainDynamicSwizzle(encoded, channels.z) * 2.0f - 1.0f;
    return normalize(n);
}

float2 TerrainSkyrimXYFromRendererPosition(float3 positionWS)
{
    return float2(positionWS.x, -positionWS.z);
}

uint TerrainGetLayerIndex(TerrainQuadrantInfo q, uint slot)
{
    if (slot == 0u) return q.layerIndices[0];
    if (slot == 1u) return q.layerIndices[1];
    if (slot == 2u) return q.layerIndices[2];
    if (slot == 3u) return q.layerIndices[3];
    if (slot == 4u) return q.layerIndices[4];
    return q.layerIndices[5];
}

float TerrainGetWeight(float4 weights0, float4 weights1, uint slot)
{
    if (slot == 0u) return weights0.x;
    if (slot == 1u) return weights0.y;
    if (slot == 2u) return weights0.z;
    if (slot == 3u) return weights0.w;
    if (slot == 4u) return weights1.x;
    return weights1.y;
}

void TerrainLoadPackedWeights(
    Texture2D<float4> weightAtlas0,
    Texture2D<float4> weightAtlas1,
    uint2 texel,
    out float4 weights0,
    out float4 weights1)
{
    weights0 = weightAtlas0.Load(int3(texel, 0));
    weights1 = weightAtlas1.Load(int3(texel, 0));
}

float4 TerrainCubicBSpline(float4 p0, float4 p1, float4 p2, float4 p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;

    float w0 = (1.0f - 3.0f * t + 3.0f * t2 - t3) * (1.0f / 6.0f);
    float w1 = (4.0f - 6.0f * t2 + 3.0f * t3) * (1.0f / 6.0f);
    float w2 = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) * (1.0f / 6.0f);
    float w3 = t3 * (1.0f / 6.0f);

    return p0 * w0 + p1 * w1 + p2 * w2 + p3 * w3;
}

void TerrainLoadWeightSample(
    Texture2D<float4> weightAtlas0,
    Texture2D<float4> weightAtlas1,
    TerrainQuadrantInfo q,
    int2 sample,
    out float4 weights0,
    out float4 weights1)
{
    int2 clampedSample = clamp(sample, 0, 16);
    uint2 texel = uint2(q.weightAtlasX, q.weightAtlasY) + uint2(clampedSample);
    TerrainLoadPackedWeights(weightAtlas0, weightAtlas1, texel, weights0, weights1);
}

void TerrainLoadWeightRowCubic(
    Texture2D<float4> weightAtlas0,
    Texture2D<float4> weightAtlas1,
    TerrainQuadrantInfo q,
    int2 baseSample,
    float t,
    out float4 weights0,
    out float4 weights1)
{
    float4 p0_0, p1_0, p2_0, p3_0;
    float4 p0_1, p1_1, p2_1, p3_1;
    TerrainLoadWeightSample(weightAtlas0, weightAtlas1, q, baseSample + int2(-1, 0), p0_0, p0_1);
    TerrainLoadWeightSample(weightAtlas0, weightAtlas1, q, baseSample + int2(0, 0), p1_0, p1_1);
    TerrainLoadWeightSample(weightAtlas0, weightAtlas1, q, baseSample + int2(1, 0), p2_0, p2_1);
    TerrainLoadWeightSample(weightAtlas0, weightAtlas1, q, baseSample + int2(2, 0), p3_0, p3_1);
    weights0 = TerrainCubicBSpline(p0_0, p1_0, p2_0, p3_0, t);
    weights1 = TerrainCubicBSpline(p0_1, p1_1, p2_1, p3_1, t);
}

void TerrainInterpolateLayerWeights(
    Texture2D<float4> weightAtlas0,
    Texture2D<float4> weightAtlas1,
    TerrainQuadrantInfo q,
    float2 quadrantLocal,
    out float4 weights0,
    out float4 weights1)
{
    // Skyrim stores close landscape weights on a 17x17 quadrant lattice and feeds
    // them as vertex attributes. Reconstruct the painted material field from that
    // lattice rather than from the CLod render triangles, which may not match the
    // original terrain topology after virtualized-geometry clustering. A cubic
    // B-spline acts as a small, non-overshooting low-pass filter over abrupt
    // 128-unit paint transitions, which better suits full-quality terrain seen
    // at arbitrary CLod distances than exact interpolation of the coarse lattice.
    float2 grid = saturate(quadrantLocal / TERRAIN_QUADRANT_SIZE) * 16.0f;
    int2 baseSample = (int2)floor(grid);
    float2 f = grid - (float2)baseSample;

    float4 r0_0, r1_0, r2_0, r3_0;
    float4 r0_1, r1_1, r2_1, r3_1;
    TerrainLoadWeightRowCubic(weightAtlas0, weightAtlas1, q, baseSample + int2(0, -1), f.x, r0_0, r0_1);
    TerrainLoadWeightRowCubic(weightAtlas0, weightAtlas1, q, baseSample + int2(0, 0), f.x, r1_0, r1_1);
    TerrainLoadWeightRowCubic(weightAtlas0, weightAtlas1, q, baseSample + int2(0, 1), f.x, r2_0, r2_1);
    TerrainLoadWeightRowCubic(weightAtlas0, weightAtlas1, q, baseSample + int2(0, 2), f.x, r3_0, r3_1);

    weights0 = saturate(TerrainCubicBSpline(r0_0, r1_0, r2_0, r3_0, f.y));
    weights1 = saturate(TerrainCubicBSpline(r0_1, r1_1, r2_1, r3_1, f.y));
}

float3x3 TerrainBasis(float3 normalWS)
{
    float3 tangentWS = cross(float3(0.0f, 0.0f, -1.0f), normalWS);
    if (dot(tangentWS, tangentWS) < 1.0e-5f)
    {
        tangentWS = cross(float3(1.0f, 0.0f, 0.0f), normalWS);
    }
    tangentWS = normalize(tangentWS);
    float3 bitangentWS = normalize(cross(normalWS, tangentWS));
    return float3x3(tangentWS, bitangentWS, normalWS);
}

void ApplyTerrainMaterialInternal(
    uint materialFlags,
    uint terrainSetIndex,
    in float3 positionWS,
    in float3 normalWSBase,
    in float3 vertexColor,
    inout MaterialInputs inputs)
{
    // Close landscape domain: sample full-quality LAND layer weights everywhere.
    // Skyrim distant land overlay/noise blending is intentionally not represented here.
    if ((materialFlags & MATERIAL_TERRAIN) == 0u)
    {
        return;
    }

    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    StructuredBuffer<TerrainLayerInfo> terrainLayers = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Layers)];
    StructuredBuffer<TerrainQuadrantInfo> terrainQuadrants = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Quadrants)];

    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    if (terrain.cellCountX == 0u || terrain.cellCountY == 0u || terrain.layerCount == 0u)
    {
        return;
    }

    float2 skyrimXY = TerrainSkyrimXYFromRendererPosition(positionWS);
    int2 cell = int2(floor(skyrimXY / TERRAIN_CELL_SIZE));
    uint2 localCell;
    localCell.x = (uint)(cell.x - terrain.minCellX);
    localCell.y = (uint)(cell.y - terrain.minCellY);
    if (cell.x < terrain.minCellX || cell.y < terrain.minCellY ||
        localCell.x >= terrain.cellCountX || localCell.y >= terrain.cellCountY)
    {
        return;
    }

    float2 cellOrigin = float2(cell) * TERRAIN_CELL_SIZE;
    float2 local = skyrimXY - cellOrigin;
    uint quadrantX = local.x >= TERRAIN_QUADRANT_SIZE ? 1u : 0u;
    uint quadrantY = local.y >= TERRAIN_QUADRANT_SIZE ? 1u : 0u;
    uint quadrant = quadrantY * 2u + quadrantX;
    uint quadrantIndex = terrain.quadrantBase + ((localCell.y * terrain.cellCountX + localCell.x) * 4u + quadrant);
    if (quadrantIndex >= terrain.quadrantBase + terrain.quadrantCount)
    {
        return;
    }
    TerrainQuadrantInfo q = terrainQuadrants[quadrantIndex];

    float2 quadrantLocal = local - float2(quadrantX, quadrantY) * TERRAIN_QUADRANT_SIZE;

    float4 weights0 = float4(1.0f, 0.0f, 0.0f, 0.0f);
    float4 weights1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (terrain.weightAtlas0TextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
        terrain.weightAtlas1TextureIndex != TERRAIN_INVALID_DESCRIPTOR)
    {
        Texture2D<float4> weightAtlas0 = ResourceDescriptorHeap[NonUniformResourceIndex(terrain.weightAtlas0TextureIndex)];
        Texture2D<float4> weightAtlas1 = ResourceDescriptorHeap[NonUniformResourceIndex(terrain.weightAtlas1TextureIndex)];
        TerrainInterpolateLayerWeights(weightAtlas0, weightAtlas1, q, quadrantLocal, weights0, weights1);
    }

    float weightSum = weights0.x + weights0.y + weights0.z + weights0.w + weights1.x + weights1.y;
    float invWeightSum = rcp(max(weightSum, 1.0e-4f));

    float2 skyrimXYDdx = ddx(skyrimXY);
    float2 skyrimXYDdy = ddy(skyrimXY);

    float3 blendedBaseColor = 0.0f.xxx;
    float3 blendedNormalTS = 0.0f.xxx;
    [unroll]
    for (uint slot = 0u; slot < TERRAIN_MAX_LAYERS; ++slot)
    {
        float weight = TerrainGetWeight(weights0, weights1, slot) * invWeightSum;
        if (weight <= 0.0001f)
        {
            continue;
        }

        uint layerIndex = min(terrain.layerBase + TerrainGetLayerIndex(q, slot), terrain.layerBase + terrain.layerCount - 1u);
        TerrainLayerInfo layer = terrainLayers[layerIndex];
        float2 layerUv = skyrimXY * layer.uvScale;
        float2 layerDUdx = skyrimXYDdx * layer.uvScale;
        float2 layerDUdy = skyrimXYDdy * layer.uvScale;

        float3 layerBaseColor = layer.fallbackColor.rgb;
        if (layer.diffuseTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            Texture2D<float4> diffuseTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.diffuseTextureIndex)];
            SamplerState diffuseSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
            layerBaseColor = diffuseTex.SampleGrad(diffuseSampler, layerUv, layerDUdx, layerDUdy).rgb;
        }
        blendedBaseColor += layerBaseColor * weight;

        float3 layerNormalTS = float3(0.0f, 0.0f, 1.0f);
        if (layer.normalTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            Texture2D<float4> normalTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.normalTextureIndex)];
            SamplerState normalSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.normalSamplerIndex)];
            layerNormalTS = TerrainDecodeNormal(normalTex.SampleGrad(normalSampler, layerUv, layerDUdx, layerDUdy), layer.normalChannels);
        }
        blendedNormalTS += layerNormalTS * weight;
    }

    float3x3 basis = TerrainBasis(normalWSBase);
    inputs.albedo = blendedBaseColor * vertexColor;
    inputs.normalWS = normalize(mul(normalize(blendedNormalTS), basis));
    inputs.metallic = 0.0f;
    inputs.roughness = 0.9f;
    inputs.ambientOcclusion = 1.0f;
    inputs.opacity = 1.0f;
    inputs.emissive = 0.0f.xxx;
}

void ApplyTerrainMaterial(
    in MaterialInfo materialInfo,
    in float3 positionWS,
    in float3 normalWSBase,
    in float3 vertexColor,
    inout MaterialInputs inputs)
{
    ApplyTerrainMaterialInternal(materialInfo.materialFlags, materialInfo.terrainSetIndex, positionWS, normalWSBase, vertexColor, inputs);
}

void ApplyTerrainMaterial(
    in MaterialEvalInfo materialInfo,
    in float3 positionWS,
    in float3 normalWSBase,
    in float3 vertexColor,
    inout MaterialInputs inputs)
{
    ApplyTerrainMaterialInternal(materialInfo.materialFlags, materialInfo.terrainSetIndex, positionWS, normalWSBase, vertexColor, inputs);
}

#endif // __TERRAIN_COMMON_HLSLI__
