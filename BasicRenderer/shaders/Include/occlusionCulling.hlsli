#ifndef OCCLUSION_CULLING_HLSLI
#define OCCLUSION_CULLING_HLSLI

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/misc/sphereScreenExtents.hlsli"

void OcclusionCulling(out bool fullyCulled, in const Camera camera, float3 viewSpaceCenter, float boundingSphereDepth, float scaledBoundingRadius, matrix viewProjection, uint depthMapDescriptorIndex)
{
    // Occlusion culling
    float3 vHZB = float3(camera.depthResX, camera.depthResY, camera.numDepthMips);
    viewSpaceCenter.y = -viewSpaceCenter.y; // Invert Y for HZB sampling
    float4 vLBRT;

    if (camera.isOrtho)
    {
        viewSpaceCenter.y = -viewSpaceCenter.y;
        vLBRT = sphere_screen_extents_ortho(viewSpaceCenter.xyz, scaledBoundingRadius, camera.projection);
    }
    else
    {
        vLBRT = sphere_screen_extents(viewSpaceCenter.xyz, scaledBoundingRadius, camera.projection);
        vLBRT.x = -vLBRT.x; // TODO: Fix this in sphere_screen_extents
        vLBRT.z = -vLBRT.z;
    }

    float4 vToUV = float4(0.5f, -0.5f, 0.5f, -0.5f);
    float4 vUV = saturate(vLBRT.xwzy * vToUV + 0.5f);
    float4 vAABB = vUV * vHZB.xyxy; // vHZB = [w, h, l]
    float2 vExtents = vAABB.zw - vAABB.xy; // In pixels
    
    float fMipLevel = ceil(log2(max(vExtents.x, vExtents.y)));
    fMipLevel = clamp(fMipLevel, 0.0f, vHZB.z - 1.0f);
    
    vUV *= camera.UVScaleToNextPowerOf2.xyxy; // Scale to next power of two, because it was padded for downsampling
    
    float4 occlusionDepth;
    if (camera.depthBufferArrayIndex < 0)
    { // Not a texture array
        Texture2D<float> depthBuffer = ResourceDescriptorHeap[depthMapDescriptorIndex];
        occlusionDepth = float4(
            depthBuffer.SampleLevel(g_pointClamp, vUV.xy, fMipLevel),
            depthBuffer.SampleLevel(g_pointClamp, vUV.zy, fMipLevel),
            depthBuffer.SampleLevel(g_pointClamp, vUV.zw, fMipLevel),
            depthBuffer.SampleLevel(g_pointClamp, vUV.xw, fMipLevel));
    }
    else
    {
        Texture2DArray<float> depthBuffer = ResourceDescriptorHeap[depthMapDescriptorIndex];
        occlusionDepth = float4(
            depthBuffer.SampleLevel(g_pointClamp, float3(vUV.xy, camera.depthBufferArrayIndex), fMipLevel),
            depthBuffer.SampleLevel(g_pointClamp, float3(vUV.zy, camera.depthBufferArrayIndex), fMipLevel),
            depthBuffer.SampleLevel(g_pointClamp, float3(vUV.zw, camera.depthBufferArrayIndex), fMipLevel),
            depthBuffer.SampleLevel(g_pointClamp, float3(vUV.xw, camera.depthBufferArrayIndex), fMipLevel));
    }
    
    float fMaxOcclusionDepth = max(max(occlusionDepth.x, occlusionDepth.y), max(occlusionDepth.z, occlusionDepth.w));
    fullyCulled = fMaxOcclusionDepth < boundingSphereDepth - scaledBoundingRadius;
}

void OcclusionCullingPerspectiveTexture2D(
    out bool fullyCulled,
    in const Camera camera,
    float3 viewSpaceCenter,
    float boundingSphereDepth,
    float scaledBoundingRadius,
    uint depthMapDescriptorIndex)
{
    const float2 viewRes = float2(camera.depthResX, camera.depthResY);
    const float3 vHZB = float3(viewRes.x, viewRes.y, camera.numDepthMips);

    viewSpaceCenter.y = -viewSpaceCenter.y;
    float4 vLBRT = sphere_screen_extents(viewSpaceCenter.xyz, scaledBoundingRadius, camera.projection);
    vLBRT.x = -vLBRT.x;
    vLBRT.z = -vLBRT.z;

    const float4 vToUV = float4(0.5f, -0.5f, 0.5f, -0.5f);
    float4 vUV = saturate(vLBRT.xwzy * vToUV + 0.5f);

    float4 vAABB = vUV * vHZB.xyxy;
    float2 vExtents = vAABB.zw - vAABB.xy;

    float fMipLevel = ceil(log2(max(vExtents.x, vExtents.y)));
    fMipLevel = clamp(fMipLevel, 0.0f, vHZB.z - 1.0f);

    Texture2D<float> depthBuffer = ResourceDescriptorHeap[depthMapDescriptorIndex];

    // Convert viewport UVs to padded HZB UVs
    float4 vUVPadded = vUV * camera.UVScaleToNextPowerOf2.xyxy;

    // Reconstruct padded base resolution from view resolution and UV scale.
    const float2 safeScale = max(camera.UVScaleToNextPowerOf2, float2(1e-6f, 1e-6f));
    const uint mipLevel = (uint)fMipLevel;
    const uint2 hzbRes = max(uint2(1, 1), (uint2)round(viewRes / safeScale));
    const uint2 mipRes = max(uint2(1, 1), hzbRes >> mipLevel);
    const uint4 maxCoord = uint4(mipRes.xy - 1, mipRes.xy - 1);
    const uint4 pixelCoords = min((uint4)floor(vUVPadded * float4(mipRes.xy, mipRes.xy)), maxCoord);

    // xy = left bottom
    // zy = right bottom
    // zw = right top
    // xw = left top
    float4 occlusionDepth = float4(
        depthBuffer.Load(int3(pixelCoords.xy, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.zy, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.zw, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.xw, (int)mipLevel)));

    const float fMaxOcclusionDepth = max(max(occlusionDepth.x, occlusionDepth.y), max(occlusionDepth.z, occlusionDepth.w));
    //float epsilon = 0.001f;
    fullyCulled = fMaxOcclusionDepth < boundingSphereDepth - scaledBoundingRadius;
}

// Overload that uses an explicit projection matrix for screen-extent computation
// (e.g. previous frame's projection for temporal reprojection against an older HZB).
void OcclusionCullingPerspectiveTexture2D(
    out bool fullyCulled,
    in const Camera camera,
    float3 viewSpaceCenter,
    float boundingSphereDepth,
    float scaledBoundingRadius,
    uint depthMapDescriptorIndex,
    row_major matrix occlusionProjection)
{
    const float2 viewRes = float2(camera.depthResX, camera.depthResY);
    const float3 vHZB = float3(viewRes.x, viewRes.y, camera.numDepthMips);

    viewSpaceCenter.y = -viewSpaceCenter.y;
    float4 vLBRT = sphere_screen_extents(viewSpaceCenter.xyz, scaledBoundingRadius, occlusionProjection);
    vLBRT.x = -vLBRT.x;
    vLBRT.z = -vLBRT.z;

    const float4 vToUV = float4(0.5f, -0.5f, 0.5f, -0.5f);
    float4 vUV = saturate(vLBRT.xwzy * vToUV + 0.5f);

    float4 vAABB = vUV * vHZB.xyxy;
    float2 vExtents = vAABB.zw - vAABB.xy;

    float fMipLevel = ceil(log2(max(vExtents.x, vExtents.y)));
    fMipLevel = clamp(fMipLevel, 0.0f, vHZB.z - 1.0f);

    Texture2D<float> depthBuffer = ResourceDescriptorHeap[depthMapDescriptorIndex];

    float4 vUVPadded = vUV * camera.UVScaleToNextPowerOf2.xyxy;

    const float2 safeScale = max(camera.UVScaleToNextPowerOf2, float2(1e-6f, 1e-6f));
    const uint mipLevel = (uint)fMipLevel;
    const uint2 hzbRes = max(uint2(1, 1), (uint2)round(viewRes / safeScale));
    const uint2 mipRes = max(uint2(1, 1), hzbRes >> mipLevel);
    const uint4 maxCoord = uint4(mipRes.xy - 1, mipRes.xy - 1);
    const uint4 pixelCoords = min((uint4)floor(vUVPadded * float4(mipRes.xy, mipRes.xy)), maxCoord);

    float4 occlusionDepth = float4(
        depthBuffer.Load(int3(pixelCoords.xy, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.zy, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.zw, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.xw, (int)mipLevel)));

    const float fMaxOcclusionDepth = max(max(occlusionDepth.x, occlusionDepth.y), max(occlusionDepth.z, occlusionDepth.w));
    fullyCulled = fMaxOcclusionDepth < boundingSphereDepth - scaledBoundingRadius;
}

// Overload: takes individual HZB parameters and explicit projection matrix
// instead of the full Camera struct, to reduce register pressure at call sites.
void OcclusionCullingPerspectiveTexture2D(
    out bool fullyCulled,
    uint2 depthRes,
    uint numDepthMips,
    float2 uvScaleToNextPow2,
    row_major matrix projMatrix,
    float3 viewSpaceCenter,
    float boundingSphereDepth,
    float scaledBoundingRadius,
    uint depthMapDescriptorIndex)
{
    const float2 viewRes = float2(depthRes);
    const float3 vHZB = float3(viewRes.x, viewRes.y, numDepthMips);

    viewSpaceCenter.y = -viewSpaceCenter.y;
    float4 vLBRT = sphere_screen_extents(viewSpaceCenter.xyz, scaledBoundingRadius, projMatrix);
    vLBRT.x = -vLBRT.x;
    vLBRT.z = -vLBRT.z;

    const float4 vToUV = float4(0.5f, -0.5f, 0.5f, -0.5f);
    float4 vUV = saturate(vLBRT.xwzy * vToUV + 0.5f);

    float4 vAABB = vUV * vHZB.xyxy;
    float2 vExtents = vAABB.zw - vAABB.xy;

    float fMipLevel = ceil(log2(max(vExtents.x, vExtents.y)));
    fMipLevel = clamp(fMipLevel, 0.0f, vHZB.z - 1.0f);

    Texture2D<float> depthBuffer = ResourceDescriptorHeap[depthMapDescriptorIndex];

    float4 vUVPadded = vUV * uvScaleToNextPow2.xyxy;

    const float2 safeScale = max(uvScaleToNextPow2, float2(1e-6f, 1e-6f));
    const uint mipLevel = (uint)fMipLevel;
    const uint2 hzbRes = max(uint2(1, 1), (uint2)round(viewRes / safeScale));
    const uint2 mipRes = max(uint2(1, 1), hzbRes >> mipLevel);
    const uint4 maxCoord = uint4(mipRes.xy - 1, mipRes.xy - 1);
    const uint4 pixelCoords = min((uint4)floor(vUVPadded * float4(mipRes.xy, mipRes.xy)), maxCoord);

    float4 occlusionDepth = float4(
        depthBuffer.Load(int3(pixelCoords.xy, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.zy, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.zw, (int)mipLevel)),
        depthBuffer.Load(int3(pixelCoords.xw, (int)mipLevel)));

    const float fMaxOcclusionDepth = max(max(occlusionDepth.x, occlusionDepth.y), max(occlusionDepth.z, occlusionDepth.w));
    fullyCulled = fMaxOcclusionDepth < boundingSphereDepth - scaledBoundingRadius;
}

void OcclusionCullingPerspectiveViewAABBTexture2D(
    out bool fullyCulled,
    uint2 depthRes,
    uint numDepthMips,
    row_major matrix projMatrix,
    float3 viewMin,
    float3 viewMax,
    uint depthMapDescriptorIndex)
{
    fullyCulled = false;
    if (depthRes.x == 0u || depthRes.y == 0u || numDepthMips == 0u ||
        !all(isfinite(viewMin)) || !all(isfinite(viewMax)) || any(viewMax < viewMin))
    {
        return;
    }

    float2 uvMin = float2(1.0e30f, 1.0e30f);
    float2 uvMax = float2(-1.0e30f, -1.0e30f);
    float nearestDepth = 1.0e30f;

    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        const float3 viewCorner = float3(
            (cornerIndex & 0x1u) != 0u ? viewMax.x : viewMin.x,
            (cornerIndex & 0x2u) != 0u ? viewMax.y : viewMin.y,
            (cornerIndex & 0x4u) != 0u ? viewMax.z : viewMin.z);
        const float cornerDepth = -viewCorner.z;
        if (!all(isfinite(viewCorner)) || cornerDepth <= 0.0f)
        {
            return;
        }

        const float4 clipCorner = mul(float4(viewCorner, 1.0f), projMatrix);
        if (!all(isfinite(clipCorner)) || clipCorner.w <= 1.0e-6f)
        {
            return;
        }

        const float2 ndc = clipCorner.xy / clipCorner.w;
        const float2 uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        nearestDepth = min(nearestDepth, cornerDepth);
    }

    if (uvMax.x < 0.0f || uvMin.x > 1.0f || uvMax.y < 0.0f || uvMin.y > 1.0f ||
        !all(isfinite(uvMin)) || !all(isfinite(uvMax)) || !isfinite(nearestDepth))
    {
        return;
    }

    const float2 viewResF = float2(depthRes);
    const float2 rectMinPx = saturate(uvMin) * viewResF;
    const float2 rectMaxPx = saturate(uvMax) * viewResF;
    int2 p0 = int2(floor(rectMinPx));
    int2 p1 = int2(ceil(rectMaxPx)) - int2(1, 1);
    const int2 maxPixel = int2(depthRes) - int2(1, 1);
    p0 = clamp(p0, int2(0, 0), maxPixel);
    p1 = clamp(p1, int2(0, 0), maxPixel);
    if (any(p1 < p0))
    {
        return;
    }

    const uint maxMipLevel = numDepthMips - 1u;
    uint mipLevel = 0u;
    [loop]
    while (mipLevel < maxMipLevel)
    {
        const int2 t0 = p0 >> mipLevel;
        const int2 t1 = p1 >> mipLevel;
        const int2 span = t1 - t0 + int2(1, 1);
        if (span.x <= 2 && span.y <= 2)
        {
            break;
        }
        ++mipLevel;
    }

    const int2 q0 = p0 >> mipLevel;
    const int2 q1 = p1 >> mipLevel;
    const int2 qn = q1 - q0 + int2(1, 1);
    Texture2D<float> depthBuffer = ResourceDescriptorHeap[depthMapDescriptorIndex];

    float hzbMax = depthBuffer.Load(int3(q0.x, q0.y, (int)mipLevel));
    if (qn.x == 2)
    {
        hzbMax = max(hzbMax, depthBuffer.Load(int3(q1.x, q0.y, (int)mipLevel)));
    }
    if (qn.y == 2)
    {
        hzbMax = max(hzbMax, depthBuffer.Load(int3(q0.x, q1.y, (int)mipLevel)));
        if (qn.x == 2)
        {
            hzbMax = max(hzbMax, depthBuffer.Load(int3(q1.x, q1.y, (int)mipLevel)));
        }
    }

    fullyCulled = hzbMax < nearestDepth;
}

bool ConservativeAnyHitTexture2DArraySphereQuery(
    Texture2DArray<uint> queryTexture,
    uint arrayLayer,
    uint2 baseResolution,
    in const Camera camera,
    float3 viewSpaceCenter,
    float scaledBoundingRadius,
    out uint sampledMipLevel,
    out bool queryClipped)
{
    viewSpaceCenter.y = -viewSpaceCenter.y;

    float4 vLBRT;
    if (camera.isOrtho)
    {
        viewSpaceCenter.y = -viewSpaceCenter.y; // Un-invert for ortho (matches OcclusionCulling)
        vLBRT = sphere_screen_extents_ortho(viewSpaceCenter.xyz, scaledBoundingRadius, camera.projection);
    }
    else
    {
        vLBRT = sphere_screen_extents(viewSpaceCenter.xyz, scaledBoundingRadius, camera.projection);
        vLBRT.x = -vLBRT.x;
        vLBRT.z = -vLBRT.z;
    }

    const float4 vToUV = float4(0.5f, -0.5f, 0.5f, -0.5f);
    const float4 vUV = vLBRT.xwzy * vToUV + 0.5f;
    const float2 uvMin = vUV.xy;
    const float2 uvMax = vUV.zw;

    if (uvMax.x < 0.0f || uvMin.x > 1.0f ||
        uvMax.y < 0.0f || uvMin.y > 1.0f)
    {
        sampledMipLevel = 0u;
        queryClipped = false;
        return false;
    }

    queryClipped = any(uvMin < 0.0f.xx) || any(uvMax > 1.0f.xx);

    const float2 clampedUvMin = saturate(uvMin);
    const float2 clampedUvMax = saturate(uvMax);
    const float2 baseResolutionF = float2(baseResolution);
    const float2 minTexel = clamp(baseResolutionF * clampedUvMin, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float2 maxTexel = clamp(baseResolutionF * clampedUvMax, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float pixelWidth = max(maxTexel.x - minTexel.x, maxTexel.y - minTexel.y);
    const uint sampleWidth = 2u;
    const uint maxMipLevel = firstbithigh(max(baseResolution.x, baseResolution.y));

    sampledMipLevel = min(
        (uint)clamp(ceil(log2(max(pixelWidth, 1.0f))) - log2((float)sampleWidth), 0.0f, (float)maxMipLevel),
        maxMipLevel);

    const int2 quadCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 minCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 maxCornerTexel = int2(maxTexel) >> sampledMipLevel;
    const int2 atMipPixelWidth = maxCornerTexel - minCornerTexel + 1;
    const int2 texelBounds = max(int2(0, 0), (int2(baseResolution) >> sampledMipLevel) - 1);

    [loop]
    for (uint x = 0u; x <= sampleWidth; ++x)
    {
        [loop]
        for (uint y = 0u; y <= sampleWidth; ++y)
        {
            if ((int)x >= atMipPixelWidth.x || (int)y >= atMipPixelWidth.y)
            {
                continue;
            }

            const int2 sampleTexel = clamp(quadCornerTexel + int2(x, y), int2(0, 0), texelBounds);
            if (queryTexture.Load(int4(sampleTexel, arrayLayer, sampledMipLevel)) != 0u)
            {
                return true;
            }
        }
    }

    return false;
}

#endif // OCCLUSION_CULLING_HLSLI
