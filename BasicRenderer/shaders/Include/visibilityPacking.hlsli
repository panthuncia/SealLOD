#ifndef VISIBILITY_PACKING_HLSL
#define VISIBILITY_PACKING_HLSL

// 31 (depth) | 26 (cluster) | 7 (tri)
// Depth keeps float ordering for positive values by dropping the least-significant bit
// of the IEEE-754 payload, which frees one bit for the Reyes synthetic patch index range.
static const uint TRI_BITS = 7;
static const uint CLUSTER_BITS = 26;
static const uint META_BITS = TRI_BITS + CLUSTER_BITS; // 33

uint64_t PackVisKey(float depth, uint clusterId, uint triId)
{
    // Raw IEEE-754 bits (order-preserving for [0,1] non-NaN floats)
    uint depthBits = asuint(depth) >> 1u;
    const uint triMask = (1u << TRI_BITS) - 1u;
    const uint clusterMask = (1u << CLUSTER_BITS) - 1u;

    uint64_t key =
        ((uint64_t) depthBits << META_BITS) |
        ((uint64_t) (clusterId & clusterMask) << TRI_BITS) |
        ((uint64_t) (triId & triMask));

    return key;
}

void UnpackVisKey(uint64_t key, out float depth, out uint clusterId, out uint triId)
{
    const uint triMask = (1u << TRI_BITS) - 1u; // 0x7F
    const uint clusterMask = (1u << CLUSTER_BITS) - 1u; // 0x3FFFFFF

    triId = (uint) (key & triMask);
    clusterId = (uint) ((key >> TRI_BITS) & clusterMask);

    uint depthBits = (uint) (key >> META_BITS);
    depthBits <<= 1u;
    depth = asfloat(depthBits);
}

float UnpackVisKeyDepth(uint64_t key)
{
    uint depthBits = (uint)(key >> META_BITS);
    depthBits <<= 1u;
    return asfloat(depthBits);
}

#endif // VISIBILITY_PACKING_HLSL
