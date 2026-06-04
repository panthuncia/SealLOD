#ifndef __TERRAIN_COMMON_HLSLI__
#define __TERRAIN_COMMON_HLSLI__

#include "structs.hlsli"
#include "materialFlags.hlsli"

static const float TERRAIN_CELL_SIZE = 4096.0f;
static const float TERRAIN_QUADRANT_SIZE = 2048.0f;
static const uint TERRAIN_INVALID_DESCRIPTOR = 0xffffffffu;
static const uint TERRAIN_LAYER_FLAG_SNOW = 1u << 0;
static const uint TERRAIN_STOCHASTIC_FLAG_DIFFUSE = 1u << 0;
static const uint TERRAIN_STOCHASTIC_FLAG_NORMAL = 1u << 1;
static const uint TERRAIN_STOCHASTIC_FLAG_DIFFUSE_COLOR_SPACE = 1u << 2;

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

uint TerrainHash(uint2 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1013904223u;
    v ^= (v >> 16u);
    v *= 2246822519u;
    v ^= (v >> 13u);
    return v.x ^ v.y;
}

float2 TerrainHash2(int2 p)
{
    uint h0 = TerrainHash(asuint(p));
    uint h1 = TerrainHash(asuint(p + int2(37, 119)));
    return float2(h0 & 0x00ffffffu, h1 & 0x00ffffffu) * (1.0f / 16777216.0f);
}

struct TerrainStochasticContext
{
    float3 weights;
    float2 offsets0;
    float2 offsets1;
    float2 offsets2;
    float2 uv;
    float2 duDx;
    float2 duDy;
    float lod;
};

TerrainStochasticContext TerrainBuildStochasticContext(
    Texture2D<float4> lodTexture,
    SamplerState lodSampler,
    float2 uv,
    float2 duDx,
    float2 duDy,
    float stochasticScale)
{
    TerrainStochasticContext ctx;
    ctx.uv = uv;
    ctx.duDx = duDx;
    ctx.duDy = duDy;
    uint textureWidth = 1u;
    uint textureHeight = 1u;
    lodTexture.GetDimensions(textureWidth, textureHeight);
    float2 textureSize = float2(max(textureWidth, 1u), max(textureHeight, 1u));
    float2 dxTexels = duDx * textureSize;
    float2 dyTexels = duDy * textureSize;
    float maxFootprintSq = max(dot(dxTexels, dxTexels), dot(dyTexels, dyTexels));
    ctx.lod = max(0.0f, 0.5f * log2(max(maxFootprintSq, 1.0e-8f)));

    float scale = max(stochasticScale, 0.001f);
    float2 grid = uv * scale;
    float2 skewed = float2(grid.x - 0.57735027f * grid.y, 1.15470054f * grid.y);
    int2 baseCell = int2(floor(skewed));
    float2 f = skewed - (float2)baseCell;
    float z = 1.0f - f.x - f.y;

    int2 v0;
    int2 v1;
    int2 v2;
    float3 w;
    if (z > 0.0f)
    {
        v0 = baseCell;
        v1 = baseCell + int2(0, 1);
        v2 = baseCell + int2(1, 0);
        w = float3(z, f.y, f.x);
    }
    else
    {
        v0 = baseCell + int2(1, 1);
        v1 = baseCell + int2(1, 0);
        v2 = baseCell + int2(0, 1);
        w = float3(-z, 1.0f - f.y, 1.0f - f.x);
    }

    ctx.weights = w;
    ctx.offsets0 = TerrainHash2(v0);
    ctx.offsets1 = TerrainHash2(v1);
    ctx.offsets2 = TerrainHash2(v2);
    return ctx;
}

float3 TerrainVariancePreservingBlend(float3 a, float3 b, float3 c, float3 weights)
{
    float varianceScale = rsqrt(max(dot(weights, weights), 1.0e-4f));
    return saturate(((a - 0.5f.xxx) * weights.x + (b - 0.5f.xxx) * weights.y + (c - 0.5f.xxx) * weights.z) * varianceScale + 0.5f.xxx);
}

float2 TerrainVariancePreservingBlend2(float2 a, float2 b, float2 c, float3 weights)
{
    float varianceScale = rsqrt(max(dot(weights, weights), 1.0e-4f));
    return saturate(((a - 0.5f.xx) * weights.x + (b - 0.5f.xx) * weights.y + (c - 0.5f.xx) * weights.z) * varianceScale + 0.5f.xx);
}

float TerrainLutY(float lod, float lutHeight)
{
    return saturate((max(lod, 0.0f) + 0.5f) / max(lutHeight, 1.0f));
}

float3 TerrainInverseLut3(Texture2D<float4> lut, SamplerState samplerState, float3 gaussian, float lod, float lutHeight)
{
    float y = TerrainLutY(lod, lutHeight);
    return float3(
        lut.SampleLevel(samplerState, float2(gaussian.r, y), 0.0f).r,
        lut.SampleLevel(samplerState, float2(gaussian.g, y), 0.0f).g,
        lut.SampleLevel(samplerState, float2(gaussian.b, y), 0.0f).b);
}

float2 TerrainInverseLut2(Texture2D<float4> lut, SamplerState samplerState, float2 gaussian, float lod, float lutHeight)
{
    float y = TerrainLutY(lod, lutHeight);
    return float2(
        lut.SampleLevel(samplerState, float2(gaussian.x, y), 0.0f).r,
        lut.SampleLevel(samplerState, float2(gaussian.y, y), 0.0f).g);
}

float3 TerrainSampleStochasticDiffuse(TerrainStochasticLayerInfo stochastic, TerrainStochasticContext ctx, SamplerState textureSampler)
{
    Texture2D<float4> gaussianTex = ResourceDescriptorHeap[NonUniformResourceIndex(stochastic.diffuseGaussianTextureIndex)];
    Texture2D<float4> inverseLut = ResourceDescriptorHeap[NonUniformResourceIndex(stochastic.diffuseInverseLutTextureIndex)];
    SamplerState lutSampler = SamplerDescriptorHeap[NonUniformResourceIndex(stochastic.diffuseInverseLutSamplerIndex)];
    float3 g0 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).rgb;
    float3 g1 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).rgb;
    float3 g2 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).rgb;
    float3 gaussian = TerrainVariancePreservingBlend(g0, g1, g2, ctx.weights);
    float3 color = TerrainInverseLut3(inverseLut, lutSampler, gaussian, ctx.lod, stochastic.diffuseLutHeight);
    if ((stochastic.diffuseFlags & TERRAIN_STOCHASTIC_FLAG_DIFFUSE_COLOR_SPACE) != 0u)
    {
        color = stochastic.diffuseColorSpaceOrigin.rgb +
            stochastic.diffuseColorSpaceVector0.rgb * color.r +
            stochastic.diffuseColorSpaceVector1.rgb * color.g +
            stochastic.diffuseColorSpaceVector2.rgb * color.b;
    }
    return saturate(color);
}

float3 TerrainSampleStochasticNormal(TerrainStochasticLayerInfo stochastic, TerrainStochasticContext ctx, SamplerState textureSampler)
{
    Texture2D<float4> gaussianTex = ResourceDescriptorHeap[NonUniformResourceIndex(stochastic.normalGaussianTextureIndex)];
    Texture2D<float4> inverseLut = ResourceDescriptorHeap[NonUniformResourceIndex(stochastic.normalInverseLutTextureIndex)];
    SamplerState lutSampler = SamplerDescriptorHeap[NonUniformResourceIndex(stochastic.normalInverseLutSamplerIndex)];
    float2 g0 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).rg;
    float2 g1 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).rg;
    float2 g2 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).rg;
    float2 encoded = TerrainInverseLut2(
        inverseLut,
        lutSampler,
        TerrainVariancePreservingBlend2(g0, g1, g2, ctx.weights),
        ctx.lod,
        stochastic.normalLutHeight);
    float2 xy = encoded * 2.0f - 1.0f;
    return normalize(float3(xy, sqrt(saturate(1.0f - dot(xy, xy)))));
}

float2 TerrainSkyrimXYFromRendererPosition(float3 positionWS)
{
    return float2(positionWS.x, -positionWS.z);
}

float TerrainCubicBSpline(float p0, float p1, float p2, float p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;

    float w0 = (1.0f - 3.0f * t + 3.0f * t2 - t3) * (1.0f / 6.0f);
    float w1 = (4.0f - 6.0f * t2 + 3.0f * t3) * (1.0f / 6.0f);
    float w2 = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) * (1.0f / 6.0f);
    float w3 = t3 * (1.0f / 6.0f);

    return p0 * w0 + p1 * w1 + p2 * w2 + p3 * w3;
}

float TerrainUnpackWeightByte(uint packed, uint sampleIndex)
{
    return ((packed >> ((sampleIndex & 3u) * 8u)) & 0xFFu) * (1.0f / 255.0f);
}

float TerrainLoadWeightSample(
    StructuredBuffer<uint> weightBlocks,
    TerrainSetInfo terrain,
    TerrainRegionInfo region,
    uint localLayer,
    int2 sample)
{
    int maxSample = (int)region.weightSampleSide - 2;
    int2 clampedSample = clamp(sample, -1, maxSample);
    uint2 packedSample = (uint2)(clampedSample + 1);
    uint sampleIndex = packedSample.y * region.weightSampleSide + packedSample.x;
    uint packedWordsPerLayer = (region.weightSampleSide * region.weightSampleSide + 3u) / 4u;
    uint wordIndex = terrain.weightBlockBase + region.weightBlockStart + localLayer * packedWordsPerLayer + sampleIndex / 4u;
    if (wordIndex >= terrain.weightBlockBase + terrain.weightBlockCount)
    {
        return 0.0f;
    }
    return TerrainUnpackWeightByte(weightBlocks[wordIndex], sampleIndex);
}

float TerrainLoadWeightRowCubic(
    StructuredBuffer<uint> weightBlocks,
    TerrainSetInfo terrain,
    TerrainRegionInfo region,
    uint localLayer,
    int2 baseSample,
    float t)
{
    float p0 = TerrainLoadWeightSample(weightBlocks, terrain, region, localLayer, baseSample + int2(-1, 0));
    float p1 = TerrainLoadWeightSample(weightBlocks, terrain, region, localLayer, baseSample + int2(0, 0));
    float p2 = TerrainLoadWeightSample(weightBlocks, terrain, region, localLayer, baseSample + int2(1, 0));
    float p3 = TerrainLoadWeightSample(weightBlocks, terrain, region, localLayer, baseSample + int2(2, 0));
    return TerrainCubicBSpline(p0, p1, p2, p3, t);
}

float TerrainInterpolateLayerWeight(
    StructuredBuffer<uint> weightBlocks,
    TerrainSetInfo terrain,
    TerrainRegionInfo region,
    uint localLayer,
    float2 regionLocal)
{
    // Skyrim stores close landscape weights on a 17x17 quadrant lattice and feeds
    // them as vertex attributes. Reconstruct the painted material field from that
    // lattice rather than from the CLod render triangles, which may not match the
    // original terrain topology after virtualized-geometry clustering. A cubic
    // B-spline acts as a small, non-overshooting low-pass filter over abrupt
    // 128-unit paint transitions, which better suits full-quality terrain seen
    // at arbitrary CLod distances than exact interpolation of the coarse lattice.
    float2 grid = saturate(regionLocal / terrain.regionSizeWorld) * 16.0f;
    int2 baseSample = (int2)floor(grid);
    float2 f = grid - (float2)baseSample;

    float r0 = TerrainLoadWeightRowCubic(weightBlocks, terrain, region, localLayer, baseSample + int2(0, -1), f.x);
    float r1 = TerrainLoadWeightRowCubic(weightBlocks, terrain, region, localLayer, baseSample + int2(0, 0), f.x);
    float r2 = TerrainLoadWeightRowCubic(weightBlocks, terrain, region, localLayer, baseSample + int2(0, 1), f.x);
    float r3 = TerrainLoadWeightRowCubic(weightBlocks, terrain, region, localLayer, baseSample + int2(0, 2), f.x);

    return saturate(TerrainCubicBSpline(r0, r1, r2, r3, f.y));
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
    StructuredBuffer<TerrainStochasticLayerInfo> terrainStochasticLayers = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::StochasticLayers)];
    StructuredBuffer<TerrainLayerRefInfo> terrainLayerRefs = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::LayerRefs)];
    StructuredBuffer<TerrainRegionInfo> terrainRegions = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Regions)];
    StructuredBuffer<uint> terrainWeightBlocks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::WeightBlocks)];

    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    bool terrainStochasticSamplingEnabled = perFrameBuffer.terrainStochasticSamplingEnabled != 0u;
    bool terrainStochasticDiffuseEnabled = terrainStochasticSamplingEnabled && perFrameBuffer.terrainStochasticDiffuseEnabled != 0u;
    bool terrainStochasticNormalEnabled = terrainStochasticSamplingEnabled && perFrameBuffer.terrainStochasticNormalEnabled != 0u;
    if (terrain.regionSizeWorld <= 0.0f ||
        terrain.regionCountX == 0u || terrain.regionCountY == 0u || terrain.layerCount == 0u)
    {
        return;
    }

    float2 skyrimXY = TerrainSkyrimXYFromRendererPosition(positionWS);
    int2 regionCoord = int2(floor(skyrimXY / terrain.regionSizeWorld));
    uint2 localRegion;
    localRegion.x = (uint)(regionCoord.x - terrain.minRegionX);
    localRegion.y = (uint)(regionCoord.y - terrain.minRegionY);
    if (regionCoord.x < terrain.minRegionX || regionCoord.y < terrain.minRegionY ||
        localRegion.x >= terrain.regionCountX || localRegion.y >= terrain.regionCountY)
    {
        return;
    }

    uint regionIndex = terrain.regionBase + localRegion.y * terrain.regionCountX + localRegion.x;
    if (regionIndex >= terrain.regionBase + terrain.regionCount)
    {
        return;
    }
    TerrainRegionInfo region = terrainRegions[regionIndex];
    if (region.layerRefCount == 0u || region.weightSampleSide < 2u)
    {
        return;
    }

    float2 regionOrigin = float2(regionCoord) * terrain.regionSizeWorld;
    float2 regionLocal = skyrimXY - regionOrigin;

    float2 skyrimXYDdx = ddx(skyrimXY);
    float2 skyrimXYDdy = ddy(skyrimXY);

    float3 blendedBaseColor = 0.0f.xxx;
    float3 blendedNormalTS = 0.0f.xxx;
    float weightSum = 0.0f;
    for (uint localLayer = 0u; localLayer < region.layerRefCount; ++localLayer)
    {
        float weight = TerrainInterpolateLayerWeight(terrainWeightBlocks, terrain, region, localLayer, regionLocal);
        if (weight <= 0.0001f)
        {
            continue;
        }
        weightSum += weight;

        uint layerRefIndex = terrain.layerRefBase + region.layerRefStart + localLayer;
        if (layerRefIndex >= terrain.layerRefBase + terrain.layerRefCount)
        {
            continue;
        }
        uint layerIndex = min(terrain.layerBase + terrainLayerRefs[layerRefIndex].layerIndex, terrain.layerBase + terrain.layerCount - 1u);
        TerrainLayerInfo layer = terrainLayers[layerIndex];
        float2 layerUv = skyrimXY * layer.uvScale;
        float2 layerDUdx = skyrimXYDdx * layer.uvScale;
        float2 layerDUdy = skyrimXYDdy * layer.uvScale;
        bool hasStochasticLayer = (terrainStochasticDiffuseEnabled || terrainStochasticNormalEnabled) &&
            layer.stochasticLayerIndex != TERRAIN_INVALID_DESCRIPTOR;
        TerrainStochasticLayerInfo stochasticLayer = (TerrainStochasticLayerInfo)0;
        TerrainStochasticContext stochasticContext = (TerrainStochasticContext)0;
        if (hasStochasticLayer)
        {
            stochasticLayer = terrainStochasticLayers[layer.stochasticLayerIndex];
            float contextScale = max(stochasticLayer.stochasticScale, 0.001f);
            if (layer.diffuseTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
            {
                Texture2D<float4> lodTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.diffuseTextureIndex)];
                SamplerState lodSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
                stochasticContext = TerrainBuildStochasticContext(lodTex, lodSampler, layerUv, layerDUdx, layerDUdy, contextScale);
            }
            else if (layer.normalTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
            {
                Texture2D<float4> lodTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.normalTextureIndex)];
                SamplerState lodSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.normalSamplerIndex)];
                stochasticContext = TerrainBuildStochasticContext(lodTex, lodSampler, layerUv, layerDUdx, layerDUdy, contextScale);
            }
            else
            {
                hasStochasticLayer = false;
            }
        }

        float3 layerBaseColor = layer.fallbackColor.rgb;
        if (terrainStochasticDiffuseEnabled &&
            hasStochasticLayer &&
            (stochasticLayer.diffuseFlags & TERRAIN_STOCHASTIC_FLAG_DIFFUSE) != 0u &&
            layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR &&
            stochasticLayer.diffuseGaussianTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            stochasticLayer.diffuseInverseLutTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            stochasticLayer.diffuseInverseLutSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            SamplerState stochasticTextureSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
            layerBaseColor = TerrainSampleStochasticDiffuse(stochasticLayer, stochasticContext, stochasticTextureSampler);
        }
        else if (layer.diffuseTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            Texture2D<float4> diffuseTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.diffuseTextureIndex)];
            SamplerState diffuseSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
            layerBaseColor = diffuseTex.SampleGrad(diffuseSampler, layerUv, layerDUdx, layerDUdy).rgb;
        }
        blendedBaseColor += layerBaseColor * weight;

        float3 layerNormalTS = float3(0.0f, 0.0f, 1.0f);
        if (terrainStochasticNormalEnabled &&
            hasStochasticLayer &&
            (stochasticLayer.normalFlags & TERRAIN_STOCHASTIC_FLAG_NORMAL) != 0u &&
            (layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR || layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR) &&
            stochasticLayer.normalGaussianTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            stochasticLayer.normalInverseLutTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            stochasticLayer.normalInverseLutSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            uint normalSampleSamplerIndex = layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR
                ? layer.normalSamplerIndex
                : layer.diffuseSamplerIndex;
            SamplerState stochasticTextureSampler = SamplerDescriptorHeap[NonUniformResourceIndex(normalSampleSamplerIndex)];
            layerNormalTS = TerrainSampleStochasticNormal(stochasticLayer, stochasticContext, stochasticTextureSampler);
        }
        else if (layer.normalTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            Texture2D<float4> normalTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.normalTextureIndex)];
            SamplerState normalSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.normalSamplerIndex)];
            layerNormalTS = TerrainDecodeNormal(normalTex.SampleGrad(normalSampler, layerUv, layerDUdx, layerDUdy), layer.normalChannels);
        }
        blendedNormalTS += layerNormalTS * weight;
    }

    if (weightSum <= 1.0e-4f)
    {
        return;
    }
    float invWeightSum = rcp(weightSum);
    blendedBaseColor *= invWeightSum;
    blendedNormalTS *= invWeightSum;

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
