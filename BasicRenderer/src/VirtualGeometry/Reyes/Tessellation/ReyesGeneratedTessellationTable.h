#pragma once

#include <cstdint>

#include "VirtualGeometry/Reyes/Tessellation/ReyesTessellationTable.h"

ReyesTessellationTableData BuildGeneratedReyesTessellationTableData(uint32_t maxEdgeSegments, uint32_t lookupSize);