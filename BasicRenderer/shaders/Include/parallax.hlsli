#ifndef __PARALLAX_HLSLI__
#define __PARALLAX_HLSLI__

struct parallaxShadowParameters {
    Texture2D<float> parallaxTexture;
    SamplerState parallaxSampler;
    float3x3 TBN;
    float heightmapScale;
    float3 lightToFrag;
    float3 viewDir;
    float2 uv;
};

// Parallax shadowing, very expensive method (per-fragment*per-light tangent-space raycast)
float getParallaxShadow(parallaxShadowParameters parameters) {
    float3 lightDir = normalize(mul(parameters.TBN, parameters.lightToFrag));
    int steps = 8;
    float maxDistance = parameters.heightmapScale * 0.2; //0.1;
    float2 uv = parameters.uv;
    float currentHeight = parameters.parallaxTexture.Sample(parameters.parallaxSampler, uv);
    float2 lightDirUV = normalize(lightDir.xy);
    float heightStep = lightDir.z / float(steps);
    float stepSizeUV = maxDistance / float(steps);

    for (int i = 0; i < steps; ++i) {
        uv += lightDirUV * stepSizeUV; // Step across
        currentHeight += heightStep; // Step up
            
        float heightAtSample = parameters.parallaxTexture.Sample(parameters.parallaxSampler, uv);
    
        if (heightAtSample > currentHeight) {
            return 0.05;
        }
    }
    
    return 1.0;
}

float2 WrapFloat2(float2 input) {
    // Apply modulo 1.0 and handle negative values by adding 1.0 and taking modulo again
    return frac(input + 1.0);
}

float2 ParallaxUvFromBound(float2 uv, float2 parallaxDirection, float maxHeight, float bound)
{
    return uv + parallaxDirection * (maxHeight * (bound - 0.5f));
}

float3 ParallaxViewDirectionTS(float3x3 TBN, float3 viewDir)
{
    float3 viewDirTS = normalize(mul(TBN, viewDir));
    float viewDenom = viewDirTS.z * 0.7f + 0.3f;
    viewDenom = viewDenom >= 0.0f ? max(viewDenom, 0.15f) : min(viewDenom, -0.15f);
    viewDirTS.xy /= viewDenom;
    return viewDirTS;
}

uint ParallaxStepCount(float viewDirTSZ, uint maxSteps)
{
    const uint clampedMaxSteps = clamp(maxSteps, 4u, 64u);
    const float grazing = saturate(1.0f - abs(viewDirTSZ));
    return max(4u, (uint)lerp(4.0f, (float)clampedMaxSteps, grazing));
}

float ParallaxSecantBound(float boundA, float fA, float boundB, float fB)
{
    const float denominator = fB - fA;
    const float root = abs(denominator) > 1.0e-5f
        ? (boundA * fB - boundB * fA) / denominator
        : 0.5f * (boundA + boundB);
    return clamp(root, min(boundA, boundB), max(boundA, boundB));
}

float3 getParallaxOcclusionMappingCoordsAndHeight(
    Texture2D<float> parallaxTexture,
    SamplerState parallaxSampler,
    float3x3 TBN,
    float2 uv,
    float3 viewDir,
    float heightmapScale,
    uint maxSteps,
    float2 dUVdx,
    float2 dUVdy)
{
    float3 viewDirTS = ParallaxViewDirectionTS(TBN, viewDir);
    // Object-space material UV parallax uses the inverse of the fragment-to-camera
    // tangent-space XY direction. Terrain has its own parallax path and basis.
    float2 parallaxDirection = -viewDirTS.xy;

    float maxHeight = max(heightmapScale, 0.0f);
    if (maxHeight <= 1.0e-5f)
    {
        float height = parallaxTexture.SampleGrad(parallaxSampler, uv, dUVdx, dUVdy);
        return float3(uv, height);
    }

    const uint numSteps = ParallaxStepCount(viewDirTS.z, maxSteps);
    const float stepSize = rcp((float)numSteps);
    float prevBound = 1.0f;
    float2 prevUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, prevBound);
    float prevHeight = parallaxTexture.SampleGrad(parallaxSampler, prevUv, dUVdx, dUVdy);
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
        const float currentHeight = parallaxTexture.SampleGrad(parallaxSampler, currentUv, dUVdx, dUVdy);
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
        return float3(bottomUv, parallaxTexture.SampleGrad(parallaxSampler, bottomUv, dUVdx, dUVdy));
    }

    [unroll] for (uint refine = 0u; refine < 3u; ++refine)
    {
        const float rootBound = ParallaxSecantBound(hitBound, hitF, missBound, missF);
        const float2 rootUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, rootBound);
        const float rootF = rootBound - parallaxTexture.SampleGrad(parallaxSampler, rootUv, dUVdx, dUVdy);
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
    return float3(parallaxUv, parallaxTexture.SampleGrad(parallaxSampler, parallaxUv, dUVdx, dUVdy));
}

float ParallaxSwizzle(float4 value, uint channel)
{
    if (channel == 0u) return value.x;
    if (channel == 1u) return value.y;
    if (channel == 2u) return value.z;
    if (channel == 3u) return value.w;
    return value.x;
}

float3 getParallaxOcclusionMappingCoordsAndHeight(
    Texture2D<float4> parallaxTexture,
    SamplerState parallaxSampler,
    uint heightChannel,
    float3x3 TBN,
    float2 uv,
    float3 viewDir,
    float heightmapScale,
    uint maxSteps,
    float2 dUVdx,
    float2 dUVdy)
{
    float3 viewDirTS = ParallaxViewDirectionTS(TBN, viewDir);
    // Object-space material UV parallax uses the inverse of the fragment-to-camera
    // tangent-space XY direction. Terrain has its own parallax path and basis.
    float2 parallaxDirection = -viewDirTS.xy;

    float maxHeight = max(heightmapScale, 0.0f);
    if (maxHeight <= 1.0e-5f)
    {
        float height = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, uv, dUVdx, dUVdy), heightChannel);
        return float3(uv, height);
    }

    const uint numSteps = ParallaxStepCount(viewDirTS.z, maxSteps);
    const float stepSize = rcp((float)numSteps);
    float prevBound = 1.0f;
    float2 prevUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, prevBound);
    float prevHeight = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, prevUv, dUVdx, dUVdy), heightChannel);
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
        const float currentHeight = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, currentUv, dUVdx, dUVdy), heightChannel);
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
        return float3(bottomUv, ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, bottomUv, dUVdx, dUVdy), heightChannel));
    }

    [unroll] for (uint refine = 0u; refine < 3u; ++refine)
    {
        const float rootBound = ParallaxSecantBound(hitBound, hitF, missBound, missF);
        const float2 rootUv = ParallaxUvFromBound(uv, parallaxDirection, maxHeight, rootBound);
        const float rootF = rootBound - ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, rootUv, dUVdx, dUVdy), heightChannel);
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
    return float3(parallaxUv, ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, parallaxUv, dUVdx, dUVdy), heightChannel));
}

// Contact-refinement parallax 
// https://www.artstation.com/blogs/andreariccardi/3VPo/a-new-approach-for-parallax-mapping-presenting-the-contact-refinement-parallax-mapping-technique
float3 getContactRefinementParallaxCoordsAndHeight(
    Texture2D<float> parallaxTexture,
    SamplerState parallaxSampler,
    float3x3 TBN,
    float2 uv,
    float3 viewDir,
    float heightmapScale,
    float2 dUVdx,
    float2 dUVdy) {
    // Get view direction in tangent space
    uv.y = 1.0 - uv.y;
    viewDir = normalize(mul(TBN, viewDir));

    float maxHeight = heightmapScale; //0.05;
    float minHeight = maxHeight * 0.5;

    int numSteps = 16;
    // Corrects for Z view angle
    float viewCorrection = (-viewDir.z) + 2.0;
    float stepSize = 1.0 / (float(numSteps) + 1.0);
    float2 stepOffset = viewDir.xy * float2(maxHeight, maxHeight) * stepSize;

    float2 lastOffset = WrapFloat2(viewDir.xy * float2(minHeight, minHeight) + uv);
    float lastRayDepth = 1.0;
    float lastHeight = 1.0;

    float2 p1;
    float2 p2;
    bool refine = false;

    while (numSteps > 0) {
        // Advance ray in direction of TS view direction
        float2 candidateOffset = WrapFloat2(lastOffset - stepOffset);

        float currentRayDepth = lastRayDepth - stepSize;

        // Sample height map at this offset
        float currentHeight = parallaxTexture.SampleGrad(parallaxSampler, candidateOffset, dUVdx, dUVdy); //texture(u_heightMap, candidateOffset).r;
        currentHeight = viewCorrection * currentHeight;
        // Test our candidate depth
        if (currentHeight > currentRayDepth) {
            p1 = float2(currentRayDepth, currentHeight);
            p2 = float2(lastRayDepth, lastHeight);
            // Break if this is the contact refinement pass
            if (refine) {
                lastHeight = currentHeight;
                break;
            // Else, continue raycasting with squared precision
            }
            else {
                refine = true;
                lastRayDepth = p2.x;
                stepSize /= float(numSteps);
                stepOffset /= float(numSteps);
                continue;
            }
        }
        lastOffset = candidateOffset;
        lastRayDepth = currentRayDepth;
        lastHeight = currentHeight;
        numSteps -= 1;
    }
    // Interpolate between final two points
    float diff1 = p1.x - p1.y;
    float diff2 = p2.x - p2.y;
    float denominator = diff2 - diff1;

    float parallaxAmount;
    if (denominator != 0.0) {
        parallaxAmount = (p1.x * diff2 - p2.x * diff1) / denominator;
    }

    float offset = ((1.0 - parallaxAmount) * -maxHeight) + minHeight;
    return float3(viewDir.xy * offset + uv, lastHeight);
}

#endif // __PARALLAX_HLSLI__
