#ifndef __IBL_HLSLI__
#define __IBL_HLSLI__

#include "include/constants.hlsli"
#include "include/cbuffers.hlsli"
#include "include/PBR.hlsli"

float3 irradianceSH(float3 n, in const uint environmentIndex, in const uint environmentBufferIndex)
{
    StructuredBuffer<EnvironmentInfo> environments = ResourceDescriptorHeap[environmentBufferIndex];
    
    return
        float3(environments[environmentIndex].sphericalHarmonics[0], environments[environmentIndex].sphericalHarmonics[1], environments[environmentIndex].sphericalHarmonics[2]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE
       + float3(environments[environmentIndex].sphericalHarmonics[3], environments[environmentIndex].sphericalHarmonics[4], environments[environmentIndex].sphericalHarmonics[5]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * n.y
       + float3(environments[environmentIndex].sphericalHarmonics[6], environments[environmentIndex].sphericalHarmonics[7], environments[environmentIndex].sphericalHarmonics[8]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * n.z
       + float3(environments[environmentIndex].sphericalHarmonics[9], environments[environmentIndex].sphericalHarmonics[10], environments[environmentIndex].sphericalHarmonics[11]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * n.x
       + float3(environments[environmentIndex].sphericalHarmonics[12], environments[environmentIndex].sphericalHarmonics[13], environments[environmentIndex].sphericalHarmonics[14]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * n.y * n.x
       + float3(environments[environmentIndex].sphericalHarmonics[15], environments[environmentIndex].sphericalHarmonics[16], environments[environmentIndex].sphericalHarmonics[17]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * n.y * n.z
       + float3(environments[environmentIndex].sphericalHarmonics[18], environments[environmentIndex].sphericalHarmonics[19], environments[environmentIndex].sphericalHarmonics[20]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * (3.0 * n.z * n.z - 1.0)
       + float3(environments[environmentIndex].sphericalHarmonics[21], environments[environmentIndex].sphericalHarmonics[22], environments[environmentIndex].sphericalHarmonics[23]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * n.z * n.x
       + float3(environments[environmentIndex].sphericalHarmonics[24], environments[environmentIndex].sphericalHarmonics[25], environments[environmentIndex].sphericalHarmonics[26]) * environments[environmentIndex].sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE * (n.x * n.x - n.y * n.y);
        
}

/**
 * Returns a color ambient occlusion based on a pre-computed visibility term.
 * The albedo term is meant to be the diffuse color or f0 for the diffuse and
 * specular terms respectively.
 */
float3 gtaoMultiBounce(float visibility, const float3 albedo)
{
    // Jimenez et al. 2016, "Practical Realtime Strategies for Accurate Indirect Occlusion"
    float3 a = 2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c = 2.7552 * albedo + 0.6903;

    return max(float3(visibility.xxx), ((visibility * a + b) * visibility + c) * visibility);
}

void multiBounceAO(float visibility, const float3 albedo, inout float3 color)
{
//#if MULTI_BOUNCE_AMBIENT_OCCLUSION == 1 // Why would we not want this?
    color *= gtaoMultiBounce(visibility, albedo);
//#endif
}

void multiBounceSpecularAO(float visibility, const float3 albedo, inout float3 color)
{
//#if MULTI_BOUNCE_AMBIENT_OCCLUSION == 1 && SPECULAR_AMBIENT_OCCLUSION != SPECULAR_AO_OFF
    color *= gtaoMultiBounce(visibility, albedo);
//#endif
}

float SpecularAO_Lagarde(float NoV, float visibility, float roughness)
{
    // Lagarde and de Rousiers 2014, "Moving Frostbite to PBR"
    return saturate(pow(NoV + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility);
}

/*
Computes a specular occlusion term from the ambient occlusion term.
 */
float computeSpecularAO(float NdotV, float visibility, float roughness)
{
#if SPECULAR_AMBIENT_OCCLUSION == SPECULAR_AO_SIMPLE
    return SpecularAO_Lagarde(NdotV, visibility, roughness);
#elif SPECULAR_AMBIENT_OCCLUSION == SPECULAR_AO_BENT_NORMALS
    return SpecularAO_Cones(materialSurface, visibility, roughness);
#else
    return 1.0;
#endif
}

float3 getSpecularDominantDirection(const float3 n, const float3 r, float roughness)
{
    return lerp(r, n, roughness * roughness);
}

float3 getReflectedVector(float3 r, const float3 n, float roughness)
{
    // Anisotropy will change R
    return getSpecularDominantDirection(n, r, roughness);
}

float3 prefilteredRadiance(float3 reflection, float roughness, unsigned int prefilteredEnvironmentDescriptorIndex)
{
    
    TextureCube<float4> prefilteredEnvironment = ResourceDescriptorHeap[prefilteredEnvironmentDescriptorIndex];
    float lod = roughness * float(12 - 1);
    float4 specularSample = prefilteredEnvironment.SampleLevel(g_linearClamp, reflection, lod);
    return specularSample.rgb;
}

struct OpenPBRViewAlignedBasis
{
    float3 tangent;
    float3 bitangent;
    float3 normal;
};

struct OpenPBRFuzzLayerState
{
    float roughness;
    float3 tint;
    float presence;
    OpenPBRViewAlignedBasis basis;
    float3 viewDirLocal;
    float viewReflected;
};

void OpenPBRBuildViewAlignedBasis(float3 normal, float3 viewDir, out float3 tangent, out float3 bitangent)
{
    const float3 projectedView = viewDir - normal * dot(viewDir, normal);
    if (dot(projectedView, projectedView) > 1.0e-6f)
    {
        tangent = normalize(projectedView);
    }
    else
    {
        const float3 helper = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
        tangent = normalize(cross(helper, normal));
    }

    bitangent = cross(normal, tangent);
}

float3 OpenPBRWorldToLocal(OpenPBRViewAlignedBasis basis, float3 direction)
{
    return float3(dot(direction, basis.tangent), dot(direction, basis.bitangent), dot(direction, basis.normal));
}

float3 OpenPBRLookUpFuzzLTCCoefficients(float roughness, float cosTheta)
{
    const float2 uvMax = float2(31.0f / 32.0f, 31.0f / 32.0f);
    const float2 uvStep = float2(0.5f / 32.0f, 0.5f / 32.0f);
    const float2 uv = float2(saturate(cosTheta), saturate(roughness)) * uvMax + uvStep;
    Texture2D<float4> lut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::OpenPBR::FuzzLTC)];
    return lut.SampleLevel(g_linearClamp, uv, 0.0f).xyz;
}

float OpenPBRFuzzDirectionalReflectance(float fuzzRoughness, float cosTheta)
{
    return saturate(OpenPBRLookUpFuzzLTCCoefficients(fuzzRoughness, cosTheta).z);
}

float OpenPBRFuzzIncomingReflected(float fuzzWeight, float fuzzRoughness, float NdotV)
{
    const float presence = saturate(fuzzWeight);
    if (presence <= 0.0f)
    {
        return 0.0f;
    }
    return saturate(presence * OpenPBRFuzzDirectionalReflectance(fuzzRoughness, NdotV));
}

float OpenPBRFuzzBaseLayerScaleIncoming(float fuzzWeight, float fuzzRoughness, float NdotV)
{
    return 1.0f - OpenPBRFuzzIncomingReflected(fuzzWeight, fuzzRoughness, NdotV);
}

OpenPBRFuzzLayerState MakeOpenPBRFuzzLayerState(float3 normal, float3 viewDir, float3 fuzzColor, float fuzzWeight, float fuzzRoughness)
{
    OpenPBRFuzzLayerState state = (OpenPBRFuzzLayerState)0;
    state.roughness = saturate(fuzzRoughness);
    state.tint = saturate(fuzzColor);
    state.presence = saturate(fuzzWeight);
    state.basis.normal = normalize(normal);
    OpenPBRBuildViewAlignedBasis(state.basis.normal, normalize(viewDir), state.basis.tangent, state.basis.bitangent);
    state.viewDirLocal = OpenPBRWorldToLocal(state.basis, normalize(viewDir));
    state.viewReflected = OpenPBRFuzzIncomingReflected(state.presence, state.roughness, state.viewDirLocal.z);
    return state;
}

float OpenPBRFuzzProportionReflected(OpenPBRFuzzLayerState state, float3 directionLocal)
{
    if (directionLocal.z <= 0.0f)
    {
        return 0.0f;
    }

    return saturate(state.presence * OpenPBRFuzzDirectionalReflectance(state.roughness, directionLocal.z));
}

float OpenPBRFuzzBaseLayerScaleIncoming(OpenPBRFuzzLayerState state)
{
    return 1.0f - state.viewReflected;
}

float OpenPBRFuzzBaseLayerScaleOutgoing(OpenPBRFuzzLayerState state, float3 lightDirection)
{
    if (state.presence <= 0.0f)
    {
        return 1.0f;
    }
    return 1.0f - OpenPBRFuzzProportionReflected(state, OpenPBRWorldToLocal(state.basis, normalize(lightDirection)));
}

float OpenPBRFuzzBaseLayerScaleComplete(OpenPBRFuzzLayerState state, float3 lightDirection)
{
    if (state.presence <= 0.0f)
    {
        return 1.0f;
    }
    return OpenPBRFuzzBaseLayerScaleIncoming(state) * OpenPBRFuzzBaseLayerScaleOutgoing(state, lightDirection);
}

float OpenPBRFuzzPhi(float3 direction)
{
    float phi = atan2(direction.y, direction.x);
    if (phi < 0.0f)
    {
        phi += 2.0f * PI;
    }
    return phi;
}

float3 OpenPBRFuzzRotateVector(float3 direction, float3 axis, float angle)
{
    const float sinAngle = sin(angle);
    const float cosAngle = cos(angle);
    return direction * cosAngle + axis * dot(direction, axis) * (1.0f - cosAngle) + sinAngle * cross(axis, direction);
}

float OpenPBRFuzzEvaluateLTC(float3 wiLocal, float3 ltcCoefficients)
{
    const float aInv = ltcCoefficients.x;
    const float bInv = ltcCoefficients.y;
    float3 wiOriginalLocal = float3(aInv * wiLocal.x + bInv * wiLocal.z, aInv * wiLocal.y, wiLocal.z);
    const float len = length(wiOriginalLocal);
    if (len <= 0.0f)
    {
        return 0.0f;
    }

    wiOriginalLocal /= len;
    const float det = aInv * aInv;
    const float jacobian = det / max(len * len * len, 1.0e-6f);
    return saturate(wiOriginalLocal.z) * (1.0f / PI) * jacobian;
}

float3 OpenPBRFuzzSheenBRDF(OpenPBRFuzzLayerState state, float3 lightDirection)
{
    if (state.presence <= 0.0f)
    {
        return 0.0f.xxx;
    }
    const float3 lightDirLocal = OpenPBRWorldToLocal(state.basis, normalize(lightDirection));
    if (state.viewDirLocal.z <= 0.0f || lightDirLocal.z <= 0.0f)
    {
        return 0.0f.xxx;
    }

    const float phiStd = OpenPBRFuzzPhi(state.viewDirLocal);
    const float3 lightDirStdLocal = OpenPBRFuzzRotateVector(lightDirLocal, float3(0.0f, 0.0f, 1.0f), -phiStd);
    const float3 ltcCoefficients = OpenPBRLookUpFuzzLTCCoefficients(state.roughness, state.viewDirLocal.z);
    return state.presence * ltcCoefficients.z * state.tint * OpenPBRFuzzEvaluateLTC(lightDirStdLocal, ltcCoefficients);
}

float3 OpenPBRFuzzSheenIBL(float3 fuzzColor, float fuzzWeight, float fuzzRoughness, float NdotV, float3 reflectedRadiance)
{
    if (fuzzWeight <= 0.0f)
    {
        return 0.0f.xxx;
    }
    const float viewReflected = OpenPBRFuzzIncomingReflected(fuzzWeight, fuzzRoughness, NdotV);
    return viewReflected * saturate(fuzzColor) * reflectedRadiance;
}

float OpenPBRMax3(float3 value)
{
    return max(value.x, max(value.y, value.z));
}

float OpenPBRAverage3(float3 value)
{
    return dot(value, (1.0f / 3.0f).xxx);
}

float OpenPBRIorToF0Local(float ior)
{
    const float safeIor = max(ior, 1.0f);
    const float f = (safeIor - 1.0f) / (safeIor + 1.0f);
    return f * f;
}

float OpenPBRAverageFresnel(float etaTOverEtaI)
{
    const float safeEta = max(etaTOverEtaI, 1.0e-4f);
    if (safeEta > 1.0f)
    {
        return (safeEta - 1.0f) / (4.08567f + 1.00071f * safeEta);
    }

    const float safeEtaSquared = safeEta * safeEta;
    return 0.997118f + 0.1014f * safeEta - 0.965241f * safeEtaSquared - 0.130607f * safeEtaSquared * safeEta;
}

struct OpenPBRBaseLayerState
{
    float3 weightedBaseColor;
    float3 diffuseColor;
    float baseDiffuseRoughness;
    float specularAlpha;
    float weightedSpecularIor;
    float3 dielectricSpecularF0;
    float dielectricSpecularWeight;
    float3 metalSpecularF0;
    float3 metalAverageFresnel;
    float3 metalMultipleScatterScale;
    float metalSpecularWeight;
};

struct OpenPBRCoatLayerState
{
    float3 tint;
    float presence;
    float ior;
    float roughness;
    float3 extraBaseLayerScale;
};

struct OpenPBRBaseLayerEvaluation
{
    float3 diffuse;
    float3 specular;
};

static const float OPENPBR_ENERGY_TABLE_SIZE = 32.0f;
static const float OPENPBR_ENERGY_TABLE_SIZE_MINUS_ONE = OPENPBR_ENERGY_TABLE_SIZE - 1.0f;
static const float OPENPBR_IOR_MAX = 2.5f;
static const float OPENPBR_INVERSE_IOR_MAX = 1.0f / OPENPBR_IOR_MAX;
static const float OPENPBR_LTC_TABLE_SIZE = 32.0f;
static const float OPENPBR_FON_CONSTANT_A = 0.5f - 2.0f / (3.0f * PI);
static const float OPENPBR_FON_CONSTANT_B = 2.0f / 3.0f - 28.0f / (15.0f * PI);

float OpenPBRIorToExactIndex(float ior)
{
    const float safeIor = max(ior, 1.0e-4f);
    const float halfTableSize = 0.5f * OPENPBR_ENERGY_TABLE_SIZE;
    const float halfTableSizeMinusOne = halfTableSize - 1.0f;
    const float inverseIorMaxMinusOne = 1.0f / (OPENPBR_IOR_MAX - 1.0f);

    if (safeIor < 1.0f)
    {
        const float inverseIor = 1.0f / safeIor;
        const float fraction = (inverseIor - 1.0f) * inverseIorMaxMinusOne;
        return halfTableSizeMinusOne - fraction * halfTableSizeMinusOne;
    }

    const float fraction = (safeIor - 1.0f) * inverseIorMaxMinusOne;
    return halfTableSize + fraction * halfTableSizeMinusOne;
}

float OpenPBRAlphaToExactIndex(float alpha)
{
    return sqrt(saturate(alpha)) * OPENPBR_ENERGY_TABLE_SIZE_MINUS_ONE;
}

float OpenPBRCosThetaToExactIndex(float cosTheta)
{
    return saturate(cosTheta) * OPENPBR_ENERGY_TABLE_SIZE_MINUS_ONE;
}

float OpenPBRClampExactIndex(float exactIndex)
{
    return clamp(exactIndex, 0.0f, OPENPBR_ENERGY_TABLE_SIZE_MINUS_ONE);
}

float OpenPBRRemapExactIndex(float exactIndex)
{
    const float inverseTableSize = 1.0f / OPENPBR_ENERGY_TABLE_SIZE;
    const float minCoordinate = 0.5f * inverseTableSize;
    const float maxCoordinate = 1.0f - minCoordinate;
    return clamp(minCoordinate + exactIndex * inverseTableSize, minCoordinate, maxCoordinate);
}

float OpenPBRExtrapolateTableValueBeyondIorMaxIfNeeded(float tableValue, float ior)
{
    if (ior > OPENPBR_IOR_MAX || ior < OPENPBR_INVERSE_IOR_MAX)
    {
        const float f0Max = OpenPBRIorToF0Local(OPENPBR_IOR_MAX);
        const float inverseF0ExtrapolationRange = 1.0f / (1.0f - f0Max);
        const float f0 = OpenPBRIorToF0Local(max(ior, 1.0e-4f));
        const float progressTowardZero = (f0 - f0Max) * inverseF0ExtrapolationRange;
        return (1.0f - progressTowardZero) * tableValue;
    }

    return tableValue;
}

float OpenPBRFresnelDielectric(float etaTOverEtaI, float cosThetaI)
{
    const float safeCosThetaI = saturate(cosThetaI);
    if (abs(etaTOverEtaI - 1.0f) <= 1.0e-6f)
    {
        return 0.0f;
    }

    const float sinThetaISquared = max(0.0f, 1.0f - safeCosThetaI * safeCosThetaI);
    const float sinThetaTSquared = sinThetaISquared / max(etaTOverEtaI * etaTOverEtaI, 1.0e-6f);
    if (sinThetaTSquared >= 1.0f)
    {
        return 1.0f;
    }

    const float cosThetaT = sqrt(max(0.0f, 1.0f - sinThetaTSquared));
    const float etaCosThetaI = etaTOverEtaI * safeCosThetaI;
    const float etaCosThetaT = etaTOverEtaI * cosThetaT;
    const float rs = (safeCosThetaI - etaCosThetaT) / max(safeCosThetaI + etaCosThetaT, 1.0e-6f);
    const float rp = (cosThetaT - etaCosThetaI) / max(cosThetaT + etaCosThetaI, 1.0e-6f);
    return 0.5f * (rs * rs + rp * rp);
}

float OpenPBRClampAverageEnergyComplementAboveZero(float averageEnergyComplement)
{
    return max(averageEnergyComplement, 1.0e-12f);
}

float OpenPBRLookUpOpaqueDielectricAverageEnergyComplement(float ior, float alpha)
{
    const float exactIndexIor = OpenPBRClampExactIndex(OpenPBRIorToExactIndex(ior));
    const float exactIndexAlpha = OpenPBRClampExactIndex(OpenPBRAlphaToExactIndex(alpha));
    const float2 uv = float2(OpenPBRRemapExactIndex(exactIndexAlpha), OpenPBRRemapExactIndex(exactIndexIor));
    Texture2D<float> lut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement)];
    const float tableValue = lut.SampleLevel(g_linearClamp, uv, 0.0f);
    return OpenPBRExtrapolateTableValueBeyondIorMaxIfNeeded(tableValue, ior);
}

float OpenPBRLookUpOpaqueDielectricEnergyComplement(float ior, float alpha, float cosTheta)
{
    const float exactIndexIor = OpenPBRClampExactIndex(OpenPBRIorToExactIndex(ior));
    const float exactIndexAlpha = OpenPBRClampExactIndex(OpenPBRAlphaToExactIndex(alpha));
    const float exactIndexCosTheta = OpenPBRClampExactIndex(OpenPBRCosThetaToExactIndex(cosTheta));
    const int slice0 = int(floor(exactIndexIor));
    const int slice1 = min(slice0 + 1, int(OPENPBR_ENERGY_TABLE_SIZE_MINUS_ONE));
    const float sliceT = exactIndexIor - float(slice0);
    const float2 uv = float2(OpenPBRRemapExactIndex(exactIndexCosTheta), OpenPBRRemapExactIndex(exactIndexAlpha));
    Texture2DArray<float> lut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::OpenPBR::OpaqueDielectricEnergyComplement)];
    const float sliceValue0 = lut.SampleLevel(g_linearClamp, float3(uv, float(slice0)), 0.0f);
    const float sliceValue1 = lut.SampleLevel(g_linearClamp, float3(uv, float(slice1)), 0.0f);
    const float tableValue = lerp(sliceValue0, sliceValue1, sliceT);
    return OpenPBRExtrapolateTableValueBeyondIorMaxIfNeeded(tableValue, ior);
}

float OpenPBRLookUpIdealMetalEnergyComplement(float alpha, float cosTheta)
{
    const float exactIndexAlpha = OpenPBRClampExactIndex(OpenPBRAlphaToExactIndex(alpha));
    const float exactIndexCosTheta = OpenPBRClampExactIndex(OpenPBRCosThetaToExactIndex(cosTheta));
    const float2 uv = float2(OpenPBRRemapExactIndex(exactIndexCosTheta), OpenPBRRemapExactIndex(exactIndexAlpha));
    Texture2D<float> lut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::OpenPBR::IdealMetalEnergyComplement)];
    return lut.SampleLevel(g_linearClamp, uv, 0.0f);
}

float OpenPBRLookUpIdealMetalAverageEnergyComplement(float alpha)
{
    const float exactIndexAlpha = OpenPBRClampExactIndex(OpenPBRAlphaToExactIndex(alpha));
    const float2 uv = float2(OpenPBRRemapExactIndex(exactIndexAlpha), 0.5f);
    Texture2D<float> lut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::OpenPBR::IdealMetalAverageEnergyComplement)];
    return lut.SampleLevel(g_linearClamp, uv, 0.0f);
}

float OpenPBRCachedOpaqueDielectricDiffuseCompensation(float NdotV, float specularAlpha, float weightedSpecularIor)
{
    const float viewComplement = OpenPBRLookUpOpaqueDielectricEnergyComplement(weightedSpecularIor, specularAlpha, saturate(NdotV));
    const float averageComplement = OpenPBRLookUpOpaqueDielectricAverageEnergyComplement(weightedSpecularIor, specularAlpha);
    return max(0.0f, viewComplement / OpenPBRClampAverageEnergyComplementAboveZero(averageComplement));
}

float OpenPBRDirectionalAlbedoFONApprox(float mu, float roughness)
{
    const float safeMu = saturate(mu);
    const float muComplement = 1.0f - safeMu;
    const float g1 = 0.0571085289f;
    const float g2 = 0.491881867f;
    const float g3 = -0.332181442f;
    const float g4 = 0.0714429953f;
    const float gOverPi = muComplement * (g1 + muComplement * (g2 + muComplement * (g3 + muComplement * g4)));
    return (1.0f + roughness * gOverPi) / (1.0f + OPENPBR_FON_CONSTANT_A * roughness);
}

float3 OpenPBRDiffuseEON(float3 diffuseAlbedo, float diffuseRoughness, float NdotV, float NdotL, float VdotL)
{
    const float muIn = saturate(NdotV);
    const float muOut = saturate(NdotL);
    const float s = VdotL - muIn * muOut;
    const float sOverT = s > 0.0f ? s / max(max(muIn, muOut), 1.0e-4f) : s;
    const float A = 1.0f / (1.0f + OPENPBR_FON_CONSTANT_A * diffuseRoughness);
    const float3 singleScatter = diffuseAlbedo * Fd_Lambert() * A * (1.0f + diffuseRoughness * sOverT);
    const float EOut = OpenPBRDirectionalAlbedoFONApprox(muOut, diffuseRoughness);
    const float EIn = OpenPBRDirectionalAlbedoFONApprox(muIn, diffuseRoughness);
    const float averageE = A * (1.0f + OPENPBR_FON_CONSTANT_B * diffuseRoughness);
    const float3 multiScatterAlbedo =
        (diffuseAlbedo * diffuseAlbedo) * averageE /
        max((1.0f.xxx - diffuseAlbedo * (1.0f - averageE)), 1.0e-4f.xxx);
    const float3 multiScatter =
        (multiScatterAlbedo * Fd_Lambert()) *
        (max(1.0e-4f, 1.0f - EOut) * max(1.0e-4f, 1.0f - EIn) / max(1.0e-4f, 1.0f - averageE)).xxx;
    return singleScatter + multiScatter;
}

float OpenPBRDiffuseSpecularEnergyCompensation(float NdotV, float NdotL, float specularAlpha, float weightedSpecularIor)
{
    const float cachedViewCompensation = OpenPBRCachedOpaqueDielectricDiffuseCompensation(NdotV, specularAlpha, weightedSpecularIor);
    const float lightComplement = OpenPBRLookUpOpaqueDielectricEnergyComplement(weightedSpecularIor, specularAlpha, saturate(NdotL));
    return max(0.0f, cachedViewCompensation * lightComplement);
}

float OpenPBRDiffuseSpecularIBLEnergyCompensation(float NdotV, float specularAlpha, float weightedSpecularIor)
{
    const float cachedViewCompensation = OpenPBRCachedOpaqueDielectricDiffuseCompensation(NdotV, specularAlpha, weightedSpecularIor);
    const float averageComplement = OpenPBRLookUpOpaqueDielectricAverageEnergyComplement(weightedSpecularIor, specularAlpha);
    return max(0.0f, cachedViewCompensation * averageComplement);
}

OpenPBRBaseLayerState MakeOpenPBRBaseLayerState(
    float3 weightedBaseColor,
    float3 diffuseColor,
    float baseDiffuseRoughness,
    float specularAlpha,
    float weightedSpecularIor,
    float3 dielectricSpecularF0,
    float dielectricSpecularWeight,
    float3 metalAverageFresnel,
    float3 metalSpecularF0,
    float metalSpecularWeight)
{
    OpenPBRBaseLayerState state = (OpenPBRBaseLayerState)0;
    state.weightedBaseColor = saturate(weightedBaseColor);
    state.diffuseColor = diffuseColor;
    state.baseDiffuseRoughness = saturate(baseDiffuseRoughness);
    state.specularAlpha = saturate(specularAlpha);
    state.weightedSpecularIor = max(weightedSpecularIor, 1.0f);
    state.dielectricSpecularF0 = saturate(dielectricSpecularF0);
    state.dielectricSpecularWeight = saturate(dielectricSpecularWeight);
    state.metalAverageFresnel = saturate(metalAverageFresnel);
    state.metalSpecularF0 = saturate(metalSpecularF0);
    state.metalSpecularWeight = saturate(metalSpecularWeight);
    state.metalMultipleScatterScale = state.metalSpecularWeight * state.metalAverageFresnel * state.metalAverageFresnel;
    return state;
}

OpenPBRBaseLayerEvaluation EvaluateOpenPBRBaseLayerDirect(OpenPBRBaseLayerState state, float3 normal, float3 viewDir, float3 lightDir)
{
    OpenPBRBaseLayerEvaluation evaluation = (OpenPBRBaseLayerEvaluation)0;
    const float NdotV = saturate(dot(normal, viewDir));
    const float NdotL = saturate(dot(normal, lightDir));
    const float3 halfwayDir = normalize(lightDir + viewDir);
    const float NdotH = saturate(dot(normal, halfwayDir));
    const float LdotH = saturate(dot(lightDir, halfwayDir));
    const float VdotL = dot(viewDir, lightDir);

    const float diffuseEnergyComp = OpenPBRDiffuseSpecularEnergyCompensation(NdotV, NdotL, state.specularAlpha, state.weightedSpecularIor);
    evaluation.diffuse = OpenPBRDiffuseEON(state.diffuseColor, state.baseDiffuseRoughness, NdotV, NdotL, VdotL) * diffuseEnergyComp;
    const float metalViewComplement = OpenPBRLookUpIdealMetalEnergyComplement(state.specularAlpha, NdotV);
    const float metalLightComplement = OpenPBRLookUpIdealMetalEnergyComplement(state.specularAlpha, NdotL);
    const float metalAverageComplement = OpenPBRLookUpIdealMetalAverageEnergyComplement(state.specularAlpha);
    const float metalTabulatedFactors =
        metalViewComplement * metalLightComplement / OpenPBRClampAverageEnergyComplementAboveZero(metalAverageComplement);
    const float metalMmsBrdfScale = min(metalTabulatedFactors, rcp(max(NdotL, 1.0e-4f))) * Fd_Lambert();

    const float3 dielectricSpecular =
        state.dielectricSpecularWeight *
        specularLobe(state.specularAlpha, state.dielectricSpecularF0, halfwayDir, NdotV, NdotL, NdotH, LdotH) *
        mx_ggx_energy_compensation(NdotV, state.specularAlpha, state.dielectricSpecularF0);
    const float3 metalSpecular =
        state.metalSpecularWeight *
        (specularLobe(state.specularAlpha, state.metalSpecularF0, halfwayDir, NdotV, NdotL, NdotH, LdotH) +
            state.metalMultipleScatterScale * metalMmsBrdfScale);

    evaluation.specular = dielectricSpecular + metalSpecular;
    return evaluation;
}

OpenPBRBaseLayerEvaluation EvaluateOpenPBRBaseLayerIBL(OpenPBRBaseLayerState state, float NdotV, float3 diffuseIrradiance, float3 specularRadiance)
{
    OpenPBRBaseLayerEvaluation evaluation = (OpenPBRBaseLayerEvaluation)0;
    const float diffuseEnergyComp = OpenPBRDiffuseSpecularIBLEnergyCompensation(NdotV, state.specularAlpha, state.weightedSpecularIor);
    const float diffuseDirectionalAlbedo = OpenPBRDirectionalAlbedoFONApprox(NdotV, state.baseDiffuseRoughness);
    const float3 dielectricE = mx_ggx_dir_albedo_analytic(NdotV, state.specularAlpha, state.dielectricSpecularF0, 1.0f.xxx);
    const float3 metalE = mx_ggx_dir_albedo_analytic(NdotV, state.specularAlpha, state.metalSpecularF0, 1.0f.xxx);
    const float metalMultipleScatter = OpenPBRLookUpIdealMetalEnergyComplement(state.specularAlpha, NdotV);

    evaluation.diffuse = state.diffuseColor * diffuseIrradiance * diffuseDirectionalAlbedo * diffuseEnergyComp;
    evaluation.specular =
        (state.dielectricSpecularWeight * dielectricE + state.metalSpecularWeight * metalE + state.metalMultipleScatterScale * metalMultipleScatter) * specularRadiance;
    return evaluation;
}

float3 EvaluateOpenPBRBaseSpecularFromSample(OpenPBRBaseLayerState state, float3 specularSample, float NdotV)
{
    const float3 dielectricE = mx_ggx_dir_albedo_analytic(NdotV, state.specularAlpha, state.dielectricSpecularF0, 1.0f.xxx);
    const float3 metalE = mx_ggx_dir_albedo_analytic(NdotV, state.specularAlpha, state.metalSpecularF0, 1.0f.xxx);
    const float metalMultipleScatter = OpenPBRLookUpIdealMetalEnergyComplement(state.specularAlpha, NdotV);
    return (state.dielectricSpecularWeight * dielectricE + state.metalSpecularWeight * metalE + state.metalMultipleScatterScale * metalMultipleScatter) * specularSample;
}

float3 OpenPBREstimateOpaqueBaseAlbedo(OpenPBRBaseLayerState state)
{
    const float dielectricSpecularity = OpenPBRAverageFresnel(state.weightedSpecularIor);
    const float3 baseAlbedoFromMetal = state.metalSpecularWeight * state.metalAverageFresnel;
    const float3 baseAlbedoFromOpaqueDielectric = state.dielectricSpecularWeight * lerp(state.weightedBaseColor, 1.0f.xxx, dielectricSpecularity);
    return saturate(baseAlbedoFromMetal + baseAlbedoFromOpaqueDielectric);
}

float3 OpenPBRComputeCoatExtraBaseLayerScale(OpenPBRBaseLayerState baseState, float coatWeight, float coatIor, float coatDarkening)
{
    const float safeCoatIor = max(coatIor, 1.0f);
    const float K_s = OpenPBRAverageFresnel(safeCoatIor);
    const float K_r = 1.0f - (1.0f - K_s) / max(safeCoatIor * safeCoatIor, 1.0e-4f);
    const float dielectricSpecularity = OpenPBRAverageFresnel(baseState.weightedSpecularIor);
    const float specularityBase = saturate(baseState.dielectricSpecularWeight * dielectricSpecularity + (1.0f - baseState.dielectricSpecularWeight));
    const float effectiveRoughnessBase = lerp(1.0f, sqrt(saturate(baseState.specularAlpha)), specularityBase);
    const float K = lerp(K_s, K_r, effectiveRoughnessBase);
    const float3 E_b = OpenPBREstimateOpaqueBaseAlbedo(baseState);
    const float3 Delta = (1.0f - K).xxx / max(1.0f.xxx - E_b * K, 1.0e-4f.xxx);
    const float modulatedCoatDarkening = saturate(coatWeight) * saturate(coatDarkening);
    return lerp(1.0f.xxx, saturate(Delta), modulatedCoatDarkening);
}

OpenPBRCoatLayerState MakeOpenPBRCoatLayerState(
    OpenPBRBaseLayerState baseState,
    float3 coatColor,
    float coatWeight,
    float coatIor,
    float coatRoughness,
    float coatDarkening)
{
    OpenPBRCoatLayerState state = (OpenPBRCoatLayerState)0;
    state.tint = saturate(coatColor);
    state.presence = saturate(coatWeight);
    state.ior = max(coatIor, 1.0f);
    state.roughness = saturate(coatRoughness);
    state.extraBaseLayerScale = OpenPBRComputeCoatExtraBaseLayerScale(baseState, state.presence, state.ior, coatDarkening);
    return state;
}

float3 OpenPBRCoatPassageColorMultiplier(OpenPBRCoatLayerState state, float NdotX)
{
    const float safeNdotX = saturate(NdotX);
    if (safeNdotX <= 0.0f || min(state.tint.x, min(state.tint.y, state.tint.z)) >= 1.0f)
    {
        return 1.0f.xxx;
    }

    const float3 coatTransmissionAtNormalIncidence = sqrt(state.tint);
    const float etaTOverEtaI = rcp(state.ior);
    const float refractedCosine = sqrt(max(0.0f, 1.0f - (1.0f - safeNdotX * safeNdotX) / max(etaTOverEtaI * etaTOverEtaI, 1.0e-4f)));
    const float distanceScale = rcp(max(refractedCosine, 1.0e-4f));
    const float3 coatTransmissionAlongRefractedRay = pow(coatTransmissionAtNormalIncidence, distanceScale.xxx);
    return lerp(1.0f.xxx, coatTransmissionAlongRefractedRay, state.presence);
}

float OpenPBRComputeDielectricEnergyReflected(float ior, float alpha, float cosTheta)
{
    const float safeIor = max(ior, 1.0e-4f);
    const float safeAlpha = saturate(alpha);
    const float safeCosTheta = saturate(cosTheta);
    if (safeAlpha <= 0.0f)
    {
        return OpenPBRFresnelDielectric(safeIor, safeCosTheta);
    }

    return 1.0f - OpenPBRLookUpOpaqueDielectricEnergyComplement(safeIor, safeAlpha, safeCosTheta);
}

float OpenPBRCoatReflectedProportion(OpenPBRCoatLayerState state, float NdotX)
{
    return saturate(state.presence * OpenPBRComputeDielectricEnergyReflected(state.ior, state.roughness, NdotX));
}

float3 OpenPBRCoatBaseLayerScaleIncoming(OpenPBRCoatLayerState state, float NdotV)
{
    const float reflectedProportion = OpenPBRCoatReflectedProportion(state, NdotV);
    return OpenPBRCoatPassageColorMultiplier(state, NdotV) * (1.0f - reflectedProportion).xxx * state.extraBaseLayerScale;
}

float3 OpenPBRCoatBaseLayerScaleOutgoing(OpenPBRCoatLayerState state, float NdotL)
{
    const float reflectedProportion = OpenPBRCoatReflectedProportion(state, NdotL);
    return OpenPBRCoatPassageColorMultiplier(state, NdotL) * (1.0f - reflectedProportion).xxx;
}

float3 OpenPBRCoatBaseLayerScaleComplete(OpenPBRCoatLayerState state, float NdotV, float NdotL)
{
    return OpenPBRCoatBaseLayerScaleIncoming(state, NdotV) * OpenPBRCoatBaseLayerScaleOutgoing(state, NdotL);
}

float3 EvaluateOpenPBREmissive(float3 emissive, OpenPBRCoatLayerState coatState, float fuzzWeight, float fuzzRoughness, float NdotV)
{
    const float fuzzBaseScale = OpenPBRFuzzBaseLayerScaleIncoming(fuzzWeight, fuzzRoughness, NdotV);
    const float3 coatTransmittance = OpenPBRCoatBaseLayerScaleIncoming(coatState, NdotV);
    return emissive * fuzzBaseScale.xxx * coatTransmittance;
}

void combineDiffuseAndSpecular(const float3 n, const float3 E, const float3 Fd, const float3 Fr, inout float3 color)
{
#if defined(HAS_REFRACTION) // TODO: Refraction
    applyRefraction(materialSurface, n, E, Fd, Fr, color);
#else
    color += Fd + Fr;
#endif
}

void evaluateIBL(inout float3 color, inout float3 debugDiffuse, inout float3 debugSpecular, float3 normal, float3 bentNormal, float3 weightedBaseColor, float3 diffuseColor, float diffuseAO, float baseDiffuseRoughness, float3 dielectricSpecularF0, float dielectricSpecularWeight, float weightedSpecularIor, float3 metalAverageFresnel, float3 metalSpecularF0, float metalSpecularWeight, float3 reflection, float roughness, float perceptualRoughness, float specularAlpha, float3 coatColor, float3 coatF0, float coatWeight, float coatIor, float coatDarkening, float coatRoughness, float coatPerceptualRoughness, float3 fuzzColor, float fuzzWeight, float fuzzRoughness, float NdotV, in const uint environmentIndex, in const uint environmentBufferDescriptorIndex)
{
    float3 r = getReflectedVector(reflection, normal, roughness);
    const OpenPBRBaseLayerState baseState = MakeOpenPBRBaseLayerState(
        weightedBaseColor,
        diffuseColor,
        baseDiffuseRoughness,
        specularAlpha,
        weightedSpecularIor,
        dielectricSpecularF0,
        dielectricSpecularWeight,
        metalAverageFresnel,
        metalSpecularF0,
        metalSpecularWeight);
    const OpenPBRCoatLayerState coatState = MakeOpenPBRCoatLayerState(baseState, coatColor, coatWeight, coatIor, coatRoughness, coatDarkening);
    
    StructuredBuffer<EnvironmentInfo> environments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Environment::InfoBuffer)];
    float3 specularRadiance = 0.0f.xxx;
    float3 fuzzRadiance = 0.0f.xxx;
#if defined (PSO_SPECULAR_IBL)
    specularRadiance = prefilteredRadiance(r, perceptualRoughness, environments[environmentIndex].prefilteredCubemapDescriptorIndex);
    fuzzRadiance = prefilteredRadiance(r, fuzzRoughness, environments[environmentIndex].prefilteredCubemapDescriptorIndex);
#endif
    float3 diffuseIrradiance = max(irradianceSH(normalize(normal + bentNormal), environmentIndex, environmentBufferDescriptorIndex), 0.0) * Fd_Lambert();
    OpenPBRBaseLayerEvaluation baseEvaluation = EvaluateOpenPBRBaseLayerIBL(baseState, NdotV, diffuseIrradiance, specularRadiance);
    float3 Fd = baseEvaluation.diffuse * diffuseAO;
    float3 Fr = baseEvaluation.specular;
    const float fuzzIncomingScale = OpenPBRFuzzBaseLayerScaleIncoming(fuzzWeight, fuzzRoughness, NdotV);
    const float3 coatViewAttenuation = OpenPBRCoatBaseLayerScaleIncoming(coatState, NdotV);
    float3 fuzzFr = OpenPBRFuzzSheenIBL(fuzzColor, fuzzWeight, fuzzRoughness, NdotV, fuzzRadiance);

    float3 coatFr = 0.0f.xxx;
#if defined (PSO_SPECULAR_IBL)
    if (coatState.presence > 0.0f)
    {
        const float coatReflected = OpenPBRCoatReflectedProportion(coatState, NdotV);
        coatFr = coatReflected.xxx * prefilteredRadiance(r, coatPerceptualRoughness, environments[environmentIndex].prefilteredCubemapDescriptorIndex);
    }
#endif

    const float3 baseAttenuation = fuzzIncomingScale.xxx * coatViewAttenuation;
    const float3 coatAttenuation = fuzzIncomingScale.xxx;
    Fd *= baseAttenuation;
    Fr *= baseAttenuation;
    coatFr *= coatAttenuation;
    
    multiBounceAO(diffuseAO, diffuseColor, Fd);
    
#if defined (PSO_SPECULAR_IBL)
    float specularAO = computeSpecularAO(NdotV, diffuseAO, roughness);
    multiBounceSpecularAO(specularAO, dielectricSpecularF0, Fr);
    multiBounceSpecularAO(specularAO, coatF0, coatFr);
    multiBounceSpecularAO(specularAO, fuzzColor, fuzzFr);
#endif
    combineDiffuseAndSpecular(normal, 0.0f.xxx, Fd, Fr + coatFr + fuzzFr, color);
    debugDiffuse = Fd;
    debugSpecular = Fr + coatFr + fuzzFr;
}

float3 evaluateSpecularIBL(float3 normal, float3 bentNormal, float diffuseAO, float3 F0, float3 reflection, float roughness, float perceptualRoughness, float NdotV, in const uint environmentIndex)
{
    float3 E = mx_ggx_dir_albedo_analytic(NdotV, roughness, F0, float3(1.0, 1.0, 1.0));
    float3 r = getReflectedVector(reflection, normal, roughness);
    StructuredBuffer<EnvironmentInfo> environments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::EnvironmentInfo::InfoBuffer)];
    float3 Fr = E * prefilteredRadiance(r, perceptualRoughness, environments[environmentIndex].prefilteredCubemapDescriptorIndex);
    return Fr;
} 

float3 evaluateSpecularIBLFromSSR(float3 specularSample, float3 normal, float3 bentNormal, float diffuseAO, float3 weightedBaseColor, float3 dielectricSpecularF0, float dielectricSpecularWeight, float weightedSpecularIor, float3 metalAverageFresnel, float3 metalSpecularF0, float metalSpecularWeight, float roughness, float perceptualRoughness, float specularAlpha, float3 coatColor, float3 coatF0, float coatWeight, float coatIor, float coatDarkening, float coatRoughness, float3 fuzzColor, float fuzzWeight, float fuzzRoughness, float NdotV)
{
    const OpenPBRBaseLayerState baseState = MakeOpenPBRBaseLayerState(
        weightedBaseColor,
        0.0f.xxx,
        0.0f,
        specularAlpha,
        weightedSpecularIor,
        dielectricSpecularF0,
        dielectricSpecularWeight,
        metalAverageFresnel,
        metalSpecularF0,
        metalSpecularWeight);
    const OpenPBRCoatLayerState coatState = MakeOpenPBRCoatLayerState(baseState, coatColor, coatWeight, coatIor, coatRoughness, coatDarkening);
    float3 Fr = EvaluateOpenPBRBaseSpecularFromSample(baseState, specularSample, NdotV);
    const float fuzzIncomingScale = OpenPBRFuzzBaseLayerScaleIncoming(fuzzWeight, fuzzRoughness, NdotV);
    const float3 baseAttenuation = fuzzIncomingScale.xxx * OpenPBRCoatBaseLayerScaleIncoming(coatState, NdotV);
    float3 coatFr = 0.0f.xxx;
    float3 fuzzFr = OpenPBRFuzzSheenIBL(fuzzColor, fuzzWeight, fuzzRoughness, NdotV, specularSample);
    if (coatState.presence > 0.0f)
    {
        const float coatReflected = OpenPBRCoatReflectedProportion(coatState, NdotV);
        coatFr = coatReflected.xxx * specularSample;
    }
    Fr *= baseAttenuation;
    coatFr *= fuzzIncomingScale.xxx;
    float specularAO = computeSpecularAO(NdotV, diffuseAO, roughness);
    multiBounceSpecularAO(specularAO, dielectricSpecularF0, Fr);
    multiBounceSpecularAO(specularAO, coatF0, coatFr);
    multiBounceSpecularAO(specularAO, fuzzColor, fuzzFr);
    return Fr + coatFr + fuzzFr;
}

#endif // __IBL_HLSLI__
