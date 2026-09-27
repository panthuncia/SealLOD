#pragma once

#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include <string_view>

void EmitClusterLODPagePackingTelemetry(
    std::string_view buildKind,
    const ClusterLODPrebuiltData& data,
    const std::vector<std::vector<std::byte>>& pageBlobs);
