#ifndef __LIGHTING_HLSLI__
#define __LIGHTING_HLSLI__

#include "include/vertex.hlsli"
#include "include/structs.hlsli"
#include "Common/defines.h"
#include "include/lightModel.hlsli"
#include "include/materialFlags.hlsli"
#include "include/parallax.hlsli"
#include "include/cbuffers.hlsli"
#include "include/PBR.hlsli"
#include "include/gammaCorrection.hlsli"
#include "include/shadows.hlsli"
#include "include/constants.hlsli"
#include "include/IBL.hlsli"
#include "include/outputTypes.hlsli"
#include "include/debugPayload.hlsli"

#if defined(CLOD_VSM_RECEIVER_TELEMETRY)
// Only a pass that binds Builtin::Shadows::CLodStats as a UAV may define
// CLOD_VSM_RECEIVER_TELEMETRY (the primary deferred pass). One atomic per wave
// per counter.
#define CLOD_VSM_RECEIVER_COUNT(statsBuffer, field, predicate) \
    { \
        const uint clodVsmReceiverCount = WaveActiveCountBits(predicate); \
        if (WaveIsFirstLane() && clodVsmReceiverCount != 0u) \
        { \
            InterlockedAdd(statsBuffer[0].field, clodVsmReceiverCount); \
        } \
    }

void CLodVirtualShadowRecordReceiverTelemetry(
    CLodVirtualShadowDebugInfo debugInfo,
    float shadow,
    uint2 pixel,
    float3 positionWS,
    float linearDepth,
    Camera mainCamera)
{
    RWStructuredBuffer<CLodVirtualShadowStats> stats =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodStats)];
    const uint preferred = debugInfo.preferredClipmapIndex;
    const uint sampled = debugInfo.sampledClipmapIndex;
    const bool noSample = sampled == 0xFFFFFFFFu;
    const bool preferredMissed = sampled != preferred;
    const uint entry = debugInfo.preferredPageEntry;
    const bool allocated = (entry & kCLodVirtualShadowAllocatedMask) != 0u;
    const bool visited = (entry & kCLodVirtualShadowVisitedMask) != 0u;
    const bool hasContent = (entry &
        (kCLodVirtualShadowContentValidMask | kCLodVirtualShadowDynamicContentMask)) != 0u;

    CLOD_VSM_RECEIVER_COUNT(stats, receiverLookupPixels, true);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverPreferredSampledPixels, !preferredMissed);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverFallbackOnePixels, !noSample && sampled == preferred + 1u);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverFallbackTwoPixels, !noSample && sampled == preferred + 2u);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverNoSamplePixels, noSample);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverPreferredUnallocatedPixels, preferredMissed && !allocated);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverPreferredNotVisitedPixels, preferredMissed && allocated && !visited);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverPreferredNoContentPixels, preferredMissed && allocated && visited && !hasContent);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverPreferredTagMismatchPixels, preferredMissed &&
        (debugInfo.flags & kCLodVirtualShadowDebugFlagCachedPageTagMismatch) != 0u);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverPreferredDepthMissingPixels, preferredMissed &&
        (debugInfo.flags & kCLodVirtualShadowDebugFlagSampledDepthMissing) != 0u);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverTexelClearedPixels,
        (debugInfo.flags & kCLodVirtualShadowDebugFlagSampledTexelCleared) != 0u);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverShadowedPixels, shadow > 0.5f);

    // Flicker: reproject to the previous frame's pixel and compare outcomes.
    // Slices ping-pong on the full frame number; a texel is only trusted when
    // the previous frame wrote it and its depth matches this surface.
    RWTexture2DArray<uint4> history =
        ResourceDescriptorHeap[OptionalResourceDescriptorIndex(Builtin::Shadows::CLodReceiverHistory)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const uint frameNumber = GetRootVirtualShadowReceiverTelemetryFrame();
    const uint writeSlice = frameNumber & 1u;
    const uint readSlice = writeSlice ^ 1u;
    const uint shadow8 = (uint)round(saturate(shadow) * 255.0f);
    const bool hard = (debugInfo.flags & kCLodVirtualShadowDebugFlagFiniteDepthShadowed) != 0u;
    const uint clip5 = min(sampled, 31u);
    const uint packed = shadow8 | (hard ? 0x100u : 0u) | (clip5 << 9u) | 0x8000u;

    const float3 previousViewPos = mul(float4(positionWS, 1.0f), mainCamera.prevView).xyz;
    const float4 previousClip = mul(float4(previousViewPos, 1.0f), mainCamera.prevJitteredProjection);
    bool historyValid = false;
    uint4 previous = uint4(0u, 0u, 0u, 0u);
    if (previousClip.w > 1.0e-6f)
    {
        const float2 previousNdc = previousClip.xy / previousClip.w;
        const float2 previousUv = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f);
        const float2 screen = float2(perFrame.screenResX, perFrame.screenResY);
        if (all(previousUv >= 0.0f) && all(previousUv < 1.0f))
        {
            const uint2 previousPixel = min((uint2)(previousUv * screen), (uint2)screen - 1u);
            previous = history[uint3(previousPixel, readSlice)];
            const float previousLinearDepth = -previousViewPos.z;
            historyValid =
                (previous.x & 0x8000u) != 0u &&
                previous.w == frameNumber - 1u &&
                abs(asfloat(previous.z) - previousLinearDepth) <= 0.02f * max(previousLinearDepth, 1.0e-3f);
        }
    }
    history[uint3(pixel, writeSlice)] = uint4(packed, debugInfo.sampledPhysicalPageIndex, asuint(linearDepth), frameNumber);

    const uint previousShadow8 = previous.x & 0xFFu;
    const bool previousHard = (previous.x & 0x100u) != 0u;
    const uint previousClip5 = (previous.x >> 9u) & 31u;
    const uint shadowChange = shadow8 > previousShadow8 ? shadow8 - previousShadow8 : previousShadow8 - shadow8;
    const bool flip = historyValid && shadowChange > 127u;
    const bool hardFlip = historyValid && hard != previousHard;
    const bool clipChanged = previousClip5 != clip5;
    const bool samePage = !clipChanged && previous.y == debugInfo.sampledPhysicalPageIndex;
    CLOD_VSM_RECEIVER_COUNT(stats, receiverHistoryValidPixels, historyValid);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverFlipPixels, flip);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverHardFlipPixels, hardFlip);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverFlipClipChangedPixels, flip && clipChanged);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverFlipSamePagePixels, flip && samePage);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverFlipPageChangedPixels, flip && !clipChanged && !samePage);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverHardFlipSamePagePixels, hardFlip && samePage);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverSoftOnlyFlipPixels, flip && !hardFlip);
    CLOD_VSM_RECEIVER_COUNT(stats, receiverFlipToLitPixels, flip && shadow8 < previousShadow8);
    if (flip && !clipChanged && !samePage && clip5 < kCLodVirtualShadowClipmapCount)
    {
        InterlockedAdd(stats[0].receiverFlipPageChangedByClip[clip5], 1u);
    }
    if (all(pixel == uint2(perFrame.screenResX / 2u, perFrame.screenResY / 2u)))
    {
        stats[0].receiverProbe[0] = packed;
        stats[0].receiverProbe[1] = debugInfo.sampledPhysicalPageIndex;
        stats[0].receiverProbe[2] = historyValid ? previous.x : 0u;
        stats[0].receiverProbe[3] = historyValid ? previous.y : 0xFFFFFFFFu;
    }
}
#endif

struct LightingParameters {
    float3 fragPos;
    float3 viewDir;
    float3 normal;
    float3 weightedBaseColor;
    float3 diffuseColor;
    float baseDiffuseRoughness;
    float specularAlpha;
    float weightedSpecularIor;
    float dielectricSpecularWeight;
    float3 dielectricSpecularF0;
    float metalSpecularWeight;
    float3 metalAverageFresnel;
    float3 metalSpecularF0;
    float metallic;
    float roughness;
    float3 F0;
    float3 coatF0;
    float coatWeight;
    float3 coatColor;
    float coatIor;
    float coatDarkening;
    float coatRoughness;
    float3 fuzzColor;
    float fuzzWeight;
    float fuzzRoughness;
    uint glintEnabled;
    float4 glintParameters;
};

LightingParameters MakeLightingParameters(FragmentInfo fragmentInfo)
{
    LightingParameters lightingParameters;
    lightingParameters.fragPos = fragmentInfo.fragPosWorldSpace.xyz;
    lightingParameters.viewDir = fragmentInfo.viewWS;
    lightingParameters.normal = fragmentInfo.normalWS;
    lightingParameters.weightedBaseColor = fragmentInfo.albedo;
    lightingParameters.diffuseColor = fragmentInfo.diffuseColor;
    lightingParameters.baseDiffuseRoughness = fragmentInfo.baseDiffuseRoughness;
    lightingParameters.specularAlpha = fragmentInfo.specularAlpha;
    lightingParameters.weightedSpecularIor = fragmentInfo.weightedSpecularIor;
    lightingParameters.dielectricSpecularWeight = fragmentInfo.dielectricSpecularWeight;
    lightingParameters.dielectricSpecularF0 = fragmentInfo.dielectricSpecularF0;
    lightingParameters.metalSpecularWeight = fragmentInfo.metalSpecularWeight;
    lightingParameters.metalAverageFresnel = fragmentInfo.metalAverageFresnel;
    lightingParameters.metalSpecularF0 = fragmentInfo.metalSpecularF0;
    lightingParameters.metallic = fragmentInfo.metallic;
    lightingParameters.roughness = fragmentInfo.roughness;
    lightingParameters.F0 = fragmentInfo.F0;
    lightingParameters.coatF0 = fragmentInfo.coatF0;
    lightingParameters.coatWeight = fragmentInfo.coatWeight;
    lightingParameters.coatColor = fragmentInfo.coatColor;
    lightingParameters.coatIor = fragmentInfo.coatIor;
    lightingParameters.coatDarkening = fragmentInfo.coatDarkening;
    lightingParameters.coatRoughness = fragmentInfo.coatRoughness;
    lightingParameters.fuzzColor = fragmentInfo.fuzzColor;
    lightingParameters.fuzzWeight = fragmentInfo.fuzzWeight;
    lightingParameters.fuzzRoughness = fragmentInfo.fuzzRoughness;
    lightingParameters.glintEnabled = fragmentInfo.glintEnabled;
    lightingParameters.glintParameters = fragmentInfo.glintParameters;
    return lightingParameters;
}

float GlintHash(float3 p)
{
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float3 EvaluateGlintContribution(LightFragmentData light, LightingParameters lightingParameters)
{
    if (lightingParameters.glintEnabled == 0u)
    {
        return 0.0f.xxx;
    }

    const float screenSpaceScale = max(lightingParameters.glintParameters.x, 0.001f);
    const float logDensity = clamp(40.0f - lightingParameters.glintParameters.y, 0.0f, 64.0f);
    const float microfacetRoughness = clamp(lightingParameters.glintParameters.z, 0.001f, 0.25f);
    const float densityRandomization = max(lightingParameters.glintParameters.w, 0.0f);
    const float3 halfVector = normalize(light.lightToFrag + lightingParameters.viewDir);
    const float ndotl = saturate(dot(lightingParameters.normal, light.lightToFrag));
    const float ndoth = saturate(dot(lightingParameters.normal, halfVector));
    const float sparkleDensity = exp2(clamp(logDensity - 36.0f, -12.0f, 12.0f));
    const float3 cell = floor(lightingParameters.fragPos * screenSpaceScale * sparkleDensity);
    const float noise = GlintHash(cell + floor(halfVector * 127.0f));
    const float randomThreshold = saturate(0.985f - 0.01f * densityRandomization);
    const float microMask = smoothstep(randomThreshold, 1.0f, noise);
    const float exponent = max(2.0f, 2.0f / max(microfacetRoughness * microfacetRoughness, 1.0e-4f));
    const float glintLobe = pow(ndoth, exponent) * microMask * ndotl;
    const float3 glintF0 = saturate(lerp(lightingParameters.dielectricSpecularF0, lightingParameters.weightedBaseColor, lightingParameters.metallic));
    return glintLobe * glintF0 * light.lightColor.rgb * light.intensity * light.attenuation * light.spotAttenuation;
}

struct LightingOutput { // Lighting + debug info
    float3 lighting;
    uint2 shadowDebugPayload;
#if defined(PSO_IMAGE_BASED_LIGHTING)
    float3 diffuseIBL;
    float3 specularIBL;
#endif // IMAGE_BASED_LIGHTING
#if defined(PSO_CLUSTERED_LIGHTING)
    uint clusterIndex;
    uint clusterLightCount;
#endif
};

// Direct-lighting BRDF for any light direction, shared by raster and the path tracer. Callers
// must have rejected NdotL <= 0 and NdotV <= 0. Excludes glint, which is not a physical lobe.
// The return value is the sum of diffuse and specular, computed separately so raster's
// floating-point results do not change; diffuse and specular are for callers that need the split.
float3 EvaluateOpenPBRSurfaceBrdf(
    LightingParameters lightingParameters,
    OpenPBRBaseLayerState baseState,
    float3 lightDir,
    float normDotView,
    float normDotLight,
    out float3 diffuse,
    out float3 specular)
{
#if !defined(CLOD_FORWARD_COAT) || CLOD_FORWARD_COAT
    const OpenPBRCoatLayerState coatState = MakeOpenPBRCoatLayerState(
        baseState,
        lightingParameters.coatColor,
        lightingParameters.coatWeight,
        lightingParameters.coatIor,
        lightingParameters.coatRoughness,
        lightingParameters.coatDarkening);
#endif
#if !defined(CLOD_FORWARD_FUZZ) || CLOD_FORWARD_FUZZ
    const OpenPBRFuzzLayerState fuzzState = MakeOpenPBRFuzzLayerState(
        lightingParameters.normal,
        lightingParameters.viewDir,
        lightingParameters.fuzzColor,
        lightingParameters.fuzzWeight,
        lightingParameters.fuzzRoughness);
#endif
    const OpenPBRBaseLayerEvaluation baseEvaluation =
        EvaluateOpenPBRBaseLayerDirect(baseState, lightingParameters.normal, lightingParameters.viewDir, lightDir);
#if !defined(CLOD_FORWARD_FUZZ) || CLOD_FORWARD_FUZZ
    const float fuzzLayerScale = OpenPBRFuzzBaseLayerScaleComplete(fuzzState, lightDir);
#else
    const float fuzzLayerScale = 1.0f;
#endif
#if !defined(CLOD_FORWARD_COAT) || CLOD_FORWARD_COAT
    const float3 baseLayerScale = OpenPBRCoatBaseLayerScaleComplete(coatState, normDotView, normDotLight);
#else
    const float3 baseLayerScale = 1.0f.xxx;
#endif

    float3 coatFr = 0.0f.xxx;
#if !defined(CLOD_FORWARD_COAT) || CLOD_FORWARD_COAT
    if (coatState.presence > 0.0f)
    {
        float3 halfwayDir = normalize(lightDir + lightingParameters.viewDir);
        float normDotHalf = saturate(dot(lightingParameters.normal, halfwayDir));
        float lightDotHalf = saturate(dot(lightDir, halfwayDir));
        coatFr = specularLobe(lightingParameters.coatRoughness, lightingParameters.coatF0, halfwayDir, normDotView, normDotLight, normDotHalf, lightDotHalf);
        coatFr *= mx_ggx_energy_compensation(normDotView, lightingParameters.coatRoughness, lightingParameters.coatF0) * coatState.presence;
    }
#endif

#if !defined(CLOD_FORWARD_FUZZ) || CLOD_FORWARD_FUZZ
    float3 fuzzFr = OpenPBRFuzzSheenBRDF(fuzzState, lightDir);
#else
    float3 fuzzFr = 0.0f.xxx;
#endif
    float3 baseAttenuation = fuzzLayerScale.xxx * baseLayerScale;
    diffuse = baseEvaluation.diffuse * baseAttenuation;
    specular = baseEvaluation.specular * baseAttenuation + coatFr * fuzzLayerScale.xxx + fuzzFr;
    return (baseEvaluation.diffuse + baseEvaluation.specular) * baseAttenuation + coatFr * fuzzLayerScale.xxx + fuzzFr;
}

// One light's unshadowed contribution. total is what raster accumulates, computed exactly as it always has been;
// diffuse and specular split it by lobe (glint counts as specular) for passes that need them apart.
struct DirectLightContribution
{
    float3 total;
    float3 diffuse;
    float3 specular;
};

DirectLightContribution EvaluateDirectLightContribution(
    LightFragmentData light,
    LightingParameters lightingParameters,
    OpenPBRBaseLayerState baseState)
{
    DirectLightContribution result = (DirectLightContribution)0;
    float normDotView = saturate(dot(lightingParameters.normal, lightingParameters.viewDir));
    float normDotLight = saturate(dot(lightingParameters.normal, light.lightToFrag));
    // A light at or below the geometric shading horizon has no direct surface
    // contribution. Besides avoiding needless BRDF work, returning here keeps
    // the GGX horizon singularity from becoming Inf * 0 when NoL is zero.
    if (normDotLight <= 0.0f || normDotView <= 0.0f)
    {
        return result;
    }
    float3 diffuse;
    float3 specular;
    float3 BRDF = EvaluateOpenPBRSurfaceBrdf(lightingParameters, baseState, light.lightToFrag, normDotView, normDotLight, diffuse, specular);

    result.total = BRDF * light.lightColor.rgb * light.intensity * light.attenuation * light.spotAttenuation * normDotLight;
    const float3 irradiance = LightIncidentRadiance(light) * normDotLight;
    result.diffuse = diffuse * irradiance;
    result.specular = specular * irradiance;
#if !defined(CLOD_FORWARD_GLINT) || CLOD_FORWARD_GLINT
    const float3 glint = EvaluateGlintContribution(light, lightingParameters);
    result.total += glint;
    result.specular += glint;
#endif
    return result;
}

uint3 ComputeClusterID(float2 pixelCoords, float viewDepth,
                          ConstantBuffer<PerFrameBuffer> perFrame, Camera mainCamera) {

    uint gridX = max(perFrame.lightClusterGridSizeX, 1u);
    uint gridY = max(perFrame.lightClusterGridSizeY, 1u);
    uint totalZ = max(perFrame.lightClusterGridSizeZ, 1u);

    float2 tileSize = float2(perFrame.screenResX, perFrame.screenResY) / float2(gridX, gridY);
    uint2 tile = min(uint2(pixelCoords / max(tileSize, float2(1.0f, 1.0f))), uint2(gridX - 1u, gridY - 1u));
    
    // Z slice piecewise
    float zNear = max(mainCamera.zNear, 1.0e-5f);
    float z = max(abs(viewDepth), zNear);
    uint nearSlices = min(perFrame.nearClusterCount, totalZ);
    float zSplit = max(perFrame.clusterZSplitDepth, zNear + 1.0e-4f);
    float zFar = max(mainCamera.zFar, zSplit + 1.0e-4f);
    uint sliceZ;

    if (nearSlices > 0u && z < zSplit) {
        // uniform up close
        float t = saturate((z - zNear) / max(zSplit - zNear, 1.0e-5f));
        sliceZ = min(uint(t * nearSlices), nearSlices - 1u);
    }
    else if (totalZ > nearSlices) {
        // logarithmic beyond zSplit
        float logStart = log(zSplit / zNear);
        float logEnd = log(zFar / zNear);
        float logZ = log(z / zNear);
        float u = saturate((logZ - logStart) / max(logEnd - logStart, 1.0e-5f));
        uint logSlices = totalZ - nearSlices;
        sliceZ = nearSlices + min(uint(u * logSlices), logSlices - 1u);
    }
    else {
        sliceZ = totalZ - 1u;
    }
    
    return uint3(tile.x, tile.y, sliceZ);
}

// ---------------------------------------------------------------------------------------------------------------------
// The direct-light loop. Every raster and ray-hit shading path runs this one loop over the analytic lights; what differs
// between them is only how visibility is known, which the caller states in DirectLightingOptions. Nothing below reads
// root constants, so a pass that does not set the lighting switches (MiscShadowingMode and the rest) builds its options
// explicitly instead of inheriting whatever its own root constants hold in those slots.
//
// Shadowing (ShadowingMode, SHADOWING_MODE_* in Common/defines.h):
//   NONE                 no occlusion.
//   VIRTUAL_SHADOW_MAPS  the sun's occlusion from the virtual shadow map, combined by max with options.sunOcclusion
//                        (the ray traced sun mask, where it runs). Local lights are unshadowed.
// Defining LIGHTING_NO_VIRTUAL_SHADOW_MAPS compiles the shadow map lookup out, for passes that never bind its resources
// (ray hits: the shadow map only has pages for what the camera sees).

struct DirectLightingOptions
{
    bool punctualLights; // False skips every analytic light.
    bool virtualShadowMaps; // The sun's occlusion from the virtual shadow map; its resources must be bound.
    float sunOcclusion; // The sun's occlusion known before the loop (a traced ray or mask), combined by max.
    uint sunLightIndex; // Which light sunOcclusion belongs to: PerFrameCB::sunLightIndex.
};

DirectLightingOptions MakeDirectLightingOptions(bool punctualLights, uint shadowingMode)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    DirectLightingOptions options;
    options.punctualLights = punctualLights;
    options.virtualShadowMaps = shadowingMode == SHADOWING_MODE_VIRTUAL_SHADOW_MAPS;
    options.sunOcclusion = 0.0f;
    options.sunLightIndex = perFrame.sunLightIndex;
    return options;
}

// The options a lighting pass's root constants describe (MiscEnablePunctualLights, MiscShadowingMode).
DirectLightingOptions DirectLightingOptionsFromRoot()
{
    return MakeDirectLightingOptions(GetRootEnablePunctualLights(), GetRootShadowingMode());
}

struct DirectLightingResult
{
    float3 lighting; // Every light's contribution, as raster writes it.
    float3 diffuse; // lighting split by lobe.
    float3 specular;
    bool hasSunShadowDebug; // The first shadow-mapped directional light's lookup, for the VSM debug views.
    CLodVirtualShadowDebugInfo sunShadowDebug;
    uint clusterIndex; // PSO_CLUSTERED_LIGHTING: the fragment's cluster Z slice and its light count.
    uint clusterLightCount;
};

// One light's occlusion at the fragment: 0 lit, 1 fully shadowed.
float EvaluateLightOcclusion(
    LightInfo light,
    uint lightIndex,
    FragmentInfo fragmentInfo,
    Camera mainCamera,
    DirectLightingOptions options,
    inout DirectLightingResult result)
{
    if (light.type != LIGHT_TYPE_DIRECTIONAL)
    {
        return 0.0f; // No shadowing mode shadows local lights yet.
    }

    float occlusion = 0.0f;
    bool shadowMapped = false;
    CLodVirtualShadowDebugInfo shadowDebugInfo = CLodVirtualShadowInitDebugInfo(0xFFFFFFFFu);
#if !defined(LIGHTING_NO_VIRTUAL_SHADOW_MAPS)
    if (options.virtualShadowMaps && light.shadowViewInfoIndex >= 0)
    {
        ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
        StructuredBuffer<unsigned int> directionalShadowViewInfoIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::DirectionalLightCascadeBuffer)];
        StructuredBuffer<Camera> cameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        occlusion = calculateDirectionalVSMShadowDetailed(
            fragmentInfo.pixelCoords,
            fragmentInfo.fragPosWorldSpace,
            fragmentInfo.fragPosViewSpace,
            fragmentInfo.normalWS,
            light,
            perFrame.numDirectionalClipmaps,
            perFrame.shadowCascadeSplits,
            directionalShadowViewInfoIndexBuffer,
            cameraBuffer,
            shadowDebugInfo);
        shadowMapped = true;
    }
#endif
    if (lightIndex == options.sunLightIndex)
    {
        occlusion = max(occlusion, options.sunOcclusion);
    }

    if (shadowMapped)
    {
#if defined(CLOD_VSM_RECEIVER_TELEMETRY)
        if (GetRootEnableVirtualShadowReceiverTelemetry())
        {
            CLodVirtualShadowRecordReceiverTelemetry(
                shadowDebugInfo,
                occlusion,
                (uint2)fragmentInfo.pixelCoords,
                fragmentInfo.fragPosWorldSpace,
                -fragmentInfo.fragPosViewSpace.z,
                mainCamera);
        }
#endif
        if (!result.hasSunShadowDebug)
        {
            result.hasSunShadowDebug = true;
            result.sunShadowDebug = shadowDebugInfo;
        }
    }
    return occlusion;
}

void AccumulateDirectLight(
    uint lightIndex,
    FragmentInfo fragmentInfo,
    Camera mainCamera,
    LightingParameters lightingParameters,
    OpenPBRBaseLayerState baseState,
    DirectLightingOptions options,
    inout DirectLightingResult result)
{
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    const LightInfo light = lights[lightIndex];
    const float occlusion = EvaluateLightOcclusion(light, lightIndex, fragmentInfo, mainCamera, options, result);
    // Full occlusion makes every direct-light term exactly zero.
    if (occlusion >= 1.0f)
    {
        return;
    }

    const LightFragmentData lightData = EvaluateLightAtPoint(light, fragmentInfo.fragPosWorldSpace.xyz);
    if (!LightInRange(light, lightData))
    {
        return;
    }
    const DirectLightContribution contribution = EvaluateDirectLightContribution(lightData, lightingParameters, baseState);
    const float visibility = 1.0f - occlusion;
    result.lighting += visibility * contribution.total;
    result.diffuse += visibility * contribution.diffuse;
    result.specular += visibility * contribution.specular;
}

DirectLightingResult EvaluateDirectLighting(FragmentInfo fragmentInfo, Camera mainCamera, DirectLightingOptions options)
{
    DirectLightingResult result = (DirectLightingResult)0;
    result.hasSunShadowDebug = false;
    result.sunShadowDebug = CLodVirtualShadowInitDebugInfo(0xFFFFFFFFu);
    if (!options.punctualLights)
    {
        return result;
    }

    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const LightingParameters lightingParameters = MakeLightingParameters(fragmentInfo);
    // This state depends only on the material/view inputs, so it is built once, outside the light loop.
    const OpenPBRBaseLayerState baseState = MakeOpenPBRBaseLayerState(
        lightingParameters.weightedBaseColor,
        lightingParameters.diffuseColor,
        lightingParameters.baseDiffuseRoughness,
        lightingParameters.specularAlpha,
        lightingParameters.weightedSpecularIor,
        lightingParameters.dielectricSpecularF0,
        lightingParameters.dielectricSpecularWeight,
        lightingParameters.metalAverageFresnel,
        lightingParameters.metalSpecularF0,
        lightingParameters.metalSpecularWeight);

    // TODO: Parallax shadows will require a forward pass.

#if defined(PSO_CLUSTERED_LIGHTING)
    StructuredBuffer<Cluster> clusterBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ClusterBuffer)];
    StructuredBuffer<LightPage> lightPagesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PagesBuffer)];

    const uint3 clusterID = ComputeClusterID(fragmentInfo.pixelCoords, fragmentInfo.fragPosViewSpace.z, perFrameBuffer, mainCamera);
    const uint clusterIndex = clusterID.x +
                              clusterID.y * perFrameBuffer.lightClusterGridSizeX +
                              clusterID.z * perFrameBuffer.lightClusterGridSizeX * perFrameBuffer.lightClusterGridSizeY;
    const Cluster activeCluster = clusterBuffer[clusterIndex];
    result.clusterIndex = clusterID.z;
    result.clusterLightCount = activeCluster.numLights;

    // Pages hold light buffer indices: lightCulling.hlsl copies them out of the active list.
    uint remainingLights = activeCluster.numLights;
    uint pageIndex = activeCluster.ptrFirstPage;
    const uint maxPagesToVisit = max(1u, (remainingLights + LIGHTS_PER_PAGE - 1u) / LIGHTS_PER_PAGE);
    uint pagesVisited = 0;
    while (pageIndex != LIGHT_PAGE_ADDRESS_NULL && remainingLights > 0 && pagesVisited < maxPagesToVisit)
    {
        const LightPage page = lightPagesBuffer[pageIndex];
        const uint lightsInPage = min(min(page.numLightsInPage, LIGHTS_PER_PAGE), remainingLights);
        if (lightsInPage == 0)
        {
            break;
        }
        for (uint i = 0; i < lightsInPage; i++)
        {
            AccumulateDirectLight(page.lightIndices[i], fragmentInfo, mainCamera, lightingParameters, baseState, options, result);
        }
        remainingLights -= lightsInPage;
        pageIndex = page.ptrNextPage;
        pagesVisited++;
    }
#else
    StructuredBuffer<unsigned int> activeLightIndices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
    for (uint i = 0; i < perFrameBuffer.numLights; i++)
    {
        AccumulateDirectLight(activeLightIndices[i], fragmentInfo, mainCamera, lightingParameters, baseState, options, result);
    }
#endif
    return result;
}

// The VSM debug views' payload for the sun's lookup, or DEBUG_SENTINEL when the view is not a VSM one.
uint2 VirtualShadowDebugPayload(DirectLightingResult direct, uint outputType)
{
    if (!direct.hasSunShadowDebug)
    {
        return uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
    }
    const CLodVirtualShadowDebugInfo shadowDebugInfo = direct.sunShadowDebug;
    switch (outputType)
    {
    case OUTPUT_VSM_PREFERRED_CLIPMAP:
        return PackDebugFloat3(CLodVirtualShadowDebugClipmapColor(shadowDebugInfo.preferredClipmapIndex));
    case OUTPUT_VSM_SAMPLED_CLIPMAP:
        return PackDebugFloat3(CLodVirtualShadowDebugClipmapColor(shadowDebugInfo.sampledClipmapIndex));
    case OUTPUT_VSM_PAGE_STATE:
        return PackDebugFloat3(CLodVirtualShadowDebugPageStateColor(shadowDebugInfo));
    case OUTPUT_VSM_PHYSICAL_PAGE:
        return PackDebugUint(shadowDebugInfo.sampledPhysicalPageIndex == 0xFFFFFFFFu ? 0u : (shadowDebugInfo.sampledPhysicalPageIndex + 1u));
    case OUTPUT_VSM_RERENDERED_THIS_FRAME:
        return PackDebugFloat3(CLodVirtualShadowDebugRerenderedThisFrameColor(shadowDebugInfo));
    case OUTPUT_VSM_CACHED_BASIS_CORRECTION:
        return PackDebugFloat3(CLodVirtualShadowDebugCachedBasisColor(shadowDebugInfo));
    case OUTPUT_VSM_PAGE_LOCAL_TEXEL:
        return PackDebugFloat3(CLodVirtualShadowDebugPageLocalTexelColor(shadowDebugInfo));
    case OUTPUT_VSM_DEPTH_MARGIN:
        return PackDebugFloat3(CLodVirtualShadowDebugDepthMarginColor(shadowDebugInfo));
    case OUTPUT_VSM_CLIP_COMPARISON:
        return PackDebugFloat3(CLodVirtualShadowDebugClipComparisonColor(shadowDebugInfo));
    case OUTPUT_VSM_CLIP_GRID_OFFSET:
        return PackDebugFloat3(CLodVirtualShadowDebugClipGridOffsetColor(shadowDebugInfo));
    case OUTPUT_VSM_TRACE_FOOTPRINT:
        return PackDebugFloat3(CLodVirtualShadowDebugTraceFootprintColor(shadowDebugInfo));
    default:
        return uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
    }
}

// Emission, through the coat and fuzz layers above it where those are compiled in.
float3 EvaluateFragmentEmission(FragmentInfo fragmentInfo)
{
#if !defined(CLOD_FORWARD_EMISSION) || CLOD_FORWARD_EMISSION
    if (all(fragmentInfo.emissive == 0.0f.xxx))
    {
        return 0.0f.xxx;
    }
#if (!defined(CLOD_FORWARD_COAT) || CLOD_FORWARD_COAT) || (!defined(CLOD_FORWARD_FUZZ) || CLOD_FORWARD_FUZZ)
    const OpenPBRBaseLayerState emissiveBaseState = MakeOpenPBRBaseLayerState(
        fragmentInfo.albedo,
        fragmentInfo.diffuseColor,
        fragmentInfo.baseDiffuseRoughness,
        fragmentInfo.specularAlpha,
        fragmentInfo.weightedSpecularIor,
        fragmentInfo.dielectricSpecularF0,
        fragmentInfo.dielectricSpecularWeight,
        fragmentInfo.metalAverageFresnel,
        fragmentInfo.metalSpecularF0,
        fragmentInfo.metalSpecularWeight);
    const OpenPBRCoatLayerState emissiveCoatState = MakeOpenPBRCoatLayerState(
        emissiveBaseState,
        fragmentInfo.coatColor,
        fragmentInfo.coatWeight,
        fragmentInfo.coatIor,
        fragmentInfo.coatRoughness,
        fragmentInfo.coatDarkening);
    return EvaluateOpenPBREmissive(
        fragmentInfo.emissive,
        emissiveCoatState,
        fragmentInfo.fuzzWeight,
        fragmentInfo.fuzzRoughness,
        fragmentInfo.NdotV);
#else
    return fragmentInfo.emissive;
#endif
#else
    return 0.0f.xxx;
#endif
}

#if defined(PSO_IMAGE_BASED_LIGHTING)
void EvaluateFragmentIBL(
    FragmentInfo fragmentInfo,
    uint activeEnvironmentIndex,
    uint environmentBufferDescriptorIndex,
    inout float3 lighting,
    inout float3 diffuseIBL,
    inout float3 specularIBL)
{
    evaluateIBL(lighting,
                diffuseIBL,
                specularIBL,
                fragmentInfo.normalWS,
                fragmentInfo.normalWS,
                fragmentInfo.albedo,
                fragmentInfo.diffuseColor,
                fragmentInfo.diffuseAmbientOcclusion,
                fragmentInfo.baseDiffuseRoughness,
                fragmentInfo.dielectricSpecularF0,
                fragmentInfo.dielectricSpecularWeight,
                fragmentInfo.weightedSpecularIor,
                fragmentInfo.metalAverageFresnel,
                fragmentInfo.metalSpecularF0,
                fragmentInfo.metalSpecularWeight,
                fragmentInfo.reflectedWS,
                fragmentInfo.roughness,
                fragmentInfo.perceptualRoughness,
                fragmentInfo.specularAlpha,
                fragmentInfo.coatColor,
                fragmentInfo.coatF0,
                fragmentInfo.coatWeight,
                fragmentInfo.coatIor,
                fragmentInfo.coatDarkening,
                fragmentInfo.coatRoughness,
                fragmentInfo.coatPerceptualRoughness,
                fragmentInfo.fuzzColor,
                fragmentInfo.fuzzWeight,
                fragmentInfo.fuzzRoughness,
                fragmentInfo.NdotV,
                activeEnvironmentIndex,
                environmentBufferDescriptorIndex);
}
#endif

// Image-based, direct and emitted light at a fragment.
float3 lightFragmentColor(FragmentInfo fragmentInfo, Camera mainCamera, uint activeEnvironmentIndex, uint environmentBufferDescriptorIndex, DirectLightingOptions options)
{
    float3 lighting = float3(0.0, 0.0, 0.0);
#if defined(PSO_IMAGE_BASED_LIGHTING)
    float3 unusedDiffuseIBL = 0.0f.xxx;
    float3 unusedSpecularIBL = 0.0f.xxx;
    EvaluateFragmentIBL(fragmentInfo, activeEnvironmentIndex, environmentBufferDescriptorIndex, lighting, unusedDiffuseIBL, unusedSpecularIBL);
#endif
    lighting += EvaluateDirectLighting(fragmentInfo, mainCamera, options).lighting;
    return lighting + EvaluateFragmentEmission(fragmentInfo);
}

// lightFragmentColor, plus what the debug views need.
LightingOutput lightFragment(FragmentInfo fragmentInfo, Camera mainCamera, uint activeEnvironmentIndex, uint environmentBufferDescriptorIndex, DirectLightingOptions options)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    LightingOutput output;
    float3 lighting = float3(0.0, 0.0, 0.0);
#if defined(PSO_IMAGE_BASED_LIGHTING)
    float3 debugDiffuse = float3(0, 0, 0);
    float3 debugSpecular = float3(0, 0, 0);
    EvaluateFragmentIBL(fragmentInfo, activeEnvironmentIndex, environmentBufferDescriptorIndex, lighting, debugDiffuse, debugSpecular);
    output.diffuseIBL = debugDiffuse;
    output.specularIBL = debugSpecular;
#endif

    const DirectLightingResult direct = EvaluateDirectLighting(fragmentInfo, mainCamera, options);
    lighting += direct.lighting;
    output.shadowDebugPayload = VirtualShadowDebugPayload(direct, perFrameBuffer.outputType);
#if defined(PSO_CLUSTERED_LIGHTING)
    output.clusterIndex = direct.clusterIndex;
    output.clusterLightCount = direct.clusterLightCount;
#endif

    output.lighting = lighting + EvaluateFragmentEmission(fragmentInfo);
    return output;
}

#endif // __LIGHTING_HLSLI__
