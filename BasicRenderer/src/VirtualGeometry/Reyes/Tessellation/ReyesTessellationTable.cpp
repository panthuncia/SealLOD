#include "VirtualGeometry/Reyes/Tessellation/ReyesTessellationTable.h"

#include <algorithm>
#include <stdexcept>

#include "VirtualGeometry/Reyes/Tessellation/ReyesTessellationReferenceSource.h"

namespace {

constexpr uint32_t kLookupSize = 16u;
constexpr uint32_t kMaxEdgeSegments = 11u;
constexpr uint32_t kLookupIndexBias =
    1u +
    kLookupSize +
    kLookupSize * kLookupSize;

struct PackedConfigEntry
{
    uint16_t firstTriangle = 0u;
    uint16_t firstVertex = 0u;
    uint16_t numTriangles = 0u;
    uint16_t numVertices = 0u;
};

uint32_t GetLookupIndex(uint32_t edge01Segments, uint32_t edge12Segments, uint32_t edge20Segments)
{
    if (edge01Segments == 0u || edge12Segments == 0u || edge20Segments == 0u) {
        throw std::runtime_error("Reyes tessellation table lookup indices must be positive.");
    }

    if (edge01Segments >= kLookupSize || edge12Segments >= kLookupSize || edge20Segments >= kLookupSize) {
        throw std::runtime_error("Reyes tessellation table lookup indices exceed the fixed lookup size.");
    }

    const uint32_t rawIndex =
        edge01Segments +
        edge12Segments * kLookupSize +
        edge20Segments * kLookupSize * kLookupSize;

    if (rawIndex < kLookupIndexBias) {
        throw std::runtime_error("Reyes tessellation table lookup index underflowed the shader bias.");
    }

    return rawIndex - kLookupIndexBias;
}

ReyesTessellationTableData BuildReferenceReyesTessellationTableData()
{
    const ReyesPackedTessellationTableSource& source = GetGeneratedReyesPackedTessellationTableSource();
    if (source.maxEdgeSegments != kMaxEdgeSegments) {
        throw std::runtime_error("Reyes tessellation table source max-edge-segment count drifted from the shader contract.");
    }

    if (source.maxEdgeSegments >= kLookupSize) {
        throw std::runtime_error("Reyes tessellation table source exceeds the fixed lookup size.");
    }

    ReyesTessellationTableData data;
    data.configs.resize(kLookupSize * kLookupSize * kLookupSize);

    const auto* packedConfigs = reinterpret_cast<const PackedConfigEntry*>(source.configs);

    uint32_t maxUsedVertexCount = 0u;
    uint32_t maxUsedTriangleCount = 0u;
    uint32_t configIndex = 0u;
    for (uint32_t edge01Segments = 1u; edge01Segments <= source.maxEdgeSegments; ++edge01Segments) {
        for (uint32_t edge12Segments = 1u; edge12Segments <= edge01Segments; ++edge12Segments) {
            for (uint32_t edge20Segments = 1u; edge20Segments <= edge12Segments; ++edge20Segments, ++configIndex) {
                if (configIndex >= source.maxConfigs) {
                    throw std::runtime_error("Reyes tessellation table config count exceeded the packed source size.");
                }

                const PackedConfigEntry packedEntry = packedConfigs[configIndex];
                const CLodReyesTessTableConfigEntry configEntry{
                    packedEntry.firstTriangle,
                    packedEntry.firstVertex,
                    packedEntry.numTriangles,
                    packedEntry.numVertices,
                };

                data.configs[GetLookupIndex(edge01Segments, edge12Segments, edge20Segments)] = configEntry;
                if (edge20Segments != edge12Segments && edge01Segments > 1u) {
                    data.configs[GetLookupIndex(edge01Segments, edge20Segments, edge12Segments)] = configEntry;
                }

                maxUsedVertexCount = (std::max)(maxUsedVertexCount, configEntry.firstVertex + configEntry.numVertices);
                maxUsedTriangleCount = (std::max)(maxUsedTriangleCount, configEntry.firstTriangle + configEntry.numTriangles);
            }
        }
    }

    if (configIndex != source.maxConfigs) {
        throw std::runtime_error("Reyes tessellation table config count does not match the packed source.");
    }

    maxUsedVertexCount = (std::max)(maxUsedVertexCount, source.maxVertices);
    data.vertices.assign(source.vertices, source.vertices + maxUsedVertexCount);
    data.triangles.assign(source.triangles, source.triangles + maxUsedTriangleCount);
    return data;
}

} // namespace

const ReyesTessellationTableData& GetReyesTessellationTableData()
{
    static const ReyesTessellationTableData generatedTableData = BuildReferenceReyesTessellationTableData();
    return generatedTableData;
}