#include "PostProcessing/AmbientOcclusion/CORTAOLookupTables.h"

#include <array>
#include <cstddef>
#include <cstring>

#include "ThirdParty/CORTAO/CORTAOBentNormalLookupTable16Rays.h"
#include "ThirdParty/CORTAO/CORTAOOccluderLookupTable16Rays.h"

namespace br::ambient_occlusion {

namespace {

constexpr size_t kOccluderTexels = size_t(CORTAO::occluderLookupTable16RaysWidth) * CORTAO::occluderLookupTable16RaysHeight
    * CORTAO::occluderLookupTable16RaysDepth;
static_assert(sizeof(CORTAO::occluderLookupTable16Rays) / sizeof(uint16_t) == kOccluderTexels);
static_assert(sizeof(CORTAO::bentNormalLookupTable16Rays) == 65536u * 2u);

constexpr std::array<uint32_t, kOccluderTexels> MakeOccluderWords() {
    std::array<uint32_t, kOccluderTexels> words{};
    for (size_t i = 0; i < kOccluderTexels; ++i) {
        words[i] = CORTAO::occluderLookupTable16Rays[i];
    }
    return words;
}

const std::array<uint32_t, kOccluderTexels> kOccluderWords = MakeOccluderWords();

// The table's bytes are x, y per entry, in entry order: copied as they are, little-endian, entry 2n lands in the low half of
// word n with x in its low byte.
std::array<uint32_t, 65536u / 2u> MakeBentNormalWords() {
    std::array<uint32_t, 65536u / 2u> words{};
    std::memcpy(words.data(), CORTAO::bentNormalLookupTable16Rays, sizeof(CORTAO::bentNormalLookupTable16Rays));
    return words;
}

const std::array<uint32_t, 65536u / 2u> kBentNormalWords = MakeBentNormalWords();

}

std::span<const uint32_t> CortaoOccluderLookupTableWords() { return kOccluderWords; }

std::span<const uint32_t> CortaoBentNormalLookupTableWords() { return kBentNormalWords; }

}
