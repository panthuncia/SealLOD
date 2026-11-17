#pragma once

#include <cstdint>
#include <span>

// CORTAO's lookup tables for 16 rays, as the words of the structured buffers CORTAOMain.hlsl reads. The tables themselves
// are vendored in include/ThirdParty/CORTAO and compiled into CORTAOLookupTables.cpp alone.
namespace br::ambient_occlusion {

// 24x24x16 texels (x fastest), one uint per texel holding the R16_UINT ray mask of occluded rays.
std::span<const uint32_t> CortaoOccluderLookupTableWords();

// 65536 R8G8_SNORM tangent-space bent normals, indexed by ray mask, two per uint (x in the low byte of each half).
std::span<const uint32_t> CortaoBentNormalLookupTableWords();

}
