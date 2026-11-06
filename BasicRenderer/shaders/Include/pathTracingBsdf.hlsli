#ifndef __PATH_TRACING_BSDF_HLSLI__
#define __PATH_TRACING_BSDF_HLSLI__

#include "include/lighting.hlsli"
#include "include/pathTracingRandom.hlsli"

// Importance sampling around the OpenPBR evaluation raster uses, so both shade alike. That
// evaluation approximates the layering with lookup tables and carries raster's terms unchanged;
// the sampler only has to be consistent with its own PDF, and nothing here makes it energy
// conserving. Glint is excluded: it is a hash-based hack with no meaningful PDF.
//
// A sample is one of four lobes:
//   diffuse:  cosine about the shading normal. Also covers the metal multiple-scatter term, which
//             is nearly uniform and which a low-roughness VNDF lobe would sample very poorly.
//   specular: GGX VNDF with the base specular alpha, dielectric and metal alike.
//   coat:     GGX VNDF with the coat alpha.
//   fuzz:     the sheen LTC, sampled exactly.
//
// Lobes are selected once from V, and the PDF sums every lobe's density, which keeps the
// BRDF / PDF ratio bounded when the selected lobe is not the one the BRDF value is dominated by.
//
// A direction under the shading or geometric horizon is not resampled. It is a sample with zero
// contribution, because the evaluation is zero there, so rejecting it stays unbiased. That is why
// the PDF is zero below the horizon and integrates to at most 1 over directions, rather than to 1.

// Floor on the normalized weight of any lobe that has one, before the final renormalization. A faint lobe can still be
// a sharp one (a weak coat over a rough base), and rarely choosing it is what makes fireflies.
#define PATH_BSDF_MIN_LOBE_PROBABILITY 0.05f

struct PathBsdfContext
{
    LightingParameters lighting;
    OpenPBRBaseLayerState baseState;
    float3 tangent;
    float3 bitangent;
    float3 normal;
    float3 geometricNormal; // Faces V.
    float3 viewDir;
    float3 viewLocal; // In the tangent, bitangent, normal basis.
    float normDotView;
    float specularAlpha;
    float coatAlpha;
    float3 fuzzLtc;
    float fuzzPhi;
    float4 lobeProbabilities; // Diffuse, specular, coat, fuzz. All zero when nothing reflects.
};

PathBsdfContext PathBsdfMakeContext(FragmentInfo fragmentInfo, float3 geometricNormal, float3 viewDir)
{
    PathBsdfContext context = (PathBsdfContext)0;
    context.lighting = MakeLightingParameters(fragmentInfo);
    context.lighting.viewDir = viewDir;
    context.baseState = MakeOpenPBRBaseLayerState(
        context.lighting.weightedBaseColor,
        context.lighting.diffuseColor,
        context.lighting.baseDiffuseRoughness,
        context.lighting.specularAlpha,
        context.lighting.weightedSpecularIor,
        context.lighting.dielectricSpecularF0,
        context.lighting.dielectricSpecularWeight,
        context.lighting.metalAverageFresnel,
        context.lighting.metalSpecularF0,
        context.lighting.metalSpecularWeight);

    context.normal = context.lighting.normal;
    context.viewDir = viewDir;
    context.geometricNormal = dot(geometricNormal, viewDir) < 0.0f ? -geometricNormal : geometricNormal;
    context.normDotView = saturate(dot(context.normal, viewDir));
    context.specularAlpha = context.baseState.specularAlpha;
    if (context.normDotView <= 0.0f)
    {
        return context;
    }

    OpenPBRBuildViewAlignedBasis(context.normal, viewDir, context.tangent, context.bitangent);
    context.viewLocal = float3(dot(viewDir, context.tangent), dot(viewDir, context.bitangent), dot(viewDir, context.normal));

    const OpenPBRCoatLayerState coatState = MakeOpenPBRCoatLayerState(
        context.baseState,
        context.lighting.coatColor,
        context.lighting.coatWeight,
        context.lighting.coatIor,
        context.lighting.coatRoughness,
        context.lighting.coatDarkening);
    const OpenPBRFuzzLayerState fuzzState = MakeOpenPBRFuzzLayerState(
        context.normal,
        viewDir,
        context.lighting.fuzzColor,
        context.lighting.fuzzWeight,
        context.lighting.fuzzRoughness);
    context.coatAlpha = coatState.roughness;

    // Each weight estimates what the lobe reflects toward V, using the same layering the
    // evaluation and the raster IBL path use.
    float fuzzWeight = 0.0f;
    if (fuzzState.presence > 0.0f)
    {
        context.fuzzLtc = OpenPBRLookUpFuzzLTCCoefficients(fuzzState.roughness, fuzzState.viewDirLocal.z);
        context.fuzzPhi = OpenPBRFuzzPhi(fuzzState.viewDirLocal);
        // The table holds zeros at its corners. A zero scale has zero BRDF, and could not be inverted to sample.
        fuzzWeight = context.fuzzLtc.x > 1.0e-4f ? fuzzState.viewReflected * OpenPBRAverage3(fuzzState.tint) : 0.0f;
    }
    const float fuzzTransmission = 1.0f - fuzzState.viewReflected;
    const float coatWeight = fuzzTransmission * OpenPBRCoatReflectedProportion(coatState, context.normDotView);
    const float3 baseTransmission = fuzzTransmission * OpenPBRCoatBaseLayerScaleIncoming(coatState, context.normDotView);

    const OpenPBRBaseLayerState baseState = context.baseState;
    const float3 singleScatter =
        baseState.dielectricSpecularWeight * mx_ggx_dir_albedo_analytic(context.normDotView, baseState.specularAlpha, baseState.dielectricSpecularF0, 1.0f.xxx) +
        baseState.metalSpecularWeight * mx_ggx_dir_albedo_analytic(context.normDotView, baseState.specularAlpha, baseState.metalSpecularF0, 1.0f.xxx);
    const float3 multiScatter =
        baseState.metalMultipleScatterScale * OpenPBRLookUpIdealMetalEnergyComplement(baseState.specularAlpha, context.normDotView);
    const float3 diffuseReflected =
        baseState.diffuseColor *
        OpenPBRDirectionalAlbedoFONApprox(context.normDotView, baseState.baseDiffuseRoughness) *
        OpenPBRDiffuseSpecularIBLEnergyCompensation(context.normDotView, baseState.specularAlpha, baseState.weightedSpecularIor);

    float4 weights = float4(
        OpenPBRAverage3(baseTransmission * (diffuseReflected + multiScatter)),
        OpenPBRAverage3(baseTransmission * singleScatter),
        coatWeight,
        fuzzWeight);
    const float total = weights.x + weights.y + weights.z + weights.w;
    if (!(total > 0.0f))
    {
        return context;
    }
    weights /= total;
    weights = select(weights > 0.0f, max(weights, PATH_BSDF_MIN_LOBE_PROBABILITY), 0.0f);
    context.lobeProbabilities = weights / (weights.x + weights.y + weights.z + weights.w);
    return context;
}

float3 PathBsdfLocalToWorld(PathBsdfContext context, float3 local)
{
    return local.x * context.tangent + local.y * context.bitangent + local.z * context.normal;
}

float3 PathSampleCosineHemisphere(float2 u)
{
    const float radius = sqrt(u.x);
    const float phi = 2.0f * PI * u.y;
    return float3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0f, 1.0f - u.x)));
}

// Samples the distribution of visible normals. Heitz 2018, "Sampling the GGX Distribution of
// Visible Normals", in the spherical cap form of Dupuy and Benyoub 2023, which draws the same
// distribution with no special case at normal incidence.
float3 PathSampleGgxVndf(float3 viewLocal, float alpha, float2 u)
{
    const float3 stretchedView = normalize(float3(alpha * viewLocal.x, alpha * viewLocal.y, viewLocal.z));
    const float phi = 2.0f * PI * u.x;
    const float z = (1.0f - u.y) * (1.0f + stretchedView.z) - stretchedView.z;
    const float sinTheta = sqrt(saturate(1.0f - z * z));
    const float3 stretchedHalf = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + stretchedView;
    return normalize(float3(alpha * stretchedHalf.x, alpha * stretchedHalf.y, max(stretchedHalf.z, 0.0f)));
}

// Density of the reflected direction, D * G1(V) / (4 * NdotV), with G1 expanded so NdotV cancels.
// Uses the D the evaluation uses, so that its precision limits cancel in the BRDF / PDF ratio.
float PathGgxVndfPdf(float alpha, float normDotView, float normDotHalf, float3 halfDir)
{
    const float alphaSquared = alpha * alpha;
    const float denominator = 2.0f * (normDotView + sqrt(alphaSquared + (1.0f - alphaSquared) * normDotView * normDotView));
    return distribution(alpha, normDotHalf, halfDir) / denominator;
}

// Returns f(V, L) * NdotL, zero under either horizon. Excludes glint.
float3 PathBsdfEval(PathBsdfContext context, float3 lightDir)
{
    const float normDotLight = saturate(dot(context.normal, lightDir));
    if (normDotLight <= 0.0f || context.normDotView <= 0.0f || dot(context.geometricNormal, lightDir) <= 0.0f)
    {
        return 0.0f.xxx;
    }
    float3 diffuse;
    float3 specular;
    return EvaluateOpenPBRSurfaceBrdf(context.lighting, context.baseState, lightDir, context.normDotView, normDotLight, diffuse, specular) * normDotLight;
}

// Solid-angle density of PathBsdfSample returning lightDir.
float PathBsdfPdf(PathBsdfContext context, float3 lightDir)
{
    const float normDotLight = dot(context.normal, lightDir);
    if (normDotLight <= 0.0f || context.normDotView <= 0.0f || dot(context.geometricNormal, lightDir) <= 0.0f)
    {
        return 0.0f;
    }

    float pdf = context.lobeProbabilities.x * normDotLight / PI;
    if (context.lobeProbabilities.y > 0.0f || context.lobeProbabilities.z > 0.0f)
    {
        const float3 halfDir = normalize(context.viewDir + lightDir);
        const float normDotHalf = saturate(dot(context.normal, halfDir));
        if (context.lobeProbabilities.y > 0.0f)
        {
            pdf += context.lobeProbabilities.y * PathGgxVndfPdf(context.specularAlpha, context.normDotView, normDotHalf, halfDir);
        }
        if (context.lobeProbabilities.z > 0.0f)
        {
            pdf += context.lobeProbabilities.z * PathGgxVndfPdf(context.coatAlpha, context.normDotView, normDotHalf, halfDir);
        }
    }
    if (context.lobeProbabilities.w > 0.0f)
    {
        const float3 lightLocal = float3(dot(lightDir, context.tangent), dot(lightDir, context.bitangent), normDotLight);
        const float3 lightStandard = OpenPBRFuzzRotateVector(lightLocal, float3(0.0f, 0.0f, 1.0f), -context.fuzzPhi);
        pdf += context.lobeProbabilities.w * OpenPBRFuzzEvaluateLTC(lightStandard, context.fuzzLtc);
    }
    return pdf;
}

// Returns false when the path should end: nothing reflects, or the sample fell under a horizon,
// which counts as a zero-contribution sample. On true, weightOverPdf is f * NdotL / pdf, the
// factor to multiply throughput by.
bool PathBsdfSample(PathBsdfContext context, inout PathRng rng, out float3 lightDir, out float pdf, out float3 weightOverPdf)
{
    lightDir = 0.0f.xxx;
    pdf = 0.0f;
    weightOverPdf = 0.0f.xxx;
    if (all(context.lobeProbabilities == 0.0f))
    {
        return false;
    }

    const float lobe = PathRngNext1(rng);
    const float2 u = PathRngNext2(rng);
    const float4 probabilities = context.lobeProbabilities;
    if (lobe < probabilities.x)
    {
        lightDir = PathBsdfLocalToWorld(context, PathSampleCosineHemisphere(u));
    }
    else if (lobe < probabilities.x + probabilities.y + probabilities.z || probabilities.w <= 0.0f)
    {
        const float alpha = lobe < probabilities.x + probabilities.y ? context.specularAlpha : context.coatAlpha;
        const float3 halfDir = PathBsdfLocalToWorld(context, PathSampleGgxVndf(context.viewLocal, alpha, u));
        lightDir = reflect(-context.viewDir, halfDir);
    }
    else
    {
        // Inverse of the transform in OpenPBRFuzzEvaluateLTC. It leaves z alone, so the mapped
        // direction stays in the upper hemisphere and the LTC density integrates to 1 there.
        const float3 original = PathSampleCosineHemisphere(u);
        const float scale = 1.0f / context.fuzzLtc.x;
        const float3 standard = normalize(float3((original.x - context.fuzzLtc.y * original.z) * scale, original.y * scale, original.z));
        lightDir = PathBsdfLocalToWorld(context, OpenPBRFuzzRotateVector(standard, float3(0.0f, 0.0f, 1.0f), context.fuzzPhi));
    }

    pdf = PathBsdfPdf(context, lightDir);
    if (!(pdf > 0.0f))
    {
        return false;
    }
    weightOverPdf = PathBsdfEval(context, lightDir) / pdf;
    return all(isfinite(weightOverPdf));
}

// Forms taking a FragmentInfo. A hit that both evaluates and samples can build one
// PathBsdfContext and pass it instead, which saves repeating the lookup table reads.
float3 PathBsdfEval(FragmentInfo fragmentInfo, float3 geometricNormal, float3 viewDir, float3 lightDir)
{
    return PathBsdfEval(PathBsdfMakeContext(fragmentInfo, geometricNormal, viewDir), lightDir);
}

float PathBsdfPdf(FragmentInfo fragmentInfo, float3 geometricNormal, float3 viewDir, float3 lightDir)
{
    return PathBsdfPdf(PathBsdfMakeContext(fragmentInfo, geometricNormal, viewDir), lightDir);
}

bool PathBsdfSample(FragmentInfo fragmentInfo, float3 geometricNormal, float3 viewDir, inout PathRng rng, out float3 lightDir, out float pdf, out float3 weightOverPdf)
{
    return PathBsdfSample(PathBsdfMakeContext(fragmentInfo, geometricNormal, viewDir), rng, lightDir, pdf, weightOverPdf);
}

#endif // __PATH_TRACING_BSDF_HLSLI__
