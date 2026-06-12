#ifndef __TERRAIN_COMMON_HLSLI__
#define __TERRAIN_COMMON_HLSLI__

#include "structs.hlsli"
#include "materialFlags.hlsli"
#include "parallax.hlsli"
#include "utilities.hlsli"

static const float TERRAIN_CELL_SIZE = 4096.0f;
static const float TERRAIN_QUADRANT_SIZE = 2048.0f;
static const float TERRAIN_DEFAULT_STOCHASTIC_SCALE = 3.4641016f;
static const uint TERRAIN_INVALID_DESCRIPTOR = 0xffffffffu;
static const uint TERRAIN_LAYER_FLAG_SNOW = 1u << 0;
static const uint TERRAIN_LAYER_FLAG_HEIGHT_FROM_DIFFUSE_ALPHA = 1u << 1;
static const uint TERRAIN_STOCHASTIC_FLAG_DIFFUSE = 1u << 0;
static const uint TERRAIN_STOCHASTIC_FLAG_NORMAL = 1u << 1;
static const uint TERRAIN_STOCHASTIC_FLAG_DIFFUSE_COLOR_SPACE = 1u << 2;
static const uint TERRAIN_STOCHASTIC_FLAG_HEIGHT = 1u << 3;
static const float TERRAIN_PARALLAX_MIN_LAYER_WEIGHT = 0.02f;
static const float TERRAIN_PARALLAX_MIN_FADE = 0.01f;

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

float TerrainBias(float value, float bias)
{
    value = saturate(value);
    bias = clamp(bias, 0.001f, 0.999f);
    return value / (((1.0f / bias) - 2.0f) * (1.0f - value) + 1.0f);
}

float TerrainGain(float value, float gain)
{
    value = saturate(value);
    gain = saturate(gain);
    if (abs(gain - 0.5f) < 1.0e-4f)
    {
        return value;
    }
    return value < 0.5f
        ? 0.5f * TerrainBias(value * 2.0f, 1.0f - gain)
        : 1.0f - 0.5f * TerrainBias(2.0f - value * 2.0f, 1.0f - gain);
}

float3 TerrainNormalizeWeights(float3 weights)
{
    weights = max(weights, 0.0f.xxx);
    float sumWeights = weights.x + weights.y + weights.z;
    return sumWeights > 1.0e-5f ? weights / sumWeights : float3(1.0f, 0.0f, 0.0f);
}

float TerrainParallaxDistanceFade(float viewDistance, float fadeStart, float fadeEnd)
{
    fadeStart = max(fadeStart, 0.0f);
    fadeEnd = max(fadeEnd, fadeStart + 1.0f);
    return 1.0f - smoothstep(fadeStart, fadeEnd, viewDistance);
}

float3 TerrainShapeStochasticWeights(float3 weights, float blendCurve)
{
    blendCurve = saturate(blendCurve);
    float sharpAmount = saturate((blendCurve - 0.5f) * 2.0f);
    float softAmount = saturate((0.5f - blendCurve) * 2.0f);
    float gamma = lerp(1.0f, 7.0f, sharpAmount);
    gamma = lerp(gamma, 0.5f, softAmount);

    float3 shaped = pow(max(weights, 1.0e-5f.xxx), gamma);
    shaped = TerrainNormalizeWeights(shaped);
    shaped = float3(
        TerrainGain(shaped.x, blendCurve),
        TerrainGain(shaped.y, blendCurve),
        TerrainGain(shaped.z, blendCurve));
    return TerrainNormalizeWeights(shaped);
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
    float2 uv,
    float2 duDx,
    float2 duDy,
    float stochasticScale,
    float blendCurve,
    float lod)
{
    TerrainStochasticContext ctx;
    ctx.uv = uv;
    ctx.duDx = duDx;
    ctx.duDy = duDy;
    ctx.lod = max(lod, 0.0f);

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

    ctx.weights = TerrainShapeStochasticWeights(w, blendCurve);
    ctx.offsets0 = TerrainHash2(v0);
    ctx.offsets1 = TerrainHash2(v1);
    ctx.offsets2 = TerrainHash2(v2);
    return ctx;
}

float TerrainEstimateTextureLod(Texture2D<float4> lodTexture, float2 duDx, float2 duDy)
{
    uint textureWidth = 1u;
    uint textureHeight = 1u;
    lodTexture.GetDimensions(textureWidth, textureHeight);
    float2 textureSize = float2(max(textureWidth, 1u), max(textureHeight, 1u));
    float2 dxTexels = duDx * textureSize;
    float2 dyTexels = duDy * textureSize;
    float maxFootprintSq = max(dot(dxTexels, dxTexels), dot(dyTexels, dyTexels));
    return max(0.0f, 0.5f * log2(max(maxFootprintSq, 1.0e-8f)));
}

TerrainStochasticContext TerrainBuildStochasticContext(
    Texture2D<float4> lodTexture,
    SamplerState lodSampler,
    float2 uv,
    float2 duDx,
    float2 duDy,
    float stochasticScale,
    float blendCurve)
{
    return TerrainBuildStochasticContext(
        uv,
        duDx,
        duDy,
        stochasticScale,
        blendCurve,
        TerrainEstimateTextureLod(lodTexture, duDx, duDy));
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

float TerrainVariancePreservingBlend1(float a, float b, float c, float3 weights)
{
    float varianceScale = rsqrt(max(dot(weights, weights), 1.0e-4f));
    return saturate(((a - 0.5f) * weights.x + (b - 0.5f) * weights.y + (c - 0.5f) * weights.z) * varianceScale + 0.5f);
}

float3 TerrainHeightBlendWeights(float3 stochasticWeights, float3 heights, float blendCurve)
{
    stochasticWeights = TerrainNormalizeWeights(stochasticWeights);
    heights = saturate(heights);

    float contrast = lerp(0.35f, 0.08f, saturate(blendCurve));
    float3 ranked = heights + stochasticWeights;
    float peak = max(ranked.x, max(ranked.y, ranked.z));
    float3 heightMask = saturate((ranked - (peak - contrast)) / max(contrast, 1.0e-4f));

    return TerrainNormalizeWeights(stochasticWeights * max(heightMask, 1.0e-4f.xxx));
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

float TerrainInverseLut1(Texture2D<float4> lut, SamplerState samplerState, float gaussian, float lod, float lutHeight)
{
    float y = TerrainLutY(lod, lutHeight);
    return lut.SampleLevel(samplerState, float2(gaussian, y), 0.0f).r;
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

float3 TerrainSampleMikkelsenDiffuse(Texture2D<float4> diffuseTex, SamplerState diffuseSampler, uint streamingTextureID, TerrainStochasticContext ctx)
{
    float3 c0 = SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, streamingTextureID, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).rgb;
    float3 c1 = SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, streamingTextureID, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).rgb;
    float3 c2 = SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, streamingTextureID, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).rgb;
    return saturate(c0 * ctx.weights.x + c1 * ctx.weights.y + c2 * ctx.weights.z);
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

float TerrainSampleStochasticHeight(TerrainStochasticLayerInfo stochastic, TerrainStochasticContext ctx, SamplerState textureSampler)
{
    Texture2D<float4> gaussianTex = ResourceDescriptorHeap[NonUniformResourceIndex(stochastic.heightGaussianTextureIndex)];
    Texture2D<float4> inverseLut = ResourceDescriptorHeap[NonUniformResourceIndex(stochastic.heightInverseLutTextureIndex)];
    SamplerState lutSampler = SamplerDescriptorHeap[NonUniformResourceIndex(stochastic.heightInverseLutSamplerIndex)];
    float g0 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).r;
    float g1 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).r;
    float g2 = gaussianTex.SampleGrad(textureSampler, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).r;
    float gaussian = TerrainVariancePreservingBlend1(g0, g1, g2, ctx.weights);
    return saturate(TerrainInverseLut1(inverseLut, lutSampler, gaussian, ctx.lod, stochastic.heightLutHeight));
}

float TerrainSampleStochasticDiffuseAlpha(Texture2D<float4> diffuseTex, SamplerState diffuseSampler, uint streamingTextureID, TerrainStochasticContext ctx)
{
    float a0 = SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, streamingTextureID, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).a;
    float a1 = SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, streamingTextureID, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).a;
    float a2 = SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, streamingTextureID, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).a;
    return saturate(a0 * ctx.weights.x + a1 * ctx.weights.y + a2 * ctx.weights.z);
}

float TerrainSampleMikkelsenHeight(Texture2D<float4> heightTex, SamplerState heightSampler, uint streamingTextureID, TerrainStochasticContext ctx)
{
    float h0 = SampleMaterialTexture2DGrad(heightTex, heightSampler, streamingTextureID, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).r;
    float h1 = SampleMaterialTexture2DGrad(heightTex, heightSampler, streamingTextureID, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).r;
    float h2 = SampleMaterialTexture2DGrad(heightTex, heightSampler, streamingTextureID, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).r;
    return saturate(h0 * ctx.weights.x + h1 * ctx.weights.y + h2 * ctx.weights.z);
}

float2 TerrainNormalToDerivative(float3 normalTS)
{
    normalTS = normalize(normalTS);
    float nz = max(normalTS.z, 0.05f);
    return -normalTS.xy / nz;
}

float3 TerrainDerivativeToNormal(float2 derivative)
{
    return normalize(float3(-derivative.x, -derivative.y, 1.0f));
}

bool TerrainLayerUsesDiffuseAlphaHeight(TerrainLayerInfo layer)
{
    return (layer.flags & TERRAIN_LAYER_FLAG_HEIGHT_FROM_DIFFUSE_ALPHA) != 0u &&
        layer.diffuseTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
        layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR;
}

bool TerrainLayerUsesExplicitHeight(TerrainLayerInfo layer)
{
    return layer.heightTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
        layer.heightSamplerIndex != TERRAIN_INVALID_DESCRIPTOR;
}

bool TerrainCanSampleHeight(TerrainLayerInfo layer)
{
    return TerrainLayerUsesDiffuseAlphaHeight(layer) || TerrainLayerUsesExplicitHeight(layer);
}

float3 TerrainSampleLayerHeightTriplet(TerrainLayerInfo layer, TerrainStochasticContext ctx)
{
    if (TerrainLayerUsesDiffuseAlphaHeight(layer))
    {
        Texture2D<float4> diffuseTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.diffuseTextureIndex)];
        SamplerState diffuseSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
        return float3(
            SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, layer.diffuseStreamingTextureID, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).a,
            SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, layer.diffuseStreamingTextureID, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).a,
            SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, layer.diffuseStreamingTextureID, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).a);
    }

    if (TerrainLayerUsesExplicitHeight(layer))
    {
        Texture2D<float4> heightTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.heightTextureIndex)];
        SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.heightSamplerIndex)];
        return float3(
            SampleMaterialTexture2DGrad(heightTex, heightSampler, layer.heightStreamingTextureID, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy).r,
            SampleMaterialTexture2DGrad(heightTex, heightSampler, layer.heightStreamingTextureID, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy).r,
            SampleMaterialTexture2DGrad(heightTex, heightSampler, layer.heightStreamingTextureID, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy).r);
    }

    return 0.5f.xxx;
}

bool TerrainTryBuildLayerStochasticContext(
    TerrainLayerInfo layer,
    float2 uv,
    float2 duDx,
    float2 duDy,
    float stochasticScale,
    float blendCurve,
    out TerrainStochasticContext ctx)
{
    ctx = (TerrainStochasticContext)0;
    if (layer.diffuseTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
    {
        Texture2D<float4> lodTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.diffuseTextureIndex)];
        SamplerState lodSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
        ctx = TerrainBuildStochasticContext(lodTex, lodSampler, uv, duDx, duDy, stochasticScale, blendCurve);
        return true;
    }

    if (layer.normalTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
    {
        Texture2D<float4> lodTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.normalTextureIndex)];
        SamplerState lodSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.normalSamplerIndex)];
        ctx = TerrainBuildStochasticContext(lodTex, lodSampler, uv, duDx, duDy, stochasticScale, blendCurve);
        return true;
    }

    if (layer.heightTextureIndex != TERRAIN_INVALID_DESCRIPTOR && layer.heightSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
    {
        Texture2D<float4> lodTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.heightTextureIndex)];
        SamplerState lodSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.heightSamplerIndex)];
        ctx = TerrainBuildStochasticContext(lodTex, lodSampler, uv, duDx, duDy, stochasticScale, blendCurve);
        return true;
    }

    return false;
}

float TerrainSampleLayerHeight(
    TerrainLayerInfo layer,
    TerrainStochasticLayerInfo stochasticLayer,
    bool hasStochasticLayer,
    bool terrainStochasticHeightEnabled,
    bool terrainGaussianStochasticEnabled,
    bool useStochasticContext,
    float stochasticScale,
    float blendCurve,
    float2 uv,
    float2 duDx,
    float2 duDy)
{
    if (TerrainLayerUsesDiffuseAlphaHeight(layer))
    {
        Texture2D<float4> diffuseTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.diffuseTextureIndex)];
        SamplerState diffuseSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
        if (terrainStochasticHeightEnabled && useStochasticContext)
        {
            TerrainStochasticContext ctx = TerrainBuildStochasticContext(
                diffuseTex,
                diffuseSampler,
                uv,
                duDx,
                duDy,
                stochasticScale,
                blendCurve);
            ctx.weights = TerrainHeightBlendWeights(ctx.weights, TerrainSampleLayerHeightTriplet(layer, ctx), blendCurve);
            return TerrainSampleStochasticDiffuseAlpha(diffuseTex, diffuseSampler, layer.diffuseStreamingTextureID, ctx);
        }
        return SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, layer.diffuseStreamingTextureID, uv, duDx, duDy).a;
    }

    if (TerrainLayerUsesExplicitHeight(layer))
    {
        Texture2D<float4> heightTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.heightTextureIndex)];
        SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.heightSamplerIndex)];
        if (terrainStochasticHeightEnabled &&
            useStochasticContext &&
            !terrainGaussianStochasticEnabled)
        {
            TerrainStochasticContext ctx = TerrainBuildStochasticContext(
                heightTex,
                heightSampler,
                uv,
                duDx,
                duDy,
                stochasticScale,
                blendCurve);
            ctx.weights = TerrainHeightBlendWeights(ctx.weights, TerrainSampleLayerHeightTriplet(layer, ctx), blendCurve);
            return TerrainSampleMikkelsenHeight(heightTex, heightSampler, layer.heightStreamingTextureID, ctx);
        }
        if (terrainStochasticHeightEnabled &&
            terrainGaussianStochasticEnabled &&
            useStochasticContext &&
            hasStochasticLayer &&
            (stochasticLayer.heightFlags & TERRAIN_STOCHASTIC_FLAG_HEIGHT) != 0u &&
            stochasticLayer.heightGaussianTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            stochasticLayer.heightInverseLutTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            stochasticLayer.heightInverseLutSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            TerrainStochasticContext ctx = TerrainBuildStochasticContext(
                heightTex,
                heightSampler,
                uv,
                duDx,
                duDy,
                stochasticScale,
                blendCurve);
            ctx.weights = TerrainHeightBlendWeights(ctx.weights, TerrainSampleLayerHeightTriplet(layer, ctx), blendCurve);
            return TerrainSampleStochasticHeight(stochasticLayer, ctx, heightSampler);
        }
        return SampleMaterialTexture2DGrad(heightTex, heightSampler, layer.heightStreamingTextureID, uv, duDx, duDy).r;
    }

    return 1.0f;
}

float3 TerrainParallaxCoordsAndHeight(
    TerrainLayerInfo layer,
    TerrainStochasticLayerInfo stochasticLayer,
    bool hasStochasticLayer,
    bool terrainStochasticHeightEnabled,
    bool terrainGaussianStochasticEnabled,
    bool useStochasticContext,
    float stochasticScale,
    float blendCurve,
    float3x3 TBN,
    float2 uv,
    float3 viewDir,
    float heightmapScale,
    uint maxSteps,
    float2 dUVdx,
    float2 dUVdy)
{
    float3 viewDirTS = ParallaxViewDirectionTS(TBN, viewDir);
    float2 parallaxDirection = viewDirTS.xy;

    float maxHeight = max(heightmapScale, 0.0f);
    if (maxHeight <= 1.0e-5f)
    {
        return float3(uv, TerrainSampleLayerHeight(layer, stochasticLayer, hasStochasticLayer, terrainStochasticHeightEnabled, terrainGaussianStochasticEnabled, useStochasticContext, stochasticScale, blendCurve, uv, dUVdx, dUVdy));
    }

    const uint numSteps = ParallaxStepCount(viewDirTS.z, maxSteps);
    const float stepSize = rcp((float)numSteps);
    float prevBound = 1.0f;
    float2 prevUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, prevBound);
    float prevHeight = TerrainSampleLayerHeight(layer, stochasticLayer, hasStochasticLayer, terrainStochasticHeightEnabled, terrainGaussianStochasticEnabled, useStochasticContext, stochasticScale, blendCurve, prevUv, dUVdx, dUVdy);
    float prevF = prevBound - prevHeight;
    float hitBound = 0.0f;
    float hitF = prevF;
    float missBound = prevBound;
    float missF = prevF;
    bool foundIntersection = false;

    [loop] for (uint i = 1u; i <= numSteps; ++i)
    {
        const float currentBound = 1.0f - (float)i * stepSize;
        const float2 currentUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, currentBound);
        const float currentHeight = TerrainSampleLayerHeight(layer, stochasticLayer, hasStochasticLayer, terrainStochasticHeightEnabled, terrainGaussianStochasticEnabled, useStochasticContext, stochasticScale, blendCurve, currentUv, dUVdx, dUVdy);
        const float currentF = currentBound - currentHeight;
        if (currentF <= 0.0f)
        {
            hitBound = currentBound;
            hitF = currentF;
            missBound = prevBound;
            missF = prevF;
            foundIntersection = true;
            break;
        }
        prevBound = currentBound;
        prevF = currentF;
    }

    if (!foundIntersection)
    {
        const float2 bottomUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, 0.0f);
        return float3(bottomUv, TerrainSampleLayerHeight(layer, stochasticLayer, hasStochasticLayer, terrainStochasticHeightEnabled, terrainGaussianStochasticEnabled, useStochasticContext, stochasticScale, blendCurve, bottomUv, dUVdx, dUVdy));
    }

    [unroll] for (uint refine = 0u; refine < 3u; ++refine)
    {
        const float rootBound = ParallaxSecantBound(hitBound, hitF, missBound, missF);
        const float2 rootUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, rootBound);
        const float rootF = rootBound - TerrainSampleLayerHeight(layer, stochasticLayer, hasStochasticLayer, terrainStochasticHeightEnabled, terrainGaussianStochasticEnabled, useStochasticContext, stochasticScale, blendCurve, rootUv, dUVdx, dUVdy);
        if (rootF <= 0.0f)
        {
            hitBound = rootBound;
            hitF = rootF;
        }
        else
        {
            missBound = rootBound;
            missF = rootF;
        }
    }

    const float finalBound = ParallaxSecantBound(hitBound, hitF, missBound, missF);
    const float2 parallaxUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, finalBound);
    return float3(parallaxUv, TerrainSampleLayerHeight(layer, stochasticLayer, hasStochasticLayer, terrainStochasticHeightEnabled, terrainGaussianStochasticEnabled, useStochasticContext, stochasticScale, blendCurve, parallaxUv, dUVdx, dUVdy));
}

float3 TerrainSampleStochasticNormalDerivative(
    Texture2D<float4> normalTex,
    SamplerState normalSampler,
    uint streamingTextureID,
    TerrainStochasticContext ctx,
    uint3 normalChannels)
{
    float3 n0 = TerrainDecodeNormal(
        SampleMaterialTexture2DGrad(normalTex, normalSampler, streamingTextureID, ctx.uv + ctx.offsets0, ctx.duDx, ctx.duDy),
        normalChannels);
    float3 n1 = TerrainDecodeNormal(
        SampleMaterialTexture2DGrad(normalTex, normalSampler, streamingTextureID, ctx.uv + ctx.offsets1, ctx.duDx, ctx.duDy),
        normalChannels);
    float3 n2 = TerrainDecodeNormal(
        SampleMaterialTexture2DGrad(normalTex, normalSampler, streamingTextureID, ctx.uv + ctx.offsets2, ctx.duDx, ctx.duDy),
        normalChannels);

    float2 d0 = TerrainNormalToDerivative(n0);
    float2 d1 = TerrainNormalToDerivative(n1);
    float2 d2 = TerrainNormalToDerivative(n2);
    float2 derivative = d0 * ctx.weights.x + d1 * ctx.weights.y + d2 * ctx.weights.z;
    return TerrainDerivativeToNormal(derivative);
}

float2 TerrainSkyrimXYFromRendererPosition(float3 positionWS)
{
    return float2(positionWS.x, -positionWS.z);
}

float2 TerrainSkyrimXYDerivativeFromRendererDerivative(float3 positionDerivativeWS)
{
    return float2(positionDerivativeWS.x, -positionDerivativeWS.z);
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

float TerrainSampleGeometricHeight(uint terrainSetIndex, float3 positionWS)
{
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    StructuredBuffer<TerrainLayerInfo> terrainLayers = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Layers)];
    StructuredBuffer<TerrainStochasticLayerInfo> terrainStochasticLayers = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::StochasticLayers)];
    StructuredBuffer<TerrainLayerRefInfo> terrainLayerRefs = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::LayerRefs)];
    StructuredBuffer<TerrainRegionInfo> terrainRegions = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Regions)];
    StructuredBuffer<uint> terrainWeightBlocks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::WeightBlocks)];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    if (terrain.regionSizeWorld <= 0.0f ||
        terrain.regionCountX == 0u || terrain.regionCountY == 0u || terrain.layerCount == 0u)
    {
        return 0.0f;
    }

    float2 skyrimXY = TerrainSkyrimXYFromRendererPosition(positionWS);
    int2 regionCoord = int2(floor(skyrimXY / terrain.regionSizeWorld));
    uint2 localRegion;
    localRegion.x = (uint)(regionCoord.x - terrain.minRegionX);
    localRegion.y = (uint)(regionCoord.y - terrain.minRegionY);
    if (regionCoord.x < terrain.minRegionX || regionCoord.y < terrain.minRegionY ||
        localRegion.x >= terrain.regionCountX || localRegion.y >= terrain.regionCountY)
    {
        return 0.0f;
    }

    uint regionIndex = terrain.regionBase + localRegion.y * terrain.regionCountX + localRegion.x;
    if (regionIndex >= terrain.regionBase + terrain.regionCount)
    {
        return 0.0f;
    }

    TerrainRegionInfo region = terrainRegions[regionIndex];
    if (region.layerRefCount == 0u || region.weightSampleSide < 2u)
    {
        return 0.0f;
    }

    float2 regionOrigin = float2(regionCoord) * terrain.regionSizeWorld;
    float2 regionLocal = skyrimXY - regionOrigin;
    bool terrainStochasticSamplingEnabled = perFrameBuffer.terrainStochasticSamplingEnabled != 0u;
    bool terrainStochasticHeightEnabled =
        terrainStochasticSamplingEnabled &&
        perFrameBuffer.terrainStochasticDiffuseEnabled != 0u;
    bool terrainGaussianStochasticEnabled = terrainStochasticHeightEnabled &&
        perFrameBuffer.terrainGaussianStochasticEnabled != 0u;
    float2 zeroDerivative = 0.0f.xx;

    float heightSum = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (uint localLayer = 0u; localLayer < region.layerRefCount; ++localLayer)
    {
        float weight = TerrainInterpolateLayerWeight(terrainWeightBlocks, terrain, region, localLayer, regionLocal);
        if (weight <= 0.0001f)
        {
            continue;
        }

        uint layerRefIndex = terrain.layerRefBase + region.layerRefStart + localLayer;
        if (layerRefIndex >= terrain.layerRefBase + terrain.layerRefCount)
        {
            continue;
        }

        uint layerIndex = min(terrain.layerBase + terrainLayerRefs[layerRefIndex].layerIndex, terrain.layerBase + terrain.layerCount - 1u);
        TerrainLayerInfo layer = terrainLayers[layerIndex];
        if (!TerrainCanSampleHeight(layer))
        {
            continue;
        }

        float2 layerUv = skyrimXY * layer.uvScale;
        bool hasStochasticLayer = layer.stochasticLayerIndex != TERRAIN_INVALID_DESCRIPTOR;
        TerrainStochasticLayerInfo stochasticLayer = (TerrainStochasticLayerInfo)0;
        float contextScale = TERRAIN_DEFAULT_STOCHASTIC_SCALE;
        if (hasStochasticLayer)
        {
            stochasticLayer = terrainStochasticLayers[layer.stochasticLayerIndex];
            contextScale = max(stochasticLayer.stochasticScale, 0.001f);
        }

        float layerHeight = TerrainSampleLayerHeight(
            layer,
            stochasticLayer,
            hasStochasticLayer,
            terrainStochasticHeightEnabled,
            terrainGaussianStochasticEnabled,
            terrainStochasticHeightEnabled,
            contextScale,
            perFrameBuffer.terrainStochasticBlendCurve,
            layerUv,
            zeroDerivative,
            zeroDerivative);
        heightSum += layerHeight * weight * max(layer.heightScale, 0.0f);
        weightSum += weight;
    }

    return weightSum > 1.0e-4f ? heightSum / weightSum : 0.0f;
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
    in float3 dpdxWS,
    in float3 dpdyWS,
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
    bool terrainStochasticHeightEnabled = terrainStochasticDiffuseEnabled;
    bool terrainGaussianStochasticEnabled = terrainStochasticSamplingEnabled &&
        perFrameBuffer.terrainGaussianStochasticEnabled != 0u;
    bool terrainStochasticDerivativeNormalsEnabled = terrainStochasticNormalEnabled &&
        perFrameBuffer.terrainStochasticDerivativeNormalsEnabled != 0u;
    const bool terrainGeometricDisplacementEnabled = (materialFlags & MATERIAL_GEOMETRIC_DISPLACEMENT) != 0u;
    bool terrainParallaxEnabled = !terrainGeometricDisplacementEnabled &&
        perFrameBuffer.parallaxOcclusionMappingEnabled != 0u &&
        perFrameBuffer.terrainParallaxOcclusionMappingEnabled != 0u &&
        perFrameBuffer.terrainParallaxHeightScale > 0.0f;
    uint terrainParallaxMaxSteps = clamp(perFrameBuffer.terrainParallaxMaxSteps, 4u, 64u);
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

    float2 skyrimXYDdx = TerrainSkyrimXYDerivativeFromRendererDerivative(dpdxWS);
    float2 skyrimXYDdy = TerrainSkyrimXYDerivativeFromRendererDerivative(dpdyWS);
    float3x3 terrainBasis = TerrainBasis(normalWSBase);
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];
    float3 terrainViewDir = normalize(mainCamera.positionWorldSpace.xyz - positionWS);
    float terrainViewDistance = length(mainCamera.positionWorldSpace.xyz - positionWS);
    float terrainParallaxFade = terrainParallaxEnabled
        ? TerrainParallaxDistanceFade(
            terrainViewDistance,
            perFrameBuffer.terrainParallaxFadeStartDistance,
            perFrameBuffer.terrainParallaxFadeEndDistance)
        : 0.0f;
    terrainParallaxEnabled = terrainParallaxFade > TERRAIN_PARALLAX_MIN_FADE;
    terrainParallaxMaxSteps = max(4u, (uint)ceil((float)terrainParallaxMaxSteps * terrainParallaxFade));

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
        bool hasStochasticLayer = layer.stochasticLayerIndex != TERRAIN_INVALID_DESCRIPTOR;
        TerrainStochasticLayerInfo stochasticLayer = (TerrainStochasticLayerInfo)0;
        float contextScale = TERRAIN_DEFAULT_STOCHASTIC_SCALE;
        if (hasStochasticLayer)
        {
            stochasticLayer = terrainStochasticLayers[layer.stochasticLayerIndex];
            contextScale = max(stochasticLayer.stochasticScale, 0.001f);
        }
        if (terrainParallaxEnabled && weight >= TERRAIN_PARALLAX_MIN_LAYER_WEIGHT && TerrainCanSampleHeight(layer))
        {
            float layerHeightScale = perFrameBuffer.terrainParallaxHeightScale * terrainParallaxFade * max(layer.heightScale, 0.0f);
            float3 parallaxUvHeight = TerrainParallaxCoordsAndHeight(
                layer,
                stochasticLayer,
                hasStochasticLayer,
                terrainStochasticHeightEnabled,
                terrainGaussianStochasticEnabled,
                terrainStochasticHeightEnabled,
                contextScale,
                perFrameBuffer.terrainStochasticBlendCurve,
                terrainBasis,
                layerUv,
                terrainViewDir,
                layerHeightScale,
                terrainParallaxMaxSteps,
                layerDUdx,
                layerDUdy);
            layerUv = parallaxUvHeight.xy;
            inputs.parallaxApplied = 1u;
        }
        bool wantsDerivativeNormalContext = terrainStochasticDerivativeNormalsEnabled &&
            layer.normalTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR;
        bool wantsDirectDiffuseContext = terrainStochasticDiffuseEnabled &&
            layer.diffuseTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR;
        bool wantsGaussianStochasticContext =
            terrainGaussianStochasticEnabled &&
            (terrainStochasticDiffuseEnabled || terrainStochasticNormalEnabled) &&
            hasStochasticLayer;
        bool wantsStochasticContext =
            wantsDirectDiffuseContext ||
            wantsGaussianStochasticContext ||
            wantsDerivativeNormalContext;
        bool hasStochasticContext = false;
        TerrainStochasticContext stochasticContext = (TerrainStochasticContext)0;
        if (wantsStochasticContext)
        {
            hasStochasticContext = TerrainTryBuildLayerStochasticContext(
                layer,
                layerUv,
                layerDUdx,
                layerDUdy,
                contextScale,
                perFrameBuffer.terrainStochasticBlendCurve,
                stochasticContext);
            if (hasStochasticContext && TerrainCanSampleHeight(layer))
            {
                const float3 heightSamples = TerrainSampleLayerHeightTriplet(layer, stochasticContext);
                stochasticContext.weights = TerrainHeightBlendWeights(
                    stochasticContext.weights,
                    heightSamples,
                    perFrameBuffer.terrainStochasticBlendCurve);
            }
        }

        float3 layerBaseColor = layer.fallbackColor.rgb;
        if (terrainStochasticDiffuseEnabled &&
            !terrainGaussianStochasticEnabled &&
            hasStochasticContext &&
            layer.diffuseTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            layer.diffuseSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            Texture2D<float4> diffuseTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.diffuseTextureIndex)];
            SamplerState diffuseSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.diffuseSamplerIndex)];
            layerBaseColor = TerrainSampleMikkelsenDiffuse(diffuseTex, diffuseSampler, layer.diffuseStreamingTextureID, stochasticContext);
        }
        else if (terrainStochasticDiffuseEnabled &&
            terrainGaussianStochasticEnabled &&
            hasStochasticLayer &&
            hasStochasticContext &&
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
            layerBaseColor = SampleMaterialTexture2DGrad(diffuseTex, diffuseSampler, layer.diffuseStreamingTextureID, layerUv, layerDUdx, layerDUdy, inputs).rgb;
        }
        blendedBaseColor += layerBaseColor * weight;

        float3 layerNormalTS = float3(0.0f, 0.0f, 1.0f);
        if (terrainStochasticDerivativeNormalsEnabled &&
            hasStochasticContext &&
            layer.normalTextureIndex != TERRAIN_INVALID_DESCRIPTOR &&
            layer.normalSamplerIndex != TERRAIN_INVALID_DESCRIPTOR)
        {
            Texture2D<float4> normalTex = ResourceDescriptorHeap[NonUniformResourceIndex(layer.normalTextureIndex)];
            SamplerState normalSampler = SamplerDescriptorHeap[NonUniformResourceIndex(layer.normalSamplerIndex)];
            layerNormalTS = TerrainSampleStochasticNormalDerivative(normalTex, normalSampler, layer.normalStreamingTextureID, stochasticContext, layer.normalChannels);
        }
        else if (terrainStochasticNormalEnabled &&
            terrainGaussianStochasticEnabled &&
            hasStochasticLayer &&
            hasStochasticContext &&
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
            layerNormalTS = TerrainDecodeNormal(
                SampleMaterialTexture2DGrad(normalTex, normalSampler, layer.normalStreamingTextureID, layerUv, layerDUdx, layerDUdy, inputs),
                layer.normalChannels);
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

    inputs.albedo = blendedBaseColor * vertexColor;
    inputs.normalWS = normalize(mul(normalize(blendedNormalTS), terrainBasis));
    inputs.metallic = 0.0f;
    inputs.roughness = 0.9f;
    inputs.ambientOcclusion = 1.0f;
    inputs.opacity = 1.0f;
    inputs.emissive = 0.0f.xxx;
}

void ApplyTerrainMaterial(
    in MaterialInfo materialInfo,
    in float3 positionWS,
    in float3 dpdxWS,
    in float3 dpdyWS,
    in float3 normalWSBase,
    in float3 vertexColor,
    inout MaterialInputs inputs)
{
    ApplyTerrainMaterialInternal(materialInfo.materialFlags, materialInfo.terrainSetIndex, positionWS, dpdxWS, dpdyWS, normalWSBase, vertexColor, inputs);
}

void ApplyTerrainMaterial(
    in MaterialEvalInfo materialInfo,
    in float3 positionWS,
    in float3 dpdxWS,
    in float3 dpdyWS,
    in float3 normalWSBase,
    in float3 vertexColor,
    inout MaterialInputs inputs)
{
    ApplyTerrainMaterialInternal(materialInfo.materialFlags, materialInfo.terrainSetIndex, positionWS, dpdxWS, dpdyWS, normalWSBase, vertexColor, inputs);
}

#endif // __TERRAIN_COMMON_HLSLI__
