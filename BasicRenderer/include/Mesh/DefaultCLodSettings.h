#pragma once

#include "Mesh\ClusterLODTypes.h"

#include <cstdint>
#include <string_view>

ClusterLODBuilderSettings GetDefaultBuilderSettings(std::string_view assetIdentifier = {});
uint64_t GetCLodBuilderSettingsOverrideConfigHash();
