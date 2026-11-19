#include <BasicRenderer/Assets/DefaultCLodSettings.h>

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
        uint64_t hash = 0u;
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
                    uint64_t ruleHash = 1469598103934665603ull;
                    HashString(ruleHash, normalizedIdentity);
                    HashString(ruleHash, clod->dump());
                    result.rules.push_back({ normalizedIdentity, *clod, ruleHash });
                    HashString(hash, normalizedIdentity);
                    HashString(hash, clod->dump());
                }
                result.hash = result.rules.empty() ? 0u : hash;
                spdlog::info(
                    "SARP asset CLod settings: loaded {} asset rule(s) from '{}' config_hash=0x{:016X}",
                    result.rules.size(),
                    path->string(),
                    result.hash);
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
                // A voxel rule keeps a triangle branch for the views without voxels (ray tracing): a forced fork.
                settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
                settings.voxelFork = ClusterLODVoxelForkMode::Forced;
            } else if (mode == "voxelReplace") {
                settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::Voxel;
            } else {
                throw std::runtime_error("coveragePreservationMode must be none, prioritizeEdges, voxel, or voxelReplace");
            }
        }
        if (const auto it = object.find("voxelFork"); it != object.end()) {
            const std::string fork = it->get<std::string>();
            if (fork == "off") {
                settings.voxelFork = ClusterLODVoxelForkMode::Off;
            } else if (fork == "auto") {
                settings.voxelFork = ClusterLODVoxelForkMode::Auto;
            } else if (fork == "forced") {
                settings.voxelFork = ClusterLODVoxelForkMode::Forced;
            } else {
                throw std::runtime_error("voxelFork must be off, auto, or forced");
            }
        }
        ReadSetting(object, "voxelForkStallRatio", settings.voxelForkStallRatio);
        ReadSetting(object, "voxelForkMinTriangles", settings.voxelForkMinTriangles);
        ReadSetting(object, "voxelForkDoubleSidedOnly", settings.voxelForkDoubleSidedOnly);
        ReadSetting(object, "voxelForkSurfaceLoss", settings.voxelForkSurfaceLoss);
        ReadSetting(object, "disableSloppyFallback", settings.disableSloppyFallback);
        ReadSetting(object, "sloppyLastResort", settings.sloppyLastResort);
        ReadSetting(object, "sloppyLastResortRejected", settings.sloppyLastResortRejected);
        ReadSetting(object, "sloppyLastResortFit", settings.sloppyLastResortFit);
        ReadSetting(object, "sloppyLastResortErrorFactor", settings.sloppyLastResortErrorFactor);
        ReadSetting(object, "sloppyFallbackErrorFactor", settings.sloppyFallbackErrorFactor);
        ReadSetting(object, "lodErrorMergePrevious", settings.lodErrorMergePrevious);
        ReadSetting(object, "lodErrorMergeAdditive", settings.lodErrorMergeAdditive);
        ReadSetting(object, "partitionSizeFloor", settings.partitionSizeFloor);
        ReadSetting(object, "nodeBoneLimit", settings.nodeBoneLimit);
        ReadSetting(object, "preserveImportedNormals", settings.preserveImportedNormals);
        ReadSetting(object, "enableNormalAttributeSimplification", settings.enableNormalAttributeSimplification);
        ReadSetting(object, "normalAttributeWeight", settings.normalAttributeWeight);
        ReadSetting(object, "simplifyPermissive", settings.simplifyPermissive);
        ReadSetting(object, "simplifyValidation", settings.simplifyValidation);
        ReadSetting(object, "simplifyValidationRetries", settings.simplifyValidationRetries);
        ReadSetting(object, "simplifyValidationFoldAngle", settings.simplifyValidationFoldAngle);
        ReadSetting(object, "simplifyValidationFoldArea", settings.simplifyValidationFoldArea);
        ReadSetting(object, "simplifyValidationShadingAngle", settings.simplifyValidationShadingAngle);
        ReadSetting(object, "simplifyValidationShadingArea", settings.simplifyValidationShadingArea);
        ReadSetting(object, "simplifyValidationRadiusScale", settings.simplifyValidationRadiusScale);
        ReadSetting(object, "simplifyValidationCoverageArea", settings.simplifyValidationCoverageArea);
        ReadSetting(object, "simplifyUpdate", settings.simplifyUpdate);
        ReadSetting(object, "simplifyUpdatePositions", settings.simplifyUpdatePositions);
        ReadSetting(object, "simplifyUpdateRefitAngle", settings.simplifyUpdateRefitAngle);
        ReadSetting(object, "simplifyUpdateMeasuredError", settings.simplifyUpdateMeasuredError);
        ReadSetting(object, "simplifyUpdateUvTransfer", settings.simplifyUpdateUvTransfer);
        ReadSetting(object, "simplifyMatchReach", settings.simplifyMatchReach);
        ReadSetting(object, "simplifyMatchCandidates", settings.simplifyMatchCandidates);
        ReadSetting(object, "simplifyMatchFacingBins", settings.simplifyMatchFacingBins);
        ReadSetting(object, "simplifyUpdateFitSamples", settings.simplifyUpdateFitSamples);
        ReadSetting(object, "simplifyUpdateRefitCheck", settings.simplifyUpdateRefitCheck);
        ReadSetting(object, "lodErrorChannelsSamples", settings.lodErrorChannelsSamples);
        ReadSetting(object, "simplifyValidationSamples", settings.simplifyValidationSamples);
        ReadSetting(object, "simplifyOptimal", settings.simplifyOptimal);
        ReadSetting(object, "simplifyOptimalSheetAngle", settings.simplifyOptimalSheetAngle);
        ReadSetting(object, "simplifyOptimalFlipAngle", settings.simplifyOptimalFlipAngle);
        ReadSetting(object, "simplifyOptimalPreserveFolds", settings.simplifyOptimalPreserveFolds);
        ReadSetting(object, "simplifyOptimalMoves", settings.simplifyOptimalMoves);
        ReadSetting(object, "lodErrorChannels", settings.lodErrorChannels);
        ReadSetting(object, "lodErrorChannelsTexture", settings.lodErrorChannelsTexture);
        ReadSetting(object, "lodErrorChannelsTextureSlide", settings.lodErrorChannelsTextureSlide);
        ReadSetting(object, "lodErrorChannelsTextureColor", settings.lodErrorChannelsTextureColor);
        ReadSetting(object, "lodErrorChannelsTextureColorFootprint", settings.lodErrorChannelsTextureColorFootprint);
        ReadSetting(object, "lodErrorChannelsTextureColorAll", settings.lodErrorChannelsTextureColorAll);
        ReadSetting(object, "lodErrorChannelsTextureRemap", settings.lodErrorChannelsTextureRemap);
        ReadSetting(object, "lodErrorChannelsTextureRemapTexel", settings.lodErrorChannelsTextureRemapTexel);
        ReadSetting(object, "lodErrorChannelsTextureRemapMaxNodes", settings.lodErrorChannelsTextureRemapMaxNodes);
        ReadSetting(object, "lodErrorChannelsTextureRemapLayout", settings.lodErrorChannelsTextureRemapLayout);
        ReadSetting(object, "lodErrorChannelsTextureRemapCellMiss", settings.lodErrorChannelsTextureRemapCellMiss);
        ReadSetting(object, "lodErrorChannelsTextureRemapReserve", settings.lodErrorChannelsTextureRemapReserve);
        ReadSetting(object, "lodNormalSpread", settings.lodNormalSpread);
        ReadSetting(object, "lodNormalSpreadFootprint", settings.lodNormalSpreadFootprint);
        ReadSetting(object, "lodNormalSpreadSampleCap", settings.lodNormalSpreadSampleCap);
        ReadSetting(object, "simplifyTopology", settings.simplifyTopology);
        ReadSetting(object, "simplifyTopologyRetries", settings.simplifyTopologyRetries);
        ReadSetting(object, "simplifyTopologyRelaxUv", settings.simplifyTopologyRelaxUv);
        ReadSetting(object, "simplifyTopologyUpdateFallback", settings.simplifyTopologyUpdateFallback);
        ReadSetting(object, "simplifyTopologyTextureArea", settings.simplifyTopologyTextureArea);
        ReadSetting(object, "simplifyTopologyTextureSlide", settings.simplifyTopologyTextureSlide);
        ReadSetting(object, "simplifyTopologyTextureCharts", settings.simplifyTopologyTextureCharts);
        ReadSetting(object, "simplifyTopologyTextureTexels", settings.simplifyTopologyTextureTexels);
        ReadSetting(object, "simplifyTopologyTextureResolution", settings.simplifyTopologyTextureResolution);
        ReadSetting(object, "simplifyTopologyTextureColor", settings.simplifyTopologyTextureColor);
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
        ReadSetting(object, "voxelSurfaceLossLimit", settings.voxelSurfaceLossLimit);
        ReadSetting(object, "doubleSidedVoxelSourceNormals", settings.doubleSidedVoxelSourceNormals);
    }
}

ClusterLODBuilderSettings GetDefaultBuilderSettings(std::string_view assetIdentifier)
{
    // Simplification and appearance settings (sloppy fallback, permissive, normal weight, stages 1-6) keep the struct's defaults:
    // the cache key hashes their effective values over those defaults (CLodCache::ComputeBuildConfigHash).
    ClusterLODBuilderSettings settings;
    settings.lodErrorMergePrevious = 1.5f;
    settings.lodErrorMergeAdditive = 0.0f;
    settings.partitionSizeFloor = 8u;
    settings.nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
    settings.preserveImportedNormals = true;
    settings.enableNormalAttributeSimplification = true;
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
    // An asset's mesh forks into triangles and voxels where voxels are shown to win (ClusterLODVoxelForkMode::Auto).
    // Builds without an asset (terrain) do not.
    settings.voxelFork = assetIdentifier.empty() ? ClusterLODVoxelForkMode::Off : ClusterLODVoxelForkMode::Auto;

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
        const char* fork = settings.voxelFork == ClusterLODVoxelForkMode::Off ? "off" :
            settings.voxelFork == ClusterLODVoxelForkMode::Auto ? "auto" : "forced";
        spdlog::info(
            "SARP asset CLod settings applied: object='{}' coverage_preservation_mode='{}' voxel_fork='{}' grid={} rays={} scale={}",
            assetIdentifier, mode, fork, settings.voxelGridBaseResolution,
            settings.voxelRaysPerCell, settings.voxelFallbackScalingFactor);
    }
    return settings;
}

uint64_t GetCLodAssetSettingsConfigHash()
{
    return GetOverrideConfig().hash;
}

uint64_t GetCLodAssetSettingsHash(std::string_view assetIdentifier)
{
    const std::string normalized = NormalizeObject(assetIdentifier);
    for (const auto& rule : GetOverrideConfig().rules) {
        if (normalized == rule.object ||
            (normalized.size() > rule.object.size() && normalized.ends_with('/' + rule.object))) {
            return rule.hash;
        }
    }
    return 0u;
}
