#ifndef CLOD_RESOLVE_REYES_HLSLI
#define CLOD_RESOLVE_REYES_HLSLI

// Maps a Reyes micro-triangle's barycentrics back to its source triangle.
// Requires clodResolveTypes.hlsli.

float3 ReyesDecodeBarycentrics(float2 barycentricsUV)
{
    return ReyesPatchDomainUVToBarycentrics(barycentricsUV);
}

BarycentricDeriv ReyesComposeSourceBarycentrics(BarycentricDeriv patchBary, float3 domain0, float3 domain1, float3 domain2)
{
    BarycentricDeriv sourceBary = (BarycentricDeriv)0;
    sourceBary.m_lambda =
        domain0 * patchBary.m_lambda.x +
        domain1 * patchBary.m_lambda.y +
        domain2 * patchBary.m_lambda.z;
    sourceBary.m_ddx =
        domain0 * patchBary.m_ddx.x +
        domain1 * patchBary.m_ddx.y +
        domain2 * patchBary.m_ddx.z;
    sourceBary.m_ddy =
        domain0 * patchBary.m_ddy.x +
        domain1 * patchBary.m_ddy.y +
        domain2 * patchBary.m_ddy.z;
    return sourceBary;
}

#endif // CLOD_RESOLVE_REYES_HLSLI
