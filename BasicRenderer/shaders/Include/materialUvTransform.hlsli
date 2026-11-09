#ifndef MATERIAL_UV_TRANSFORM_HLSLI
#define MATERIAL_UV_TRANSFORM_HLSLI

#include "include/structs.hlsli"

// A material's KHR_texture_transform (MaterialUvTransform, MaterialDescription.h), applied to a material texture's UV
// after its UV set is selected. One transform covers every texture slot of the material. The tables store the 2x2
// linear part minus identity and the offset (BuildMaterialUvTransformGpu), so a zeroed MaterialInfo is the identity:
//   u' = u + dot(delta.xy, uv) + offset.x
//   v' = v + dot(delta.zw, uv) + offset.y
// UV gradients take the linear part only.

float2 ApplyMaterialUvTransformToUv(float4 linearDelta, float2 offset, float2 uv)
{
    return uv + float2(dot(linearDelta.xy, uv), dot(linearDelta.zw, uv)) + offset;
}

float2 ApplyMaterialUvTransformToGradient(float4 linearDelta, float2 dUV)
{
    return dUV + float2(dot(linearDelta.xy, dUV), dot(linearDelta.zw, dUV));
}

float2 TransformMaterialUv(MaterialInfo materialInfo, float2 uv)
{
    return ApplyMaterialUvTransformToUv(materialInfo.uvTransformLinearDelta, materialInfo.uvTransformOffset, uv);
}

float2 TransformMaterialUv(MaterialEvalInfo materialInfo, float2 uv)
{
    return ApplyMaterialUvTransformToUv(materialInfo.uvTransformLinearDelta, materialInfo.uvTransformOffset, uv);
}

float2 TransformMaterialUvGradient(MaterialInfo materialInfo, float2 dUV)
{
    return ApplyMaterialUvTransformToGradient(materialInfo.uvTransformLinearDelta, dUV);
}

float2 TransformMaterialUvGradient(MaterialEvalInfo materialInfo, float2 dUV)
{
    return ApplyMaterialUvTransformToGradient(materialInfo.uvTransformLinearDelta, dUV);
}

// How the transform scales an isotropic UV footprint (a ray cone's): the square root of its linear part's determinant.
float MaterialUvTransformFootprintScale(float4 linearDelta)
{
    const float m00 = 1.0f + linearDelta.x;
    const float m11 = 1.0f + linearDelta.w;
    return sqrt(abs(m00 * m11 - linearDelta.y * linearDelta.z));
}

void TransformMaterialUvAndGradients(MaterialInfo materialInfo, inout float2 uv, inout float2 dUVdx, inout float2 dUVdy)
{
    uv = TransformMaterialUv(materialInfo, uv);
    dUVdx = TransformMaterialUvGradient(materialInfo, dUVdx);
    dUVdy = TransformMaterialUvGradient(materialInfo, dUVdy);
}

void TransformMaterialUvAndGradients(MaterialEvalInfo materialInfo, inout float2 uv, inout float2 dUVdx, inout float2 dUVdy)
{
    uv = TransformMaterialUv(materialInfo, uv);
    dUVdx = TransformMaterialUvGradient(materialInfo, dUVdx);
    dUVdy = TransformMaterialUvGradient(materialInfo, dUVdy);
}

#endif // MATERIAL_UV_TRANSFORM_HLSLI
