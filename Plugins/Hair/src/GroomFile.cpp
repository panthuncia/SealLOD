#include "Hair/GroomFile.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace br::hair {

namespace {

    constexpr std::uint32_t kArrayCount = static_cast<std::uint32_t>(GroomFileArray::Count);
    constexpr const char* kArrayNames[kArrayCount] = { "cluster",     "aggregate",          "strand",            "root binding",
                                                       "guide binding", "guide strand index", "guide level offset", "point" };

    // Calls visit(array, vector) for each array of the groom, in file order. `Groom` is BuiltGroom or const BuiltGroom.
    template <typename Groom, typename Visit>
    bool ForEachArray(Groom& groom, Visit&& visit)
    {
        return visit(GroomFileArray::Clusters, groom.clusters) && visit(GroomFileArray::Aggregates, groom.aggregates) &&
               visit(GroomFileArray::Strands, groom.strands) && visit(GroomFileArray::RootBindings, groom.rootBindings) &&
               visit(GroomFileArray::GuideBindings, groom.guideBindings) && visit(GroomFileArray::GuideStrandIndices, groom.guideStrandIndices) &&
               visit(GroomFileArray::GuideLevelOffsets, groom.guideLevelOffsets) && visit(GroomFileArray::Points, groom.points);
    }

    bool Parse(const std::vector<char>& bytes, BuiltGroom& groom, std::string& error)
    {
        if (bytes.size() < sizeof(GroomFileHeader)) {
            error = "file is shorter than the .groom header";
            return false;
        }
        GroomFileHeader header;
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (std::memcmp(header.magic, kGroomFileMagic, sizeof(kGroomFileMagic)) != 0) {
            error = "missing .groom magic";
            return false;
        }
        if (header.schemaVersion != kGroomSchemaVersion) {
            error = "schema version " + std::to_string(header.schemaVersion) + " is not the supported version " + std::to_string(kGroomSchemaVersion);
            return false;
        }
        if (header.headerBytes != sizeof(GroomFileHeader)) {
            error = "unexpected header size";
            return false;
        }
        if (header.fileBytes != bytes.size()) {
            error = "file is truncated or padded: header says " + std::to_string(header.fileBytes) + " bytes, file has " + std::to_string(bytes.size());
            return false;
        }

        const bool arraysRead = ForEachArray(groom, [&](GroomFileArray array, auto& values) {
            const GroomFileArrayDesc& desc = header.arrays[static_cast<std::uint32_t>(array)];
            const char* name = kArrayNames[static_cast<std::uint32_t>(array)];
            if (desc.stride != sizeof(values[0])) {
                error = std::string("unexpected element size in the ") + name + " array";
                return false;
            }
            const std::uint64_t byteCount = std::uint64_t{ desc.count } * desc.stride;
            if (desc.byteOffset < sizeof(GroomFileHeader) || desc.byteOffset > bytes.size() || byteCount > bytes.size() - desc.byteOffset) {
                error = std::string("the ") + name + " array lies outside the file";
                return false;
            }
            values.resize(desc.count);
            if (byteCount != 0u) {
                std::memcpy(values.data(), bytes.data() + desc.byteOffset, static_cast<std::size_t>(byteCount));
            }
            return true;
        });
        if (!arraysRead) {
            return false;
        }
        groom.boundsMin = header.boundsMin;
        groom.boundsMax = header.boundsMax;
        error = ValidateBuiltGroom(groom);
        return error.empty();
    }

} // namespace

bool SaveGroomFile(const std::filesystem::path& path, const BuiltGroom& groom, std::string* error)
{
    GroomFileHeader header{};
    std::memcpy(header.magic, kGroomFileMagic, sizeof(kGroomFileMagic));
    header.schemaVersion = kGroomSchemaVersion;
    header.headerBytes = sizeof(GroomFileHeader);
    header.boundsMin = groom.boundsMin;
    header.boundsMax = groom.boundsMax;
    std::uint64_t cursor = sizeof(GroomFileHeader);
    ForEachArray(groom, [&](GroomFileArray array, const auto& values) {
        GroomFileArrayDesc& desc = header.arrays[static_cast<std::uint32_t>(array)];
        desc.byteOffset = cursor;
        desc.count = static_cast<std::uint32_t>(values.size());
        desc.stride = sizeof(values[0]);
        cursor += std::uint64_t{ desc.count } * desc.stride;
        return true;
    });
    header.fileBytes = cursor;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    ForEachArray(groom, [&](GroomFileArray, const auto& values) {
        file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(values[0])));
        return true;
    });
    file.flush();
    if (!file) {
        if (error != nullptr) {
            *error = path.string() + ": cannot write file";
        }
        return false;
    }
    return true;
}

bool LoadGroomFile(const std::filesystem::path& path, BuiltGroom& groom, std::string* error)
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
