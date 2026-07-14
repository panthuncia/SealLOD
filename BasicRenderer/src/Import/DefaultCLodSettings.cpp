#include "Mesh/DefaultCLodSettings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
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
        char* configuredRaw = nullptr;
        size_t configuredLength = 0;
        if (_dupenv_s(&configuredRaw, &configuredLength, "SARP_ASSET_SETTINGS_CONFIG") == 0) {
            const std::unique_ptr<char, decltype(&std::free)> configured(configuredRaw, &std::free);
            if (configuredLength > 1) return std::filesystem::path(configured.get());
        }

        std::error_code ec;
        auto current = std::filesystem::current_path(ec);
        while (!ec && !current.empty()) {
            for (const auto& candidate : {
                current / "config" / "asset_settings.json",
                current / "asset_settings.json" }) {
                if (std::filesystem::is_regular_file(candidate, ec) && !ec) return candidate;
                ec.clear();
            }
            if (!current.has_parent_path() || current.parent_path() == current) break;
            current = current.parent_path();
        }
        return std::nullopt;
    }

    void HashString(uint64_t& hash, std::string_view value)
    {
        for (unsigned char byte : value) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        hash ^= 0xffu;
        hash *= 1099511628211ull;
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
                const auto document = nlohmann::json::parse(bytes);
                const auto& assets = document.at("assets");
                if (!assets.is_object()) throw std::runtime_error("'assets' must be an object");

                uint64_t hash = 1469598103934665603ull;
                for (const auto& [identity, assetSettings] : assets.items()) {
                    if (!assetSettings.is_object()) throw std::runtime_error("asset settings entries must be objects");
                    const auto clod = assetSettings.find("clod");
                    if (clod == assetSettings.end()) continue;
                    if (!clod->is_object()) throw std::runtime_error("asset 'clod' settings must be an object");

                    const std::string normalizedIdentity = NormalizeObject(identity);
                    if (normalizedIdentity.empty()) throw std::runtime_error("asset settings identity must be non-empty");
                    result.rules.push_back({ normalizedIdentity, *clod });
                    HashString(hash, normalizedIdentity);
                    HashString(hash, clod->dump());
                }
                result.hash = result.rules.empty() ? 0u : hash;
                spdlog::info("SARP asset CLod settings: loaded {} asset rule(s) from '{}'", result.rules.size(), path->string());
            } catch (const std::exception& error) {
                spdlog::error("SARP asset CLod settings: failed to load '{}': {}", path->string(), error.what());
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
        if (!object.is_object()) throw std::runtime_error("asset 'clod' settings must be an object");
        if (const auto it = object.find("coveragePreservationMode"); it != object.end()) {
            std::string mode = it->get<std::string>();
            if (mode == "none") {
                settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::None;
            } else if (mode == "prioritizeEdges") {
                settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
            } else if (mode == "voxel") {
                settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::Voxel;
            } else {
                throw std::runtime_error("coveragePreservationMode must be none, prioritizeEdges, or voxel");
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
    settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
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
                spdlog::error("SARP asset CLod settings for '{}' are invalid: {}", assetIdentifier, error.what());
            }
            break;
        }
    }
    settings = ApplyClusterLODBuilderEnvironmentOverrides(std::move(settings));
    if (matchedOverride) {
        const char* mode = settings.coveragePreservationMode == ClusterLODCoveragePreservationMode::None ? "none" :
            settings.coveragePreservationMode == ClusterLODCoveragePreservationMode::Voxel ? "voxel" : "prioritizeEdges";
        spdlog::info(
            "SARP asset CLod settings applied: object='{}' coverage_preservation_mode='{}' grid={} rays={} scale={}",
            assetIdentifier, mode, settings.voxelGridBaseResolution,
            settings.voxelRaysPerCell, settings.voxelFallbackScalingFactor);
    }
    return settings;
}

uint64_t GetCLodAssetSettingsConfigHash()
{
    return GetOverrideConfig().hash;
}
