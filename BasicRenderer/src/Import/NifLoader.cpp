#include "Import/NifLoader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

#include "Import/BRNiflyClient.h"
#include "Import/CLodCache.h"
#include "Import/USDGeometryExtractor.h"
#include "Animation/Skeleton.h"
#include "Materials/Material.h"
#include "Mesh/Mesh.h"
#include "Scene/Scene.h"
#include "Utilities/CachePathUtilities.h"
#include "Utilities/Utilities.h"

namespace NifLoader {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kNifMetaCacheSuffix = ".nifmeta";

std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point end)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
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
    return fs::current_path() / "cache" / "clod";
}

fs::path AssetPathIndexRoot()
{
    return CLodCacheRoot() / "nif_meta_index";
}

fs::path AssetManifestPath()
{
    return AssetPathIndexRoot() / "manifest.tsv";
}

constexpr std::uint32_t kPayloadCacheVersion = 11u;

struct AssetCacheIndex {
    std::mutex mutex;
    bool manifestLoaded{ false };
    std::unordered_map<std::string, std::vector<fs::path>> byPathHash;
};

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
            if (!pathHash.empty() && HasAssetCacheSuffix(path) && ExtractPathHashFromFileName(path) == pathHash) {
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
    const std::string& layerIdentifierHint)
{
    USDLoader::InMemoryStageOptions options{};
    options.sourceIdentifier = sourceIdentifier;
    options.sourceDirectory = sourceDirectory;
    options.layerIdentifierHint = layerIdentifierHint;
    options.isUsdPackage = false;
    return options;
}

struct AssetCacheWriteResult
{
    fs::path path;
    bool wrote{ false };
};

fs::path PayloadCachePathForAssetCache(const fs::path& cachePath)
{
    return cachePath;
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

void WriteTextureBinding(BinaryWriter& writer, const TextureAndConstant& binding)
{
    writer.String(!binding.sourcePath.empty() ? binding.sourcePath : (binding.texture ? binding.texture->Meta().filePath : std::string{}));
    writer.Pod(binding.factor.Get());
    writer.Pod(binding.uvSetIndex);
    writer.String(binding.uvSetName);
    writer.PodVector(binding.channels);
}

bool ReadTextureBinding(BinaryReader& reader, TextureAndConstant& binding, bool preferSRGB)
{
    std::string texturePath;
    float factor = 1.0f;
    if (!reader.String(texturePath) || !reader.Pod(factor) || !reader.Pod(binding.uvSetIndex) || !reader.String(binding.uvSetName) || !reader.PodVector(binding.channels)) {
        return false;
    }
    binding.factor = factor;
    binding.sourcePath = texturePath;
    if (!texturePath.empty() && fs::is_regular_file(texturePath)) {
        try {
            binding.texture = LoadTextureFromFile(s2ws(texturePath), nullptr, preferSRGB);
        } catch (const std::exception& ex) {
            spdlog::warn("nif_asset_payload_cache: failed to reload texture '{}': {}", texturePath, ex.what());
            binding.texture.reset();
        }
    }
    return true;
}

void WriteMaterialDescription(BinaryWriter& writer, const MaterialDescription& desc)
{
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
    writer.Pod(desc.brniflyVertexAlpha);
    writer.Pod(desc.brniflyZBufferWrite);
    writer.Pod(desc.brniflyDecal);
    writer.Pod(desc.brniflyDynamicDecal);
    writer.Pod(desc.brniflyModelSpaceNormals);
    writer.Pod(static_cast<std::uint32_t>(desc.blendState));
    WriteTextureBinding(writer, desc.baseColor);
    WriteTextureBinding(writer, desc.metallic);
    WriteTextureBinding(writer, desc.roughness);
    WriteTextureBinding(writer, desc.emissive);
    WriteTextureBinding(writer, desc.opacity);
    WriteTextureBinding(writer, desc.aoMap);
    WriteTextureBinding(writer, desc.heightMap);
    WriteTextureBinding(writer, desc.normal);
    writer.Pod(desc.openPBR);
    WriteTextureBinding(writer, desc.openPBRTextures.coatColor);
    WriteTextureBinding(writer, desc.openPBRTextures.coatWeight);
    WriteTextureBinding(writer, desc.openPBRTextures.coatRoughness);
    WriteTextureBinding(writer, desc.openPBRTextures.fuzzColor);
    WriteTextureBinding(writer, desc.openPBRTextures.fuzzWeight);
    WriteTextureBinding(writer, desc.openPBRTextures.fuzzRoughness);
}

bool ReadMaterialDescription(BinaryReader& reader, MaterialDescription& desc)
{
    std::uint32_t model = 0;
    std::uint32_t blend = 0;
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
        !reader.Pod(desc.brniflyVertexAlpha) ||
        !reader.Pod(desc.brniflyZBufferWrite) ||
        !reader.Pod(desc.brniflyDecal) ||
        !reader.Pod(desc.brniflyDynamicDecal) ||
        !reader.Pod(desc.brniflyModelSpaceNormals) ||
        !reader.Pod(blend)) {
        return false;
    }
    desc.materialModel = static_cast<MaterialModel>(model);
    desc.blendState = static_cast<BlendState>(blend);
    return ReadTextureBinding(reader, desc.baseColor, true) &&
        ReadTextureBinding(reader, desc.metallic, false) &&
        ReadTextureBinding(reader, desc.roughness, false) &&
        ReadTextureBinding(reader, desc.emissive, true) &&
        ReadTextureBinding(reader, desc.opacity, false) &&
        ReadTextureBinding(reader, desc.aoMap, false) &&
        ReadTextureBinding(reader, desc.heightMap, false) &&
        ReadTextureBinding(reader, desc.normal, false) &&
        reader.Pod(desc.openPBR) &&
        ReadTextureBinding(reader, desc.openPBRTextures.coatColor, true) &&
        ReadTextureBinding(reader, desc.openPBRTextures.coatWeight, false) &&
        ReadTextureBinding(reader, desc.openPBRTextures.coatRoughness, false) &&
        ReadTextureBinding(reader, desc.openPBRTextures.fuzzColor, true) &&
        ReadTextureBinding(reader, desc.openPBRTextures.fuzzWeight, false) &&
        ReadTextureBinding(reader, desc.openPBRTextures.fuzzRoughness, false);
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
        reader.Pod(data.maxDepth) &&
        reader.Pod(data.maxTraversalDepth);
}

bool WritePayloadCache(
    const fs::path& cachePath,
    const std::string& normalizedCacheKey,
    const std::string& pathHash,
    const std::string& contentHash,
    const USDLoader::ImportedAssetPayload& payload)
{
    if (payload.meshes.empty() || payload.parts.empty()) {
        spdlog::warn(
            "nif_asset_payload_cache: refusing to cache empty renderable payload for '{}'",
            normalizedCacheKey);
        return false;
    }

    const fs::path payloadPath = PayloadCachePathForAssetCache(cachePath);
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

    std::unordered_map<const Mesh*, std::uint32_t> meshIndices;
    for (std::uint32_t i = 0; i < payload.meshes.size(); ++i) {
        meshIndices[payload.meshes[i].get()] = i;
    }

    const std::uint64_t meshCount = payload.meshes.size();
    writer.Pod(meshCount);
    for (const auto& mesh : payload.meshes) {
        if (!mesh || !mesh->material) {
            return false;
        }
        WriteMaterialDescription(writer, mesh->material->ToCacheDescription());
        WritePrebuilt(writer, mesh->GetClusterLODPrebuiltData());
        const auto& meshCB = mesh->GetPerMeshCBData();
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
        std::uint8_t hasSkeleton = mesh->HasBaseSkin() ? 1u : 0u;
        writer.Pod(hasSkeleton);
        if (hasSkeleton) {
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

    const std::uint64_t partCount = payload.parts.size();
    writer.Pod(partCount);
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
    }
    return writer.Good();
}

std::optional<USDLoader::ImportedAssetPayload> TryLoadPayloadCache(
    const fs::path& cachePath,
    const std::string& normalizedCacheKey,
    const std::string& pathHash,
    const std::string& contentHash)
{
    BinaryReader reader(PayloadCachePathForAssetCache(cachePath));
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

    USDLoader::ImportedAssetPayload payload;
    std::uint64_t meshCount = 0;
    if (!reader.Pod(meshCount) || meshCount > 100000u) {
        return std::nullopt;
    }
    payload.meshes.reserve(static_cast<std::size_t>(meshCount));
    for (std::uint64_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        MaterialDescription desc{};
        ClusterLODPrebuiltData prebuilt{};
        if (!ReadMaterialDescription(reader, desc) || !ReadPrebuilt(reader, prebuilt)) {
            return std::nullopt;
        }
        std::uint32_t vertexFlags = 0;
        std::uint32_t vertexByteSize = 0;
        std::uint32_t skinningVertexByteSize = 0;
        if (!reader.Pod(vertexFlags) || !reader.Pod(vertexByteSize) || !reader.Pod(skinningVertexByteSize)) {
            return std::nullopt;
        }
        auto material = Material::CreateShared(desc);
        auto vertices = std::make_unique<std::vector<std::byte>>();
        std::vector<UINT32> indices;
        std::vector<MeshUvSetData> uvSets;
        auto mesh = Mesh::CreateSharedFromIngest(
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
        if (!mesh) {
            return std::nullopt;
        }
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
        std::uint8_t hasSkeleton = 0;
        if (!reader.Pod(hasSkeleton)) {
            return std::nullopt;
        }
        if (hasSkeleton != 0u) {
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
    }

    std::uint64_t partCount = 0;
    if (!reader.Pod(partCount) || partCount > 100000u) {
        return std::nullopt;
    }
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
        payload.parts.push_back(std::move(part));
    }
    return payload;
}

} // namespace

std::optional<CachedAssetLoadResult> TryLoadCachedModel(std::string cacheKey, const USDLoader::ImportSettings& settings, LoadTimingStats* stats)
{
    (void)cacheKey;
    (void)settings;
    const auto probeBegin = std::chrono::steady_clock::now();
    if (stats) {
        stats->cacheProbeMs += ElapsedMs(probeBegin, std::chrono::steady_clock::now());
    }
    return std::nullopt;
}

std::optional<USDLoader::ImportedAssetPayload> TryLoadCachedImportedAsset(std::string cacheKey, const USDLoader::ImportSettings& settings, LoadTimingStats* stats)
{
    const auto probeBegin = std::chrono::steady_clock::now();
    const std::string normalizedCacheKey = NormalizeNifCacheKey(cacheKey);
    if (normalizedCacheKey.empty()) {
        if (stats) {
            stats->cacheProbeMs += ElapsedMs(probeBegin, std::chrono::steady_clock::now());
        }
        return std::nullopt;
    }

    const std::string pathHash = Hex64(Fnv1a64(normalizedCacheKey));
    auto candidates = FindCachedAssets(pathHash);
    if (candidates.empty()) {
        spdlog::debug("nif_meta_cache=miss game='{}' path_hash='{}' reason='no nif metadata found'", normalizedCacheKey, pathHash);
        if (stats) {
            stats->cacheProbeMs += ElapsedMs(probeBegin, std::chrono::steady_clock::now());
        }
        return std::nullopt;
    }

    for (const auto& cachePath : candidates) {
        const std::string fileContentHash = ExtractContentHashFromFileName(cachePath);

        if (auto payload = TryLoadPayloadCache(cachePath, normalizedCacheKey, pathHash, fileContentHash)) {
            spdlog::debug(
                "nif_meta_cache=hit game='{}' path='{}' content_hash='{}'",
                normalizedCacheKey,
                PayloadCachePathForAssetCache(cachePath).string(),
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
    const std::string normalizedCacheKey = NormalizeNifCacheKey(cacheKey.empty() ? filePath : cacheKey);
    const std::string pathHash = Hex64(Fnv1a64(normalizedCacheKey));
    if (IsKnownNonRenderableNif(normalizedCacheKey)) {
        spdlog::info("Skipping known non-renderable NIF '{}'", normalizedCacheKey);
        return std::nullopt;
    }

    std::string errorMessage;
    const auto brniflyBegin = std::chrono::steady_clock::now();
    BRNiflyClient::TimingStats brniflyTiming{};
    auto package = BRNiflyClient::ConvertNifToUsd(filePath, {}, &errorMessage, std::addressof(brniflyTiming));
    if (stats) {
        stats->brniflyMs += ElapsedMs(brniflyBegin, std::chrono::steady_clock::now());
        stats->brniflyDescribeMs += brniflyTiming.describeServicesMs;
        stats->brniflyConvertMs += brniflyTiming.convertProcessMs;
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

    const std::string stableSourceIdentifier = MakeStableSourceIdentifier(normalizedCacheKey, package->contentHash);
    const std::string sourceDirectory = fs::path(filePath).parent_path().string();
    auto options = MakeStageOptions(
        stableSourceIdentifier,
        sourceDirectory,
        "brnifly_" + package->contentHash + ".usda");
    const auto extractBegin = std::chrono::steady_clock::now();
    auto payload = USDLoader::LoadImportedAssetFromUsdBytes(package->rootLayerText, options, settings);
    if (stats) {
        const auto elapsed = ElapsedMs(extractBegin, std::chrono::steady_clock::now());
        stats->usdExtractMs += elapsed;
        stats->meshBuildMs += elapsed;
        stats->usdLoadMs += elapsed;
    }
    if (payload) {
        const fs::path cachePath = CLodCache::GetCacheFilePathForSource(
            s2ws(MakeAssetFileName(normalizedCacheKey, pathHash, package->contentHash)),
            stableSourceIdentifier);
        const auto cacheWriteBegin = std::chrono::steady_clock::now();
        const bool wrote = WritePayloadCache(cachePath, normalizedCacheKey, pathHash, package->contentHash, *payload);
        if (stats) {
            stats->assetWriteMs += ElapsedMs(cacheWriteBegin, std::chrono::steady_clock::now());
            stats->assetCacheWritten = wrote;
            stats->cachePath = cachePath;
            stats->sourceIdentifier = stableSourceIdentifier;
            stats->contentHash = package->contentHash;
        }
        if (wrote) {
            RegisterCachedAsset(pathHash, cachePath);
            spdlog::info(
                "nif_meta_cache=write game='{}' path='{}' content_hash='{}'",
                normalizedCacheKey,
                cachePath.string(),
                package->contentHash);
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
    auto package = BRNiflyClient::ConvertNifToUsd(filePath, {}, &errorMessage, std::addressof(brniflyTiming));
    if (stats) {
        stats->brniflyMs += ElapsedMs(brniflyBegin, std::chrono::steady_clock::now());
        stats->brniflyDescribeMs += brniflyTiming.describeServicesMs;
        stats->brniflyConvertMs += brniflyTiming.convertProcessMs;
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

    const std::string stableSourceIdentifier = MakeStableSourceIdentifier(normalizedCacheKey, package->contentHash);
    const std::string sourceDirectory = fs::path(filePath).parent_path().string();
    auto options = MakeStageOptions(
        stableSourceIdentifier,
        sourceDirectory,
        "brnifly_" + package->contentHash + ".usda");
    const auto usdLoadBegin = std::chrono::steady_clock::now();
    auto scene = USDLoader::LoadModelFromUsdBytes(package->rootLayerText, options, settings);
    if (stats) {
        stats->usdLoadMs += ElapsedMs(usdLoadBegin, std::chrono::steady_clock::now());
    }
    return scene;
}

std::shared_ptr<Scene> LoadModel(std::string filePath, const USDLoader::ImportSettings& settings)
{
    if (auto cached = TryLoadCachedModel(filePath, settings, nullptr)) {
        return cached->scene;
    }

    return LoadModelWithCacheKey(filePath, filePath, settings, nullptr);
}

} // namespace NifLoader
