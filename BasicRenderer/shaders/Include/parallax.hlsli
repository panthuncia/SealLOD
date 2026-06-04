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
    float3 viewDirTS = normalize(mul(TBN, viewDir));
    float viewDenom = viewDirTS.z * 0.7f + 0.3f;
    viewDenom = viewDenom >= 0.0f ? max(viewDenom, 0.15f) : min(viewDenom, -0.15f);
    float2 parallaxDirection = viewDirTS.xy / viewDenom;

    float maxHeight = max(heightmapScale, 0.0f);
    if (maxHeight <= 1.0e-5f)
    {
        float height = parallaxTexture.SampleGrad(parallaxSampler, uv, dUVdx, dUVdy);
        return float3(uv, height);
    }

    const uint clampedMaxSteps = clamp(maxSteps, 4u, 64u);
    const float grazing = saturate(1.0f - abs(viewDirTS.z));
    uint numSteps = (uint)lerp(4.0f, (float)clampedMaxSteps, grazing);
    numSteps = max(4u, (numSteps + 3u) & ~3u);

    float minHeight = maxHeight * 0.5f;
    float stepSize = rcp((float)numSteps);
    float2 offsetPerStep = parallaxDirection * maxHeight * stepSize;
    float2 prevOffset = uv + parallaxDirection * minHeight;
    float prevBound = 1.0f;
    float prevHeight = parallaxTexture.SampleGrad(parallaxSampler, prevOffset, dUVdx, dUVdy);
    float2 pt1 = float2(prevBound, prevHeight);
    float2 pt2 = pt1;
    bool foundIntersection = false;
    bool contactRefinement = false;
    uint refinementSteps = numSteps;

    [loop] while (numSteps > 0u)
    {
        float4 currentOffset[2];
        currentOffset[0] = prevOffset.xyxy - float4(1.0f, 1.0f, 2.0f, 2.0f) * offsetPerStep.xyxy;
        currentOffset[1] = prevOffset.xyxy - float4(3.0f, 3.0f, 4.0f, 4.0f) * offsetPerStep.xyxy;
        float4 currentBound = prevBound.xxxx - float4(1.0f, 2.0f, 3.0f, 4.0f) * stepSize;

        float4 currentHeight;
        currentHeight.x = parallaxTexture.SampleGrad(parallaxSampler, currentOffset[0].xy, dUVdx, dUVdy);
        currentHeight.y = parallaxTexture.SampleGrad(parallaxSampler, currentOffset[0].zw, dUVdx, dUVdy);
        currentHeight.z = parallaxTexture.SampleGrad(parallaxSampler, currentOffset[1].xy, dUVdx, dUVdy);
        currentHeight.w = parallaxTexture.SampleGrad(parallaxSampler, currentOffset[1].zw, dUVdx, dUVdy);

        bool4 hit = currentHeight >= currentBound;
        [branch] if (any(hit))
        {
            float2 outOffset = prevOffset;
            [flatten] if (hit.w)
            {
                outOffset = currentOffset[1].xy;
                pt1 = float2(currentBound.w, currentHeight.w);
                pt2 = float2(currentBound.z, currentHeight.z);
            }
            [flatten] if (hit.z)
            {
                outOffset = currentOffset[0].zw;
                pt1 = float2(currentBound.z, currentHeight.z);
                pt2 = float2(currentBound.y, currentHeight.y);
            }
            [flatten] if (hit.y)
            {
                outOffset = currentOffset[0].xy;
                pt1 = float2(currentBound.y, currentHeight.y);
                pt2 = float2(currentBound.x, currentHeight.x);
            }
            [flatten] if (hit.x)
            {
                outOffset = prevOffset;
                pt1 = float2(currentBound.x, currentHeight.x);
                pt2 = float2(prevBound, prevHeight);
            }

            foundIntersection = true;
            if (contactRefinement)
            {
                break;
            }

            contactRefinement = true;
            prevOffset = outOffset;
            prevBound = pt2.x;
            prevHeight = pt2.y;
            numSteps = refinementSteps;
            stepSize /= (float)numSteps;
            offsetPerStep /= (float)numSteps;
            continue;
        }

        prevOffset = currentOffset[1].zw;
        prevBound = currentBound.w;
        prevHeight = currentHeight.w;
        numSteps -= 4u;
    }

    if (!foundIntersection)
    {
        return float3(uv, prevHeight);
    }

    float delta2 = pt2.x - pt2.y;
    float delta1 = pt1.x - pt1.y;
    float denominator = delta2 - delta1;
    float parallaxAmount = abs(denominator) > 1.0e-5f
        ? (pt1.x * delta2 - pt2.x * delta1) / denominator
        : 0.0f;
    float offset = (1.0f - parallaxAmount) * -maxHeight + minHeight;
    float2 parallaxUv = uv + parallaxDirection * offset;
    return float3(parallaxUv, lerp(pt1.y, pt2.y, saturate(parallaxAmount)));
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
    float3 viewDirTS = normalize(mul(TBN, viewDir));
    float viewDenom = viewDirTS.z * 0.7f + 0.3f;
    viewDenom = viewDenom >= 0.0f ? max(viewDenom, 0.15f) : min(viewDenom, -0.15f);
    float2 parallaxDirection = viewDirTS.xy / viewDenom;

    float maxHeight = max(heightmapScale, 0.0f);
    if (maxHeight <= 1.0e-5f)
    {
        float height = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, uv, dUVdx, dUVdy), heightChannel);
        return float3(uv, height);
    }

    const uint clampedMaxSteps = clamp(maxSteps, 4u, 64u);
    const float grazing = saturate(1.0f - abs(viewDirTS.z));
    uint numSteps = (uint)lerp(4.0f, (float)clampedMaxSteps, grazing);
    numSteps = max(4u, (numSteps + 3u) & ~3u);

    float minHeight = maxHeight * 0.5f;
    float stepSize = rcp((float)numSteps);
    float2 offsetPerStep = parallaxDirection * maxHeight * stepSize;
    float2 prevOffset = uv + parallaxDirection * minHeight;
    float prevBound = 1.0f;
    float prevHeight = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, prevOffset, dUVdx, dUVdy), heightChannel);
    float2 pt1 = float2(prevBound, prevHeight);
    float2 pt2 = pt1;
    bool foundIntersection = false;
    bool contactRefinement = false;
    uint refinementSteps = numSteps;

    [loop] while (numSteps > 0u)
    {
        float4 currentOffset[2];
        currentOffset[0] = prevOffset.xyxy - float4(1.0f, 1.0f, 2.0f, 2.0f) * offsetPerStep.xyxy;
        currentOffset[1] = prevOffset.xyxy - float4(3.0f, 3.0f, 4.0f, 4.0f) * offsetPerStep.xyxy;
        float4 currentBound = prevBound.xxxx - float4(1.0f, 2.0f, 3.0f, 4.0f) * stepSize;

        float4 currentHeight;
        currentHeight.x = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, currentOffset[0].xy, dUVdx, dUVdy), heightChannel);
        currentHeight.y = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, currentOffset[0].zw, dUVdx, dUVdy), heightChannel);
        currentHeight.z = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, currentOffset[1].xy, dUVdx, dUVdy), heightChannel);
        currentHeight.w = ParallaxSwizzle(parallaxTexture.SampleGrad(parallaxSampler, currentOffset[1].zw, dUVdx, dUVdy), heightChannel);

        bool4 hit = currentHeight >= currentBound;
        [branch] if (any(hit))
        {
            float2 outOffset = prevOffset;
            [flatten] if (hit.w)
            {
                outOffset = currentOffset[1].xy;
                pt1 = float2(currentBound.w, currentHeight.w);
                pt2 = float2(currentBound.z, currentHeight.z);
            }
            [flatten] if (hit.z)
            {
                outOffset = currentOffset[0].zw;
                pt1 = float2(currentBound.z, currentHeight.z);
                pt2 = float2(currentBound.y, currentHeight.y);
            }
            [flatten] if (hit.y)
            {
                outOffset = currentOffset[0].xy;
                pt1 = float2(currentBound.y, currentHeight.y);
                pt2 = float2(currentBound.x, currentHeight.x);
            }
            [flatten] if (hit.x)
            {
                outOffset = prevOffset;
                pt1 = float2(currentBound.x, currentHeight.x);
                pt2 = float2(prevBound, prevHeight);
            }

            foundIntersection = true;
            if (contactRefinement)
            {
                break;
            }

            contactRefinement = true;
            prevOffset = outOffset;
            prevBound = pt2.x;
            prevHeight = pt2.y;
            numSteps = refinementSteps;
            stepSize /= (float)numSteps;
            offsetPerStep /= (float)numSteps;
            continue;
        }

        prevOffset = currentOffset[1].zw;
        prevBound = currentBound.w;
        prevHeight = currentHeight.w;
        numSteps -= 4u;
    }

    if (!foundIntersection)
    {
        return float3(uv, prevHeight);
    }

    float delta2 = pt2.x - pt2.y;
    float delta1 = pt1.x - pt1.y;
    float denominator = delta2 - delta1;
    float parallaxAmount = abs(denominator) > 1.0e-5f
        ? (pt1.x * delta2 - pt2.x * delta1) / denominator
        : 0.0f;
    float offset = (1.0f - parallaxAmount) * -maxHeight + minHeight;
    float2 parallaxUv = uv + parallaxDirection * offset;
    return float3(parallaxUv, lerp(pt1.y, pt2.y, saturate(parallaxAmount)));
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
