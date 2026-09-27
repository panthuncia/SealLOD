#pragma once

#include <cstdint>

struct ReyesPackedTessellationTableSource
{
    uint32_t maxEdgeSegments = 0u;
    uint32_t maxVertices = 0u;
    //uint32_t maxTriangles = 0u;
    uint32_t maxConfigs = 0u;
    const uint32_t* vertices = nullptr;
    const uint32_t* triangles = nullptr;
    const uint16_t* configs = nullptr;
};

const ReyesPackedTessellationTableSource& GetGeneratedReyesPackedTessellationTableSource();