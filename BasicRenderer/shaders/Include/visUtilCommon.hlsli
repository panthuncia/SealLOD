#ifndef VIS_UTIL_COMMON_HLSLI
#define VIS_UTIL_COMMON_HLSLI

struct PixelRef
{
    uint pixelXY;
    uint2 visibilityKey;
};

#ifndef MATERIAL_EXECUTION_GROUP_SIZE
#define MATERIAL_EXECUTION_GROUP_SIZE 64u
#endif

#endif // VIS_UTIL_COMMON_HLSLI
