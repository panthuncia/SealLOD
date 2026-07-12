#include "Mesh/DefaultCLodSettings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace
{
    struct OverrideRule
    {
        std::string object;
        nlohmann::json settings;
    };

    struct OverrideConfig
    {
        std::vector<OverrideRule> rules;
        uint64_t hash = 0u;
    };

    std::string NormalizeObject(std::string_view value)
    {
        std::string result(value.substr(0, value.find('#')));
        std::replace(result.begin(), result.end(), '\\', '/');
        std::ranges::transform(result, result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        while (result.starts_with("./")) result.erase(0, 2);
        return result;
    }

    std::optional<std::filesystem::path> FindConfigPath()
    {
        if (const char* configured = std::getenv("SARP_CLOD_BUILDER_CONFIG"); configured && *configured) {
            return std::filesystem::path(configured);
        }
        const std::filesystem::path candidates[] = {
            std::filesystem::current_path() / "config" / "clod_builder_overrides.json",
            std::filesystem::current_path() / "clod_builder_overrides.json",
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate)) return candidate;
        }
        return std::nullopt;
    }

    const OverrideConfig& GetOverrideConfig()
    {
        static const OverrideConfig config = [] {
            OverrideConfig result;
            const auto path = FindConfigPath();
            if (!path) return result;
            try {
                std::ifstream input(*path, std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(input)), {});
                uint64_t hash = 1469598103934665603ull;
                for (unsigned char byte : bytes) {
                    hash ^= byte;
                    hash *= 1099511628211ull;
                }
                result.hash = hash;
                const auto document = nlohmann::json::parse(bytes);
                const auto& overrides = document.at("overrides");
                if (!overrides.is_array()) throw std::runtime_error("'overrides' must be an array");
                for (const auto& entry : overrides) {
                    if (!entry.is_object() || !entry.contains("object") || !entry.contains("settings")) {
                        throw std::runtime_error("each override requires 'object' and 'settings'");
                    }
                    result.rules.push_back({ NormalizeObject(entry.at("object").get<std::string>()), entry.at("settings") });
                }
                spdlog::info("SARP CLod builder overrides: loaded {} rule(s) from '{}'", result.rules.size(), path->string());
            } catch (const std::exception& error) {
                spdlog::error("SARP CLod builder overrides: failed to load '{}': {}", path->string(), error.what());
                result = {};
            }
            return result;
        }();
        return config;
    }

    template <class T>
    void ReadSetting(const nlohmann::json& object, const char* name, T& value)
    {
        if (const auto it = object.find(name); it != object.end()) value = it->get<T>();
    }

    void ApplySettings(const nlohmann::json& object, ClusterLODBuilderSettings& settings)
    {
        if (!object.is_object()) throw std::runtime_error("override 'settings' must be an object");
        if (const auto it = object.find("voxelFallbackMode"); it != object.end()) {
            std::string mode = it->get<std::string>();
            std::ranges::transform(mode, mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (mode == "mesh" || mode == "mesh-only") {
                settings.enableVoxelFallback = false;
                settings.voxelFallbackMode = ClusterLODVoxelFallbackMode::MeshOnly;
            } else if (mode == "auto") {
                settings.enableVoxelFallback = true;
                settings.voxelFallbackMode = ClusterLODVoxelFallbackMode::Auto;
            } else if (mode == "voxel" || mode == "voxel-only") {
                settings.enableVoxelFallback = true;
                settings.voxelFallbackMode = ClusterLODVoxelFallbackMode::VoxelOnly;
            } else {
                throw std::runtime_error("voxelFallbackMode must be mesh-only, auto, or voxel-only");
            }
        }
        ReadSetting(object, "disableSloppyFallback", settings.disableSloppyFallback);
        ReadSetting(object, "sloppyFallbackErrorFactor", settings.sloppyFallbackErrorFactor);
        ReadSetting(object, "lodErrorMergePrevious", settings.lodErrorMergePrevious);
        ReadSetting(object, "lodErrorMergeAdditive", settings.lodErrorMergeAdditive);
        ReadSetting(object, "partitionSizeFloor", settings.partitionSizeFloor);
        ReadSetting(object, "preserveImportedNormals", settings.preserveImportedNormals);
        ReadSetting(object, "enableNormalAttributeSimplification", settings.enableNormalAttributeSimplification);
        ReadSetting(object, "normalAttributeWeight", settings.normalAttributeWeight);
        ReadSetting(object, "simplifyTangentWeight", settings.simplifyTangentWeight);
        ReadSetting(object, "simplifyTangentSignWeight", settings.simplifyTangentSignWeight);
        ReadSetting(object, "enableVoxelFallback", settings.enableVoxelFallback);
        ReadSetting(object, "voxelGridBaseResolution", settings.voxelGridBaseResolution);
        ReadSetting(object, "voxelMinResolution", settings.voxelMinResolution);
        ReadSetting(object, "voxelRaysPerCell", settings.voxelRaysPerCell);
        ReadSetting(object, "voxelFallbackScalingFactor", settings.voxelFallbackScalingFactor);
        ReadSetting(object, "voxelFallbackMaxRetryCount", settings.voxelFallbackMaxRetryCount);
        ReadSetting(object, "voxelFallbackGrowthFactor", settings.voxelFallbackGrowthFactor);
        ReadSetting(object, "voxelFallbackAcceptanceBias", settings.voxelFallbackAcceptanceBias);
        ReadSetting(object, "voxelFallbackOpacityThreshold", settings.voxelFallbackOpacityThreshold);
        ReadSetting(object, "voxelTailMaxLevels", settings.voxelTailMaxLevels);
        ReadSetting(object, "voxelTailGrowthFactor", settings.voxelTailGrowthFactor);
        ReadSetting(object, "doubleSidedVoxelSourceNormals", settings.doubleSidedVoxelSourceNormals);
    }
}

ClusterLODBuilderSettings GetDefaultBuilderSettings(std::string_view assetIdentifier)
{
    ClusterLODBuilderSettings settings;
    settings.disableSloppyFallback = false;
    settings.sloppyFallbackErrorFactor = 2.0f;
    settings.lodErrorMergePrevious = 1.5f;
    settings.lodErrorMergeAdditive = 0.0f;
    settings.partitionSizeFloor = 8u;
    settings.preserveImportedNormals = true;
    settings.enableNormalAttributeSimplification = true;
    settings.normalAttributeWeight = 1.0f;
    settings.simplifyTangentWeight = 0.01f;
    settings.simplifyTangentSignWeight = 0.5f;
    settings.enableVoxelFallback = true;
    settings.voxelFallbackMode = ClusterLODVoxelFallbackMode::Auto;
    settings.voxelGridBaseResolution = 32u;
    settings.voxelMinResolution = 0u;
    settings.voxelRaysPerCell = 8u;
    settings.voxelFallbackScalingFactor = 2.5f;
    settings.voxelFallbackMaxRetryCount = 10u;
    settings.voxelFallbackGrowthFactor = 1.1f;
    settings.voxelFallbackAcceptanceBias = 1.0f;
    settings.voxelFallbackOpacityThreshold = 0.0f;
    settings.voxelTailMaxLevels = 4u;
    settings.voxelTailGrowthFactor = 1.5f;

    const std::string normalized = NormalizeObject(assetIdentifier);
    bool matchedOverride = false;
    for (const auto& rule : GetOverrideConfig().rules) {
        if (normalized == rule.object || (normalized.size() > rule.object.size() && normalized.ends_with('/' + rule.object))) {
            try {
                ApplySettings(rule.settings, settings);
                matchedOverride = true;
            } catch (const std::exception& error) {
                spdlog::error("SARP CLod builder override for '{}' is invalid: {}", assetIdentifier, error.what());
            }
            break;
        }
    }
    settings = ApplyClusterLODBuilderEnvironmentOverrides(std::move(settings));
    if (matchedOverride) {
        const char* mode = settings.voxelFallbackMode == ClusterLODVoxelFallbackMode::MeshOnly ? "mesh-only" :
            settings.voxelFallbackMode == ClusterLODVoxelFallbackMode::VoxelOnly ? "voxel-only" : "auto";
        spdlog::info(
            "SARP CLod builder override applied: object='{}' voxel_enabled={} voxel_mode='{}' grid={} rays={} scale={}",
            assetIdentifier, settings.enableVoxelFallback, mode, settings.voxelGridBaseResolution,
            settings.voxelRaysPerCell, settings.voxelFallbackScalingFactor);
    }
    return settings;
}

uint64_t GetCLodBuilderSettingsOverrideConfigHash()
{
    return GetOverrideConfig().hash;
}
