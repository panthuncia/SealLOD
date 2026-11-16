#ifndef __DYNAMIC_SWIZZLE_HLSLI__
#define __DYNAMIC_SWIZZLE_HLSLI__

// Components are picked with selects: v[idx] with a run-time idx made DXC spill the vector to an indexable array, one per
// call (eight in a four-texture material evaluation variant). An index past the end reads the last component.
float DynamicSwizzleSelect(float2 v, uint idx)
{
    return idx == 0u ? v.x : v.y;
}
float DynamicSwizzleSelect(float3 v, uint idx)
{
    return idx == 0u ? v.x : (idx == 1u ? v.y : v.z);
}
float DynamicSwizzleSelect(float4 v, uint idx)
{
    return idx == 0u ? v.x : (idx == 1u ? v.y : (idx == 2u ? v.z : v.w));
}

// ============================================================
// 1-component extract (always returns a scalar)
// idx0 = 0...3
float DynamicSwizzle(float2 v, uint idx)
{
    return DynamicSwizzleSelect(v, idx);
}
float DynamicSwizzle(float3 v, uint idx)
{
    return DynamicSwizzleSelect(v, idx);
}
float DynamicSwizzle(float4 v, uint idx)
{
    return DynamicSwizzleSelect(v, idx);
}

// ============================================================
// 2-component extract (returns float2)
// idx0, idx1 = 0...3
float2 DynamicSwizzle(float v, uint2 idx)
{
    float x = v;
    float y = v;
    return float2(x, y);
}
float2 DynamicSwizzle(float2 v, uint2 idx)
{
    return float2(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y));

}
float2 DynamicSwizzle(float3 v, uint2 idx)
{
    return float2(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y));

}
float2 DynamicSwizzle(float4 v, uint2 idx)
{
    return float2(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y));
}

// ============================================================
// 3-component extract (returns float3)
// idx0, idx1, idx2 = 0...3
float3 DynamicSwizzle(float v, uint3 idx)
{
    return float3(v, v, v);
}
float3 DynamicSwizzle(float2 v, uint3 idx)
{
    return float3(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y), DynamicSwizzleSelect(v, idx.z));
}
float3 DynamicSwizzle(float3 v, uint3 idx)
{
    return float3(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y), DynamicSwizzleSelect(v, idx.z));
}
float3 DynamicSwizzle(float4 v, uint3 idx)
{
    return float3(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y), DynamicSwizzleSelect(v, idx.z));
}

// ============================================================
// 4-component extract (returns float4)
// idx0, idx1, idx2, idx3 = 0...3
float4 DynamicSwizzle(float v, uint4 idx)
{
    return float4(v, v, v, v);
}
float4 DynamicSwizzle(float2 v, uint4 idx)
{
    return float4(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y), DynamicSwizzleSelect(v, idx.z), DynamicSwizzleSelect(v, idx.w));
}
float4 DynamicSwizzle(float3 v, uint4 idx)
{
    return float4(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y), DynamicSwizzleSelect(v, idx.z), DynamicSwizzleSelect(v, idx.w));
}
float4 DynamicSwizzle(float4 v, uint4 idx)
{
    return float4(DynamicSwizzleSelect(v, idx.x), DynamicSwizzleSelect(v, idx.y), DynamicSwizzleSelect(v, idx.z), DynamicSwizzleSelect(v, idx.w));
}

#endif // __DYNAMIC_SWIZZLE_HLSLI__