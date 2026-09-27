#pragma once

#include <cstdint>
#include <vector>

#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"

struct ReyesTessellationTableData
{
    std::vector<uint32_t> vertices;
    std::vector<uint32_t> triangles;
    std::vector<CLodReyesTessTableConfigEntry> configs;
};

const ReyesTessellationTableData& GetReyesTessellationTableData();