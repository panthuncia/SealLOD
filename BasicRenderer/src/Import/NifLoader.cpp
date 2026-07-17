#include "Import/NifLoader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Import/BRNiflyClient.h"
#include "Import/CLodCache.h"
#include "Import/SkeletonArtifactCache.h"
#include "Import/USDGeometryExtractor.h"
#include "Animation/Skeleton.h"
#include "Managers/Singletons/TextureProcessingManager.h"
#include "Materials/Material.h"
#include "Mesh/Mesh.h"
#include "Resources/Texture.h"
#include "Scene/Scene.h"
#include "Utilities/CachePathUtilities.h"
#include "Utilities/Utilities.h"

namespace NifLoader {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr std::string_view kNifMetaCacheSuffix = ".nifmeta";
constexpr std::string_view kObjectReyesConfigVersion = "31";

std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
}

void AccumulateBRNiflyChildTimingStats(LoadTimingStats& stats, const BRNiflyClient::TimingStats& brniflyTiming)
{
    stats.brniflyClientPersistentStartMs += brniflyTiming.clientPersistentStartMs;
    stats.brniflyClientWriteRequestMs += brniflyTiming.clientWriteRequestMs;
    stats.brniflyClientWaitResponseMs += brniflyTiming.clientWaitResponseMs;
    stats.brniflyClientWaitFirstByteMs += brniflyTiming.clientWaitFirstByteMs;
    stats.brniflyClientWaitMoreResponseMs += brniflyTiming.clientWaitMoreResponseMs;
    stats.brniflyClientReadResponseMs += brniflyTiming.clientReadResponseMs;
    stats.brniflyClientResponseBytes += brniflyTiming.clientResponseBytes;
    stats.brniflyClientResponseChunks += brniflyTiming.clientResponseChunks;
    stats.brniflyClientSharedMemoryReadMs += brniflyTiming.clientSharedMemoryReadMs;
    stats.brniflyClientParseJsonMs += brniflyTiming.clientParseJsonMs;
    stats.brniflyChildLoadNiflyApiMs += brniflyTiming.childLoadNiflyApiMs;
    stats.brniflyChildNiflyLoadMs += brniflyTiming.childNiflyLoadMs;
    stats.brniflyChildGetGameNameMs += brniflyTiming.childGetGameNameMs;
    stats.brniflyChildReadNodesMs += brniflyTiming.childReadNodesMs;
    stats.brniflyChildReadShapesMs += brniflyTiming.childReadShapesMs;
    stats.brniflyChildReadExtraDataMs += brniflyTiming.childReadExtraDataMs;
    stats.brniflyChildDestroyNifMs += brniflyTiming.childDestroyNifMs;
    stats.brniflyChildConvertShapesToUsdMs += brniflyTiming.childConvertShapesToUsdMs;
    stats.brniflyChildUsdExportToStringMs += brniflyTiming.childUsdExportToStringMs;
    stats.brniflyChildHashAndResponseMs += brniflyTiming.childHashAndResponseMs;
    stats.brniflyChildSharedMemoryCreateMs += brniflyTiming.childSharedMemoryCreateMs;
    stats.brniflyChildJsonDumpMs += brniflyTiming.childJsonDumpMs;
}

std::string ReadEnvironmentString(const char* name)
{
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return {};
    }

    std::string result(value);
    std::free(value);
    return result;
}

std::string Hex64(std::uint64_t value)
{
    std::ostringstream out;
    out << std::hex;
    out.width(16);
    out.fill('0');
    out << value;
    return out.str();
}

std::uint64_t Fnv1a64(std::string_view text)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string NormalizeNifCacheKey(std::string_view cacheKey)
{
    std::string normalized;
    normalized.reserve(cacheKey.size());
    for (unsigned char ch : cacheKey) {
        char out = static_cast<char>(ch);
        if (out == '/') {
            out = '\\';
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(out))));
    }

    while (!normalized.empty() && (normalized.front() == '\\' || normalized.front() == '/')) {
        normalized.erase(normalized.begin());
    }
    return normalized;
}

std::vector<std::string> MergeTextureSearchRoots(
    const std::vector<std::string>& packageRoots,
    const std::vector<std::string>& additionalRoots)
{
    std::vector<std::string> roots;
    roots.reserve(packageRoots.size() + additionalRoots.size());
    auto append = [&](const std::string& root) {
        if (!root.empty() && std::find(roots.begin(), roots.end(), root) == roots.end()) {
            roots.push_back(root);
        }
    };
    for (const std::string& root : packageRoots) {
        append(root);
    }
    for (const std::string& root : additionalRoots) {
        append(root);
    }
    return roots;
}

std::string TextureSearchRootsHash(const std::vector<std::string>& roots)
{
    std::string key = "texture-roots:";
    for (const std::string& root : roots) {
        key.append(root);
        key.push_back('\n');
    }
    return Hex64(Fnv1a64(key));
}

std::string NormalizeObjectReyesWhitelistPath(std::string_view path)
{
    std::string normalized;
    normalized.reserve(path.size());
    for (unsigned char ch : path) {
        char out = static_cast<char>(std::tolower(ch));
        if (out == '\\') {
            out = '/';
        }
        normalized.push_back(out);
    }

    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    return normalized;
}

std::string NormalizeObjectReyesNifWhitelistPath(std::string_view path)
{
    std::string normalized = NormalizeObjectReyesWhitelistPath(path);
    constexpr std::string_view meshesPrefix = "meshes/";
    if (normalized.starts_with(meshesPrefix)) {
        normalized.erase(0, meshesPrefix.size());
    }
    return normalized;
}

std::string NormalizeObjectReyesHeightAtlasStorageSetting(std::string_view text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (unsigned char ch : text) {
        normalized.push_back(ch == '-' || ch == ' '
            ? '_'
            : static_cast<char>(std::tolower(ch)));
    }
    if (normalized == "r16" ||
        normalized == "r16_unorm" ||
        normalized == "r16_unorm_mips" ||
        normalized == "r16unormmips" ||
        normalized == "legacy" ||
        normalized == "old") {
        return "r16_unorm_mips";
    }
    if (normalized == "r8" ||
        normalized == "r8_unorm" ||
        normalized == "r8_unorm_nomips" ||
        normalized == "r8u" ||
        normalized == "r8u_nomips") {
        return "r8_unorm";
    }
    if (!normalized.empty() &&
        normalized != "bc4u" &&
        normalized != "bc4_unorm" &&
        normalized != "bc4_unorm_nomips" &&
        normalized != "bc4u_nomips") {
        spdlog::warn("Object Reyes heightAtlasStorage='{}' is unknown; using 'r8_unorm'.", text);
    }
    return normalized == "bc4u" ||
            normalized == "bc4_unorm" ||
            normalized == "bc4_unorm_nomips" ||
            normalized == "bc4u_nomips"
        ? "bc4u"
        : "r8_unorm";
}

struct ObjectReyesConfig
{
    struct BakedHeightMaterialEntry
    {
        std::string nifPath;
        std::vector<std::string> materialTexturePaths;
    };

    std::unordered_set<std::string> nifPaths;
    std::unordered_set<std::string> texturePaths;
    std::unordered_set<std::string> surfaceSamplingNifPaths;
    std::unordered_set<std::string> surfaceSamplingTexturePaths;
    std::unordered_set<std::string> bakedHeightNifPaths;
    std::unordered_set<std::string> triplanarProjectionNifPaths;
    std::unordered_set<std::string> triplanarProjectionTexturePaths;
    std::unordered_set<std::string> tripleTapStochasticNifPaths;
    std::unordered_set<std::string> tripleTapStochasticTexturePaths;
    std::unordered_map<std::string, float> displacementScaleOverrides;
    std::vector<BakedHeightMaterialEntry> bakedHeightMaterials;
    std::string surfaceSamplingMode;
    bool surfaceSamplingIncludeSelected = false;
    bool triplanarProjectionIncludeSelected = false;
    bool tripleTapStochasticIncludeSelected = false;
    std::uint32_t atlasBakeResolution = 4096u;
    std::uint32_t atlasBakePaddingTexels = 8u;
    std::string heightAtlasStorage = "r8_unorm";
    std::string contentHash = Hex64(Fnv1a64("object-reyes:v1:missing"));
    bool loaded = false;
};

std::optional<fs::path> FindObjectReyesConfigPath()
{
    std::error_code ec;

    auto findFromRoot = [&](fs::path current) -> std::optional<fs::path> {
        for (;;) {
            const fs::path candidate = current / "config" / "object_reyes.json";
            if (fs::exists(candidate, ec) && !ec) {
                return candidate;
            }
            const fs::path flatCandidate = current / "object_reyes.json";
            if (fs::exists(flatCandidate, ec) && !ec) {
                return flatCandidate;
            }
            if (!current.has_parent_path() || current.parent_path() == current) {
                break;
            }
            current = current.parent_path();
        }
        return std::nullopt;
    };

    if (const std::string overridePath = ReadEnvironmentString("SARP_OBJECT_REYES_CONFIG"); !overridePath.empty()) {
        fs::path candidate = fs::path(overridePath);
        if (candidate.is_relative()) {
            if (auto current = fs::current_path(ec); !ec) {
                candidate = current / candidate;
            }
        }
        if (fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
        spdlog::warn("SARP_OBJECT_REYES_CONFIG='{}' does not point to an existing Object Reyes config.", overridePath);
    }

    if (auto current = fs::current_path(ec); !ec) {
        if (auto found = findFromRoot(current)) {
            return found;
        }
    }

    std::array<wchar_t, MAX_PATH> modulePath{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (moduleLength > 0 && moduleLength < modulePath.size()) {
        const fs::path moduleDir = fs::path(modulePath.data()).parent_path();
        if (auto found = findFromRoot(moduleDir)) {
            return found;
        }
    }

    return std::nullopt;
}

ObjectReyesConfig LoadObjectReyesConfig()
{
    ObjectReyesConfig config;
    const auto path = FindObjectReyesConfigPath();
    if (!path) {
        return config;
    }

    std::ifstream in(*path);
    if (!in) {
        spdlog::warn("Object Reyes config '{}' could not be opened.", path->string());
        return config;
    }

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        const json doc = json::parse(text);
        auto readTexturePathArray = [](const json& node, std::unordered_set<std::string>& out) {
            if (!node.is_array()) {
                return;
            }
            for (const auto& value : node) {
                if (value.is_string()) {
                    const std::string normalized = NormalizeObjectReyesWhitelistPath(value.get<std::string>());
                    if (!normalized.empty()) {
                        out.insert(normalized);
                    }
                }
            }
        };
        auto readNifPathArray = [](const json& node, std::unordered_set<std::string>& out) {
            if (!node.is_array()) {
                return;
            }
            for (const auto& value : node) {
                if (value.is_string()) {
                    const std::string normalized = NormalizeObjectReyesNifWhitelistPath(value.get<std::string>());
                    if (!normalized.empty()) {
                        out.insert(normalized);
                    }
                }
            }
        };

        if (const auto nifPaths = doc.find("nifPaths"); nifPaths != doc.end()) {
            readNifPathArray(*nifPaths, config.nifPaths);
        }
        readTexturePathArray(doc.value("texturePaths", json::array()), config.texturePaths);
        if (const auto overrides = doc.find("displacementScaleOverrides");
            overrides != doc.end() && overrides->is_object()) {
            for (auto it = overrides->begin(); it != overrides->end(); ++it) {
                const std::string normalized = NormalizeObjectReyesWhitelistPath(it.key());
                if (normalized.empty() || !it.value().is_number()) {
                    continue;
                }
                const float scale = it.value().get<float>();
                if (!std::isfinite(scale) || scale < 0.0f) {
                    spdlog::warn(
                        "Object Reyes displacementScaleOverrides['{}']={} is invalid; ignoring.",
                        it.key(),
                        scale);
                    continue;
                }
                config.displacementScaleOverrides[normalized] = scale;
            }
        }
        if (const auto storage = doc.find("heightAtlasStorage"); storage != doc.end() && storage->is_string()) {
            config.heightAtlasStorage = NormalizeObjectReyesHeightAtlasStorageSetting(storage->get<std::string>());
        }
        auto readBakedHeightObject = [&](const json& bakedHeight) {
            if (const auto nifPaths = bakedHeight.find("nifPaths"); nifPaths != bakedHeight.end() && nifPaths->is_array()) {
                std::unordered_set<std::string> bakedHeightNifPaths;
                readNifPathArray(*nifPaths, bakedHeightNifPaths);
                for (std::string nifPath : bakedHeightNifPaths) {
                    if (!nifPath.empty()) {
                        config.bakedHeightNifPaths.insert(nifPath);
                        config.bakedHeightMaterials.push_back(ObjectReyesConfig::BakedHeightMaterialEntry{
                            .nifPath = std::move(nifPath),
                            .materialTexturePaths = {}
                        });
                    }
                }
            }
            if (const auto entries = bakedHeight.find("entries"); entries != bakedHeight.end() && entries->is_array()) {
                for (const auto& entryNode : *entries) {
                    if (!entryNode.is_object()) {
                        continue;
                    }
                    ObjectReyesConfig::BakedHeightMaterialEntry entry{};
                    entry.nifPath = NormalizeObjectReyesNifWhitelistPath(entryNode.value("nifPath", std::string{}));
                    const json textureArray = entryNode.contains("materialTexturePaths")
                        ? entryNode.at("materialTexturePaths")
                        : entryNode.value("texturePaths", json::array());
                    std::unordered_set<std::string> uniqueTextures;
                    readTexturePathArray(textureArray, uniqueTextures);
                    entry.materialTexturePaths.assign(uniqueTextures.begin(), uniqueTextures.end());
                    std::sort(entry.materialTexturePaths.begin(), entry.materialTexturePaths.end());
                    if (!entry.nifPath.empty()) {
                        config.bakedHeightNifPaths.insert(entry.nifPath);
                        config.bakedHeightMaterials.push_back(std::move(entry));
                    }
                }
            }
            if (const auto atlasBake = bakedHeight.find("atlasBake");
                atlasBake != bakedHeight.end() && atlasBake->is_object()) {
                config.atlasBakeResolution = atlasBake->value("resolution", config.atlasBakeResolution);
                config.atlasBakePaddingTexels = atlasBake->value("paddingTexels", config.atlasBakePaddingTexels);
            }
        };

        if (const auto bakedHeight = doc.find("bakedHeight"); bakedHeight != doc.end() && bakedHeight->is_object()) {
            readBakedHeightObject(*bakedHeight);
        }
        if (const auto triplanarProjection = doc.find("triplanarProjection");
            triplanarProjection != doc.end() && triplanarProjection->is_object()) {
            config.triplanarProjectionIncludeSelected =
                triplanarProjection->value("includeSelected", config.triplanarProjectionIncludeSelected);
            if (const auto nifPaths = triplanarProjection->find("nifPaths"); nifPaths != triplanarProjection->end()) {
                readNifPathArray(*nifPaths, config.triplanarProjectionNifPaths);
            }
            readTexturePathArray(
                triplanarProjection->value("texturePaths", json::array()),
                config.triplanarProjectionTexturePaths);
        }
        if (const auto surfaceSampling = doc.find("surfaceSampling"); surfaceSampling != doc.end() && surfaceSampling->is_object()) {
            config.surfaceSamplingMode = surfaceSampling->value("mode", std::string{});
            config.surfaceSamplingIncludeSelected = surfaceSampling->value("includeSelected", config.surfaceSamplingIncludeSelected);
            if (const auto nifPaths = surfaceSampling->find("nifPaths"); nifPaths != surfaceSampling->end()) {
                readNifPathArray(*nifPaths, config.surfaceSamplingNifPaths);
            }
            readTexturePathArray(surfaceSampling->value("texturePaths", json::array()), config.surfaceSamplingTexturePaths);
            if (const auto atlasBake = surfaceSampling->find("atlasBake");
                atlasBake != surfaceSampling->end() && atlasBake->is_object()) {
                config.atlasBakeResolution = atlasBake->value("resolution", config.atlasBakeResolution);
                config.atlasBakePaddingTexels = atlasBake->value("paddingTexels", config.atlasBakePaddingTexels);
                if (config.atlasBakeResolution < 256u || config.atlasBakeResolution > 8192u) {
                    spdlog::warn(
                        "Object Reyes surfaceSampling.atlasBake.resolution={} is invalid; using 4096.",
                        config.atlasBakeResolution);
                    config.atlasBakeResolution = 4096u;
                }
                if (config.atlasBakePaddingTexels > 64u) {
                    spdlog::warn(
                        "Object Reyes surfaceSampling.atlasBake.paddingTexels={} is invalid; using 8.",
                        config.atlasBakePaddingTexels);
                    config.atlasBakePaddingTexels = 8u;
                }
            }
            if (const auto bakedHeight = surfaceSampling->find("bakedHeight");
                bakedHeight != surfaceSampling->end() && bakedHeight->is_object()) {
                readBakedHeightObject(*bakedHeight);
            }
            if (const auto tripleTap = surfaceSampling->find("tripleTapStochastic");
                tripleTap != surfaceSampling->end() && tripleTap->is_object()) {
                config.tripleTapStochasticIncludeSelected =
                    tripleTap->value("includeSelected", config.tripleTapStochasticIncludeSelected);
                if (const auto nifPaths = tripleTap->find("nifPaths"); nifPaths != tripleTap->end()) {
                    readNifPathArray(*nifPaths, config.tripleTapStochasticNifPaths);
                }
                readTexturePathArray(
                    tripleTap->value("texturePaths", json::array()),
                    config.tripleTapStochasticTexturePaths);
            }
        }
        if (const std::string envStorage = ReadEnvironmentString("SARP_OBJECT_REYES_HEIGHT_ATLAS_STORAGE");
            !envStorage.empty()) {
            config.heightAtlasStorage = NormalizeObjectReyesHeightAtlasStorageSetting(envStorage);
        }
        if (config.atlasBakeResolution < 256u || config.atlasBakeResolution > 8192u) {
            spdlog::warn(
                "Object Reyes atlasBake.resolution={} is invalid; using 4096.",
                config.atlasBakeResolution);
            config.atlasBakeResolution = 4096u;
        }
        if (config.atlasBakePaddingTexels > 64u) {
            spdlog::warn(
                "Object Reyes atlasBake.paddingTexels={} is invalid; using 8.",
                config.atlasBakePaddingTexels);
            config.atlasBakePaddingTexels = 8u;
        }
        config.loaded = true;
        config.contentHash = Hex64(Fnv1a64(
            std::string("object-reyes:v1:") +
            text +
            "|heightAtlasStorage=" +
            config.heightAtlasStorage));
        spdlog::info(
            "Loaded Object Reyes config '{}' ({} opt-in nif path(s), {} opt-in texture path(s), displacementScaleOverrides={}, surfaceSampling mode='{}', includeSelected={}, {} surface nif path(s), {} surface texture path(s), bakedHeight nifs={}, bakedHeight entries={}, triplanarProjection nifs={}, textures={}, includeSelected={}, tripleTapStochastic nifs={}, textures={}, includeSelected={}, atlasBake resolution={}, paddingTexels={}, heightAtlasStorage={}, hash={}).",
            path->string(),
            config.nifPaths.size(),
            config.texturePaths.size(),
            config.displacementScaleOverrides.size(),
            config.surfaceSamplingMode,
            config.surfaceSamplingIncludeSelected,
            config.surfaceSamplingNifPaths.size(),
            config.surfaceSamplingTexturePaths.size(),
            config.bakedHeightNifPaths.size(),
            config.bakedHeightMaterials.size(),
            config.triplanarProjectionNifPaths.size(),
            config.triplanarProjectionTexturePaths.size(),
            config.triplanarProjectionIncludeSelected,
            config.tripleTapStochasticNifPaths.size(),
            config.tripleTapStochasticTexturePaths.size(),
            config.tripleTapStochasticIncludeSelected,
            config.atlasBakeResolution,
            config.atlasBakePaddingTexels,
            config.heightAtlasStorage,
            config.contentHash);
    }
    catch (const std::exception& e) {
        spdlog::warn("Object Reyes config '{}' is invalid JSON: {}", path->string(), e.what());
    }

    return config;
}

bool ObjectReyesConfigMayAffectCachedPayload(const ObjectReyesConfig& config, const std::string& normalizedNifCacheKey)
{
    if (!config.loaded) {
        return false;
    }
    const std::string normalizedSlashKey = NormalizeObjectReyesNifWhitelistPath(normalizedNifCacheKey);
    const bool nifListed =
        config.nifPaths.contains(normalizedSlashKey) ||
        config.surfaceSamplingNifPaths.contains(normalizedSlashKey) ||
        config.bakedHeightNifPaths.contains(normalizedSlashKey) ||
        config.triplanarProjectionNifPaths.contains(normalizedSlashKey) ||
        config.tripleTapStochasticNifPaths.contains(normalizedSlashKey) ||
        std::any_of(
            config.bakedHeightMaterials.begin(),
            config.bakedHeightMaterials.end(),
            [&](const ObjectReyesConfig::BakedHeightMaterialEntry& entry) { return entry.nifPath == normalizedSlashKey; });
    if (config.surfaceSamplingMode == "atlasBakedHeight" || config.bakedHeightNifPaths.contains(normalizedSlashKey)) {
        return nifListed;
    }
    return config.nifPaths.contains(normalizedSlashKey) ||
        config.surfaceSamplingNifPaths.contains(normalizedSlashKey) ||
        config.bakedHeightNifPaths.contains(normalizedSlashKey) ||
        config.triplanarProjectionNifPaths.contains(normalizedSlashKey) ||
        config.tripleTapStochasticNifPaths.contains(normalizedSlashKey) ||
        std::any_of(
            config.bakedHeightMaterials.begin(),
            config.bakedHeightMaterials.end(),
            [&](const ObjectReyesConfig::BakedHeightMaterialEntry& entry) { return entry.nifPath == normalizedSlashKey; }) ||
        !config.texturePaths.empty() ||
        !config.surfaceSamplingTexturePaths.empty() ||
        !config.triplanarProjectionTexturePaths.empty() ||
        !config.tripleTapStochasticTexturePaths.empty() ||
        !config.displacementScaleOverrides.empty();
}

std::string ObjectReyesContentHashMarker(const ObjectReyesConfig& config)
{
    return "_object_reyes_" + std::string(kObjectReyesConfigVersion) + "_" + config.contentHash + "_texroots_";
}

bool CachedContentHashMatchesObjectReyesConfig(const std::string& contentHash, const ObjectReyesConfig& config)
{
    if (!config.loaded) {
        return true;
    }

    const std::string marker = ObjectReyesContentHashMarker(config);
    return contentHash.find(marker) != std::string::npos;
}

std::string SanitizeFileStem(std::string_view value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '_' || ch == '-') {
            sanitized.push_back(static_cast<char>(ch));
        } else {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty()) {
        return "nif";
    }
    return sanitized;
}

std::string NifStemFromCacheKey(const std::string& normalizedCacheKey)
{
    fs::path path(s2ws(normalizedCacheKey));
    auto stem = ws2s(path.stem().wstring());
    return SanitizeFileStem(stem);
}

bool IsKnownNonRenderableNif(const std::string& normalizedCacheKey)
{
    fs::path path(s2ws(normalizedCacheKey));
    const std::string stem = ws2s(path.stem().wstring());
    const bool isCameraPath = normalizedCacheKey.starts_with("meshes\\cameras\\");
    const bool isEditorOrCollisionHelper =
        stem.find("trigger") != std::string::npos ||
        stem.find("extracollision") != std::string::npos;
    return stem == "skeleton" ||
        stem == "skeleton_female" ||
        stem == "skeletonbeast" ||
        stem == "skeletonbeast_female" ||
        stem == "camerashake" ||
        isCameraPath ||
        isEditorOrCollisionHelper;
}

std::string MakeStableSourceIdentifier(
    const std::string& normalizedCacheKey,
    const std::string& contentHash)
{
    std::string uriPath = normalizedCacheKey;
    std::replace(uriPath.begin(), uriPath.end(), '\\', '/');
    return "sarp-nif://" + uriPath + "#brnifly=" + contentHash;
}

std::string MakeAssetFileName(const std::string& normalizedCacheKey, const std::string& pathHash, const std::string& contentHash)
{
    return NifStemFromCacheKey(normalizedCacheKey) + "__p" + pathHash + "__c" + contentHash + std::string(kNifMetaCacheSuffix);
}

bool HasAssetCacheSuffix(const fs::path& path)
{
    const auto fileName = path.filename().string();
    return fileName.size() >= kNifMetaCacheSuffix.size() &&
        fileName.ends_with(kNifMetaCacheSuffix);
}

std::string ExtractContentHashFromFileName(const fs::path& path)
{
    const auto fileName = path.filename().string();
    const auto marker = fileName.find("__c");
    if (marker == std::string::npos) {
        return {};
    }
    const auto begin = marker + 3;
    const auto end = fileName.find(std::string(kNifMetaCacheSuffix), begin);
    if (end == std::string::npos || end <= begin) {
        return {};
    }
    return fileName.substr(begin, end - begin);
}

std::string ExtractPathHashFromFileName(const fs::path& path)
{
    const auto fileName = path.filename().string();
    const auto pathMarker = fileName.find("__p");
    if (pathMarker == std::string::npos) {
        return {};
    }
    const auto begin = pathMarker + 3;
    const auto contentMarker = fileName.find("__c", begin);
    if (contentMarker == std::string::npos || contentMarker <= begin) {
        return {};
    }
    return fileName.substr(begin, contentMarker - begin);
}

fs::path CLodCacheRoot()
{
    return fs::path(GetCacheFilePath(L"", L"clod"));
}

fs::path AssetPathIndexRoot()
{
    return CLodCacheRoot() / "nif_meta_index";
}

fs::path AssetManifestPath()
{
    return AssetPathIndexRoot() / "manifest.tsv";
}

// Payloads retain the CLod assembly references created by USDLoader.  Bump this
// whenever that ownership/serialization contract changes so preprocessing
// cannot hide a stale inline-skeleton assembly behind an otherwise valid NIF
// payload cache.
constexpr std::uint32_t kPayloadCacheVersion = 47u;

enum class CachedSkeletonStorage : std::uint8_t
{
    None,
    SharedArtifact,
    Inline
};

struct AssetCacheIndex {
    std::mutex mutex;
    bool manifestLoaded{ false };
    std::unordered_map<std::string, std::vector<fs::path>> byPathHash;
};

template <class T>
void HashPod(std::uint64_t& hash, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const unsigned char*>(std::addressof(value));
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

void HashString(std::uint64_t& hash, const std::string& value)
{
    const auto size = static_cast<std::uint64_t>(value.size());
    HashPod(hash, size);
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
}

void HashChannels(std::uint64_t& hash, const std::vector<std::uint32_t>& channels)
{
    const auto size = static_cast<std::uint64_t>(channels.size());
    HashPod(hash, size);
    for (const auto channel : channels) {
        HashPod(hash, channel);
    }
}

void HashBinding(std::uint64_t& hash, const TextureAndConstant& binding)
{
    HashString(hash, binding.sourcePath);
    HashPod(hash, binding.factor.Get());
    HashChannels(hash, binding.channels);
    HashPod(hash, binding.uvSetIndex);
    HashString(hash, binding.uvSetName);
}

void HashOpenPBR(std::uint64_t& hash, const OpenPBRMaterialParameters& pbr)
{
    HashPod(hash, pbr.baseWeight);
    HashPod(hash, pbr.baseColor);
    HashPod(hash, pbr.baseDiffuseRoughness);
    HashPod(hash, pbr.baseMetalness);
    HashPod(hash, pbr.subsurfaceWeight);
    HashPod(hash, pbr.subsurfaceColor);
    HashPod(hash, pbr.subsurfaceRadius);
    HashPod(hash, pbr.subsurfaceRadiusScale);
    HashPod(hash, pbr.subsurfaceScatterAnisotropy);
    HashPod(hash, pbr.specularWeight);
    HashPod(hash, pbr.specularColor);
    HashPod(hash, pbr.specularRoughness);
    HashPod(hash, pbr.specularRoughnessAnisotropy);
    HashPod(hash, pbr.specularIor);
    HashPod(hash, pbr.specularAnisotropyRotationCosSin);
    HashPod(hash, pbr.coatWeight);
    HashPod(hash, pbr.coatColor);
    HashPod(hash, pbr.coatRoughness);
    HashPod(hash, pbr.coatRoughnessAnisotropy);
    HashPod(hash, pbr.coatIor);
    HashPod(hash, pbr.coatDarkening);
    HashPod(hash, pbr.coatAnisotropyRotationCosSin);
    HashPod(hash, pbr.fuzzWeight);
    HashPod(hash, pbr.fuzzColor);
    HashPod(hash, pbr.fuzzRoughness);
    HashPod(hash, pbr.transmissionWeight);
    HashPod(hash, pbr.transmissionColor);
    HashPod(hash, pbr.transmissionDepth);
    HashPod(hash, pbr.transmissionScatter);
    HashPod(hash, pbr.transmissionScatterAnisotropy);
    HashPod(hash, pbr.transmissionDispersionScale);
    HashPod(hash, pbr.transmissionDispersionAbbeNumber);
    HashPod(hash, pbr.thinFilmWeight);
    HashPod(hash, pbr.thinFilmThickness);
    HashPod(hash, pbr.thinFilmIor);
    HashPod(hash, pbr.emissionLuminance);
    HashPod(hash, pbr.emissionColor);
    HashPod(hash, pbr.geometryOpacity);
    HashPod(hash, pbr.geometryThinWalled);
}

std::uint64_t ComputeMaterialHash(const MaterialDescription& desc)
{
    std::uint64_t hash = 14695981039346656037ull;
    HashPod(hash, desc.materialModel);
    HashString(hash, desc.name);
    HashPod(hash, desc.diffuseColor);
    HashPod(hash, desc.emissiveColor);
    HashPod(hash, desc.alphaCutoff);
    HashPod(hash, desc.heightMapScale);
    HashPod(hash, desc.geometricDisplacementMin);
    HashPod(hash, desc.geometricDisplacementMax);
    HashPod(hash, desc.negateNormals);
    HashPod(hash, desc.invertNormalGreen);
    HashPod(hash, desc.forceDoubleSided);
    HashPod(hash, desc.enableGeometricDisplacement);
    HashPod(hash, desc.geometricDisplacementOptIn);
    HashPod(hash, desc.brniflyVertexAlpha);
    HashPod(hash, desc.brniflyZBufferWrite);
    HashPod(hash, desc.brniflyDecal);
    HashPod(hash, desc.brniflyDynamicDecal);
    HashPod(hash, desc.brniflyModelSpaceNormals);
    HashPod(hash, desc.heightMapFromBaseColorAlpha);
    HashPod(hash, desc.objectSurfaceSamplingMode);
    HashPod(hash, desc.objectSurfaceUseTriplanarProjection);
    HashPod(hash, desc.objectSurfaceUseTripleTapStochastic);
    HashPod(hash, desc.objectSurfaceTexelDensity);
    HashString(hash, desc.staticTextureOverrideSourceName);
    HashPod(hash, desc.blendState);
    HashBinding(hash, desc.baseColor);
    HashBinding(hash, desc.metallic);
    HashBinding(hash, desc.roughness);
    HashBinding(hash, desc.emissive);
    HashBinding(hash, desc.opacity);
    HashBinding(hash, desc.aoMap);
    HashBinding(hash, desc.heightMap);
    HashBinding(hash, desc.normal);
    HashOpenPBR(hash, desc.openPBR);
    HashPod(hash, desc.glintEnabled);
    HashPod(hash, desc.glintParameters);
    HashBinding(hash, desc.openPBRTextures.coatColor);
    HashBinding(hash, desc.openPBRTextures.coatWeight);
    HashBinding(hash, desc.openPBRTextures.coatRoughness);
    HashBinding(hash, desc.openPBRTextures.fuzzColor);
    HashBinding(hash, desc.openPBRTextures.fuzzWeight);
    HashBinding(hash, desc.openPBRTextures.fuzzRoughness);
    return hash;
}

std::uint64_t ComputeMaterialHash(const Material& material)
{
    return ComputeMaterialHash(material.ToCacheDescription());
}

void EnsurePayloadMaterialHashes(USDLoader::ImportedAssetPayload& payload)
{
    if (payload.meshMaterialHashes.size() == payload.meshes.size()) {
        return;
    }

    payload.meshMaterialHashes.clear();
    payload.meshMaterialHashes.reserve(payload.meshes.size());
    for (const auto& mesh : payload.meshes) {
        if (!mesh || !mesh->material) {
            payload.meshMaterialHashes.push_back(0);
            continue;
        }
        payload.meshMaterialHashes.push_back(ComputeMaterialHash(*mesh->material));
    }
}

AssetCacheIndex& GetAssetCacheIndex()
{
    static AssetCacheIndex index;
    return index;
}

void SortNewestFirst(std::vector<fs::path>& paths)
{
    std::sort(paths.begin(), paths.end(), [](const fs::path& lhs, const fs::path& rhs) {
        std::error_code lhsEc;
        std::error_code rhsEc;
        const auto lhsTime = fs::last_write_time(lhs, lhsEc);
        const auto rhsTime = fs::last_write_time(rhs, rhsEc);
        if (lhsEc || rhsEc || lhsTime == rhsTime) {
            return lhs.string() > rhs.string();
        }
        return lhsTime > rhsTime;
    });
}

void StoreAssetManifest(const std::unordered_map<std::string, std::vector<fs::path>>& byPathHash)
{
    const fs::path manifestPath = AssetManifestPath();
    std::error_code ec;
    fs::create_directories(manifestPath.parent_path(), ec);
    if (ec) {
        return;
    }

    const fs::path tempPath = manifestPath.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }

        for (const auto& [pathHash, paths] : byPathHash) {
            for (const auto& path : paths) {
                out << pathHash << '\t' << path.string() << '\n';
            }
        }
    }

    fs::rename(tempPath, manifestPath, ec);
    if (ec) {
        fs::remove(manifestPath, ec);
        ec.clear();
        fs::rename(tempPath, manifestPath, ec);
    }
}

void LoadAssetManifestLocked(AssetCacheIndex& index)
{
    if (index.manifestLoaded) {
        return;
    }
    index.manifestLoaded = true;

    std::unordered_map<std::string, std::vector<fs::path>> loaded;
    const fs::path manifestPath = AssetManifestPath();
    std::ifstream manifest(manifestPath, std::ios::binary);
    if (manifest) {
        std::string line;
        while (std::getline(manifest, line)) {
            if (line.empty()) {
                continue;
            }

            const auto tab = line.find('\t');
            if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
                continue;
            }

            std::string pathHash = line.substr(0, tab);
            fs::path path(line.substr(tab + 1));
            if (!pathHash.empty() &&
                HasAssetCacheSuffix(path) &&
                ExtractPathHashFromFileName(path) == pathHash) {
                loaded[std::move(pathHash)].push_back(std::move(path));
            }
        }
    }

    index.byPathHash = std::move(loaded);
}

std::vector<fs::path> FindCachedAssets(const std::string& pathHash)
{
    auto& index = GetAssetCacheIndex();
    {
        std::lock_guard<std::mutex> lock(index.mutex);
        LoadAssetManifestLocked(index);
        auto it = index.byPathHash.find(pathHash);
        if (it != index.byPathHash.end()) {
            return it->second;
        }
    }
    return {};
}

void RegisterCachedAsset(const std::string& pathHash, const fs::path& cachePath)
{
    if (pathHash.empty() || cachePath.empty()) {
        return;
    }

    auto& index = GetAssetCacheIndex();

    std::lock_guard<std::mutex> lock(index.mutex);
    LoadAssetManifestLocked(index);
    auto& paths = index.byPathHash[pathHash];
    if (std::find(paths.begin(), paths.end(), cachePath) == paths.end()) {
        paths.push_back(cachePath);
        SortNewestFirst(paths);
        StoreAssetManifest(index.byPathHash);
    }
}

USDLoader::InMemoryStageOptions MakeStageOptions(
    const std::string& sourceIdentifier,
    const std::string& sourceDirectory,
    const std::vector<std::string>& textureSearchRoots,
    const std::string& layerIdentifierHint,
    const ObjectReyesConfig& objectReyesConfig,
    const std::string& normalizedCacheKey)
{
    USDLoader::InMemoryStageOptions options{};
    options.sourceIdentifier = sourceIdentifier;
    options.sourceDirectory = sourceDirectory;
    options.textureSearchRoots = textureSearchRoots;
    options.layerIdentifierHint = layerIdentifierHint;
    options.objectReyesNifPath = NormalizeObjectReyesNifWhitelistPath(normalizedCacheKey);
    options.objectReyesConfigHash = objectReyesConfig.contentHash;
    options.objectReyesTexturePaths.assign(
        objectReyesConfig.texturePaths.begin(),
        objectReyesConfig.texturePaths.end());
    std::sort(options.objectReyesTexturePaths.begin(), options.objectReyesTexturePaths.end());
    options.objectReyesSurfaceSamplingTexturePaths.assign(
        objectReyesConfig.surfaceSamplingTexturePaths.begin(),
        objectReyesConfig.surfaceSamplingTexturePaths.end());
    std::sort(options.objectReyesSurfaceSamplingTexturePaths.begin(), options.objectReyesSurfaceSamplingTexturePaths.end());
    options.objectReyesTriplanarProjectionTexturePaths.assign(
        objectReyesConfig.triplanarProjectionTexturePaths.begin(),
        objectReyesConfig.triplanarProjectionTexturePaths.end());
    std::sort(options.objectReyesTriplanarProjectionTexturePaths.begin(), options.objectReyesTriplanarProjectionTexturePaths.end());
    options.objectReyesTripleTapStochasticTexturePaths.assign(
        objectReyesConfig.tripleTapStochasticTexturePaths.begin(),
        objectReyesConfig.tripleTapStochasticTexturePaths.end());
    std::sort(options.objectReyesTripleTapStochasticTexturePaths.begin(), options.objectReyesTripleTapStochasticTexturePaths.end());
    options.objectReyesDisplacementScaleOverrides = objectReyesConfig.displacementScaleOverrides;
    options.objectReyesNifMatched =
        objectReyesConfig.loaded &&
        objectReyesConfig.nifPaths.contains(options.objectReyesNifPath);
    const bool bakedHeightNifMatched =
        objectReyesConfig.loaded &&
        objectReyesConfig.bakedHeightNifPaths.contains(options.objectReyesNifPath);
    options.objectReyesSurfaceSamplingEnabled =
        objectReyesConfig.loaded &&
        (bakedHeightNifMatched ||
         objectReyesConfig.surfaceSamplingMode == "triplanarStochastic" ||
         objectReyesConfig.surfaceSamplingMode == "atlasBakedHeight");
    if (bakedHeightNifMatched || objectReyesConfig.surfaceSamplingMode == "atlasBakedHeight") {
        options.objectReyesSurfaceSamplingMode = ObjectSurfaceSamplingMode::AtlasBakedHeight;
    }
    else if (objectReyesConfig.surfaceSamplingMode == "triplanarStochastic") {
        options.objectReyesSurfaceSamplingMode = ObjectSurfaceSamplingMode::TriplanarStochastic;
    }
    else {
        options.objectReyesSurfaceSamplingMode = ObjectSurfaceSamplingMode::None;
    }
    options.objectReyesSurfaceSamplingIncludeSelected = objectReyesConfig.surfaceSamplingIncludeSelected;
    options.objectReyesSurfaceSamplingNifMatched =
        options.objectReyesSurfaceSamplingEnabled &&
        (bakedHeightNifMatched || objectReyesConfig.surfaceSamplingNifPaths.contains(options.objectReyesNifPath));
    options.objectReyesTriplanarProjectionIncludeSelected = objectReyesConfig.triplanarProjectionIncludeSelected;
    options.objectReyesTriplanarProjectionNifMatched =
        objectReyesConfig.triplanarProjectionNifPaths.contains(options.objectReyesNifPath);
    options.objectReyesTripleTapStochasticIncludeSelected = objectReyesConfig.tripleTapStochasticIncludeSelected;
    options.objectReyesTripleTapStochasticNifMatched =
        objectReyesConfig.tripleTapStochasticNifPaths.contains(options.objectReyesNifPath);
    options.objectReyesAtlasBakeResolution = objectReyesConfig.atlasBakeResolution;
    options.objectReyesAtlasBakePaddingTexels = objectReyesConfig.atlasBakePaddingTexels;
    options.objectReyesHeightAtlasStorage = objectReyesConfig.heightAtlasStorage;
    options.objectReyesBakedHeightMaterials.reserve(objectReyesConfig.bakedHeightMaterials.size());
    for (const auto& entry : objectReyesConfig.bakedHeightMaterials) {
        options.objectReyesBakedHeightMaterials.push_back(USDLoader::ObjectReyesBakedHeightMaterialEntry{
            .nifPath = entry.nifPath,
            .materialTexturePaths = entry.materialTexturePaths
        });
    }
    options.isUsdPackage = false;
    return options;
}

class BinaryWriter
{
public:
    explicit BinaryWriter(const fs::path& path) : out(path, std::ios::binary | std::ios::trunc) {}
    explicit operator bool() const { return static_cast<bool>(out); }

    template <class T>
    void Pod(const T& value)
    {
        out.write(reinterpret_cast<const char*>(std::addressof(value)), sizeof(T));
    }

    void Bytes(const void* data, std::uint64_t size)
    {
        if (size != 0) {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
    }

    void String(const std::string& value)
    {
        const std::uint64_t size = value.size();
        Pod(size);
        Bytes(value.data(), size);
    }

    void WString(const std::wstring& value)
    {
        const std::uint64_t size = value.size();
        Pod(size);
        Bytes(value.data(), size * sizeof(wchar_t));
    }

    template <class T>
    void PodVector(const std::vector<T>& values)
    {
        const std::uint64_t size = values.size();
        Pod(size);
        Bytes(values.data(), size * sizeof(T));
    }

    bool Good() const { return static_cast<bool>(out); }

private:
    std::ofstream out;
};

class BinaryReader
{
public:
    explicit BinaryReader(const fs::path& path)
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec || size > 512ull * 1024ull * 1024ull) {
            return;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return;
        }

        bytes.resize(static_cast<std::size_t>(size));
        if (!bytes.empty()) {
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            valid = static_cast<bool>(in);
        }
        else {
            valid = true;
        }
    }

    explicit operator bool() const { return valid; }

    template <class T>
    bool Pod(T& value)
    {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
            return false;
        }
        std::memcpy(std::addressof(value), bytes.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    bool Bytes(void* data, std::uint64_t size)
    {
        if (size == 0) {
            return true;
        }
        if (offset > bytes.size() || bytes.size() - offset < size) {
            return false;
        }
        std::memcpy(data, bytes.data() + offset, static_cast<std::size_t>(size));
        offset += static_cast<std::size_t>(size);
        return true;
    }

    bool String(std::string& value)
    {
        std::uint64_t size = 0;
        if (!Pod(size) || size > (64ull * 1024ull * 1024ull)) {
            return false;
        }
        value.resize(static_cast<std::size_t>(size));
        return Bytes(value.data(), size);
    }

    bool WString(std::wstring& value)
    {
        std::uint64_t size = 0;
        if (!Pod(size) || size > (64ull * 1024ull * 1024ull / sizeof(wchar_t))) {
            return false;
        }
        value.resize(static_cast<std::size_t>(size));
        return Bytes(value.data(), size * sizeof(wchar_t));
    }

    template <class T>
    bool PodVector(std::vector<T>& values)
    {
        std::uint64_t size = 0;
        if (!Pod(size) || size > (256ull * 1024ull * 1024ull / std::max<std::uint64_t>(1, sizeof(T)))) {
            return false;
        }
        values.resize(static_cast<std::size_t>(size));
        return Bytes(values.data(), size * sizeof(T));
    }

private:
    std::vector<std::uint8_t> bytes;
    std::size_t offset{ 0 };
    bool valid{ false };
};

void WriteMatrix(BinaryWriter& writer, const DirectX::XMMATRIX& matrix)
{
    DirectX::XMFLOAT4X4 stored{};
    DirectX::XMStoreFloat4x4(std::addressof(stored), matrix);
    writer.Pod(stored);
}

bool ReadMatrix(BinaryReader& reader, DirectX::XMMATRIX& matrix)
{
    DirectX::XMFLOAT4X4 stored{};
    if (!reader.Pod(stored)) {
        return false;
    }
    matrix = DirectX::XMLoadFloat4x4(std::addressof(stored));
    return true;
}

void WriteStringVector(BinaryWriter& writer, std::span<const std::string> values)
{
    const std::uint64_t size = values.size();
    writer.Pod(size);
    for (const auto& value : values) {
        writer.String(value);
    }
}

bool ReadStringVector(BinaryReader& reader, std::vector<std::string>& values)
{
    std::uint64_t size = 0;
    if (!reader.Pod(size) || size > 100000u) {
        return false;
    }
    values.resize(static_cast<std::size_t>(size));
    for (auto& value : values) {
        if (!reader.String(value)) {
            return false;
        }
    }
    return true;
}

void WritePrototypeGeometry(BinaryWriter& writer, const br::import::RenderablePrototypeGeometry& geometry)
{
    writer.Pod(geometry.vertexFlags);
    writer.PodVector(geometry.vertices);
    writer.PodVector(geometry.indices);
}

bool ReadPrototypeGeometry(BinaryReader& reader, br::import::RenderablePrototypeGeometry& geometry)
{
    if (!reader.Pod(geometry.vertexFlags) ||
        !reader.PodVector(geometry.vertices) ||
        !reader.PodVector(geometry.indices)) {
        return false;
    }
    return geometry.vertices.size() <= 10000000u && geometry.indices.size() <= 30000000u;
}

void WritePrototypeGeometryVector(
    BinaryWriter& writer,
    const std::vector<br::import::RenderablePrototypeGeometry>& geometries)
{
    const std::uint64_t size = geometries.size();
    writer.Pod(size);
    for (const auto& geometry : geometries) {
        WritePrototypeGeometry(writer, geometry);
    }
}

bool ReadPrototypeGeometryVector(
    BinaryReader& reader,
    std::vector<br::import::RenderablePrototypeGeometry>& geometries)
{
    std::uint64_t size = 0;
    if (!reader.Pod(size) || size > 100000u) {
        return false;
    }
    geometries.resize(static_cast<std::size_t>(size));
    for (auto& geometry : geometries) {
        if (!ReadPrototypeGeometry(reader, geometry)) {
            return false;
        }
    }
    return true;
}

std::string BuildCachedTextureSourceIdentity(
    const std::string& texturePath,
    TextureSemantic semantic,
    bool preferSRGB,
    NormalMapConvention normalConvention)
{
    return texturePath + "|semantic:" + std::to_string(static_cast<std::uint32_t>(semantic)) +
        (preferSRGB ? "|srgb" : "|linear") +
        "|normalconv:" + std::to_string(static_cast<std::uint32_t>(normalConvention));
}

TextureProcessingSettings MakeCachedTextureProcessingSettings(
    const std::string& texturePath,
    TextureSemantic semantic,
    bool preferSRGB,
    NormalMapConvention normalConvention)
{
    return MakeMaterialTextureProcessingSettings(
        semantic,
        preferSRGB,
        BuildCachedTextureSourceIdentity(texturePath, semantic, preferSRGB, normalConvention),
        false,
        normalConvention);
}

TextureProcessingSettings GetTextureBindingProcessingSettings(
    const TextureAndConstant& binding,
    TextureSemantic semantic,
    bool preferSRGB,
    NormalMapConvention normalConvention,
    const std::string& texturePath)
{
    if (binding.texture) {
        const TextureProcessingSettings& settings = binding.texture->ProcessingSettings();
        if (settings.isParticipatingMaterialTexture || settings.semantic != TextureSemantic::Unknown) {
            return settings;
        }
    }
    return MakeCachedTextureProcessingSettings(texturePath, semantic, preferSRGB, normalConvention);
}

std::string NormalizeTextureRelativePath(std::string path)
{
    for (char& ch : path) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    while (!path.empty() && (path.front() == '\\' || path.front() == '/')) {
        path.erase(path.begin());
    }
    return path;
}

std::optional<fs::path> ResolveCachedTexturePath(
    const std::string& texturePath,
    const std::vector<std::string>& textureSearchRoots)
{
    if (texturePath.empty()) {
        return std::nullopt;
    }

    std::error_code ec;
    const fs::path input(texturePath);
    if (fs::is_regular_file(input, ec)) {
        fs::path resolved = fs::weakly_canonical(input, ec);
        return ec ? input : resolved;
    }

    const std::string normalizedRelative = NormalizeTextureRelativePath(texturePath);
    std::string withoutTexturesPrefix = normalizedRelative;
    std::string lower = normalizedRelative;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower.rfind("textures\\", 0) == 0) {
        withoutTexturesPrefix = normalizedRelative.substr(std::string_view("textures\\").size());
    }

    std::vector<fs::path> candidateRoots;
    for (const std::string& root : textureSearchRoots) {
        if (!root.empty()) {
            candidateRoots.emplace_back(root);
        }
    }

    candidateRoots.emplace_back(fs::current_path(ec));
    char* tempEnv = nullptr;
    std::size_t tempEnvLength = 0;
    if (_dupenv_s(&tempEnv, &tempEnvLength, "LOCALAPPDATA") == 0 && tempEnv && *tempEnv) {
        candidateRoots.emplace_back(fs::path(tempEnv) / "Temp" / "SARP" / "ResourceCache");
    }
    std::free(tempEnv);
    candidateRoots.emplace_back(fs::temp_directory_path(ec) / "SARP" / "ResourceCache");

    for (const fs::path& root : candidateRoots) {
        std::array<fs::path, 2> candidates = {
            root / normalizedRelative,
            root / "textures" / withoutTexturesPrefix
        };
        for (const fs::path& candidate : candidates) {
            ec.clear();
            if (fs::is_regular_file(candidate, ec)) {
                fs::path resolved = fs::weakly_canonical(candidate, ec);
                return ec ? candidate : resolved;
            }
        }
    }

    return std::nullopt;
}

void WriteTextureProcessingSettings(BinaryWriter& writer, const TextureProcessingSettings& settings)
{
    writer.Pod(static_cast<std::uint32_t>(settings.semantic));
    writer.Pod(settings.isParticipatingMaterialTexture);
    writer.Pod(settings.requestMipChain);
    writer.Pod(settings.requestBlockCompression);
    writer.Pod(settings.allowAsyncPlaceholder);
    writer.Pod(settings.allowCpuBootstrapBeforeAsyncProcessing);
    writer.Pod(settings.preferSRGB);
    writer.Pod(settings.preservePackedChannels);
    writer.Pod(static_cast<std::uint32_t>(settings.normalConvention));
    writer.String(settings.sourceIdentity);
}

bool ReadTextureProcessingSettings(BinaryReader& reader, TextureProcessingSettings& settings)
{
    std::uint32_t semantic = 0;
    std::uint32_t normalConvention = 0;
    if (!reader.Pod(semantic) ||
        !reader.Pod(settings.isParticipatingMaterialTexture) ||
        !reader.Pod(settings.requestMipChain) ||
        !reader.Pod(settings.requestBlockCompression) ||
        !reader.Pod(settings.allowAsyncPlaceholder) ||
        !reader.Pod(settings.allowCpuBootstrapBeforeAsyncProcessing) ||
        !reader.Pod(settings.preferSRGB) ||
        !reader.Pod(settings.preservePackedChannels) ||
        !reader.Pod(normalConvention) ||
        !reader.String(settings.sourceIdentity)) {
        return false;
    }
    settings.semantic = static_cast<TextureSemantic>(semantic);
    settings.normalConvention = static_cast<NormalMapConvention>(normalConvention);
    return true;
}

bool NormalTextureNeedsReconstructedZ(rhi::Format format)
{
    switch (format) {
    case rhi::Format::BC5_UNorm:
    case rhi::Format::BC5_SNorm:
    case rhi::Format::R8G8_UNorm:
    case rhi::Format::R8G8_SNorm:
        return true;
    default:
        return false;
    }
}

void WriteTextureBinding(
    BinaryWriter& writer,
    const TextureAndConstant& binding,
    TextureSemantic semantic,
    bool preferSRGB,
    NormalMapConvention normalConvention = NormalMapConvention::DirectX)
{
    const std::string texturePath = !binding.sourcePath.empty()
        ? binding.sourcePath
        : (binding.texture ? binding.texture->Meta().filePath : std::string{});
    writer.Pod(binding.texture != nullptr || !texturePath.empty());
    writer.String(texturePath);
    writer.Pod(binding.factor.Get());
    writer.Pod(binding.uvSetIndex);
    writer.String(binding.uvSetName);
    writer.PodVector(binding.channels);
    WriteTextureProcessingSettings(
        writer,
        GetTextureBindingProcessingSettings(binding, semantic, preferSRGB, normalConvention, texturePath));
}

bool ReadTextureBinding(
    BinaryReader& reader,
    TextureAndConstant& binding,
    TextureSemantic semantic,
    bool preferSRGB,
    const std::vector<std::string>& textureSearchRoots,
    bool loadMaterialTextures)
{
    ZoneScopedN("NifLoader::ReadTextureBinding");
    bool hadTexture = false;
    std::string texturePath;
    float factor = 1.0f;
    if (!reader.Pod(hadTexture) || !reader.String(texturePath) || !reader.Pod(factor) || !reader.Pod(binding.uvSetIndex) || !reader.String(binding.uvSetName) || !reader.PodVector(binding.channels)) {
        return false;
    }
    TextureProcessingSettings processing{};
    if (!ReadTextureProcessingSettings(reader, processing)) {
        return false;
    }
    if (processing.semantic == TextureSemantic::Unknown && !texturePath.empty()) {
        processing = MakeCachedTextureProcessingSettings(
            texturePath,
            TextureSemantic::Unknown,
            preferSRGB,
            NormalMapConvention::DirectX);
    }
    binding.factor = factor;
    binding.sourcePath = texturePath;
    if (!texturePath.empty()) {
        ZoneText(texturePath.data(), texturePath.size());
    }
    if (!hadTexture || !loadMaterialTextures) {
        return true;
    }

    std::optional<fs::path> resolvedTexturePath;
    {
        ZoneScopedN("NifLoader::ReadTextureBinding::ResolveCachedTexturePath");
        resolvedTexturePath = ResolveCachedTexturePath(texturePath, textureSearchRoots);
    }
    if (resolvedTexturePath) {
        try {
            const bool texturePreferSRGB = processing.isParticipatingMaterialTexture
                ? processing.preferSRGB
                : preferSRGB;
            TextureFileMeta cacheProbeMeta{};
            cacheProbeMeta.filePath = resolvedTexturePath->string();
            cacheProbeMeta.preferSRGB = texturePreferSRGB;
            cacheProbeMeta.processing = processing;

            std::wstring conditionedCachePath;
            {
                ZoneScopedN("NifLoader::ReadTextureBinding::GetExistingTextureCachePath");
                conditionedCachePath = TextureProcessingManager::GetInstance().GetExistingCachePathForFile(cacheProbeMeta);
            }
            if (!conditionedCachePath.empty()) {
                TextureFileMeta deferredMeta = cacheProbeMeta;
                deferredMeta.filePath = ws2s(conditionedCachePath);
                deferredMeta.isProcessingCacheArtifact = true;
                {
                    ZoneScopedN("NifLoader::ReadTextureBinding::LoadDeferredConditionedTexture");
                    binding.texture = LoadTextureFromFileDeferred(conditionedCachePath, nullptr, texturePreferSRGB, std::addressof(deferredMeta));
                }
                if (binding.texture) {
                    binding.texture->Meta().filePath = texturePath;
                    binding.texture->Meta().isProcessingCacheArtifact = true;
                }
            } else {
                TextureFileMeta deferredMeta = cacheProbeMeta;
                deferredMeta.filePath = resolvedTexturePath->string();
                {
                    ZoneScopedN("NifLoader::ReadTextureBinding::LoadDeferredSourceTexture");
                    binding.texture = LoadTextureFromFileDeferred(resolvedTexturePath->wstring(), nullptr, texturePreferSRGB, std::addressof(deferredMeta));
                }
                if (binding.texture) {
                    binding.texture->Meta().filePath = texturePath;
                }
            }
            if (binding.texture) {
                binding.texture->Meta().preferSRGB = texturePreferSRGB;
                binding.texture->SetProcessingSettings(processing);
                if (semantic == TextureSemantic::Normal &&
                    NormalTextureNeedsReconstructedZ(binding.texture->Description().format)) {
                    binding.channels = { 0u, 1u, 4u };
                }
            }
        } catch (const std::exception& ex) {
            spdlog::warn("nif_asset_payload_cache: failed to reload texture '{}': {}", texturePath, ex.what());
            binding.texture.reset();
        }
    }
    return true;
}

void WriteMaterialDescription(BinaryWriter& writer, const MaterialDescription& desc)
{
    ZoneScopedN("NifLoader::WriteMaterialDescription");
    ZoneText(desc.name.data(), desc.name.size());
    writer.Pod(static_cast<std::uint32_t>(desc.materialModel));
    writer.String(desc.name);
    writer.Pod(desc.diffuseColor);
    writer.Pod(desc.emissiveColor);
    writer.Pod(desc.alphaCutoff);
    writer.Pod(desc.heightMapScale);
    writer.Pod(desc.geometricDisplacementMin);
    writer.Pod(desc.geometricDisplacementMax);
    writer.Pod(desc.negateNormals);
    writer.Pod(desc.invertNormalGreen);
    writer.Pod(desc.forceDoubleSided);
    writer.Pod(desc.enableGeometricDisplacement);
    writer.Pod(desc.geometricDisplacementOptIn);
    writer.Pod(desc.brniflyVertexAlpha);
    writer.Pod(desc.brniflyZBufferWrite);
    writer.Pod(desc.brniflyDecal);
    writer.Pod(desc.brniflyDynamicDecal);
    writer.Pod(desc.brniflyModelSpaceNormals);
    writer.Pod(desc.heightMapFromBaseColorAlpha);
    writer.Pod(static_cast<std::uint32_t>(desc.objectSurfaceSamplingMode));
    writer.Pod(desc.objectSurfaceUseTriplanarProjection);
    writer.Pod(desc.objectSurfaceUseTripleTapStochastic);
    writer.Pod(desc.objectSurfaceTexelDensity);
    writer.String(desc.staticTextureOverrideSourceName);
    writer.Pod(static_cast<std::uint32_t>(desc.blendState));
    WriteTextureBinding(writer, desc.baseColor, TextureSemantic::BaseColor, true);
    WriteTextureBinding(writer, desc.metallic, TextureSemantic::Metallic, false);
    WriteTextureBinding(writer, desc.roughness, TextureSemantic::Roughness, false);
    WriteTextureBinding(writer, desc.emissive, TextureSemantic::Emissive, true);
    WriteTextureBinding(writer, desc.opacity, TextureSemantic::Opacity, false);
    WriteTextureBinding(writer, desc.aoMap, TextureSemantic::AO, false);
    WriteTextureBinding(writer, desc.heightMap, TextureSemantic::Height, false);
    WriteTextureBinding(writer, desc.normal, TextureSemantic::Normal, false, NormalMapConvention::OpenGL);
    writer.Pod(desc.openPBR);
    writer.Pod(desc.glintEnabled);
    writer.Pod(desc.glintParameters);
    WriteTextureBinding(writer, desc.openPBRTextures.coatColor, TextureSemantic::OpenPBRColor, true);
    WriteTextureBinding(writer, desc.openPBRTextures.coatWeight, TextureSemantic::OpenPBRScalar, false);
    WriteTextureBinding(writer, desc.openPBRTextures.coatRoughness, TextureSemantic::Roughness, false);
    WriteTextureBinding(writer, desc.openPBRTextures.fuzzColor, TextureSemantic::OpenPBRColor, true);
    WriteTextureBinding(writer, desc.openPBRTextures.fuzzWeight, TextureSemantic::OpenPBRScalar, false);
    WriteTextureBinding(writer, desc.openPBRTextures.fuzzRoughness, TextureSemantic::Roughness, false);
}

bool ReadMaterialDescription(
    BinaryReader& reader,
    MaterialDescription& desc,
    const std::vector<std::string>& textureSearchRoots,
    bool loadMaterialTextures)
{
    ZoneScopedN("NifLoader::ReadMaterialDescription");
    std::uint32_t model = 0;
    std::uint32_t blend = 0;
    std::uint32_t objectSurfaceSamplingMode = 0;
    if (!reader.Pod(model) ||
        !reader.String(desc.name) ||
        !reader.Pod(desc.diffuseColor) ||
        !reader.Pod(desc.emissiveColor) ||
        !reader.Pod(desc.alphaCutoff) ||
        !reader.Pod(desc.heightMapScale) ||
        !reader.Pod(desc.geometricDisplacementMin) ||
        !reader.Pod(desc.geometricDisplacementMax) ||
        !reader.Pod(desc.negateNormals) ||
        !reader.Pod(desc.invertNormalGreen) ||
        !reader.Pod(desc.forceDoubleSided) ||
        !reader.Pod(desc.enableGeometricDisplacement) ||
        !reader.Pod(desc.geometricDisplacementOptIn) ||
        !reader.Pod(desc.brniflyVertexAlpha) ||
        !reader.Pod(desc.brniflyZBufferWrite) ||
        !reader.Pod(desc.brniflyDecal) ||
        !reader.Pod(desc.brniflyDynamicDecal) ||
        !reader.Pod(desc.brniflyModelSpaceNormals) ||
        !reader.Pod(desc.heightMapFromBaseColorAlpha) ||
        !reader.Pod(objectSurfaceSamplingMode) ||
        !reader.Pod(desc.objectSurfaceUseTriplanarProjection) ||
        !reader.Pod(desc.objectSurfaceUseTripleTapStochastic) ||
        !reader.Pod(desc.objectSurfaceTexelDensity) ||
        !reader.String(desc.staticTextureOverrideSourceName) ||
        !reader.Pod(blend)) {
        return false;
    }
    desc.materialModel = static_cast<MaterialModel>(model);
    desc.objectSurfaceSamplingMode = static_cast<ObjectSurfaceSamplingMode>(objectSurfaceSamplingMode);
    desc.blendState = static_cast<BlendState>(blend);
    return ReadTextureBinding(reader, desc.baseColor, TextureSemantic::BaseColor, true, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.metallic, TextureSemantic::Metallic, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.roughness, TextureSemantic::Roughness, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.emissive, TextureSemantic::Emissive, true, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.opacity, TextureSemantic::Opacity, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.aoMap, TextureSemantic::AO, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.heightMap, TextureSemantic::Height, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.normal, TextureSemantic::Normal, false, textureSearchRoots, loadMaterialTextures) &&
        reader.Pod(desc.openPBR) &&
        reader.Pod(desc.glintEnabled) &&
        reader.Pod(desc.glintParameters) &&
        ReadTextureBinding(reader, desc.openPBRTextures.coatColor, TextureSemantic::OpenPBRColor, true, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.openPBRTextures.coatWeight, TextureSemantic::OpenPBRScalar, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.openPBRTextures.coatRoughness, TextureSemantic::Roughness, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.openPBRTextures.fuzzColor, TextureSemantic::OpenPBRColor, true, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.openPBRTextures.fuzzWeight, TextureSemantic::OpenPBRScalar, false, textureSearchRoots, loadMaterialTextures) &&
        ReadTextureBinding(reader, desc.openPBRTextures.fuzzRoughness, TextureSemantic::Roughness, false, textureSearchRoots, loadMaterialTextures);
}

void WriteObjectReyesAtlasBakeData(
    BinaryWriter& writer,
    const std::shared_ptr<const Mesh::ObjectReyesAtlasBakeData>& data)
{
    const std::uint8_t hasData = data ? 1u : 0u;
    writer.Pod(hasData);
    if (!data) {
        return;
    }

    writer.Pod(data->atlasWidth);
    writer.Pod(data->atlasHeight);
    writer.Pod(data->atlasUvSetIndex);
    writer.Pod(data->texelsPerUnit);
    writer.Pod(data->blendWidthObjectUnits);
    writer.PodVector(data->positions);
    writer.PodVector(data->normals);
    writer.PodVector(data->atlasUvs);
    const std::uint64_t uvSetCount = data->uvSets.size();
    writer.Pod(uvSetCount);
    for (const auto& uvSet : data->uvSets) {
        writer.PodVector(uvSet);
    }
    writer.PodVector(data->indices);
    writer.PodVector(data->triangleMaterialIndices);
    WriteStringVector(writer, data->sourceMaterialNames);

    const std::uint64_t sourceMaterialCount = data->sourceMaterials.size();
    writer.Pod(sourceMaterialCount);
    for (const MaterialDescription& desc : data->sourceMaterials) {
        WriteMaterialDescription(writer, desc);
    }
}

bool ReadObjectReyesAtlasBakeData(
    BinaryReader& reader,
    std::shared_ptr<const Mesh::ObjectReyesAtlasBakeData>& data,
    const std::vector<std::string>& textureSearchRoots,
    bool loadMaterialTextures)
{
    std::uint8_t hasData = 0u;
    if (!reader.Pod(hasData)) {
        return false;
    }
    if (hasData == 0u) {
        data.reset();
        return true;
    }

    auto mutableData = std::make_shared<Mesh::ObjectReyesAtlasBakeData>();
    if (!reader.Pod(mutableData->atlasWidth) ||
        !reader.Pod(mutableData->atlasHeight) ||
        !reader.Pod(mutableData->atlasUvSetIndex) ||
        !reader.Pod(mutableData->texelsPerUnit) ||
        !reader.Pod(mutableData->blendWidthObjectUnits) ||
        !reader.PodVector(mutableData->positions) ||
        !reader.PodVector(mutableData->normals) ||
        !reader.PodVector(mutableData->atlasUvs)) {
        return false;
    }
    std::uint64_t uvSetCount = 0u;
    if (!reader.Pod(uvSetCount) || uvSetCount > 64u) {
        return false;
    }
    mutableData->uvSets.resize(static_cast<std::size_t>(uvSetCount));
    for (auto& uvSet : mutableData->uvSets) {
        if (!reader.PodVector(uvSet)) {
            return false;
        }
    }
    if (!reader.PodVector(mutableData->indices) ||
        !reader.PodVector(mutableData->triangleMaterialIndices) ||
        !ReadStringVector(reader, mutableData->sourceMaterialNames)) {
        return false;
    }

    std::uint64_t sourceMaterialCount = 0;
    if (!reader.Pod(sourceMaterialCount) || sourceMaterialCount > 1024u) {
        return false;
    }
    mutableData->sourceMaterials.resize(static_cast<std::size_t>(sourceMaterialCount));
    for (MaterialDescription& desc : mutableData->sourceMaterials) {
        if (!ReadMaterialDescription(reader, desc, textureSearchRoots, loadMaterialTextures)) {
            return false;
        }
    }

    const std::size_t vertexCount = mutableData->positions.size();
    const std::size_t triangleCount = mutableData->indices.size() / 3u;
    if (mutableData->atlasWidth == 0u ||
        mutableData->atlasHeight == 0u ||
        mutableData->atlasWidth > 16384u ||
        mutableData->atlasHeight > 16384u ||
        !std::isfinite(mutableData->texelsPerUnit) ||
        mutableData->texelsPerUnit <= 0.0f ||
        mutableData->normals.size() != vertexCount ||
        mutableData->atlasUvs.size() != vertexCount ||
        mutableData->indices.empty() ||
        mutableData->indices.size() % 3u != 0u ||
        mutableData->triangleMaterialIndices.size() != triangleCount ||
        mutableData->sourceMaterialNames.size() != mutableData->sourceMaterials.size() ||
        mutableData->sourceMaterials.empty()) {
        return false;
    }
    for (const auto& uvSet : mutableData->uvSets) {
        if (!uvSet.empty() && uvSet.size() != vertexCount) {
            return false;
        }
    }
    if (vertexCount > 10000000u || mutableData->indices.size() > 30000000u) {
        return false;
    }
    for (const std::uint32_t index : mutableData->indices) {
        if (index >= vertexCount) {
            return false;
        }
    }
    for (const std::uint32_t materialIndex : mutableData->triangleMaterialIndices) {
        if (materialIndex >= mutableData->sourceMaterials.size()) {
            return false;
        }
    }

    data = std::move(mutableData);
    return true;
}

void WritePrebuilt(BinaryWriter& writer, const ClusterLODPrebuiltData& data)
{
    writer.PodVector(data.groups);
    writer.PodVector(data.segments);
    writer.PodVector(data.segmentBounds);
    writer.Pod(data.objectBoundingSphere);
    writer.PodVector(data.groupChunks);
    writer.PodVector(data.groupDiskLocators);
    writer.PodVector(data.pageDiskLocators);
    writer.PodVector(data.groupPageReferences);
    writer.PodVector(data.groupPageReferenceOffsets);
    writer.Pod(data.trianglePageCount);
    writer.Pod(data.voxelPageBase);
    writer.Pod(data.voxelPageCount);
    writer.String(data.cacheSource.sourceIdentifier);
    writer.String(data.cacheSource.primPath);
    writer.String(data.cacheSource.subsetName);
    writer.Pod(data.cacheSource.buildConfigHash);
    writer.WString(data.cacheSource.containerFileName);
    writer.PodVector(data.nodes);
    writer.PodVector(data.lodNodeRanges);
    writer.PodVector(data.lodLevelRoots);
    writer.PodVector(data.assemblyTransforms);
    writer.PodVector(data.assemblyInstances);
    writer.PodVector(data.assemblyBoneRemaps);
    writer.PodVector(data.assemblyBoneRemapIndices);
    writer.Pod(data.assemblySkeletonArtifact.id.digest);
    writer.Pod(data.assemblySkeletonArtifact.schemaVersion);
    writer.Pod(data.assemblySkeletonArtifact.jointCount);
    writer.PodVector(data.partRecords);
    writer.Pod(data.rootPartIndex);
    writer.Pod(data.maxDepth);
    writer.Pod(data.maxTraversalDepth);
}

bool ReadPrebuilt(BinaryReader& reader, ClusterLODPrebuiltData& data)
{
    return reader.PodVector(data.groups) &&
        reader.PodVector(data.segments) &&
        reader.PodVector(data.segmentBounds) &&
        reader.Pod(data.objectBoundingSphere) &&
        reader.PodVector(data.groupChunks) &&
        reader.PodVector(data.groupDiskLocators) &&
        reader.PodVector(data.pageDiskLocators) &&
        reader.PodVector(data.groupPageReferences) &&
        reader.PodVector(data.groupPageReferenceOffsets) &&
        reader.Pod(data.trianglePageCount) &&
        reader.Pod(data.voxelPageBase) &&
        reader.Pod(data.voxelPageCount) &&
        reader.String(data.cacheSource.sourceIdentifier) &&
        reader.String(data.cacheSource.primPath) &&
        reader.String(data.cacheSource.subsetName) &&
        reader.Pod(data.cacheSource.buildConfigHash) &&
        reader.WString(data.cacheSource.containerFileName) &&
        reader.PodVector(data.nodes) &&
        reader.PodVector(data.lodNodeRanges) &&
        reader.PodVector(data.lodLevelRoots) &&
        reader.PodVector(data.assemblyTransforms) &&
        reader.PodVector(data.assemblyInstances) &&
        reader.PodVector(data.assemblyBoneRemaps) &&
        reader.PodVector(data.assemblyBoneRemapIndices) &&
        reader.Pod(data.assemblySkeletonArtifact.id.digest) &&
        reader.Pod(data.assemblySkeletonArtifact.schemaVersion) &&
        reader.Pod(data.assemblySkeletonArtifact.jointCount) &&
        reader.PodVector(data.partRecords) &&
        reader.Pod(data.rootPartIndex) &&
        reader.Pod(data.maxDepth) &&
        reader.Pod(data.maxTraversalDepth);
}

bool WritePayloadCache(
    const fs::path& cachePath,
    const std::string& normalizedCacheKey,
    const std::string& pathHash,
    const std::string& contentHash,
    const std::vector<std::string>& textureSearchRoots,
    const USDLoader::ImportedAssetPayload& payload)
{
    ZoneScopedN("NifLoader::WritePayloadCache");
    ZoneText(normalizedCacheKey.data(), normalizedCacheKey.size());
    TracyPlot("SARP.Import.NifMeta.Write.MeshCount", static_cast<int64_t>(payload.meshes.size()));
    TracyPlot("SARP.Import.NifMeta.Write.PartCount", static_cast<int64_t>(payload.parts.size()));

    if (payload.meshes.empty() || payload.parts.empty()) {
		spdlog::debug(
			"nif_asset_payload_cache: refusing to cache empty renderable payload for '{}'",
            normalizedCacheKey);
        return false;
    }

    const fs::path payloadPath = cachePath;
    std::error_code ec;
    fs::create_directories(payloadPath.parent_path(), ec);
    BinaryWriter writer(payloadPath);
    if (!writer) {
        return false;
    }

    const std::uint32_t magic = 0x50524153u; // SARP
    const std::uint32_t version = kPayloadCacheVersion;
    writer.Pod(magic);
    writer.Pod(version);
    writer.String(normalizedCacheKey);
    writer.String(pathHash);
    writer.String(contentHash);
    WriteStringVector(writer, textureSearchRoots);

    std::unordered_map<const Mesh*, std::uint32_t> meshIndices;
    {
        ZoneScopedN("NifLoader::WritePayloadCache::BuildMeshIndex");
        for (std::uint32_t i = 0; i < payload.meshes.size(); ++i) {
            meshIndices[payload.meshes[i].get()] = i;
        }
    }

    const std::uint64_t meshCount = payload.meshes.size();
    writer.Pod(meshCount);
    for (std::uint64_t meshIndex = 0; meshIndex < payload.meshes.size(); ++meshIndex) {
        ZoneScopedN("NifLoader::WritePayloadCache::Mesh");
        const auto& mesh = payload.meshes[static_cast<std::size_t>(meshIndex)];
        if (!mesh || !mesh->material) {
            return false;
        }
        MaterialDescription desc = mesh->material->ToCacheDescription();
        ZoneText(desc.name.data(), desc.name.size());
        std::uint64_t materialHash = 0;
        {
            ZoneScopedN("NifLoader::WritePayloadCache::Mesh::ComputeMaterialHash");
            materialHash = ComputeMaterialHash(*mesh->material);
        }
        WriteMaterialDescription(writer, mesh->material->ToCacheDescription());
        const ClusterLODPrebuiltData prebuiltData = mesh->GetClusterLODPrebuiltData();
        {
            ZoneScopedN("NifLoader::WritePayloadCache::Mesh::WritePrebuilt");
            writer.Pod(materialHash);
            WritePrebuilt(writer, prebuiltData);
            WriteObjectReyesAtlasBakeData(writer, mesh->GetObjectReyesAtlasBakeData());
        }
        const auto& meshCB = mesh->GetPerMeshCBData();
        {
            ZoneScopedN("NifLoader::WritePayloadCache::Mesh::WriteSkinning");
            writer.Pod(meshCB.vertexFlags);
            writer.Pod(meshCB.vertexByteSize);
            writer.Pod(meshCB.skinningVertexByteSize);
            WriteStringVector(writer, mesh->GetSkinJointNames());
            writer.PodVector(mesh->GetSkinJointSourceIndices());
            const auto& inverseBinds = mesh->GetSkinInverseBindMatrices();
            const std::uint64_t inverseBindCount = inverseBinds.size();
            writer.Pod(inverseBindCount);
            for (const auto& matrix : inverseBinds) {
                WriteMatrix(writer, matrix);
            }
            const CachedSkeletonStorage skeletonStorage = !prebuiltData.assemblySkeletonArtifact.Empty()
                ? CachedSkeletonStorage::SharedArtifact
                : (mesh->HasBaseSkin() ? CachedSkeletonStorage::Inline : CachedSkeletonStorage::None);
            writer.Pod(skeletonStorage);
            if (skeletonStorage == CachedSkeletonStorage::Inline) {
                const auto skeleton = mesh->GetBaseSkin();
                const auto boneNames = skeleton->GetBoneNames();
                const auto parents = skeleton->GetParentIndices();
                const auto skeletonInverseBinds = skeleton->GetInverseBindMatrices();
                const std::uint64_t boneCount = boneNames.size();
                writer.Pod(boneCount);
                for (const auto& name : boneNames) {
                    writer.String(name);
                }
                writer.PodVector(std::vector<std::int32_t>(parents.begin(), parents.end()));
                const std::uint64_t skeletonBindCount = skeletonInverseBinds.size();
                writer.Pod(skeletonBindCount);
                for (const auto& matrix : skeletonInverseBinds) {
                    WriteMatrix(writer, matrix);
                }
            }
        }
    }

    const std::uint64_t partCount = payload.parts.size();
    writer.Pod(partCount);
    {
        ZoneScopedN("NifLoader::WritePayloadCache::Parts");
        for (const auto& part : payload.parts) {
            writer.String(part.name);
            WriteMatrix(writer, part.localMatrix);
            writer.Pod(part.skinnedShapeIndex);
            const std::uint64_t partMeshCount = part.meshes.size();
            writer.Pod(partMeshCount);
            for (const auto& mesh : part.meshes) {
                auto it = meshIndices.find(mesh.get());
                const std::uint32_t meshIndex = it == meshIndices.end() ? UINT32_MAX : it->second;
                writer.Pod(meshIndex);
            }
            WritePrototypeGeometryVector(writer, part.prototypeGeometries);
        }
    }
    return writer.Good();
}

std::optional<USDLoader::ImportedAssetPayload> TryLoadPayloadCache(
    const fs::path& cachePath,
    const std::string& normalizedCacheKey,
    const std::string& pathHash,
    const std::string& contentHash,
    bool loadMaterialTextures)
{
    ZoneScopedN("NifLoader::TryLoadPayloadCache");
    const auto cachePathText = cachePath.string();
    ZoneText(cachePathText.data(), cachePathText.size());
    BinaryReader reader(cachePath);
    if (!reader) {
        return std::nullopt;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::string fileKey;
    std::string filePathHash;
    std::string fileContentHash;
    if (!reader.Pod(magic) || !reader.Pod(version) ||
        magic != 0x50524153u || version != kPayloadCacheVersion ||
        !reader.String(fileKey) || !reader.String(filePathHash) || !reader.String(fileContentHash) ||
        fileKey != normalizedCacheKey || filePathHash != pathHash || fileContentHash != contentHash) {
        return std::nullopt;
    }

    std::vector<std::string> textureSearchRoots;
    if (!ReadStringVector(reader, textureSearchRoots)) {
        return std::nullopt;
    }
    USDLoader::ImportedAssetPayload payload;
    std::uint64_t meshCount = 0;
    if (!reader.Pod(meshCount) || meshCount > 100000u) {
        return std::nullopt;
    }
    payload.meshes.reserve(static_cast<std::size_t>(meshCount));
    payload.meshMaterialHashes.reserve(static_cast<std::size_t>(meshCount));
    TracyPlot("SARP.Import.NifMeta.Read.MeshCount", static_cast<int64_t>(meshCount));
    for (std::uint64_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        ZoneScopedN("NifLoader::TryLoadPayloadCache::Mesh");
        MaterialDescription desc{};
        ClusterLODPrebuiltData prebuilt{};
        std::shared_ptr<const Mesh::ObjectReyesAtlasBakeData> atlasBakeData;
        std::uint64_t materialHash = 0;
        {
            ZoneScopedN("NifLoader::TryLoadPayloadCache::Mesh::ReadMaterialAndPrebuilt");
            if (!ReadMaterialDescription(reader, desc, textureSearchRoots, loadMaterialTextures) ||
                !reader.Pod(materialHash) ||
                !ReadPrebuilt(reader, prebuilt) ||
                !ReadObjectReyesAtlasBakeData(reader, atlasBakeData, textureSearchRoots, loadMaterialTextures)) {
                return std::nullopt;
            }
        }
        const std::uint64_t expectedBuildConfigHash =
            CLodCache::ComputeBuildConfigHash(prebuilt.cacheSource.sourceIdentifier);
        if (prebuilt.cacheSource.buildConfigHash != expectedBuildConfigHash) {
            spdlog::info(
                "nif_asset_payload_cache: stale CLod build config for '{}' mesh={} cached=0x{:016X} expected=0x{:016X}",
                normalizedCacheKey,
                meshIndex,
                prebuilt.cacheSource.buildConfigHash,
                expectedBuildConfigHash);
            return std::nullopt;
        }
        ZoneText(desc.name.data(), desc.name.size());
        const SkeletonArtifactReference skeletonArtifact = prebuilt.assemblySkeletonArtifact;
        std::uint32_t vertexFlags = 0;
        std::uint32_t vertexByteSize = 0;
        std::uint32_t skinningVertexByteSize = 0;
        if (!reader.Pod(vertexFlags) || !reader.Pod(vertexByteSize) || !reader.Pod(skinningVertexByteSize)) {
            return std::nullopt;
        }
        std::shared_ptr<Mesh> mesh;
        {
            ZoneScopedN("NifLoader::TryLoadPayloadCache::Mesh::CreateMesh");
            auto material = Material::CreateShared(desc);
            if (desc.objectSurfaceSamplingMode == ObjectSurfaceSamplingMode::AtlasBakedHeight) {
                const auto materialData = material->GetData();
                spdlog::info(
                    "nif_meta_cache=atlas_material game='{}' material='{}' height='{}' flags=0x{:x} compileFlags=0x{:x} rasterFlags=0x{:x} geomEnabled={} heightUv={} heightSourcePathOnly={}",
                    normalizedCacheKey,
                    desc.name,
                    desc.heightMap.sourcePath,
                    materialData.materialFlags,
                    static_cast<std::uint64_t>(material->Technique().compileFlags),
                    static_cast<std::uint32_t>(material->Technique().rasterFlags),
                    materialData.geometricDisplacementEnabled,
                    materialData.heightUvSetIndex,
                    desc.heightMap.texture ? 0 : 1);
            }
            auto vertices = std::make_unique<std::vector<std::byte>>();
            std::vector<UINT32> indices;
            std::vector<MeshUvSetData> uvSets;
            mesh = Mesh::CreateSharedFromIngest(
                std::move(vertices),
                vertexByteSize,
                std::nullopt,
                skinningVertexByteSize,
                std::move(indices),
                std::move(uvSets),
                material,
                vertexFlags,
                std::move(prebuilt),
                MeshCpuDataPolicy::ReleaseAfterUpload);
        }
        if (!mesh) {
            return std::nullopt;
        }
        mesh->SetObjectReyesAtlasBakeData(std::move(atlasBakeData));
        std::vector<std::string> jointNames;
        std::vector<std::uint32_t> jointSourceIndices;
        if (!ReadStringVector(reader, jointNames) || !reader.PodVector(jointSourceIndices)) {
            return std::nullopt;
        }
        mesh->SetSkinJointNames(std::move(jointNames));
        mesh->SetSkinJointSourceIndices(std::move(jointSourceIndices));
        std::uint64_t inverseBindCount = 0;
        if (!reader.Pod(inverseBindCount) || inverseBindCount > 100000u) {
            return std::nullopt;
        }
        std::vector<DirectX::XMMATRIX> inverseBinds;
        inverseBinds.resize(static_cast<std::size_t>(inverseBindCount));
        for (auto& matrix : inverseBinds) {
            if (!ReadMatrix(reader, matrix)) {
                return std::nullopt;
            }
        }
        mesh->SetSkinInverseBindMatrices(std::move(inverseBinds));
        CachedSkeletonStorage skeletonStorage = CachedSkeletonStorage::None;
        if (!reader.Pod(skeletonStorage) || skeletonStorage > CachedSkeletonStorage::Inline) {
            return std::nullopt;
        }
        if (skeletonStorage == CachedSkeletonStorage::SharedArtifact) {
            if (skeletonArtifact.Empty()) {
                return std::nullopt;
            }
            std::string artifactError;
            auto baseSkeleton = SkeletonArtifactCache::ResolveSkeleton(skeletonArtifact, &artifactError);
            if (!baseSkeleton) {
                spdlog::warn(
                    "nif_asset_payload_cache: skeleton artifact {} could not be resolved for '{}': {}",
                    skeletonArtifact.id.ToString(), normalizedCacheKey, artifactError);
                return std::nullopt;
            }
            mesh->SetBaseSkin(std::move(baseSkeleton));
        }
        else if (skeletonStorage == CachedSkeletonStorage::Inline) {
            std::uint64_t boneCount = 0;
            if (!reader.Pod(boneCount) || boneCount > 100000u) {
                return std::nullopt;
            }
            std::vector<std::string> boneNames;
            boneNames.resize(static_cast<std::size_t>(boneCount));
            for (auto& name : boneNames) {
                if (!reader.String(name)) {
                    return std::nullopt;
                }
            }
            std::vector<std::int32_t> parents;
            if (!reader.PodVector(parents)) {
                return std::nullopt;
            }
            std::uint64_t skeletonBindCount = 0;
            if (!reader.Pod(skeletonBindCount) || skeletonBindCount > 100000u) {
                return std::nullopt;
            }
            std::vector<DirectX::XMMATRIX> skeletonInverseBinds;
            skeletonInverseBinds.resize(static_cast<std::size_t>(skeletonBindCount));
            for (auto& matrix : skeletonInverseBinds) {
                if (!ReadMatrix(reader, matrix)) {
                    return std::nullopt;
                }
            }
            mesh->SetBaseSkin(std::make_shared<Skeleton>(std::move(boneNames), std::move(parents), std::move(skeletonInverseBinds)));
        }
        payload.meshes.push_back(std::move(mesh));
        payload.meshMaterialHashes.push_back(materialHash);
    }

    std::uint64_t partCount = 0;
    if (!reader.Pod(partCount) || partCount > 100000u) {
        return std::nullopt;
    }
    {
        ZoneScopedN("NifLoader::TryLoadPayloadCache::Parts");
        TracyPlot("SARP.Import.NifMeta.Read.PartCount", static_cast<int64_t>(partCount));
        payload.parts.reserve(static_cast<std::size_t>(partCount));
        for (std::uint64_t partIndex = 0; partIndex < partCount; ++partIndex) {
            USDLoader::RenderablePartPayload part;
            if (!reader.String(part.name) || !ReadMatrix(reader, part.localMatrix) || !reader.Pod(part.skinnedShapeIndex)) {
                return std::nullopt;
            }
            std::uint64_t partMeshCount = 0;
            if (!reader.Pod(partMeshCount) || partMeshCount > 100000u) {
                return std::nullopt;
            }
            part.meshes.reserve(static_cast<std::size_t>(partMeshCount));
            for (std::uint64_t i = 0; i < partMeshCount; ++i) {
                std::uint32_t meshIndex = UINT32_MAX;
                if (!reader.Pod(meshIndex) || meshIndex >= payload.meshes.size()) {
                    return std::nullopt;
                }
                part.meshes.push_back(payload.meshes[meshIndex]);
            }
            if (!ReadPrototypeGeometryVector(reader, part.prototypeGeometries)) {
                return std::nullopt;
            }
            payload.parts.push_back(std::move(part));
        }
    }
    return payload;
}

} // namespace

std::optional<USDLoader::ImportedAssetPayload> TryLoadCachedImportedAsset(std::string cacheKey, const USDLoader::ImportSettings& settings, LoadTimingStats* stats)
{
    ZoneScopedN("NifLoader::TryLoadCachedImportedAsset");
    ZoneText(cacheKey.data(), cacheKey.size());
    const auto probeBegin = std::chrono::steady_clock::now();
    const std::string normalizedCacheKey = NormalizeNifCacheKey(cacheKey);
    if (normalizedCacheKey.empty()) {
        if (stats) {
            stats->cacheProbeMs += ElapsedMs(probeBegin, std::chrono::steady_clock::now());
        }
        return std::nullopt;
    }
    const ObjectReyesConfig objectReyesConfig = LoadObjectReyesConfig();
    const bool objectReyesRequiresCurrentPayload =
        ObjectReyesConfigMayAffectCachedPayload(objectReyesConfig, normalizedCacheKey);

    const std::string pathHash = Hex64(Fnv1a64(normalizedCacheKey));
    std::vector<fs::path> candidates;
    {
        ZoneScopedN("NifLoader::TryLoadCachedImportedAsset::FindCachedAssets");
        candidates = FindCachedAssets(pathHash);
    }
    TracyPlot("SARP.Import.NifMeta.CandidateCount", static_cast<int64_t>(candidates.size()));
    if (candidates.empty()) {
        spdlog::debug("nif_meta_cache=miss game='{}' path_hash='{}' reason='no nif metadata found'", normalizedCacheKey, pathHash);
        if (stats) {
            stats->cacheProbeMs += ElapsedMs(probeBegin, std::chrono::steady_clock::now());
        }
        return std::nullopt;
    }

    for (const auto& cachePath : candidates) {
        ZoneScopedN("NifLoader::TryLoadCachedImportedAsset::ProbeCandidate");
        const std::string fileContentHash = ExtractContentHashFromFileName(cachePath);
        if (objectReyesRequiresCurrentPayload &&
            !CachedContentHashMatchesObjectReyesConfig(fileContentHash, objectReyesConfig)) {
            spdlog::debug(
                "nif_meta_cache=skip_candidate game='{}' path='{}' content_hash='{}' reason='object Reyes config hash {} may affect payload'",
                normalizedCacheKey,
                cachePath.string(),
                fileContentHash,
                objectReyesConfig.contentHash);
            continue;
        }

        if (auto payload = TryLoadPayloadCache(cachePath, normalizedCacheKey, pathHash, fileContentHash, settings.loadMaterialTextures)) {
            spdlog::debug(
                "nif_meta_cache=hit game='{}' path='{}' content_hash='{}'",
                normalizedCacheKey,
                cachePath.string(),
                fileContentHash);
            if (stats) {
                stats->cacheProbeMs += ElapsedMs(probeBegin, std::chrono::steady_clock::now());
                stats->cacheHit = true;
                stats->payloadCacheHit = true;
                stats->cachePath = cachePath;
                stats->contentHash = fileContentHash;
                stats->sourceIdentifier = MakeStableSourceIdentifier(normalizedCacheKey, fileContentHash);
            }
            return payload;
        }
    }

    if (stats) {
        stats->cacheProbeMs += ElapsedMs(probeBegin, std::chrono::steady_clock::now());
    }
    return std::nullopt;
}

std::optional<USDLoader::ImportedAssetPayload> LoadImportedAssetWithCacheKey(std::string filePath, std::string cacheKey, const USDLoader::ImportSettings& settings, LoadTimingStats* stats)
{
    ZoneScopedN("NifLoader::LoadImportedAssetWithCacheKey");
    ZoneText(cacheKey.empty() ? filePath.data() : cacheKey.data(), cacheKey.empty() ? filePath.size() : cacheKey.size());
    const std::string normalizedCacheKey = NormalizeNifCacheKey(cacheKey.empty() ? filePath : cacheKey);
    const std::string pathHash = Hex64(Fnv1a64(normalizedCacheKey));
    if (IsKnownNonRenderableNif(normalizedCacheKey)) {
        spdlog::info("Skipping known non-renderable NIF '{}'", normalizedCacheKey);
        return std::nullopt;
    }

    std::string errorMessage;
    const auto brniflyBegin = std::chrono::steady_clock::now();
    BRNiflyClient::TimingStats brniflyTiming{};
    std::optional<BRNiflyClient::UsdAssetPackage> package;
    {
        ZoneScopedN("NifLoader::LoadImportedAssetWithCacheKey::BRNiflyConvertNifToUsd");
        ZoneText(normalizedCacheKey.data(), normalizedCacheKey.size());
        package = BRNiflyClient::ConvertNifToUsd(filePath, {}, &errorMessage, std::addressof(brniflyTiming));
    }
    if (stats) {
        stats->brniflyMs += ElapsedMs(brniflyBegin, std::chrono::steady_clock::now());
        stats->brniflyDescribeMs += brniflyTiming.describeServicesMs;
        stats->brniflyConvertMs += brniflyTiming.convertProcessMs;
        AccumulateBRNiflyChildTimingStats(*stats, brniflyTiming);
    }
    if (!package) {
        if (stats) {
            stats->importFailureReason = errorMessage;
        }
        if (errorMessage.find("No USD-representable data was emitted from the NIF") != std::string::npos) {
            spdlog::info("NIF import skipped for '{}': {}", filePath, errorMessage);
        }
        else {
            spdlog::error("NIF import failed for '{}': {}", filePath, errorMessage);
        }
        return std::nullopt;
    }

    for (const auto& diagnostic : package->diagnostics) {
        if (diagnostic.level == "warning") {
            spdlog::warn("BRNifly: {}", diagnostic.message);
        }
        else if (diagnostic.level == "error") {
            spdlog::error("BRNifly: {}", diagnostic.message);
        }
        else {
            spdlog::info("BRNifly: {}", diagnostic.message);
        }
    }

    const ObjectReyesConfig objectReyesConfig = LoadObjectReyesConfig();
    const std::vector<std::string> textureSearchRoots =
        MergeTextureSearchRoots(package->textureSearchRoots, settings.additionalTextureSearchRoots);
    const std::string effectiveContentHash = package->contentHash + "_object_reyes_" +
        std::string(kObjectReyesConfigVersion) + "_" + objectReyesConfig.contentHash +
        "_texroots_" + TextureSearchRootsHash(textureSearchRoots);
    const std::string stableSourceIdentifier = MakeStableSourceIdentifier(normalizedCacheKey, effectiveContentHash);
    const std::string sourceDirectory = fs::path(filePath).parent_path().string();
    auto options = MakeStageOptions(
        stableSourceIdentifier,
        sourceDirectory,
        textureSearchRoots,
        "brnifly_" + package->contentHash + ".usda",
        objectReyesConfig,
        normalizedCacheKey);
    const auto extractBegin = std::chrono::steady_clock::now();
    std::optional<USDLoader::ImportedAssetPayload> payload;
    USDLoader::ImportTimingStats usdTiming;
    {
        ZoneScopedN("NifLoader::LoadImportedAssetWithCacheKey::LoadUsdPayload");
        payload = USDLoader::LoadImportedAssetFromUsdBytes(package->rootLayerText, options, settings, &usdTiming);
    }
    if (stats) {
        const auto elapsed = ElapsedMs(extractBegin, std::chrono::steady_clock::now());
        stats->usdLoadMs += elapsed;
        stats->usdOpenMs += usdTiming.layerImportMs + usdTiming.stageOpenMs;
        stats->usdExtractMs += usdTiming.payloadParseMs;
        stats->meshBuildMs += usdTiming.meshPreprocessMs;
    }
    if (payload) {
        {
            ZoneScopedN("NifLoader::LoadImportedAssetWithCacheKey::EnsurePayloadMaterialHashes");
            EnsurePayloadMaterialHashes(*payload);
        }
        const fs::path cachePath = CLodCache::GetCacheFilePathForSource(
            s2ws(MakeAssetFileName(normalizedCacheKey, pathHash, effectiveContentHash)),
            stableSourceIdentifier);
        const auto cacheWriteBegin = std::chrono::steady_clock::now();
        bool wrote = false;
        bool reusedExisting = false;
        if (auto existingPayload = TryLoadPayloadCache(cachePath, normalizedCacheKey, pathHash, effectiveContentHash, settings.loadMaterialTextures)) {
            payload = std::move(existingPayload);
            reusedExisting = true;
            RegisterCachedAsset(pathHash, cachePath);
            spdlog::info(
                "nif_meta_cache=existing game='{}' path='{}' content_hash='{}'",
                normalizedCacheKey,
                cachePath.string(),
                effectiveContentHash);
        } else {
            wrote = WritePayloadCache(
                cachePath,
                normalizedCacheKey,
                pathHash,
                effectiveContentHash,
                textureSearchRoots,
                *payload);
        }
        if (stats) {
            stats->assetWriteMs += ElapsedMs(cacheWriteBegin, std::chrono::steady_clock::now());
            stats->payloadCacheHit = stats->payloadCacheHit || reusedExisting;
            stats->assetCacheWritten = wrote;
            stats->cachePath = cachePath;
            stats->sourceIdentifier = stableSourceIdentifier;
            stats->contentHash = effectiveContentHash;
        }
        if (wrote) {
            RegisterCachedAsset(pathHash, cachePath);
            spdlog::info(
                "nif_meta_cache=write game='{}' path='{}' content_hash='{}'",
                normalizedCacheKey,
                cachePath.string(),
                effectiveContentHash);
        }
    }
    return payload;
}

PreprocessResult PreprocessNifWithCacheKey(std::string filePath, std::string cacheKey, const USDLoader::ImportSettings& settings, LoadTimingStats* stats)
{
    PreprocessResult result{};
    LoadTimingStats localStats{};
    LoadTimingStats* timing = stats ? stats : std::addressof(localStats);

    const std::string normalizedCacheKey = NormalizeNifCacheKey(cacheKey.empty() ? filePath : cacheKey);
    if (normalizedCacheKey.empty()) {
        result.failureReason = "empty cache key";
        return result;
    }
    if (IsKnownNonRenderableNif(normalizedCacheKey)) {
        result.skipped = true;
        result.success = true;
        result.failureReason = "known non-renderable NIF";
        return result;
    }

    auto payload = TryLoadCachedImportedAsset(normalizedCacheKey, settings, timing);
    if (!payload) {
        payload = LoadImportedAssetWithCacheKey(std::move(filePath), normalizedCacheKey, settings, timing);
    }

    result.cacheHit = timing->cacheHit;
    result.payloadCacheHit = timing->payloadCacheHit;
    result.assetCacheWritten = timing->assetCacheWritten;
    result.assetCachePath = timing->cachePath;
    result.sourceIdentifier = timing->sourceIdentifier;
    result.contentHash = timing->contentHash;

    if (!payload) {
        result.failureReason = timing->importFailureReason.empty() ?
            "NIF import/cache preprocessing failed" :
            timing->importFailureReason;
        if (result.failureReason.find("No USD-representable data was emitted from the NIF") != std::string::npos) {
            result.failureReason = "no renderable geometry: " + result.failureReason;
            result.skipped = true;
            result.success = true;
        }
        return result;
    }

    result.submeshes = payload->meshes.size();
    std::unordered_set<std::uint64_t> materialCompileFlags;
    const auto collectMaterialFlags = [&materialCompileFlags](const std::shared_ptr<Mesh>& mesh) {
        if (mesh && mesh->material) {
            materialCompileFlags.insert(static_cast<std::uint64_t>(mesh->material->Technique().compileFlags));
        }
    };
    for (const auto& mesh : payload->meshes) {
        collectMaterialFlags(mesh);
    }
    for (const auto& part : payload->parts) {
        for (const auto& mesh : part.meshes) {
            collectMaterialFlags(mesh);
			if (!mesh || !mesh->material) {
				continue;
			}
			PreprocessResult::MaterialMetadata metadata;
			metadata.description = mesh->material->ToCacheDescription();
			metadata.compileFlags = static_cast<std::uint64_t>(mesh->material->Technique().compileFlags);
			metadata.partName = part.name;
			metadata.skinnedShapeIndex = part.skinnedShapeIndex;
			auto clearTexture = [](TextureAndConstant& binding) {
				binding.texture.reset();
			};
			clearTexture(metadata.description.baseColor);
			clearTexture(metadata.description.metallic);
			clearTexture(metadata.description.roughness);
			clearTexture(metadata.description.emissive);
			clearTexture(metadata.description.opacity);
			clearTexture(metadata.description.aoMap);
			clearTexture(metadata.description.heightMap);
			clearTexture(metadata.description.normal);
			clearTexture(metadata.description.openPBRTextures.coatColor);
			clearTexture(metadata.description.openPBRTextures.coatWeight);
			clearTexture(metadata.description.openPBRTextures.coatRoughness);
			clearTexture(metadata.description.openPBRTextures.fuzzColor);
			clearTexture(metadata.description.openPBRTextures.fuzzWeight);
			clearTexture(metadata.description.openPBRTextures.fuzzRoughness);
			result.materials.push_back(std::move(metadata));
        }
    }
    result.materialCompileFlags.assign(materialCompileFlags.begin(), materialCompileFlags.end());
    result.success = true;
    return result;
}

std::shared_ptr<Scene> LoadModelWithCacheKey(std::string filePath, std::string cacheKey, const USDLoader::ImportSettings& settings, LoadTimingStats* stats)
{
    const std::string normalizedCacheKey = NormalizeNifCacheKey(cacheKey.empty() ? filePath : cacheKey);
    if (IsKnownNonRenderableNif(normalizedCacheKey)) {
        spdlog::info("Skipping known non-renderable NIF '{}'", normalizedCacheKey);
        return nullptr;
    }

    std::string errorMessage;
    const auto brniflyBegin = std::chrono::steady_clock::now();
    BRNiflyClient::TimingStats brniflyTiming{};
    std::optional<BRNiflyClient::UsdAssetPackage> package;
    {
        ZoneScopedN("NifLoader::LoadModelWithCacheKey::BRNiflyConvertNifToUsd");
        ZoneText(normalizedCacheKey.data(), normalizedCacheKey.size());
        package = BRNiflyClient::ConvertNifToUsd(filePath, {}, &errorMessage, std::addressof(brniflyTiming));
    }
    if (stats) {
        stats->brniflyMs += ElapsedMs(brniflyBegin, std::chrono::steady_clock::now());
        stats->brniflyDescribeMs += brniflyTiming.describeServicesMs;
        stats->brniflyConvertMs += brniflyTiming.convertProcessMs;
        AccumulateBRNiflyChildTimingStats(*stats, brniflyTiming);
    }
    if (!package) {
        spdlog::error("NIF import failed for '{}': {}", filePath, errorMessage);
        return nullptr;
    }

    for (const auto& diagnostic : package->diagnostics) {
        if (diagnostic.level == "warning") {
            spdlog::warn("BRNifly: {}", diagnostic.message);
        }
        else if (diagnostic.level == "error") {
            spdlog::error("BRNifly: {}", diagnostic.message);
        }
        else {
            spdlog::info("BRNifly: {}", diagnostic.message);
        }
    }

    const ObjectReyesConfig objectReyesConfig = LoadObjectReyesConfig();
    const std::vector<std::string> textureSearchRoots =
        MergeTextureSearchRoots(package->textureSearchRoots, settings.additionalTextureSearchRoots);
    const std::string effectiveContentHash = package->contentHash + "_object_reyes_" +
        std::string(kObjectReyesConfigVersion) + "_" + objectReyesConfig.contentHash +
        "_texroots_" + TextureSearchRootsHash(textureSearchRoots);
    const std::string stableSourceIdentifier = MakeStableSourceIdentifier(normalizedCacheKey, effectiveContentHash);
    const std::string sourceDirectory = fs::path(filePath).parent_path().string();
    auto options = MakeStageOptions(
        stableSourceIdentifier,
        sourceDirectory,
        textureSearchRoots,
        "brnifly_" + package->contentHash + ".usda",
        objectReyesConfig,
        normalizedCacheKey);
    const auto usdLoadBegin = std::chrono::steady_clock::now();
    auto scene = USDLoader::LoadModelFromUsdBytes(package->rootLayerText, options, settings);
    if (stats) {
        stats->usdLoadMs += ElapsedMs(usdLoadBegin, std::chrono::steady_clock::now());
    }
    return scene;
}

std::shared_ptr<Scene> LoadModel(std::string filePath, const USDLoader::ImportSettings& settings)
{
    return LoadModelWithCacheKey(filePath, filePath, settings, nullptr);
}

} // namespace NifLoader
