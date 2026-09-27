#pragma once

#include <cstdint>
#include <vector>

// Canonical adaptive patterns follow the ARKFPI paper's reduced-storage ordering:
// store only factor triplets where edge01 >= edge12 >= edge20 and derive the rest
// via runtime rotation / flip handling.
struct ReyesCanonicalPatternKey
{
    uint32_t edge01Segments = 0u;
    uint32_t edge12Segments = 0u;
    uint32_t edge20Segments = 0u;
};

bool IsCanonicalReyesPatternKey(const ReyesCanonicalPatternKey& key);
uint32_t CountCanonicalReyesPatternKeys(uint32_t maxEdgeSegments);
std::vector<ReyesCanonicalPatternKey> EnumerateCanonicalReyesPatternKeys(uint32_t maxEdgeSegments);
uint32_t EncodeReyesLookupIndex(uint32_t edge01Segments, uint32_t edge12Segments, uint32_t edge20Segments, uint32_t lookupSize);