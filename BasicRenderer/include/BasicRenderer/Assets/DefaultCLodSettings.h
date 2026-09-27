#pragma once

#include <BasicRenderer/Assets/ClusterLODTypes.h>

#include <cstdint>
#include <string_view>

ClusterLODBuilderSettings GetDefaultBuilderSettings(std::string_view assetIdentifier = {});
uint64_t GetCLodAssetSettingsConfigHash();
uint64_t GetCLodAssetSettingsHash(std::string_view assetIdentifier);
