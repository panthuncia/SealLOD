#pragma once

#include <algorithm>
#include <cstdint>

inline constexpr const char* MaterialTextureStreamingSettingName = "enableMaterialTextureStreaming";
inline constexpr const char* AlphaTestedMaterialTextureMaxResidentTopMipSettingName =
    "alphaTestedMaterialTextureMaxResidentTopMip";
inline constexpr uint32_t AlphaTestedMaterialTextureMaxResidentTopMipDefault = 4u;
// Alpha-tested coverage collapses in coarse mips: box-filtered alpha drops below the
// cutoff and every fragment is discarded. Alpha-tested textures therefore keep at
// least this many texels along their larger axis resident (or the full texture).
inline constexpr const char* AlphaTestedMaterialTextureMinResidentDimensionSettingName =
    "alphaTestedMaterialTextureMinResidentDimension";
inline constexpr uint32_t AlphaTestedMaterialTextureMinResidentDimensionDefault = 128u;

bool IsMaterialTextureStreamingEnabledSetting();
uint32_t GetAlphaTestedMaterialTextureMaxResidentTopMipSetting();
uint32_t GetAlphaTestedMaterialTextureMinResidentDimensionSetting();

// The coarsest top mip an alpha-tested texture may have resident: the mip-index cap,
// further limited so the resident top mip keeps the minimum resident dimension.
inline uint32_t AlphaTestedMaterialTextureMaxResidentTopMip(
    uint32_t fullWidth, uint32_t fullHeight, uint32_t totalMipCount) {
    const uint32_t lastMip = totalMipCount == 0u ? 0u : totalMipCount - 1u;
    uint32_t topMip = (std::min)(lastMip, GetAlphaTestedMaterialTextureMaxResidentTopMipSetting());
    const uint32_t minimumDimension = GetAlphaTestedMaterialTextureMinResidentDimensionSetting();
    const uint32_t largestDimension = (std::max)(fullWidth, fullHeight);
    // An unknown source shape keeps the index cap rather than forcing mip 0.
    while (largestDimension != 0u && topMip > 0u && (largestDimension >> topMip) < minimumDimension) {
        --topMip;
    }
    return topMip;
}
