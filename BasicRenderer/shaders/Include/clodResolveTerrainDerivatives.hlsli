#ifndef CLOD_RESOLVE_TERRAIN_DERIVATIVES_HLSLI
#define CLOD_RESOLVE_TERRAIN_DERIVATIVES_HLSLI

// World-space derivatives from the terrain tangent plane, which stay stable across CLod triangles.

#if defined(PSO_TERRAIN)
void CLodResolveCameraRayWS(Camera cam, float2 pixelCenter, float2 winSize, out float3 rayOriginWS, out float3 rayDirectionWS)
{
    const float2 pixelUv = pixelCenter / winSize;
    const float2 ndc = float2(pixelUv.x * 2.0f - 1.0f, 1.0f - pixelUv.y * 2.0f);
    float4 viewNear = mul(float4(ndc, 0.0f, 1.0f), cam.projectionInverse);
    float4 viewFar = mul(float4(ndc, 1.0f, 1.0f), cam.projectionInverse);
    viewNear.xyz /= max(abs(viewNear.w), 1.0e-6f);
    viewFar.xyz /= max(abs(viewFar.w), 1.0e-6f);
    if (cam.isOrtho)
    {
        const float4 worldNear = mul(float4(viewNear.xyz, 1.0f), cam.viewInverse);
        rayOriginWS = worldNear.xyz / max(abs(worldNear.w), 1.0e-6f);
        rayDirectionWS = normalize(mul(float4(viewFar.xyz - viewNear.xyz, 0.0f), cam.viewInverse).xyz);
    }
    else
    {
        rayOriginWS = cam.positionWorldSpace.xyz;
        rayDirectionWS = normalize(mul(float4(viewFar.xyz, 0.0f), cam.viewInverse).xyz);
    }
}

bool CLodResolveIntersectRayWithPlane(
    float3 rayOriginWS,
    float3 rayDirectionWS,
    float3 planePointWS,
    float3 planeNormalWS,
    out float3 positionWS)
{
    positionWS = planePointWS;
    const float denom = dot(rayDirectionWS, planeNormalWS);
    if (abs(denom) <= 1.0e-5f)
    {
        return false;
    }

    const float t = dot(planePointWS - rayOriginWS, planeNormalWS) / denom;
    positionWS = rayOriginWS + rayDirectionWS * t;
    return all(isfinite(positionWS));
}

bool CLodResolveEstimateTerrainTangentPlaneDerivatives(
    Camera cam,
    uint2 pixel,
    float2 winSize,
    float3 positionWS,
    float3 normalWS,
    out float3 dpdxWS,
    out float3 dpdyWS)
{
    dpdxWS = 0.0f.xxx;
    dpdyWS = 0.0f.xxx;

    const float normalLengthSq = dot(normalWS, normalWS);
    if (normalLengthSq <= 1.0e-10f)
    {
        return false;
    }

    const float3 planeNormalWS = normalWS * rsqrt(normalLengthSq);
    const float2 pixelCenter = float2(pixel) + 0.5f;
    float3 rayOrigin0WS;
    float3 rayOriginXWS;
    float3 rayOriginYWS;
    float3 rayDirection0WS;
    float3 rayDirectionXWS;
    float3 rayDirectionYWS;
    CLodResolveCameraRayWS(cam, pixelCenter, winSize, rayOrigin0WS, rayDirection0WS);
    CLodResolveCameraRayWS(cam, pixelCenter + float2(1.0f, 0.0f), winSize, rayOriginXWS, rayDirectionXWS);
    CLodResolveCameraRayWS(cam, pixelCenter + float2(0.0f, 1.0f), winSize, rayOriginYWS, rayDirectionYWS);

    float3 planePosition0WS;
    float3 planePositionXWS;
    float3 planePositionYWS;
    if (!CLodResolveIntersectRayWithPlane(rayOrigin0WS, rayDirection0WS, positionWS, planeNormalWS, planePosition0WS) ||
        !CLodResolveIntersectRayWithPlane(rayOriginXWS, rayDirectionXWS, positionWS, planeNormalWS, planePositionXWS) ||
        !CLodResolveIntersectRayWithPlane(rayOriginYWS, rayDirectionYWS, positionWS, planeNormalWS, planePositionYWS))
    {
        return false;
    }

    dpdxWS = planePositionXWS - planePosition0WS;
    dpdyWS = planePositionYWS - planePosition0WS;
    return all(isfinite(dpdxWS)) && all(isfinite(dpdyWS));
}
#endif

#endif // CLOD_RESOLVE_TERRAIN_DERIVATIVES_HLSLI
