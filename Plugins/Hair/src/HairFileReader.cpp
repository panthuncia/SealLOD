#include "Hair/HairFileReader.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace br::hair {

namespace {

    constexpr std::size_t kHeaderBytes = 128u;
    constexpr std::uint32_t kHasSegments = 1u << 0;
    constexpr std::uint32_t kHasPoints = 1u << 1;
    constexpr std::uint32_t kHasThickness = 1u << 2;
    constexpr std::uint32_t kHasTransparency = 1u << 3;
    constexpr std::uint32_t kHasColor = 1u << 4;

    // The file is little-endian, as is every host this builds for, so values are copied out directly.
    template <typename T>
    T ReadAt(const std::vector<char>& bytes, std::size_t offset)
    {
        T value;
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    bool Parse(const std::vector<char>& bytes, GroomSource& groom, std::string& error)
    {
        if (bytes.size() < kHeaderBytes) {
            error = "file is shorter than the 128-byte .hair header";
            return false;
        }
        if (std::memcmp(bytes.data(), "HAIR", 4u) != 0) {
            error = "missing HAIR signature";
            return false;
        }
        const auto strandCount = ReadAt<std::uint32_t>(bytes, 4u);
        const auto pointCount = ReadAt<std::uint32_t>(bytes, 8u);
        const auto flags = ReadAt<std::uint32_t>(bytes, 12u);
        const auto defaultSegments = ReadAt<std::uint32_t>(bytes, 16u);
        const auto defaultThickness = ReadAt<float>(bytes, 20u);
        if ((flags & kHasPoints) == 0u) {
            error = "file has no points array";
            return false;
        }
        if (strandCount == 0u || pointCount == 0u) {
            error = "file has no strands";
            return false;
        }

        // Each array is checked against the file size before it is read. Counts are 32-bit, so the 64-bit sizes
        // cannot overflow.
        std::uint64_t cursor = kHeaderBytes;
        const auto takeArray = [&](std::uint64_t byteCount, const char* name, std::size_t& offset) {
            if (cursor + byteCount > bytes.size()) {
                error = std::string("file is truncated in the ") + name + " array";
                return false;
            }
            offset = static_cast<std::size_t>(cursor);
            cursor += byteCount;
            return true;
        };

        std::size_t segmentsOffset = 0u, pointsOffset = 0u, thicknessOffset = 0u, transparencyOffset = 0u, colorOffset = 0u;
        if ((flags & kHasSegments) != 0u && !takeArray(std::uint64_t{ strandCount } * 2u, "segments", segmentsOffset)) return false;
        if (!takeArray(std::uint64_t{ pointCount } * 12u, "points", pointsOffset)) return false;
        if ((flags & kHasThickness) != 0u && !takeArray(std::uint64_t{ pointCount } * 4u, "thickness", thicknessOffset)) return false;
        if ((flags & kHasTransparency) != 0u && !takeArray(std::uint64_t{ pointCount } * 4u, "transparency", transparencyOffset)) return false;
        if ((flags & kHasColor) != 0u && !takeArray(std::uint64_t{ pointCount } * 12u, "colour", colorOffset)) return false;

        groom.strandPointOffsets.reserve(std::size_t{ strandCount } + 1u);
        groom.strandPointOffsets.push_back(0u);
        std::uint64_t pointTotal = 0u;
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            const std::uint32_t segments =
                (flags & kHasSegments) != 0u ? ReadAt<std::uint16_t>(bytes, segmentsOffset + std::size_t{ strand } * 2u) : defaultSegments;
            if (segments == 0u) {
                error = "strand " + std::to_string(strand) + " has no segments";
                return false;
            }
            pointTotal += std::uint64_t{ segments } + 1u;
            if (pointTotal > pointCount) {
                break;
            }
            groom.strandPointOffsets.push_back(static_cast<std::uint32_t>(pointTotal));
        }
        if (pointTotal != pointCount) {
            error = "segment counts do not add up to the header's point count";
            return false;
        }

        groom.positions.resize(pointCount);
        std::memcpy(groom.positions.data(), bytes.data() + pointsOffset, std::size_t{ pointCount } * 12u);
        groom.widths.assign(pointCount, defaultThickness);
        if ((flags & kHasThickness) != 0u) {
            std::memcpy(groom.widths.data(), bytes.data() + thicknessOffset, std::size_t{ pointCount } * 4u);
        }
        if ((flags & kHasColor) != 0u) {
            groom.colors.resize(pointCount);
            std::memcpy(groom.colors.data(), bytes.data() + colorOffset, std::size_t{ pointCount } * 12u);
        }

        for (std::uint32_t point = 0; point < pointCount; ++point) {
            const Float3& position = groom.positions[point];
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) || !std::isfinite(groom.widths[point])) {
                error = "point " + std::to_string(point) + " has a non-finite position or thickness";
                return false;
            }
        }
        return true;
    }

} // namespace

static_assert(sizeof(Float3) == 12u, "positions and colours are copied straight from the file");

bool LoadHairFile(const std::filesystem::path& path, GroomSource& groom, std::string* error)
{
    groom = {};
    std::string message;
    std::vector<char> bytes;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        message = "cannot open file";
    } else {
        bytes.resize(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            message = "cannot read file";
        } else if (Parse(bytes, groom, message)) {
            return true;
        }
    }
    groom = {};
    if (error != nullptr) {
        *error = path.string() + ": " + message;
    }
    return false;
}

} // namespace br::hair
