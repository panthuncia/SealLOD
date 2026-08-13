#include "../Include/cbuffers.hlsli"

[shader("compute")]
[numthreads(8, 8, 1)]
void CanonicalSurfaceFinalizeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 dimensions = uint2(UintRootConstant18, UintRootConstant19);
    if (any(dispatchThreadId.xy >= dimensions)) return;
    const uint2 pixel = dispatchThreadId.xy;

    Texture2D<float4> normals = ResourceDescriptorHeap[UintRootConstant0];
    Texture2D<float4> albedoAo = ResourceDescriptorHeap[UintRootConstant1];
    Texture2D<float4> coat = ResourceDescriptorHeap[UintRootConstant2];
    Texture2D<float4> fuzz = ResourceDescriptorHeap[UintRootConstant3];
    Texture2D<float4> metallicRoughness = ResourceDescriptorHeap[UintRootConstant4];
    Texture2D<float4> emissive = ResourceDescriptorHeap[UintRootConstant5];
    Texture2D<float2> motion = ResourceDescriptorHeap[UintRootConstant6];
    Texture2D<float> depth = ResourceDescriptorHeap[UintRootConstant7];

    RWTexture2D<float4> outBaseColorOpacity = ResourceDescriptorHeap[UintRootConstant8];
    RWTexture2D<float4> outNormalRoughness = ResourceDescriptorHeap[UintRootConstant9];
    RWTexture2D<float4> outSpecularAo = ResourceDescriptorHeap[UintRootConstant10];
    RWTexture2D<float4> outEmissive = ResourceDescriptorHeap[UintRootConstant11];
    RWTexture2D<float2> outMotion = ResourceDescriptorHeap[UintRootConstant12];
    RWTexture2D<float> outDepth = ResourceDescriptorHeap[UintRootConstant13];
    RWTexture2D<uint2> outIdentity = ResourceDescriptorHeap[UintRootConstant14];
    RWTexture2D<float4> outPayload0 = ResourceDescriptorHeap[UintRootConstant15];
    RWTexture2D<float4> outPayload1 = ResourceDescriptorHeap[UintRootConstant16];

    const float4 normalMaterial = normals[pixel];
    const float4 baseAo = albedoAo[pixel];
    const float4 mr = metallicRoughness[pixel];
    const bool background = dot(normalMaterial.xyz, normalMaterial.xyz) < 1.0e-8f;
    const float3 f0 = lerp(0.04f.xxx, baseAo.rgb, saturate(mr.x));

    // Canonical base color is resolved diffuse reflectance. Metallic response is
    // represented explicitly by RGB F0 in SpecularAo, so consumers never need
    // to reconstruct a metallic workflow.
    outBaseColorOpacity[pixel] = background ? 0.0f.xxxx : float4(baseAo.rgb * (1.0f - saturate(mr.x)), 1.0f);
    outNormalRoughness[pixel] = background ? 0.0f.xxxx : float4(normalize(normalMaterial.xyz), saturate(mr.y));
    outSpecularAo[pixel] = background ? 0.0f.xxxx : float4(f0, saturate(baseAo.a));
    outEmissive[pixel] = background ? 0.0f.xxxx : float4(emissive[pixel].rgb, 0.0f);
    outMotion[pixel] = background ? 0.0f.xx : motion[pixel];
    outDepth[pixel] = depth[pixel];
    outPayload0[pixel] = background ? 0.0f.xxxx : coat[pixel];
    outPayload1[pixel] = background ? 0.0f.xxxx : fuzz[pixel];

    if (background) {
        outIdentity[pixel] = uint2(0xffffffffu, 0u);
        return;
    }

}
