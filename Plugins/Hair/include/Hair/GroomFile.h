#pragma once

#include "Hair/GroomBuilder.h"
#include "Hair/HairMath.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace br::hair {

// Bump with any change to GroomFileHeader, to the array list, or to a struct in HairGPUTypes.h.
inline constexpr std::uint32_t kGroomSchemaVersion = 1u;
inline constexpr char kGroomFileMagic[8] = { 'B', 'R', 'G', 'R', 'O', 'O', 'M', '\0' };

// The arrays of a BuiltGroom, in file order.
enum class GroomFileArray : std::uint32_t {
    Clusters,
    Aggregates,
    Strands,
    RootBindings,
    GuideBindings,
    GuideStrandIndices,
    GuideLevelOffsets,
    Points,
    Count
};

struct GroomFileArrayDesc {
    std::uint64_t byteOffset; // from the start of the file
    std::uint32_t count;      // elements
    std::uint32_t stride;     // bytes per element; must equal the reader's struct size
};

// A .groom file is this header followed by the raw arrays (little-endian, no compression).
struct GroomFileHeader {
    char magic[8];
    std::uint32_t schemaVersion;
    std::uint32_t headerBytes;
    std::uint64_t fileBytes;
    Float3 boundsMin;
    Float3 boundsMax;
    GroomFileArrayDesc arrays[static_cast<std::uint32_t>(GroomFileArray::Count)];
};
static_assert(sizeof(GroomFileHeader) == 176u);

bool SaveGroomFile(const std::filesystem::path& path, const BuiltGroom& groom, std::string* error = nullptr);

// Checks the magic, schema version, sizes and every array's bounds, then runs ValidateBuiltGroom. Returns false and
// sets `error` (when given) on any failure; `groom` is left empty in that case.
bool LoadGroomFile(const std::filesystem::path& path, BuiltGroom& groom, std::string* error = nullptr);

} // namespace br::hair
