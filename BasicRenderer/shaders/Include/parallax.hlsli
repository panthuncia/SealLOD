#ifndef __PARALLAX_HLSLI__
#define __PARALLAX_HLSLI__

// Finds where a view ray enters a height field. A bound is the ray's height, running from the top
// of the height volume down to 0, and the ray hits where the bound first falls below the surface.
//
// Search: while the ray is above the surface at its current column it may still be far from the
// surface, so it moves by half of that gap. Close to the surface the gap is small and the ray moves
// by a minimum step sized from its length on screen. The minimum step can skip features thinner than
// half a pixel of ray travel, and the gap-sized steps can skip narrow peaks well above the surface.
//
// Refinement: once a hit is bracketed, a secant estimate is sampled to tighten the bracket, and a
// final secant estimate needs no further sample. See Risser, Shah and Pattanaik, "Faster Relief
// Mapping Using the Secant Method" (2007). Step sizes are from measurements logged in
// docs/clod-optimization-experiments.md.
//
// Each path samples its own height source in a loop:
//
//     ParallaxMarch march = ParallaxMarchBegin(top, rayLengthPixels, maxSteps);
//     while (!march.finished) { ParallaxMarchAdvance(march, SampleHeightAt(march.bound)); }
//     const float bound = ParallaxMarchResult(march);

static const float PARALLAX_PIXELS_PER_MIN_STEP = 0.5f;
static const float PARALLAX_SURFACE_GAP_STEP = 0.5f;
static const uint PARALLAX_REFINE_SAMPLES = 1u;

float3 ParallaxViewDirectionTS(float3x3 TBN, float3 viewDir)
{
    float3 viewDirTS = normalize(mul(TBN, viewDir));
    float viewDenom = viewDirTS.z * 0.7f + 0.3f;
    viewDenom = viewDenom >= 0.0f ? max(viewDenom, 0.15f) : min(viewDenom, -0.15f);
    viewDirTS.xy /= viewDenom;
    return viewDirTS;
}

// Length on screen of a ray given in the same space as the derivatives.
float ParallaxRayLengthPixels(float2 ray, float2 ddxOfSpace, float2 ddyOfSpace)
{
    const float det = ddxOfSpace.x * ddyOfSpace.y - ddxOfSpace.y * ddyOfSpace.x;
    if (abs(det) < 1.0e-12f)
    {
        return length(ray) * rcp(sqrt(max(max(dot(ddxOfSpace, ddxOfSpace), dot(ddyOfSpace, ddyOfSpace)), 1.0e-12f)));
    }
    return length(float2(
        ray.x * ddyOfSpace.y - ray.y * ddyOfSpace.x,
        ddxOfSpace.x * ray.y - ddxOfSpace.y * ray.x)) * rcp(abs(det));
}

float ParallaxSecantBound(float boundA, float fA, float boundB, float fB)
{
    const float denominator = fB - fA;
    const float root = abs(denominator) > 1.0e-5f
        ? (boundA * fB - boundB * fA) / denominator
        : 0.5f * (boundA + boundB);
    return clamp(root, min(boundA, boundB), max(boundA, boundB));
}

struct ParallaxMarch
{
    float bound; // The next bound to sample.
    float minStep;
    float hitBound;
    float hitF;
    float missBound;
    float missF;
    uint refineSamplesLeft;
    bool bracketed;
    bool finished;
};

ParallaxMarch ParallaxMarchBegin(float top, float rayLengthPixels, uint maxSteps)
{
    const uint stepCount = clamp((uint)ceil(rayLengthPixels * rcp(PARALLAX_PIXELS_PER_MIN_STEP)), 1u, max(maxSteps, 1u));
    ParallaxMarch march;
    march.bound = top;
    march.minStep = top * rcp((float)stepCount);
    march.hitBound = top;
    march.hitF = 0.0f;
    march.missBound = top;
    march.missF = 0.0f;
    march.refineSamplesLeft = PARALLAX_REFINE_SAMPLES;
    march.bracketed = false;
    march.finished = false;
    return march;
}

void ParallaxMarchAdvance(inout ParallaxMarch march, float height)
{
    const float f = march.bound - height;
    if (!march.bracketed && f > 0.0f)
    {
        march.missBound = march.bound;
        march.missF = f;
        if (march.bound <= 0.0f)
        {
            // Never hit, so the result is the bottom of the volume.
            march.hitBound = 0.0f;
            march.hitF = 0.0f;
            march.finished = true;
            return;
        }
        march.bound = max(march.bound - max(march.minStep, PARALLAX_SURFACE_GAP_STEP * f), 0.0f);
        return;
    }

    if (march.bracketed)
    {
        march.refineSamplesLeft--;
    }
    else if (march.missF <= 0.0f)
    {
        // A hit on the first sample has no ray above the surface to refine against.
        march.refineSamplesLeft = 0u;
    }
    march.bracketed = true;

    if (f <= 0.0f)
    {
        march.hitBound = march.bound;
        march.hitF = f;
    }
    else
    {
        march.missBound = march.bound;
        march.missF = f;
    }

    march.finished = march.refineSamplesLeft == 0u;
    march.bound = ParallaxSecantBound(march.hitBound, march.hitF, march.missBound, march.missF);
}

float ParallaxMarchResult(ParallaxMarch march)
{
    return ParallaxSecantBound(march.hitBound, march.hitF, march.missBound, march.missF);
}

// Object materials: a bound of 1 is the top of the height map. Heights come from one channel.
float2 ParallaxOcclusionUv(
    Texture2D<float4> heightTexture,
    SamplerState heightSampler,
    uint heightChannel,
    float3x3 TBN,
    float2 uv,
    float3 viewDir,
    float heightmapScale,
    uint maxSteps,
    float2 dUVdx,
    float2 dUVdy)
{
    const float maxHeight = max(heightmapScale, 0.0f);
    if (maxHeight <= 1.0e-5f)
    {
        return uv;
    }

    // Object-space material UV parallax uses the inverse of the fragment-to-camera
    // tangent-space XY direction. Terrain has its own parallax path and basis.
    const float2 rayPerBound = -ParallaxViewDirectionTS(TBN, viewDir).xy * maxHeight;

    ParallaxMarch march = ParallaxMarchBegin(1.0f, ParallaxRayLengthPixels(rayPerBound, dUVdx, dUVdy), maxSteps);
    [loop]
    while (!march.finished)
    {
        const float2 sampleUv = uv + rayPerBound * (march.bound - 0.5f);
        ParallaxMarchAdvance(march, heightTexture.SampleGrad(heightSampler, sampleUv, dUVdx, dUVdy)[heightChannel]);
    }
    return uv + rayPerBound * (ParallaxMarchResult(march) - 0.5f);
}

#endif // __PARALLAX_HLSLI__
