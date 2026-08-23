#include "include/utilities.hlsli"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "PerPassRootConstants/debugGridRootConstants.h"

// Implementation of https://bgolus.medium.com/the-best-darn-grid-shader-yet-727f9278b9d8

static const uint GROUP_X = 8;
static const uint GROUP_Y = 8;

// Derivative-length helper
float2 DerivLen(float2 v)
{
    float4 dd = float4(ddx(v), ddy(v));
    return float2(length(dd.xz), length(dd.yw));
}

float PristineGrid(float2 uv, float2 lineWidth01)
{
    lineWidth01 = saturate(lineWidth01);

    float2 uvDeriv = max(DerivLen(uv), 1e-6);

    bool2 invertLine = lineWidth01 > 0.5;
    float2 targetWidth = select(invertLine, (1. - lineWidth01), lineWidth01);

    float2 drawWidth = clamp(targetWidth, uvDeriv, 0.5);
    float2 lineAA = uvDeriv * 1.5;

    float2 gridUV = abs(frac(uv) * 2.0 - 1.0);
    gridUV = select(invertLine, gridUV, (1. - gridUV));

    float2 grid2 = smoothstep(drawWidth + lineAA, drawWidth - lineAA, gridUV);

    grid2 *= saturate(targetWidth / drawWidth);
    grid2 = lerp(grid2, targetWidth, saturate(uvDeriv * 2.0 - 1.0));
    grid2 = select(invertLine, (1. - grid2), grid2);

    return lerp(grid2.x, 1.0, grid2.y);
}

float PhoneWireSingleLine(float coordWorld, float halfWidthWorld)
{
    float d = abs(coordWorld);

    float dd = max(length(float2(ddx(coordWorld), ddy(coordWorld))), 1e-6);
    float drawW = max(halfWidthWorld, dd);
    float aa = dd * 1.5;

    float fLine = smoothstep(drawW + aa, drawW - aa, d);
    fLine *= saturate(halfWidthWorld / drawW);
    return fLine;
}

// Premultiplied alpha "over": out = src + dst*(1-srcA)
float3 PremulOver(float3 dst, float srcA, float3 srcPremul)
{
    return srcPremul + dst * (1.0 - srcA);
}

float4x4 inverse(float4x4 m) {
    float n11 = m[0][0], n12 = m[1][0], n13 = m[2][0], n14 = m[3][0];
    float n21 = m[0][1], n22 = m[1][1], n23 = m[2][1], n24 = m[3][1];
    float n31 = m[0][2], n32 = m[1][2], n33 = m[2][2], n34 = m[3][2];
    float n41 = m[0][3], n42 = m[1][3], n43 = m[2][3], n44 = m[3][3];

    float t11 = n23 * n34 * n42 - n24 * n33 * n42 + n24 * n32 * n43 - n22 * n34 * n43 - n23 * n32 * n44 + n22 * n33 * n44;
    float t12 = n14 * n33 * n42 - n13 * n34 * n42 - n14 * n32 * n43 + n12 * n34 * n43 + n13 * n32 * n44 - n12 * n33 * n44;
    float t13 = n13 * n24 * n42 - n14 * n23 * n42 + n14 * n22 * n43 - n12 * n24 * n43 - n13 * n22 * n44 + n12 * n23 * n44;
    float t14 = n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 + n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34;

    float det = n11 * t11 + n21 * t12 + n31 * t13 + n41 * t14;
    float idet = 1.0f / det;

    float4x4 ret;

    ret[0][0] = t11 * idet;
    ret[0][1] = (n24 * n33 * n41 - n23 * n34 * n41 - n24 * n31 * n43 + n21 * n34 * n43 + n23 * n31 * n44 - n21 * n33 * n44) * idet;
    ret[0][2] = (n22 * n34 * n41 - n24 * n32 * n41 + n24 * n31 * n42 - n21 * n34 * n42 - n22 * n31 * n44 + n21 * n32 * n44) * idet;
    ret[0][3] = (n23 * n32 * n41 - n22 * n33 * n41 - n23 * n31 * n42 + n21 * n33 * n42 + n22 * n31 * n43 - n21 * n32 * n43) * idet;

    ret[1][0] = t12 * idet;
    ret[1][1] = (n13 * n34 * n41 - n14 * n33 * n41 + n14 * n31 * n43 - n11 * n34 * n43 - n13 * n31 * n44 + n11 * n33 * n44) * idet;
    ret[1][2] = (n14 * n32 * n41 - n12 * n34 * n41 - n14 * n31 * n42 + n11 * n34 * n42 + n12 * n31 * n44 - n11 * n32 * n44) * idet;
    ret[1][3] = (n12 * n33 * n41 - n13 * n32 * n41 + n13 * n31 * n42 - n11 * n33 * n42 - n12 * n31 * n43 + n11 * n32 * n43) * idet;

    ret[2][0] = t13 * idet;
    ret[2][1] = (n14 * n23 * n41 - n13 * n24 * n41 - n14 * n21 * n43 + n11 * n24 * n43 + n13 * n21 * n44 - n11 * n23 * n44) * idet;
    ret[2][2] = (n12 * n24 * n41 - n14 * n22 * n41 + n14 * n21 * n42 - n11 * n24 * n42 - n12 * n21 * n44 + n11 * n22 * n44) * idet;
    ret[2][3] = (n13 * n22 * n41 - n12 * n23 * n41 - n13 * n21 * n42 + n11 * n23 * n42 + n12 * n21 * n43 - n11 * n22 * n43) * idet;

    ret[3][0] = t14 * idet;
    ret[3][1] = (n13 * n24 * n31 - n14 * n23 * n31 + n14 * n21 * n33 - n11 * n24 * n33 - n13 * n21 * n34 + n11 * n23 * n34) * idet;
    ret[3][2] = (n14 * n22 * n31 - n12 * n24 * n31 - n14 * n21 * n32 + n11 * n24 * n32 + n12 * n21 * n34 - n11 * n22 * n34) * idet;
    ret[3][3] = (n12 * n23 * n31 - n13 * n22 * n31 + n13 * n21 * n32 - n11 * n23 * n32 - n12 * n21 * n33 + n11 * n22 * n33) * idet;

    return ret;
}

[numthreads(GROUP_X, GROUP_Y, 1)]
void DebugGridCSMain(uint3 dtid : SV_DispatchThreadID)
{
    // relies on derivatives, keep lanes coherent:
    // clamp coords for computations, then guard the store.
    uint2 dim;
    RWTexture2D<float4> hdr = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Color::HDRColorTarget)];
    hdr.GetDimensions(dim.x, dim.y);

    uint2 pix = dtid.xy;
    bool inBounds = (pix.x < dim.x) && (pix.y < dim.y);

    uint2 clampedPix = uint2(min(pix.x, dim.x - 1), min(pix.y, dim.y - 1));

    // Screen UV at pixel center
    float2 uv = (float2(clampedPix) + 0.5) / float2(dim);

    // Camera
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera cam = cameras[perFrame.mainCameraIndex];

    float planeY = asfloat(RC_PlaneY);
    float minorCell = max(asfloat(RC_MinorCellSize), 1e-6);
    float majorCell = max(asfloat(RC_MajorCellSize), minorCell);
    float minorLW = saturate(asfloat(RC_MinorLineWidth));
    float majorLW = saturate(asfloat(RC_MajorLineWidth));
    float axisHalfW = max(asfloat(RC_AxisHalfWidthWorld), 0.0);

    float minorOp = saturate(asfloat(RC_MinorOpacity));
    float majorOp = saturate(asfloat(RC_MajorOpacity));
    float axisOp = saturate(asfloat(RC_AxisOpacity));
    float overallOp = saturate(asfloat(RC_OverallOpacity));

    float3 camPos = cam.viewInverse[3].xyz;

    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = (1.0 - uv.y) * 2.0 - 1.0;

    float4 viewH = mul(float4(ndc, 0.0, 1.0), cam.projectionInverse);
    float3 viewP = viewH.xyz / max(viewH.w, 1e-6);
    float4 worldH = mul(float4(viewP, 1.0), cam.viewInverse);
    float3 farP = worldH.xyz / max(worldH.w, 1e-6);

    float3 rayDir = normalize(farP - camPos);

    float denom = rayDir.y;
    // Keep denom nonzero to avoid divergence messing with derivatives;
    // we'll mask the final alpha instead.
    float safeDenom = (abs(denom) < 1e-6) ? (denom >= 0 ? 1e-6 : -1e-6) : denom;

    float t = (planeY - camPos.y) / safeDenom;
    bool hitValid = (abs(denom) >= 1e-6) && (t > 0.0);

    float3 hit = camPos + rayDir * t;

    // Depth-aware occlusion against scene linear depth (-viewSpaceZ)
    Texture2D<float> linearDepthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    // Remap output-resolution pixel coords to render-resolution coords
    int2 depthPix = int2(clamp(uv * float2(cam.depthResX, cam.depthResY), 0, float2(cam.depthResX - 1, cam.depthResY - 1)));
    float sceneLinearDepth = linearDepthTexture[depthPix];
    const float noGeometryDepth = asfloat(0x7F7FFFFF);

    float gridLinearDepth = -mul(float4(hit, 1.0), cam.view).z;
    const float depthEpsilon = 1e-3;
    bool occludedByScene = (sceneLinearDepth != noGeometryDepth) && (sceneLinearDepth + depthEpsilon < gridLinearDepth);

    // Defer (skip) rendering when the grid is nearly coplanar with scene geometry to avoid Z-fighting
    const float deferEpsilon = 5e-3;
    bool deferredByProximity = (sceneLinearDepth != noGeometryDepth) && (abs(sceneLinearDepth - gridLinearDepth) < deferEpsilon * gridLinearDepth);

    // Camera-relative snapping to reduce far-from-origin precision issues
    float2 camXZ = camPos.xz;

    float2 minorOrigin = floor(camXZ / minorCell) * minorCell;
    float2 majorOrigin = floor(camXZ / majorCell) * majorCell;
    float2 minorUV = (hit.xz - minorOrigin) / minorCell;
    float2 majorUV = (hit.xz - majorOrigin) / majorCell;

    float minorMask = PristineGrid(minorUV, minorLW.xx);
    float majorMask = PristineGrid(majorUV, majorLW.xx);

    float axisX = (axisHalfW > 0.0) ? PhoneWireSingleLine(hit.z, axisHalfW) : 0.0;
    float axisZ = (axisHalfW > 0.0) ? PhoneWireSingleLine(hit.x, axisHalfW) : 0.0;

    // Linear colors
    float3 minorCol = float3(0.65, 0.65, 0.70);
    float3 majorCol = float3(0.85, 0.85, 0.92);
    float3 axisXCol = float3(0.95, 0.25, 0.25);
    float3 axisZCol = float3(0.25, 0.45, 1.00);

    float aMinor = minorMask * minorOp * overallOp;
    float aMajor = majorMask * majorOp * overallOp;
    float aAxisX = axisX * axisOp * overallOp;
    float aAxisZ = axisZ * axisOp * overallOp;

    // Mask out invalid hits
    float valid = (hitValid && !occludedByScene && !deferredByProximity) ? 1.0 : 0.0;
    aMinor *= valid;
    aMajor *= valid;
    aAxisX *= valid;
    aAxisZ *= valid;

    // Compose premultiplied layers
    float3 premul = 0.0;
    float a = 0.0;

    // major over minor
    {
        float3 minorPremul = minorCol * aMinor;
        float3 majorPremul = majorCol * aMajor;

        float3 outPremul = majorPremul + minorPremul * (1.0 - aMajor);
        a = aMajor + aMinor * (1.0 - aMajor);
        premul = outPremul;
    }

    // axes over
    {
        premul = premul + axisXCol * aAxisX * (1.0 - a);
        a = a + aAxisX * (1.0 - a);
    }
    {
        premul = premul + axisZCol * aAxisZ * (1.0 - a);
        a = a + aAxisZ * (1.0 - a);
    }

    // Read-modify-write HDR (premul over)
    if (inBounds)
    {
        float4 dst = hdr[pix];
        float3 outRGB = PremulOver(dst.rgb, a, premul);
        hdr[pix] = float4(outRGB, dst.a); // keep dst alpha
    }
}
