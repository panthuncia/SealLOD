#ifndef __SGGX_COMMON_HLSLI__
#define __SGGX_COMMON_HLSLI__

#ifndef SGGX_PI
#define SGGX_PI 3.14159265358979323846f
#endif

void SGGXBuildOrthonormalBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    direction = normalize(dot(direction, direction) > 1.0e-12f ? direction : float3(0.0f, 0.0f, 1.0f));
    const float3 helper = abs(direction.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    tangent = normalize(cross(helper, direction));
    bitangent = cross(direction, tangent);
}

float3 SGGXMul(float3 sDiag, float3 sOff, float3 v)
{
    return float3(
        sDiag.x * v.x + sOff.x * v.y + sOff.y * v.z,
        sOff.x * v.x + sDiag.y * v.y + sOff.z * v.z,
        sOff.y * v.x + sOff.z * v.y + sDiag.z * v.z);
}

float3 SGGXDecodeOctAxis(float2 encoded)
{
    float3 axis = float3(encoded.xy, 1.0f - abs(encoded.x) - abs(encoded.y));
    if (axis.z < 0.0f)
    {
        const float2 axisSign = float2(axis.x >= 0.0f ? 1.0f : -1.0f, axis.y >= 0.0f ? 1.0f : -1.0f);
        const float2 folded = (1.0f - abs(axis.yx)) * axisSign;
        axis.xy = folded;
    }
    return normalize(dot(axis, axis) > 1.0e-12f ? axis : float3(0.0f, 0.0f, 1.0f));
}

float3 SGGXMulAxial(float4 sggxAxisAndSigmas, float3 v)
{
    const float3 axis = SGGXDecodeOctAxis(sggxAxisAndSigmas.xy);
    const float sigmaPerp = max(sggxAxisAndSigmas.z, 1.0e-4f);
    const float sigmaParallel = max(sggxAxisAndSigmas.w, 1.0e-4f);
    const float sp2 = sigmaPerp * sigmaPerp;
    const float sa2 = sigmaParallel * sigmaParallel;
    return sp2 * v + (sa2 - sp2) * axis * dot(axis, v);
}

float SGGXDet(float3 sDiag, float3 sOff)
{
    const float sxx = sDiag.x;
    const float syy = sDiag.y;
    const float szz = sDiag.z;
    const float sxy = sOff.x;
    const float sxz = sOff.y;
    const float syz = sOff.z;
    return sxx * (syy * szz - syz * syz) - sxy * (sxy * szz - sxz * syz) + sxz * (sxy * syz - sxz * syy);
}

float3 SGGXDominantNormal(float3 sDiag, float3 sOff)
{
    float3 axis = float3(0.0f, 0.0f, 1.0f);
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        axis = normalize(SGGXMul(sDiag, sOff, axis));
    }
    return axis;
}

float3 SGGXDominantAxialNormal(float4 sggxAxisAndSigmas)
{
    return SGGXDecodeOctAxis(sggxAxisAndSigmas.xy);
}

float3 SGGXSampleVNDF(float4 sggxAxisAndSigmas, float3 wi, float u1, float u2)
{
    wi = normalize(dot(wi, wi) > 1.0e-12f ? wi : float3(0.0f, 0.0f, 1.0f));
    float3 k, j;
    SGGXBuildOrthonormalBasis(wi, k, j);

    const float3 Sk = SGGXMulAxial(sggxAxisAndSigmas, k);
    const float3 Sj = SGGXMulAxial(sggxAxisAndSigmas, j);
    const float3 Si = SGGXMulAxial(sggxAxisAndSigmas, wi);
    const float Skj = dot(k, Sj);
    const float Ski = dot(k, Si);
    const float Sjj = dot(j, Sj);
    const float Sji = dot(j, Si);
    float Sii = dot(wi, Si);

    const float eps = 1.0e-8f;
    const float sigmaPerp = max(sggxAxisAndSigmas.z, 1.0e-4f);
    const float sigmaParallel = max(sggxAxisAndSigmas.w, 1.0e-4f);
    const float sp2 = sigmaPerp * sigmaPerp;
    const float sa2 = sigmaParallel * sigmaParallel;
    const float detS = max(sp2 * sp2 * sa2, eps);
    Sii = max(Sii, eps);
    const float tmp = max(Sjj * Sii - Sji * Sji, eps);
    const float sqrtSii = sqrt(Sii);
    const float sqrtTmp = sqrt(tmp);

    const float3 Mk = float3(sqrt(detS / tmp), 0.0f, 0.0f);
    const float3 Mj = float3(
        -(Skj * Sii - Ski * Sji) / (sqrtSii * sqrtTmp),
        sqrtTmp / sqrtSii,
        0.0f);
    const float3 Mi = float3(Ski / sqrtSii, Sji / sqrtSii, sqrtSii);

    const float r = sqrt(saturate(u1));
    const float phi = 2.0f * SGGX_PI * u2;
    const float diskU = r * cos(phi);
    const float diskV = r * sin(phi);
    const float diskW = sqrt(max(0.0f, 1.0f - diskU * diskU - diskV * diskV));
    const float3 wmLocal = normalize(diskU * Mk + diskV * Mj + diskW * Mi);
    return normalize(wmLocal.x * k + wmLocal.y * j + wmLocal.z * wi);
}

#endif
