#ifndef __LIGHTING_HLSLI__
#define __LIGHTING_HLSLI__

#include "include/vertex.hlsli"
#include "include/structs.hlsli"
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

struct LightFragmentData {
    uint lightType;
    float3 lightPos;
    float3 lightColor;
    float intensity;
    float3 lightToFrag;
    float attenuation;
    float distance;
    float spotAttenuation;
};

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

// Models spotlight falloff with linear interpolation between inner and outer cone angles
float spotAttenuation(float3 pointToLight, float3 lightDirection, float outerConeCos, float innerConeCos) {
    float cos = dot(normalize(lightDirection), normalize(-pointToLight));
    if (cos > outerConeCos) {
        if (cos < innerConeCos) {
            return smoothstep(outerConeCos, innerConeCos, cos);
        }
        return 1.0;
    }
    return 0.0;
}

LightFragmentData getLightParametersForFragment(LightInfo light, float3 fragPos) {
    LightFragmentData result;
    result.lightType = light.type;
    result.lightPos = light.posWorldSpace.xyz;
    result.lightColor = light.color.xyz;
    result.intensity = light.color.w;
    result.distance = 0.0;
    
    switch (light.type) {
        case 2:{
                result.lightToFrag = -light.dirWorldSpace.xyz;
                result.attenuation = 1.0;
                break;
            }
        default:{
                float constantAttenuation = light.attenuation.x;
                float linearAttenuation = light.attenuation.y;
                float quadraticAttenuation = light.attenuation.z;
                result.lightToFrag = normalize(light.posWorldSpace.xyz - fragPos);
                result.distance = length(light.posWorldSpace.xyz - fragPos);
                result.attenuation = 1.0 / ((constantAttenuation + linearAttenuation * result.distance + quadraticAttenuation * result.distance * result.distance) + 0.0001); //+0.0001 fudge-factor to prevent division by 0;
                break;
            }
    }
    
    if (light.type == 1) {
        result.spotAttenuation = spotAttenuation(result.lightToFrag, light.dirWorldSpace.xyz, light.outerConeAngle, light.innerConeAngle);
    }
    else {
        result.spotAttenuation = 1.0;
    }
    return result;
}


float3 calculateLightContributionPBR(LightFragmentData light, LightingParameters lightingParameters)
{
    float normDotView = saturate(dot(lightingParameters.normal, lightingParameters.viewDir));
    float normDotLight = saturate(dot(lightingParameters.normal, light.lightToFrag));
    // A light at or below the geometric shading horizon has no direct surface
    // contribution. Besides avoiding needless BRDF work, returning here keeps
    // the GGX horizon singularity from becoming Inf * 0 when NoL is zero.
    if (normDotLight <= 0.0f || normDotView <= 0.0f)
    {
        return 0.0f.xxx;
    }
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
    const OpenPBRCoatLayerState coatState = MakeOpenPBRCoatLayerState(
        baseState,
        lightingParameters.coatColor,
        lightingParameters.coatWeight,
        lightingParameters.coatIor,
        lightingParameters.coatRoughness,
        lightingParameters.coatDarkening);
    const OpenPBRFuzzLayerState fuzzState = MakeOpenPBRFuzzLayerState(
        lightingParameters.normal,
        lightingParameters.viewDir,
        lightingParameters.fuzzColor,
        lightingParameters.fuzzWeight,
        lightingParameters.fuzzRoughness);
    const OpenPBRBaseLayerEvaluation baseEvaluation =
        EvaluateOpenPBRBaseLayerDirect(baseState, lightingParameters.normal, lightingParameters.viewDir, light.lightToFrag);
    const float fuzzLayerScale = OpenPBRFuzzBaseLayerScaleComplete(fuzzState, light.lightToFrag);
    const float3 baseLayerScale = OpenPBRCoatBaseLayerScaleComplete(coatState, normDotView, normDotLight);

    float3 coatFr = 0.0f.xxx;
    if (coatState.presence > 0.0f)
    {
        float3 halfwayDir = normalize(light.lightToFrag + lightingParameters.viewDir);
        float normDotHalf = saturate(dot(lightingParameters.normal, halfwayDir));
        float lightDotHalf = saturate(dot(light.lightToFrag, halfwayDir));
        coatFr = specularLobe(lightingParameters.coatRoughness, lightingParameters.coatF0, halfwayDir, normDotView, normDotLight, normDotHalf, lightDotHalf);
        coatFr *= mx_ggx_energy_compensation(normDotView, lightingParameters.coatRoughness, lightingParameters.coatF0) * coatState.presence;
    }

    float3 fuzzFr = OpenPBRFuzzSheenBRDF(fuzzState, light.lightToFrag);
    float3 baseAttenuation = fuzzLayerScale.xxx * baseLayerScale;
    float3 BRDF = (baseEvaluation.diffuse + baseEvaluation.specular) * baseAttenuation + coatFr * fuzzLayerScale.xxx + fuzzFr;

    return BRDF * light.lightColor.rgb * light.intensity * light.attenuation * light.spotAttenuation * normDotLight +
        EvaluateGlintContribution(light, lightingParameters);
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

float3 lightFragmentColor(FragmentInfo fragmentInfo, Camera mainCamera, uint activeEnvironmentIndex, uint environmentBufferDescriptorIndex) {
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];
    float3 lighting = float3(0.0, 0.0, 0.0);

#if defined(PSO_IMAGE_BASED_LIGHTING)
    float3 unusedDiffuseIBL = 0.0f.xxx;
    float3 unusedSpecularIBL = 0.0f.xxx;
    evaluateIBL(lighting,
                unusedDiffuseIBL,
                unusedSpecularIBL,
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
#endif

    if (GetRootEnablePunctualLights())
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

        StructuredBuffer<unsigned int> pointShadowViewInfoIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PointLightCubemapBuffer)];
        StructuredBuffer<unsigned int> spotShadowViewInfoIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::SpotLightMatrixBuffer)];
        StructuredBuffer<unsigned int> directionalShadowViewInfoIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::DirectionalLightCascadeBuffer)];
        StructuredBuffer<Camera> cameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];

        StructuredBuffer<unsigned int> activeLightIndices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
        StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];

#if defined(PSO_CLUSTERED_LIGHTING)
        StructuredBuffer<Cluster> clusterBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ClusterBuffer)];
        StructuredBuffer<LightPage> lightPagesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PagesBuffer)];

        float3 clusterID = ComputeClusterID(fragmentInfo.pixelCoords, fragmentInfo.fragPosViewSpace.z, perFrameBuffer, cameraBuffer[perFrameBuffer.mainCameraIndex]);
        uint clusterIndex = clusterID.x +
                            clusterID.y * perFrameBuffer.lightClusterGridSizeX +
                            clusterID.z * perFrameBuffer.lightClusterGridSizeX * perFrameBuffer.lightClusterGridSizeY;

        Cluster activeCluster = clusterBuffer[clusterIndex];

        uint remainingLights = activeCluster.numLights;
        uint pageIndex = activeCluster.ptrFirstPage;
        uint maxPagesToVisit = max(1u, (remainingLights + LIGHTS_PER_PAGE - 1u) / LIGHTS_PER_PAGE);
        uint pagesVisited = 0;

        while (pageIndex != LIGHT_PAGE_ADDRESS_NULL && remainingLights > 0 && pagesVisited < maxPagesToVisit) {
            LightPage page = lightPagesBuffer[pageIndex];
            uint lightsInPage = min(page.numLightsInPage, LIGHTS_PER_PAGE);
            lightsInPage = min(lightsInPage, remainingLights);
            if (lightsInPage == 0) {
                break;
            }

            for (uint i = 0; i < lightsInPage; i++) {
                unsigned int index = activeLightIndices[page.lightIndices[i]];
#else
        for (uint i = 0; i < perFrameBuffer.numLights; i++)
        {
            unsigned int index = activeLightIndices[i];
#endif
            LightInfo light = lights[index];
            float shadow = 0.0;
            if (GetRootEnableShadows())
            {
                if (light.shadowViewInfoIndex != -1)
                {
                    switch (light.type)
                    {
                    case 0:{
                            if (light.shadowMapIndex == -1)
                            {
                                break;
                            }
                            shadow = calculatePointShadow(fragmentInfo.fragPosWorldSpace, fragmentInfo.normalWS.xyz, light, pointShadowViewInfoIndexBuffer, cameraBuffer);
                            break;
                        }
                    case 1:{
                            if (light.shadowMapIndex == -1)
                            {
                                break;
                            }
                            uint spotShadowCameraIndex = spotShadowViewInfoIndexBuffer[light.shadowViewInfoIndex];
                            Camera camera = cameraBuffer[spotShadowCameraIndex];
                            shadow = calculateSpotShadow(fragmentInfo.fragPosWorldSpace, fragmentInfo.normalWS, light, camera.viewProjection, light.nearPlane, light.farPlane);
                            break;
                        }
                    case 2:{
                            shadow = calculateDirectionalVSMShadow(
                                fragmentInfo.pixelCoords,
                                fragmentInfo.fragPosWorldSpace,
                                fragmentInfo.fragPosViewSpace,
                                fragmentInfo.normalWS,
                                light,
                                perFrameBuffer.numDirectionalClipmaps,
                                perFrameBuffer.shadowCascadeSplits,
                                directionalShadowViewInfoIndexBuffer,
                                cameraBuffer);
                            break;
                        }
                    }
                }
            }

            LightFragmentData lightFragmentInfo = getLightParametersForFragment(light, fragmentInfo.fragPosWorldSpace.xyz);
            if (light.type != 2 && lightFragmentInfo.distance > light.maxRange)
            {
                continue;
            }
            lighting += (1.0 - shadow) * calculateLightContributionPBR(lightFragmentInfo, lightingParameters);
        }
#if defined(PSO_CLUSTERED_LIGHTING)
            remainingLights -= lightsInPage;
            pageIndex = page.ptrNextPage;
            pagesVisited++;
        }
#endif
    }

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
    return lighting + EvaluateOpenPBREmissive(
        fragmentInfo.emissive,
        emissiveCoatState,
        fragmentInfo.fuzzWeight,
        fragmentInfo.fuzzRoughness,
        fragmentInfo.NdotV);
}

LightingOutput lightFragment(FragmentInfo fragmentInfo, Camera mainCamera, uint activeEnvironmentIndex, uint environmentBufferDescriptorIndex, bool isFrontFace) {
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[0];
    float3 lighting = float3(0.0, 0.0, 0.0);

#if defined(PSO_IMAGE_BASED_LIGHTING)
    float3 debugDiffuse = float3(0, 0, 0);
    float3 debugSpecular = float3(0, 0, 0);
#endif

    LightingOutput output;

#if defined(PSO_CLUSTERED_LIGHTING)
    output.shadowDebugPayload = uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
#endif

#if defined(PSO_IMAGE_BASED_LIGHTING)
    evaluateIBL(lighting,
                debugDiffuse,
                debugSpecular,
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
#endif // IMAGE_BASED_LIGHTING

    // Direct lighting
#if defined(PSO_CLUSTERED_LIGHTING)
    uint clusterIndex = 0; // Which light cluster this fragment belongs to
    uint clusterLightCount = 0; // Number of lights in the cluster
#endif
        
    if (GetRootEnablePunctualLights())
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
        
        // TODO: Parallax shadows will require a forward pass
        //parallaxShadowParameters parallaxShadowParams;
        //if (materialInfo.materialFlags & MATERIAL_PARALLAX)
        // {
        //    parallaxShadowParams.parallaxTexture = ResourceDescriptorHeap[materialInfo.heightMapIndex];
        //    parallaxShadowParams.parallaxSampler = ResourceDescriptorHeap[materialInfo.heightSamplerIndex];
        //    parallaxShadowParams.TBN = TBN;
        //    parallaxShadowParams.heightmapScale = materialInfo.heightMapScale;
        //    parallaxShadowParams.lightToFrag = viewDir;
        //    parallaxShadowParams.viewDir = viewDir;
        //    parallaxShadowParams.uv = uv;
        //}
        
        StructuredBuffer<unsigned int> pointShadowViewInfoIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PointLightCubemapBuffer)];
        StructuredBuffer<unsigned int> spotShadowViewInfoIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::SpotLightMatrixBuffer)];
        StructuredBuffer<unsigned int> directionalShadowViewInfoIndexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::DirectionalLightCascadeBuffer)];
        StructuredBuffer<Camera> cameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        
        StructuredBuffer<unsigned int> activeLightIndices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
        StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];

#if defined(PSO_CLUSTERED_LIGHTING)
        
        StructuredBuffer<Cluster> clusterBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ClusterBuffer)];
        StructuredBuffer<LightPage> lightPagesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PagesBuffer)];
        
        float3 clusterID = ComputeClusterID(fragmentInfo.pixelCoords, fragmentInfo.fragPosViewSpace.z, perFrameBuffer, cameraBuffer[perFrameBuffer.mainCameraIndex]);
        clusterIndex = clusterID.x +
                        clusterID.y * perFrameBuffer.lightClusterGridSizeX +
                        clusterID.z * perFrameBuffer.lightClusterGridSizeX * perFrameBuffer.lightClusterGridSizeY;
        
        Cluster activeCluster = clusterBuffer[clusterIndex];
        
        clusterIndex = clusterID.z;

        clusterLightCount = activeCluster.numLights;
        // Loop through all pages of lights in the cluster
        uint pageIndex = activeCluster.ptrFirstPage;

        uint remainingLights = clusterLightCount;
        uint maxPagesToVisit = max(1u, (clusterLightCount + LIGHTS_PER_PAGE - 1u) / LIGHTS_PER_PAGE);
        uint pagesVisited = 0;

        while (pageIndex != LIGHT_PAGE_ADDRESS_NULL && remainingLights > 0 && pagesVisited < maxPagesToVisit) {
            LightPage page = lightPagesBuffer[pageIndex];
            uint lightsInPage = min(page.numLightsInPage, LIGHTS_PER_PAGE);
            lightsInPage = min(lightsInPage, remainingLights);
            if (lightsInPage == 0) {
                break;
            }

            for (uint i = 0; i < lightsInPage; i++) {
                unsigned int index = activeLightIndices[page.lightIndices[i]];
#else
        for (uint i = 0; i < perFrameBuffer.numLights; i++)
        {
            unsigned int index = activeLightIndices[i];
#endif
            LightInfo light = lights[index];
            float shadow = 0.0;
            if (GetRootEnableShadows())
            {
                if (light.shadowViewInfoIndex != -1)
                {
                    switch (light.type)
                    {
                    case 0:{ // Point light
                            if (light.shadowMapIndex == -1)
                            {
                                break;
                            }
                            shadow = calculatePointShadow(fragmentInfo.fragPosWorldSpace, fragmentInfo.normalWS.xyz, light, pointShadowViewInfoIndexBuffer, cameraBuffer);
                        //return float4(shadow, shadow, shadow, 1.0);
                            break;
                        }
                    case 1:{ // Spot light
                            if (light.shadowMapIndex == -1)
                            {
                                break;
                            }
                            uint spotShadowCameraIndex = spotShadowViewInfoIndexBuffer[light.shadowViewInfoIndex];
                            Camera camera = cameraBuffer[spotShadowCameraIndex];
                            shadow = calculateSpotShadow(fragmentInfo.fragPosWorldSpace, fragmentInfo.normalWS, light, camera.viewProjection, light.nearPlane, light.farPlane);
                            break;
                        }
                    case 2:{// Directional light
                            CLodVirtualShadowDebugInfo shadowDebugInfo = CLodVirtualShadowInitDebugInfo(0xFFFFFFFFu);
	                         shadow = calculateDirectionalVSMShadowDetailed(
	                             fragmentInfo.pixelCoords,
	                             fragmentInfo.fragPosWorldSpace,
	                             fragmentInfo.fragPosViewSpace,
	                             fragmentInfo.normalWS,
	                             light,
	                             perFrameBuffer.numDirectionalClipmaps,
	                             perFrameBuffer.shadowCascadeSplits,
	                             directionalShadowViewInfoIndexBuffer,
	                             cameraBuffer,
	                             shadowDebugInfo);

                            if (output.shadowDebugPayload.x == DEBUG_SENTINEL)
                            {
                                switch (perFrameBuffer.outputType)
                                {
                                case OUTPUT_VSM_PREFERRED_CLIPMAP:
                                    output.shadowDebugPayload = PackDebugFloat3(CLodVirtualShadowDebugClipmapColor(shadowDebugInfo.preferredClipmapIndex));
                                    break;
                                case OUTPUT_VSM_SAMPLED_CLIPMAP:
                                    output.shadowDebugPayload = PackDebugFloat3(CLodVirtualShadowDebugClipmapColor(shadowDebugInfo.sampledClipmapIndex));
                                    break;
                                case OUTPUT_VSM_PAGE_STATE:
                                    output.shadowDebugPayload = PackDebugFloat3(CLodVirtualShadowDebugPageStateColor(shadowDebugInfo));
                                    break;
                                case OUTPUT_VSM_PHYSICAL_PAGE:
                                    output.shadowDebugPayload = PackDebugUint(shadowDebugInfo.sampledPhysicalPageIndex == 0xFFFFFFFFu ? 0u : (shadowDebugInfo.sampledPhysicalPageIndex + 1u));
                                    break;
                                case OUTPUT_VSM_RERENDERED_THIS_FRAME:
                                    output.shadowDebugPayload = PackDebugFloat3(CLodVirtualShadowDebugRerenderedThisFrameColor(shadowDebugInfo));
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
            
            LightFragmentData lightFragmentInfo = getLightParametersForFragment(light, fragmentInfo.fragPosWorldSpace.xyz);
            // if (shadow > 0.95)
            // {
            //     continue; // skip light if shadowed
            // }
            if (light.type != 2 && lightFragmentInfo.distance > light.maxRange)
            {
                continue;
            }
            lighting += (1.0 - shadow) * calculateLightContributionPBR(lightFragmentInfo, lightingParameters);
            //if (materialInfo.materialFlags & MATERIAL_PARALLAX)
            //{
            //    float parallaxShadow = getParallaxShadow(parallaxShadowParams);
            //}
        }
#if defined(PSO_CLUSTERED_LIGHTING)
            remainingLights -= lightsInPage;
            pageIndex = page.ptrNextPage;
            pagesVisited++;
        }
#endif
    }

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
    lighting += EvaluateOpenPBREmissive(
        fragmentInfo.emissive,
        emissiveCoatState,
        fragmentInfo.fuzzWeight,
        fragmentInfo.fuzzRoughness,
        fragmentInfo.NdotV);
    
    output.lighting = lighting;
    
#if defined(PSO_IMAGE_BASED_LIGHTING)
    output.diffuseIBL = debugDiffuse;
    output.specularIBL = debugSpecular;
#endif // IMAGE_BASED_LIGHTING
#if defined(PSO_CLUSTERED_LIGHTING)
    output.clusterIndex = clusterIndex;
    output.clusterLightCount = clusterLightCount;
#endif
    return output;
}

#endif // __LIGHTING_HLSLI__
