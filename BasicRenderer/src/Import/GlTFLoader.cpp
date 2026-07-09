#include "Import/GlTFLoader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Managers/Singletons/TextureProcessingManager.h"
#include "Animation/Animation.h"
#include "Animation/AnimationController.h"
#include "Animation/Skeleton.h"
#include "Import/GlTFGeometryExtractor.h"
#include "Materials/Material.h"
#include "Mesh/Mesh.h"
#include "Mesh/MeshInstance.h"
#include "Mesh/VertexFlags.h"
#include "Resources/Sampler.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Utilities/Utilities.h"

using nlohmann::json;
using namespace DirectX;

namespace {

struct PrimitiveData {
    std::shared_ptr<Mesh> mesh;
};

struct PreparedPrimitiveData {
    MeshPreprocessResult result;
    std::shared_ptr<Material> material;
};

struct BufferViewInfo {
    size_t bufferIndex = 0;
    uint64_t byteOffset = 0;
    uint64_t byteLength = 0;
    uint64_t byteStride = 0;
};

struct AccessorInfo {
    size_t bufferViewIndex = 0;
    uint64_t byteOffset = 0;
    size_t count = 0;
    int componentType = 0;
    std::string type;
    bool normalized = false;
};

struct NodeHierarchyBuildResult {
    std::vector<flecs::entity> entities;
    std::vector<int32_t> meshIndices;
};

struct MeshBindingKey {
    size_t meshIndex = 0;
    int skinIndex = -1;

    bool operator==(const MeshBindingKey& other) const noexcept {
        return meshIndex == other.meshIndex && skinIndex == other.skinIndex;
    }
};

struct MeshBindingKeyHasher {
    size_t operator()(const MeshBindingKey& key) const noexcept {
        size_t seed = std::hash<size_t>{}(key.meshIndex);
        seed ^= std::hash<int>{}(key.skinIndex) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }
};

enum class BufferBacking {
    FileSpan,
    DataUri,
    Fallback,
};

struct BufferSource {
    BufferBacking backing = BufferBacking::FileSpan;
    std::filesystem::path filePath;
    uint64_t fileOffset = 0;
    uint64_t fileLength = 0;
    std::vector<uint8_t> inlineBytes;
};

struct GlbBinaryChunkSpan {
    uint64_t offset = 0;
    uint64_t length = 0;
};

struct GlTFMaterialCache {
    std::unordered_map<std::string, std::shared_ptr<TextureAsset>> textureCache;
    std::vector<std::shared_ptr<Material>> materialCache;
    std::vector<BufferSource> bufferSources;
    std::string sourceKey;
};

struct SharedTextureCacheEntry {
    std::weak_ptr<TextureAsset> texture;
};

struct SharedMaterialCacheEntry {
    std::weak_ptr<Material> material;
};

std::mutex g_gltfMaterialCacheMutex;
std::unordered_map<std::string, SharedTextureCacheEntry> g_sharedTextureCache;
std::unordered_map<std::string, SharedMaterialCacheEntry> g_sharedMaterialCache;

constexpr uint32_t kGlbMagic = 0x46546C67;
constexpr uint32_t kGlbJsonChunkType = 0x4E4F534A;
constexpr uint32_t kGlbBinChunkType = 0x004E4942;
constexpr uint32_t kMaterialTextureMaxAnisotropy = 16;

bool GltfMaterialDebugLoggingEnabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        size_t valueLength = 0;
        const bool isSet =
            _dupenv_s(&value, &valueLength, "SARP_GLTF_MATERIAL_LOG") == 0 &&
            value != nullptr &&
            value[0] != '\0' &&
            value[0] != '0';
        std::free(value);
        return isSet;
    }();
    return enabled;
}

std::string NormalizeSourceKey(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = path.lexically_normal();
    }

    auto key = normalized.generic_string();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
    return key;
}

std::string MakeMaterialCacheKey(const std::string& sourceKey, size_t materialIndex) {
    return sourceKey + "#mat:" + std::to_string(materialIndex);
}

std::string BuildGlTFSamplerSignature(const json& gltf, const json& textureNode) {
    int wrapS = 10497;
    int wrapT = 10497;
    int minFilter = 9987;
    int magFilter = 9729;

    if (textureNode.contains("sampler")) {
        const auto& samplers = gltf.contains("samplers") ? gltf["samplers"] : json::array();
        const size_t samplerIndex = textureNode["sampler"].get<size_t>();
        if (samplerIndex >= samplers.size()) {
            throw std::runtime_error("glTF sampler index out of range");
        }

        const auto& samplerNode = samplers[samplerIndex];
        wrapS = samplerNode.value<int>("wrapS", wrapS);
        wrapT = samplerNode.value<int>("wrapT", wrapT);
        minFilter = samplerNode.value<int>("minFilter", minFilter);
        magFilter = samplerNode.value<int>("magFilter", magFilter);
    }

    return "sampler:" +
        std::to_string(wrapS) + ":" +
        std::to_string(wrapT) + ":" +
        std::to_string(minFilter) + ":" +
        std::to_string(magFilter);
}

std::string BuildGlTFImageIdentity(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    const std::string& sourceKey,
    size_t imageIndex)
{
    const auto& images = gltf.at("images");
    if (imageIndex >= images.size()) {
        throw std::runtime_error("glTF image index out of range");
    }

    const auto& image = images[imageIndex];
    if (image.contains("uri")) {
        const std::string uri = image["uri"].get<std::string>();
        if (uri.rfind("data:", 0) == 0) {
            return sourceKey + "#image-data-uri:" + std::to_string(imageIndex);
        }

        return NormalizeSourceKey(sourcePath.parent_path() / std::filesystem::path(uri));
    }

    if (image.contains("bufferView")) {
        const size_t bufferViewIndex = image["bufferView"].get<size_t>();
        const std::string mimeType = image.value("mimeType", std::string());
        return sourceKey + "#image-bv:" + std::to_string(bufferViewIndex) + ":" + mimeType;
    }

    return sourceKey + "#image:" + std::to_string(imageIndex);
}

size_t ResolveTextureImageIndex(const json& textureNode) {
    if (textureNode.contains("source")) {
        return textureNode["source"].get<size_t>();
    }
    if (textureNode.contains("extensions") && textureNode["extensions"].contains("KHR_texture_basisu")) {
        return textureNode["extensions"]["KHR_texture_basisu"].at("source").get<size_t>();
    }

    throw std::runtime_error("glTF texture has no source image");
}

std::string BuildTextureResourceKey(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    const GlTFMaterialCache& cache,
    size_t textureIndex,
    bool preferSRGB,
    TextureSemantic semantic,
    bool preservePackedChannels,
    NormalMapConvention normalConvention)
{
    const auto& textures = gltf.at("textures");
    if (textureIndex >= textures.size()) {
        throw std::runtime_error("glTF texture index out of range");
    }

    const auto& textureNode = textures[textureIndex];
    const size_t imageIndex = ResolveTextureImageIndex(textureNode);
    const std::string imageIdentity = BuildGlTFImageIdentity(gltf, sourcePath, cache.sourceKey, imageIndex);
    const std::string samplerSignature = BuildGlTFSamplerSignature(gltf, textureNode);
    return imageIdentity + "|" + samplerSignature + (preferSRGB ? "|srgb" : "|linear") +
        "|semantic:" + std::to_string(static_cast<uint32_t>(semantic)) +
        (preservePackedChannels ? "|packed" : "|unpadded") +
        "|normalconv:" + std::to_string(static_cast<uint32_t>(normalConvention));
}

uint64_t GetFileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("Failed to query file size: " + path.string());
    }
    return static_cast<uint64_t>(size);
}

std::vector<uint8_t> ReadFileRange(const std::filesystem::path& path, uint64_t offset, uint64_t size) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    const uint64_t fileSize = GetFileSize(path);
    if (offset > fileSize || size > fileSize - offset) {
        throw std::runtime_error("Read range out of file bounds: " + path.string());
    }

    std::vector<uint8_t> out(static_cast<size_t>(size));
    if (size == 0) {
        return out;
    }

    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (!file) {
        throw std::runtime_error("Failed to read file range: " + path.string());
    }

    return out;
}

uint32_t ReadU32LE(const std::filesystem::path& path, uint64_t offset) {
    const auto bytes = ReadFileRange(path, offset, sizeof(uint32_t));
    uint32_t value = 0;
    std::memcpy(&value, bytes.data(), sizeof(uint32_t));
    return value;
}

std::optional<GlbBinaryChunkSpan> GetGlbBinaryChunkSpan(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path) || path.extension() != ".glb") {
        return std::nullopt;
    }

    const uint64_t fileSize = GetFileSize(path);
    if (fileSize < 12) {
        throw std::runtime_error("GLB file too small: " + path.string());
    }

    const uint32_t magic = ReadU32LE(path, 0);
    if (magic != kGlbMagic) {
        throw std::runtime_error("Invalid GLB magic: " + path.string());
    }

    uint64_t offset = 12;
    while (offset + 8 <= fileSize) {
        const uint32_t chunkLength = ReadU32LE(path, offset);
        const uint32_t chunkType = ReadU32LE(path, offset + 4);
        offset += 8;

        if (offset + chunkLength > fileSize) {
            throw std::runtime_error("Invalid GLB chunk length in: " + path.string());
        }

        if (chunkType == kGlbBinChunkType) {
            return GlbBinaryChunkSpan{ offset, chunkLength };
        }

        if (chunkType != kGlbJsonChunkType) {
            spdlog::debug("Skipping non-JSON/ BIN GLB chunk type {} in {}", chunkType, path.string());
        }

        offset += chunkLength;
    }

    return std::nullopt;
}

int Base64CharToValue(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

std::vector<uint8_t> DecodeBase64(const std::string& encoded) {
    std::vector<uint8_t> decoded;
    decoded.reserve((encoded.size() * 3) / 4);

    int value = 0;
    int bitCount = -8;
    for (const unsigned char c : encoded) {
        if (std::isspace(c) != 0) {
            continue;
        }

        const int digit = Base64CharToValue(c);
        if (digit == -1) {
            throw std::runtime_error("Invalid base64 data in data URI");
        }
        if (digit == -2) {
            break;
        }

        value = (value << 6) + digit;
        bitCount += 6;
        if (bitCount >= 0) {
            decoded.push_back(static_cast<uint8_t>((value >> bitCount) & 0xFF));
            bitCount -= 8;
        }
    }

    return decoded;
}

std::vector<uint8_t> DecodeDataUri(const std::string& uri) {
    const size_t commaPos = uri.find(',');
    if (commaPos == std::string::npos) {
        throw std::runtime_error("Invalid data URI");
    }

    const std::string metadata = uri.substr(0, commaPos);
    const std::string payload = uri.substr(commaPos + 1);
    if (metadata.find(";base64") == std::string::npos) {
        throw std::runtime_error("Only base64 data URIs are supported");
    }

    return DecodeBase64(payload);
}

bool DocumentUsesMeshoptCompression(const json& gltf) {
    if (!gltf.contains("extensionsUsed") || !gltf["extensionsUsed"].is_array()) {
        return false;
    }

    for (const auto& extension : gltf["extensionsUsed"]) {
        if (extension.is_string() && extension.get<std::string>() == "EXT_meshopt_compression") {
            return true;
        }
    }

    return false;
}

std::vector<BufferSource> BuildBufferSources(const json& gltf, const std::filesystem::path& sourcePath) {
    std::vector<BufferSource> bufferSources;
    if (!gltf.contains("buffers") || !gltf["buffers"].is_array()) {
        return bufferSources;
    }

    const auto binaryChunk = GetGlbBinaryChunkSpan(sourcePath);
    std::string extension = sourcePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
    const bool isGlb = extension == ".glb";
    const bool hasMeshoptCompression = DocumentUsesMeshoptCompression(gltf);
    const auto& buffers = gltf["buffers"];
    bufferSources.resize(buffers.size());

    bool usedBinaryChunk = false;
    for (size_t bufferIndex = 0; bufferIndex < buffers.size(); ++bufferIndex) {
        const auto& buffer = buffers[bufferIndex];
        BufferSource source;

        if (buffer.contains("uri")) {
            const std::string uri = buffer["uri"].get<std::string>();
            if (uri.rfind("data:", 0) == 0) {
                source.backing = BufferBacking::DataUri;
                source.inlineBytes = DecodeDataUri(uri);
                source.fileLength = source.inlineBytes.size();
            }
            else {
                const std::filesystem::path resolvedPath = sourcePath.parent_path() / std::filesystem::path(uri);
                source.backing = BufferBacking::FileSpan;
                source.filePath = resolvedPath;
                source.fileLength = GetFileSize(resolvedPath);
            }
        }
        else {
            if (!isGlb) {
                if (hasMeshoptCompression) {
                    source.backing = BufferBacking::Fallback;
                    source.fileLength = buffer.value<uint64_t>("byteLength", 0);
                    bufferSources[bufferIndex] = std::move(source);
                    continue;
                }

                throw std::runtime_error("glTF buffer has no URI and source is not a GLB: " + sourcePath.string());
            }
            if (!binaryChunk.has_value()) {
                throw std::runtime_error("glTF buffer has no URI and source is not a GLB: " + sourcePath.string());
            }
            if (usedBinaryChunk) {
                throw std::runtime_error("Multiple URI-less glTF buffers are not supported in: " + sourcePath.string());
            }

            source.backing = BufferBacking::FileSpan;
            source.filePath = sourcePath;
            source.fileOffset = binaryChunk->offset;
            source.fileLength = binaryChunk->length;
            usedBinaryChunk = true;
        }

        bufferSources[bufferIndex] = std::move(source);
    }

    return bufferSources;
}

std::vector<uint8_t> ReadBufferSlice(const BufferSource& source, uint64_t offset, uint64_t size) {
    if (source.backing == BufferBacking::DataUri) {
        if (offset > source.inlineBytes.size() || size > source.inlineBytes.size() - offset) {
            throw std::runtime_error("Data URI buffer slice out of bounds");
        }

        return std::vector<uint8_t>(
            source.inlineBytes.begin() + static_cast<std::ptrdiff_t>(offset),
            source.inlineBytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
    }

    if (source.backing == BufferBacking::Fallback) {
        throw std::runtime_error(
            "Attempted to read a URI-less fallback glTF buffer directly. "
            "This buffer is reserved for EXT_meshopt_compression data and should not be read by GlTFLoader.");
    }

    if (offset > source.fileLength || size > source.fileLength - offset) {
        throw std::runtime_error("Buffer slice out of bounds for: " + source.filePath.string());
    }

    return ReadFileRange(source.filePath, source.fileOffset + offset, size);
}

size_t NumComponentsForType(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    throw std::runtime_error("Unsupported glTF accessor type: " + type);
}

size_t BytesPerComponent(int componentType) {
    switch (componentType) {
    case 5120:
    case 5121:
        return 1;
    case 5122:
    case 5123:
        return 2;
    case 5125:
    case 5126:
        return 4;
    default:
        throw std::runtime_error("Unsupported glTF accessor component type");
    }
}

AccessorInfo GetAccessorInfo(const json& gltf, size_t accessorIndex) {
    const auto& accessors = gltf.at("accessors");
    if (accessorIndex >= accessors.size()) {
        throw std::runtime_error("glTF accessor index out of range");
    }

    const auto& accessor = accessors[accessorIndex];
    if (!accessor.contains("bufferView")) {
        throw std::runtime_error("Sparse glTF accessors are not supported in the loader");
    }

    AccessorInfo info;
    info.bufferViewIndex = accessor.at("bufferView").get<size_t>();
    info.byteOffset = accessor.value<uint64_t>("byteOffset", 0);
    info.count = accessor.at("count").get<size_t>();
    info.componentType = accessor.at("componentType").get<int>();
    info.type = accessor.at("type").get<std::string>();
    info.normalized = accessor.value<bool>("normalized", false);
    return info;
}

BufferViewInfo GetBufferViewInfo(const json& gltf, size_t bufferViewIndex) {
    const auto& bufferViews = gltf.at("bufferViews");
    if (bufferViewIndex >= bufferViews.size()) {
        throw std::runtime_error("glTF bufferView index out of range");
    }

    const auto& bufferView = bufferViews[bufferViewIndex];
    BufferViewInfo info;
    info.bufferIndex = bufferView.at("buffer").get<size_t>();
    info.byteOffset = bufferView.value<uint64_t>("byteOffset", 0);
    info.byteLength = bufferView.at("byteLength").get<uint64_t>();
    info.byteStride = bufferView.value<uint64_t>("byteStride", 0);
    return info;
}

template <typename T>
T ReadTyped(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("glTF accessor read out of bounds");
    }

    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

double ReadComponentAsDouble(const std::vector<uint8_t>& bytes, int componentType, bool normalized, size_t offset) {
    switch (componentType) {
    case 5120: {
        const int8_t value = ReadTyped<int8_t>(bytes, offset);
        if (!normalized) {
            return static_cast<double>(value);
        }
        return std::max(static_cast<double>(value) / 127.0, -1.0);
    }
    case 5121: {
        const uint8_t value = ReadTyped<uint8_t>(bytes, offset);
        if (!normalized) {
            return static_cast<double>(value);
        }
        return static_cast<double>(value) / 255.0;
    }
    case 5122: {
        const int16_t value = ReadTyped<int16_t>(bytes, offset);
        if (!normalized) {
            return static_cast<double>(value);
        }
        return std::max(static_cast<double>(value) / 32767.0, -1.0);
    }
    case 5123: {
        const uint16_t value = ReadTyped<uint16_t>(bytes, offset);
        if (!normalized) {
            return static_cast<double>(value);
        }
        return static_cast<double>(value) / 65535.0;
    }
    case 5125:
        return static_cast<double>(ReadTyped<uint32_t>(bytes, offset));
    case 5126:
        return static_cast<double>(ReadTyped<float>(bytes, offset));
    default:
        throw std::runtime_error("Unsupported glTF accessor component type");
    }
}

std::vector<uint8_t> ReadAccessorRawWindow(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    size_t accessorIndex,
    size_t firstElement,
    size_t elementCount,
    size_t* outStride,
    size_t* outComponentCount,
    int* outComponentType,
    bool* outNormalized)
{
    const AccessorInfo accessor = GetAccessorInfo(gltf, accessorIndex);
    const BufferViewInfo view = GetBufferViewInfo(gltf, accessor.bufferViewIndex);

    if (gltf["bufferViews"][accessor.bufferViewIndex].contains("extensions") &&
        gltf["bufferViews"][accessor.bufferViewIndex]["extensions"].contains("EXT_meshopt_compression")) {
        throw std::runtime_error("glTF loader does not yet support EXT_meshopt_compression for skinning or animation accessors");
    }

    const size_t componentCount = NumComponentsForType(accessor.type);
    const size_t componentBytes = BytesPerComponent(accessor.componentType);
    const size_t packedElementSize = componentCount * componentBytes;
    const size_t stride = view.byteStride == 0 ? packedElementSize : view.byteStride;

    if (firstElement > accessor.count || elementCount > accessor.count - firstElement) {
        throw std::runtime_error("glTF accessor window out of bounds");
    }

    *outStride = stride;
    *outComponentCount = componentCount;
    *outComponentType = accessor.componentType;
    *outNormalized = accessor.normalized;

    if (elementCount == 0) {
        return {};
    }

    if (view.bufferIndex >= bufferSources.size()) {
        throw std::runtime_error("glTF accessor buffer index out of range");
    }

    const uint64_t start = view.byteOffset + accessor.byteOffset + static_cast<uint64_t>(firstElement * stride);
    const uint64_t byteLength = static_cast<uint64_t>((elementCount - 1) * stride + packedElementSize);
    if (start + byteLength > view.byteOffset + view.byteLength) {
        throw std::runtime_error("glTF accessor window exceeds bufferView bounds");
    }

    return ReadBufferSlice(bufferSources[view.bufferIndex], start, byteLength);
}

std::vector<float> ReadAccessorScalarsAsFloat(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    size_t accessorIndex)
{
    const AccessorInfo accessor = GetAccessorInfo(gltf, accessorIndex);
    if (NumComponentsForType(accessor.type) != 1) {
        throw std::runtime_error("glTF accessor must be SCALAR");
    }

    std::vector<float> values(accessor.count);
    constexpr size_t kChunkSize = 32768;
    for (size_t first = 0; first < accessor.count; first += kChunkSize) {
        const size_t count = std::min(kChunkSize, accessor.count - first);
        size_t stride = 0;
        size_t componentCount = 0;
        int componentType = 0;
        bool normalized = false;
        const auto bytes = ReadAccessorRawWindow(gltf, bufferSources, accessorIndex, first, count, &stride, &componentCount, &componentType, &normalized);
        for (size_t i = 0; i < count; ++i) {
            values[first + i] = static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, i * stride));
        }
    }

    return values;
}

std::vector<XMFLOAT3> ReadAccessorVec3AsFloat(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    size_t accessorIndex)
{
    const AccessorInfo accessor = GetAccessorInfo(gltf, accessorIndex);
    if (NumComponentsForType(accessor.type) != 3) {
        throw std::runtime_error("glTF accessor must be VEC3");
    }

    std::vector<XMFLOAT3> values(accessor.count);
    constexpr size_t kChunkSize = 32768;
    for (size_t first = 0; first < accessor.count; first += kChunkSize) {
        const size_t count = std::min(kChunkSize, accessor.count - first);
        size_t stride = 0;
        size_t componentCount = 0;
        int componentType = 0;
        bool normalized = false;
        const auto bytes = ReadAccessorRawWindow(gltf, bufferSources, accessorIndex, first, count, &stride, &componentCount, &componentType, &normalized);
        const size_t componentBytes = BytesPerComponent(componentType);
        for (size_t i = 0; i < count; ++i) {
            const size_t base = i * stride;
            values[first + i] = XMFLOAT3(
                static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, base + componentBytes * 0)),
                static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, base + componentBytes * 1)),
                static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, base + componentBytes * 2)));
        }
    }

    return values;
}

std::vector<XMFLOAT4> ReadAccessorVec4AsFloat(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    size_t accessorIndex)
{
    const AccessorInfo accessor = GetAccessorInfo(gltf, accessorIndex);
    if (NumComponentsForType(accessor.type) != 4) {
        throw std::runtime_error("glTF accessor must be VEC4");
    }

    std::vector<XMFLOAT4> values(accessor.count);
    constexpr size_t kChunkSize = 32768;
    for (size_t first = 0; first < accessor.count; first += kChunkSize) {
        const size_t count = std::min(kChunkSize, accessor.count - first);
        size_t stride = 0;
        size_t componentCount = 0;
        int componentType = 0;
        bool normalized = false;
        const auto bytes = ReadAccessorRawWindow(gltf, bufferSources, accessorIndex, first, count, &stride, &componentCount, &componentType, &normalized);
        const size_t componentBytes = BytesPerComponent(componentType);
        for (size_t i = 0; i < count; ++i) {
            const size_t base = i * stride;
            values[first + i] = XMFLOAT4(
                static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, base + componentBytes * 0)),
                static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, base + componentBytes * 1)),
                static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, base + componentBytes * 2)),
                static_cast<float>(ReadComponentAsDouble(bytes, componentType, normalized, base + componentBytes * 3)));
        }
    }

    return values;
}

std::vector<XMMATRIX> ReadAccessorMat4AsMatrices(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    size_t accessorIndex)
{
    const AccessorInfo accessor = GetAccessorInfo(gltf, accessorIndex);
    if (NumComponentsForType(accessor.type) != 16) {
        throw std::runtime_error("glTF accessor must be MAT4");
    }

    std::vector<XMMATRIX> values(accessor.count, XMMatrixIdentity());
    constexpr size_t kChunkSize = 4096;
    for (size_t first = 0; first < accessor.count; first += kChunkSize) {
        const size_t count = std::min(kChunkSize, accessor.count - first);
        size_t stride = 0;
        size_t componentCount = 0;
        int componentType = 0;
        bool normalized = false;
        const auto bytes = ReadAccessorRawWindow(gltf, bufferSources, accessorIndex, first, count, &stride, &componentCount, &componentType, &normalized);
        const size_t componentBytes = BytesPerComponent(componentType);
        for (size_t i = 0; i < count; ++i) {
            const size_t base = i * stride;
            std::array<float, 16> m{};
            for (size_t componentIndex = 0; componentIndex < 16; ++componentIndex) {
                m[componentIndex] = static_cast<float>(ReadComponentAsDouble(
                    bytes,
                    componentType,
                    normalized,
                    base + componentBytes * componentIndex));
            }

            values[first + i] = XMMatrixSet(
                m[0], m[1], m[2], m[3],
                m[4], m[5], m[6], m[7],
                m[8], m[9], m[10], m[11],
                m[12], m[13], m[14], m[15]);
        }
    }

    return values;
}

AnimationInterpolationMode ParseInterpolationMode(const std::string& interpolation) {
    if (interpolation == "LINEAR") {
        return AnimationInterpolationMode::Linear;
    }
    if (interpolation == "STEP") {
        return AnimationInterpolationMode::Step;
    }

    throw std::runtime_error("Unsupported glTF interpolation mode: " + interpolation);
}

std::vector<std::string> BuildNodeAnimationKeys(const json& gltf) {
    std::vector<std::string> keys;
    if (!gltf.contains("nodes")) {
        return keys;
    }

    const auto& nodes = gltf["nodes"];
    keys.resize(nodes.size());
    for (size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        const std::string nodeName = nodes[nodeIndex].value("name", "glTF_Node_" + std::to_string(nodeIndex));
        keys[nodeIndex] = nodeName + "#node_" + std::to_string(nodeIndex);
    }
    return keys;
}

std::vector<uint8_t> ReadImageBytes(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    const std::vector<BufferSource>& bufferSources,
    size_t imageIndex)
{
    const auto& images = gltf.at("images");
    if (imageIndex >= images.size()) {
        throw std::runtime_error("glTF image index out of range");
    }

    const auto& image = images[imageIndex];
    if (image.contains("uri")) {
        const std::string uri = image["uri"].get<std::string>();
        if (uri.rfind("data:", 0) == 0) {
            return DecodeDataUri(uri);
        }

        const auto imagePath = sourcePath.parent_path() / std::filesystem::path(uri);
        return ReadFileRange(imagePath, 0, GetFileSize(imagePath));
    }

    if (image.contains("bufferView")) {
        const auto& bufferViews = gltf.at("bufferViews");
        const size_t bufferViewIndex = image["bufferView"].get<size_t>();
        if (bufferViewIndex >= bufferViews.size()) {
            throw std::runtime_error("glTF image bufferView index out of range");
        }

        const auto& bufferView = bufferViews[bufferViewIndex];
        const size_t bufferIndex = bufferView.at("buffer").get<size_t>();
        if (bufferIndex >= bufferSources.size()) {
            throw std::runtime_error("glTF image buffer index out of range");
        }

        const uint64_t byteOffset = bufferView.value<uint64_t>("byteOffset", 0);
        const uint64_t byteLength = bufferView.at("byteLength").get<uint64_t>();
        return ReadBufferSlice(bufferSources[bufferIndex], byteOffset, byteLength);
    }

    throw std::runtime_error("glTF image has neither uri nor bufferView");
}

rhi::AddressMode ConvertWrapMode(int wrapMode) {
    switch (wrapMode) {
    case 33071:
        return rhi::AddressMode::Clamp;
    case 33648:
        return rhi::AddressMode::Mirror;
    case 10497:
    default:
        return rhi::AddressMode::Wrap;
    }
}

std::shared_ptr<Sampler> CreateTextureSampler(const json& gltf, const json& textureNode) {
    rhi::SamplerDesc samplerDesc = {};
    samplerDesc.addressU = rhi::AddressMode::Wrap;
    samplerDesc.addressV = rhi::AddressMode::Wrap;
    samplerDesc.addressW = rhi::AddressMode::Wrap;
    samplerDesc.mipLodBias = 0.0f;
    samplerDesc.minLod = 0.0f;
    samplerDesc.maxLod = (std::numeric_limits<float>::max)();
    samplerDesc.maxAnisotropy = kMaterialTextureMaxAnisotropy;
    samplerDesc.compareEnable = false;
    samplerDesc.compareOp = rhi::CompareOp::Always;
    samplerDesc.reduction = rhi::ReductionMode::Standard;
    samplerDesc.borderPreset = rhi::BorderPreset::TransparentBlack;
    samplerDesc.minFilter = rhi::Filter::Linear;
    samplerDesc.magFilter = rhi::Filter::Linear;
    samplerDesc.mipFilter = rhi::MipFilter::Linear;

    if (!textureNode.contains("sampler")) {
        return Sampler::CreateSampler(samplerDesc);
    }

    const auto& samplers = gltf.contains("samplers") ? gltf["samplers"] : json::array();
    const size_t samplerIndex = textureNode["sampler"].get<size_t>();
    if (samplerIndex >= samplers.size()) {
        throw std::runtime_error("glTF sampler index out of range");
    }

    const auto& samplerNode = samplers[samplerIndex];
    samplerDesc.addressU = ConvertWrapMode(samplerNode.value<int>("wrapS", 10497));
    samplerDesc.addressV = ConvertWrapMode(samplerNode.value<int>("wrapT", 10497));

    const int minFilter = samplerNode.value<int>("minFilter", 9987);
    const int magFilter = samplerNode.value<int>("magFilter", 9729);

    samplerDesc.magFilter = (magFilter == 9728) ? rhi::Filter::Nearest : rhi::Filter::Linear;
    switch (minFilter) {
    case 9728:
        samplerDesc.minFilter = rhi::Filter::Nearest;
        samplerDesc.mipFilter = rhi::MipFilter::None;
        break;
    case 9729:
        samplerDesc.minFilter = rhi::Filter::Linear;
        samplerDesc.mipFilter = rhi::MipFilter::None;
        break;
    case 9984:
        samplerDesc.minFilter = rhi::Filter::Nearest;
        samplerDesc.mipFilter = rhi::MipFilter::Nearest;
        break;
    case 9985:
        samplerDesc.minFilter = rhi::Filter::Linear;
        samplerDesc.mipFilter = rhi::MipFilter::Nearest;
        break;
    case 9986:
        samplerDesc.minFilter = rhi::Filter::Nearest;
        samplerDesc.mipFilter = rhi::MipFilter::Linear;
        break;
    case 9987:
    default:
        samplerDesc.minFilter = rhi::Filter::Linear;
        samplerDesc.mipFilter = rhi::MipFilter::Linear;
        break;
    }

    return Sampler::CreateSampler(samplerDesc);
}

std::shared_ptr<TextureAsset> LoadTexture(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    GlTFMaterialCache& cache,
    size_t textureIndex,
    bool preferSRGB,
    TextureSemantic semantic,
    bool preservePackedChannels = false,
    NormalMapConvention normalConvention = NormalMapConvention::DirectX)
{
    const std::string cacheKey = BuildTextureResourceKey(gltf, sourcePath, cache, textureIndex, preferSRGB, semantic, preservePackedChannels, normalConvention);
    auto existingTexture = cache.textureCache.find(cacheKey);
    if (existingTexture != cache.textureCache.end()) {
        return existingTexture->second;
    }

    const auto& textures = gltf.at("textures");
    if (textureIndex >= textures.size()) {
        throw std::runtime_error("glTF texture index out of range");
    }

    const auto& textureNode = textures[textureIndex];
    const size_t imageIndex = ResolveTextureImageIndex(textureNode);

    auto sampler = CreateTextureSampler(gltf, textureNode);

    const auto& imageNode = gltf.at("images").at(imageIndex);
    std::filesystem::path cacheProbePath = sourcePath;
    std::string cacheProbeDetail = sourcePath.string();
    if (imageNode.contains("uri")) {
        const std::string uri = imageNode["uri"].get<std::string>();
        if (uri.rfind("data:", 0) != 0) {
            cacheProbePath = sourcePath.parent_path() / std::filesystem::path(uri);
            cacheProbeDetail = cacheProbePath.string();
        }
        else {
            cacheProbeDetail = cache.sourceKey + "#image-data-uri:" + std::to_string(imageIndex);
        }
    }
    else if (imageNode.contains("bufferView")) {
        cacheProbeDetail = BuildGlTFImageIdentity(gltf, sourcePath, cache.sourceKey, imageIndex);
    }

    TextureFileMeta cacheProbeMeta{};
    cacheProbeMeta.filePath = cacheProbePath.string();
    cacheProbeMeta.preferSRGB = preferSRGB;
    cacheProbeMeta.processing = MakeMaterialTextureProcessingSettings(semantic, preferSRGB, cacheKey, preservePackedChannels, normalConvention);

    const std::string sharedCacheKey = cacheKey;

    const std::wstring cachePath = TextureProcessingManager::GetInstance().GetExistingCachePathForFile(cacheProbeMeta);
    if (!cachePath.empty()) {
        auto cachedTexture = LoadTextureFromFile(cachePath, sampler, preferSRGB);
        cachedTexture->Meta().isProcessingCacheArtifact = true;
        cachedTexture->Meta().filePath = cacheProbeMeta.filePath;
        cachedTexture->Meta().preferSRGB = preferSRGB;
        cachedTexture->SetProcessingSettings(cacheProbeMeta.processing);
        cache.textureCache[cacheKey] = cachedTexture;

        {
            std::lock_guard<std::mutex> lock(g_gltfMaterialCacheMutex);
            g_sharedTextureCache[sharedCacheKey] = SharedTextureCacheEntry{ cachedTexture };
        }

        spdlog::info("GlTFLoader: texture processing cache hit for '{}' -> '{}'", cacheProbeDetail, ws2s(cachePath));
        if (GltfMaterialDebugLoggingEnabled()) {
            spdlog::info(
                "SARPDBG glTF texture textureIndex={} imageIndex={} semantic={} srgb={} packed={} normalConvention={} source='{}' cacheHit='{}' alphaOpaque={} fallback={} usable={}",
                textureIndex,
                imageIndex,
                static_cast<uint32_t>(semantic),
                preferSRGB,
                preservePackedChannels,
                static_cast<uint32_t>(normalConvention),
                cacheProbeDetail,
                ws2s(cachePath),
                cachedTexture->Meta().alphaIsAllOpaque,
                cachedTexture->IsUsingFallbackImage(),
                cachedTexture->HasUsableImage());
        }
        return cachedTexture;
    }

    {
        std::lock_guard<std::mutex> lock(g_gltfMaterialCacheMutex);
        auto sharedIt = g_sharedTextureCache.find(sharedCacheKey);
        if (sharedIt != g_sharedTextureCache.end()) {
            if (auto texture = sharedIt->second.texture.lock()) {
                cache.textureCache[cacheKey] = texture;
                return texture;
            }

            g_sharedTextureCache.erase(sharedIt);
        }
    }

    auto textureBytes = ReadImageBytes(gltf, sourcePath, cache.bufferSources, imageIndex);
    auto texture = LoadTextureFromMemory(textureBytes.data(), textureBytes.size(), sampler, {}, preferSRGB);
    texture->Meta().filePath = cacheProbeMeta.filePath;
    texture->SetProcessingSettings(cacheProbeMeta.processing);
    texture->SetGenerateMipmaps(true);
    cache.textureCache[cacheKey] = texture;

    {
        std::lock_guard<std::mutex> lock(g_gltfMaterialCacheMutex);
        g_sharedTextureCache[sharedCacheKey] = SharedTextureCacheEntry{ texture };
    }

    if (GltfMaterialDebugLoggingEnabled()) {
        spdlog::info(
            "SARPDBG glTF texture textureIndex={} imageIndex={} semantic={} srgb={} packed={} normalConvention={} source='{}' bytes={} alphaOpaque={} fallback={} usable={}",
            textureIndex,
            imageIndex,
            static_cast<uint32_t>(semantic),
            preferSRGB,
            preservePackedChannels,
            static_cast<uint32_t>(normalConvention),
            cacheProbeDetail,
            textureBytes.size(),
            texture->Meta().alphaIsAllOpaque,
            texture->IsUsingFallbackImage(),
            texture->HasUsableImage());
    }

    return texture;
}

DirectX::XMFLOAT4 ReadFloat4(const json& values, const DirectX::XMFLOAT4& fallback) {
    if (!values.is_array() || values.size() != 4) {
        return fallback;
    }

    return DirectX::XMFLOAT4(
        values[0].get<float>(),
        values[1].get<float>(),
        values[2].get<float>(),
        values[3].get<float>());
}

DirectX::XMFLOAT3 ReadFloat3(const json& values, const DirectX::XMFLOAT3& fallback) {
    if (!values.is_array() || values.size() != 3) {
        return fallback;
    }

    return DirectX::XMFLOAT3(
        values[0].get<float>(),
        values[1].get<float>(),
        values[2].get<float>());
}

uint32_t ReadTextureUvSetIndex(const json& textureInfoNode) {
    return textureInfoNode.value<uint32_t>("texCoord", 0u);
}

std::shared_ptr<Material> LoadMaterial(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    GlTFMaterialCache& cache,
    size_t materialIndex)
{
    if (materialIndex >= cache.materialCache.size()) {
        throw std::runtime_error("glTF material index out of range");
    }
    if (cache.materialCache[materialIndex] != nullptr) {
        return cache.materialCache[materialIndex];
    }

    const std::string sharedCacheKey = MakeMaterialCacheKey(cache.sourceKey, materialIndex);
    {
        std::lock_guard<std::mutex> lock(g_gltfMaterialCacheMutex);
        auto sharedIt = g_sharedMaterialCache.find(sharedCacheKey);
        if (sharedIt != g_sharedMaterialCache.end()) {
            if (auto material = sharedIt->second.material.lock()) {
                cache.materialCache[materialIndex] = material;
                return material;
            }

            g_sharedMaterialCache.erase(sharedIt);
        }
    }

    const auto& materialNode = gltf.at("materials").at(materialIndex);

    MaterialDescription desc;
    desc.name = materialNode.value("name", "glTF_Material_" + std::to_string(materialIndex));
    desc.baseColor = { nullptr, 1.0f, { 0, 1, 2, 3 } };
    desc.metallic = { nullptr, 1.0f, { 2 } };
    desc.roughness = { nullptr, 1.0f, { 1 } };
    desc.emissive = { nullptr, 1.0f, { 0, 1, 2 } };
    desc.opacity = { nullptr, 1.0f, { 3 } };
    desc.aoMap = { nullptr, 1.0f, { 0 } };
    desc.heightMap = { nullptr, 1.0f, { 0 } };
    desc.normal = { nullptr, 1.0f, { 0, 1, 2 } };
    desc.invertNormalGreen = false;

    if (materialNode.contains("pbrMetallicRoughness")) {
        const auto& pbr = materialNode["pbrMetallicRoughness"];
        if (pbr.contains("baseColorFactor")) {
            desc.diffuseColor = ReadFloat4(pbr["baseColorFactor"], desc.diffuseColor);
        }
        if (pbr.contains("metallicFactor")) {
            desc.metallic.factor = pbr["metallicFactor"].get<float>();
        }
        if (pbr.contains("roughnessFactor")) {
            desc.roughness.factor = pbr["roughnessFactor"].get<float>();
        }
        if (pbr.contains("baseColorTexture")) {
            const auto& textureInfo = pbr["baseColorTexture"];
            const size_t textureIndex = textureInfo.at("index").get<size_t>();
            desc.baseColor.texture = LoadTexture(gltf, sourcePath, cache, textureIndex, true, TextureSemantic::BaseColor);
            desc.baseColor.uvSetIndex = ReadTextureUvSetIndex(textureInfo);
        }
        if (pbr.contains("metallicRoughnessTexture")) {
            const auto& textureInfo = pbr["metallicRoughnessTexture"];
            const size_t textureIndex = textureInfo.at("index").get<size_t>();
            auto texture = LoadTexture(gltf, sourcePath, cache, textureIndex, false, TextureSemantic::MetallicRoughness, true);
            desc.metallic.texture = texture;
            desc.roughness.texture = texture;
            const uint32_t uvSetIndex = ReadTextureUvSetIndex(textureInfo);
            desc.metallic.uvSetIndex = uvSetIndex;
            desc.roughness.uvSetIndex = uvSetIndex;
        }
    }

    if (materialNode.contains("normalTexture")) {
        const auto& textureInfo = materialNode["normalTexture"];
        const size_t textureIndex = textureInfo.at("index").get<size_t>();
        desc.normal.texture = LoadTexture(gltf, sourcePath, cache, textureIndex, false, TextureSemantic::Normal, false, NormalMapConvention::OpenGL);
        desc.normal.uvSetIndex = ReadTextureUvSetIndex(textureInfo);
    }

    if (materialNode.contains("occlusionTexture")) {
        const auto& occlusion = materialNode["occlusionTexture"];
        const size_t textureIndex = occlusion.at("index").get<size_t>();
        desc.aoMap.texture = LoadTexture(gltf, sourcePath, cache, textureIndex, false, TextureSemantic::AO);
        desc.aoMap.uvSetIndex = ReadTextureUvSetIndex(occlusion);
        if (occlusion.contains("strength")) {
            desc.aoMap.factor = occlusion["strength"].get<float>();
        }
    }

    if (materialNode.contains("emissiveFactor")) {
        const auto emissive = ReadFloat3(materialNode["emissiveFactor"], { 0.0f, 0.0f, 0.0f });
        desc.emissiveColor = { emissive.x, emissive.y, emissive.z, 1.0f };
    }
    if (materialNode.contains("emissiveTexture")) {
        const auto& textureInfo = materialNode["emissiveTexture"];
        const size_t textureIndex = textureInfo.at("index").get<size_t>();
        desc.emissive.texture = LoadTexture(gltf, sourcePath, cache, textureIndex, true, TextureSemantic::Emissive);
        desc.emissive.uvSetIndex = ReadTextureUvSetIndex(textureInfo);
    }

    const std::string alphaMode = materialNode.value("alphaMode", std::string("OPAQUE"));
    if (alphaMode == "MASK") {
        desc.blendState = BlendState::BLEND_STATE_MASK;
        desc.alphaCutoff = materialNode.value("alphaCutoff", 0.5f);
    }
    else if (alphaMode == "BLEND") {
        desc.blendState = BlendState::BLEND_STATE_BLEND;
        desc.opacity.factor = desc.diffuseColor.w;
        if (desc.baseColor.texture != nullptr) {
            desc.opacity.texture = desc.baseColor.texture;
            desc.opacity.uvSetIndex = desc.baseColor.uvSetIndex;
        }
    }
    else {
        desc.diffuseColor.w = 1.0f;
        desc.opacity.factor = 1.0f;
    }

    desc.forceDoubleSided = materialNode.value("doubleSided", false);

    auto material = Material::CreateShared(desc);
    cache.materialCache[materialIndex] = material;

    if (GltfMaterialDebugLoggingEnabled()) {
        const auto data = material->GetData();
        spdlog::info(
            "SARPDBG glTF material index={} name='{}' baseTex={} mrTex=({}, {}) normalTex={} aoTex={} emissiveTex={} opacityTex={} factors base=({}, {}, {}, {}) metal={} rough={} alphaMode='{}' doubleSided={} flags=0x{:x} techniqueFlags=0x{:x}",
            materialIndex,
            desc.name,
            desc.baseColor.texture != nullptr,
            desc.metallic.texture != nullptr,
            desc.roughness.texture != nullptr,
            desc.normal.texture != nullptr,
            desc.aoMap.texture != nullptr,
            desc.emissive.texture != nullptr,
            desc.opacity.texture != nullptr,
            desc.diffuseColor.x,
            desc.diffuseColor.y,
            desc.diffuseColor.z,
            desc.diffuseColor.w,
            desc.metallic.factor.Get(),
            desc.roughness.factor.Get(),
            alphaMode,
            desc.forceDoubleSided,
            data.materialFlags,
            static_cast<uint64_t>(material->Technique().compileFlags));
    }

    {
        std::lock_guard<std::mutex> lock(g_gltfMaterialCacheMutex);
        g_sharedMaterialCache[sharedCacheKey] = SharedMaterialCacheEntry{ cache.materialCache[materialIndex] };
    }

    return cache.materialCache[materialIndex];
}

std::shared_ptr<Material> ResolvePrimitiveMaterial(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    GlTFMaterialCache& cache,
    const json& primitiveNode,
    const std::shared_ptr<Material>& defaultMaterial)
{
    if (!primitiveNode.contains("material") || !gltf.contains("materials") || !gltf["materials"].is_array()) {
        return defaultMaterial;
    }

    return LoadMaterial(gltf, sourcePath, cache, primitiveNode["material"].get<size_t>());
}

void ApplyNodeTransform(const json& gltfNode, flecs::entity entity) {
    XMVECTOR translation = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
    XMVECTOR rotation = XMQuaternionIdentity();
    XMVECTOR scale = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);

    if (gltfNode.contains("matrix")) {
        const auto matrixValues = gltfNode["matrix"].get<std::vector<float>>();
        if (matrixValues.size() != 16) {
            throw std::runtime_error("Node matrix must contain 16 floats");
        }

        XMMATRIX matrix = XMMatrixSet(
            matrixValues[0], matrixValues[1], matrixValues[2], matrixValues[3],
            matrixValues[4], matrixValues[5], matrixValues[6], matrixValues[7],
            matrixValues[8], matrixValues[9], matrixValues[10], matrixValues[11],
            matrixValues[12], matrixValues[13], matrixValues[14], matrixValues[15]);
        XMMatrixDecompose(&scale, &rotation, &translation, matrix);
    }
    else {
        if (gltfNode.contains("translation")) {
            const auto t = gltfNode["translation"].get<std::vector<float>>();
            if (t.size() == 3) {
                translation = XMVectorSet(t[0], t[1], t[2], 0.0f);
            }
        }
        if (gltfNode.contains("rotation")) {
            const auto r = gltfNode["rotation"].get<std::vector<float>>();
            if (r.size() == 4) {
                rotation = XMVectorSet(r[0], r[1], r[2], r[3]);
            }
        }
        if (gltfNode.contains("scale")) {
            const auto s = gltfNode["scale"].get<std::vector<float>>();
            if (s.size() == 3) {
                scale = XMVectorSet(s[0], s[1], s[2], 0.0f);
            }
        }
    }

    entity.set<Components::Position>({ translation });
    entity.set<Components::Rotation>({ rotation });
    entity.set<Components::Scale>({ scale });
}

void SetEntityMeshes(flecs::entity entity, const std::vector<std::shared_ptr<Mesh>>& meshes) {
    const auto* oldMeshInstances = entity.try_get<Components::MeshInstances>();

    Components::MeshInstances meshInstances;
    if (oldMeshInstances != nullptr) {
        meshInstances.generation = oldMeshInstances->generation + 1;
    }

    bool isSkinned = false;
    for (const auto& mesh : meshes) {
        if (mesh == nullptr) {
            continue;
        }

        auto meshInstance = MeshInstance::CreateUnique(mesh);
        if (meshInstance->HasSkin()) {
            isSkinned = true;
        }
        meshInstances.meshInstances.push_back(std::move(meshInstance));
    }

    if (!meshInstances.meshInstances.empty()) {
        entity.set<Components::MeshInstances>(meshInstances);
    }
    else if (oldMeshInstances != nullptr) {
        entity.remove<Components::MeshInstances>();
    }

    if (isSkinned) {
        entity.add<Components::Skinned>();
    }
    else if (entity.has<Components::Skinned>()) {
        entity.remove<Components::Skinned>();
    }
}

NodeHierarchyBuildResult BuildNodeHierarchy(
    std::shared_ptr<Scene> scene,
    const json& gltf,
    const std::vector<std::vector<PrimitiveData>>& meshes)
{
    NodeHierarchyBuildResult result;
    if (!gltf.contains("nodes")) {
        return result;
    }

    const auto& nodeArray = gltf["nodes"];
    result.entities.resize(nodeArray.size());
    result.meshIndices.assign(nodeArray.size(), -1);
    std::vector<bool> hasParent(nodeArray.size(), false);

    for (size_t nodeIndex = 0; nodeIndex < nodeArray.size(); ++nodeIndex) {
        const auto& gltfNode = nodeArray[nodeIndex];
        std::string nodeName = gltfNode.value("name", "glTF_Node_" + std::to_string(nodeIndex));

        if (gltfNode.contains("mesh")) {
            const size_t meshIndex = gltfNode["mesh"].get<size_t>();
            if (meshIndex >= meshes.size()) {
                throw std::runtime_error("Node mesh index out of range");
            }

            std::vector<std::shared_ptr<Mesh>> nodeMeshes;
            nodeMeshes.reserve(meshes[meshIndex].size());
            for (const auto& primitive : meshes[meshIndex]) {
                if (primitive.mesh != nullptr) {
                    nodeMeshes.push_back(primitive.mesh);
                }
            }

            result.entities[nodeIndex] = scene->CreateRenderableEntityECS(nodeMeshes, s2ws(nodeName));
            result.meshIndices[nodeIndex] = static_cast<int32_t>(meshIndex);
        }
        else {
            result.entities[nodeIndex] = scene->CreateNodeECS(s2ws(nodeName));
        }

        ApplyNodeTransform(gltfNode, result.entities[nodeIndex]);
    }

    for (size_t nodeIndex = 0; nodeIndex < nodeArray.size(); ++nodeIndex) {
        const auto& gltfNode = nodeArray[nodeIndex];
        if (!gltfNode.contains("children")) {
            continue;
        }

        for (const auto& childIndexValue : gltfNode["children"]) {
            const size_t childIndex = childIndexValue.get<size_t>();
            if (childIndex >= result.entities.size()) {
                throw std::runtime_error("Node child index out of range");
            }

            result.entities[childIndex].child_of(result.entities[nodeIndex]);
            hasParent[childIndex] = true;
        }
    }

    std::vector<size_t> rootNodes;
    if (gltf.contains("scenes") && gltf["scenes"].is_array() && !gltf["scenes"].empty()) {
        size_t sceneIndex = gltf.value<size_t>("scene", static_cast<size_t>(0));
        if (sceneIndex >= gltf["scenes"].size()) {
            throw std::runtime_error("Default scene index out of range");
        }

        const auto& selectedScene = gltf["scenes"][sceneIndex];
        if (selectedScene.contains("nodes") && selectedScene["nodes"].is_array()) {
            rootNodes.reserve(selectedScene["nodes"].size());
            for (const auto& nodeValue : selectedScene["nodes"]) {
                const size_t rootIndex = nodeValue.get<size_t>();
                if (rootIndex >= result.entities.size()) {
                    throw std::runtime_error("Scene root node index out of range");
                }
                rootNodes.push_back(rootIndex);
            }
        }
    }

    if (rootNodes.empty()) {
        for (size_t nodeIndex = 0; nodeIndex < result.entities.size(); ++nodeIndex) {
            if (!hasParent[nodeIndex]) {
                rootNodes.push_back(nodeIndex);
            }
        }
    }

    flecs::entity sceneRoot = scene->GetRoot();
    for (const size_t rootIndex : rootNodes) {
        result.entities[rootIndex].child_of(sceneRoot);
    }

    return result;
}

std::vector<std::shared_ptr<Animation>> ParseAnimations(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    const std::vector<std::string>& nodeAnimationKeys)
{
    std::vector<std::shared_ptr<Animation>> animations;
    if (!gltf.contains("animations") || !gltf["animations"].is_array()) {
        return animations;
    }

    const auto& animationArray = gltf["animations"];
    animations.reserve(animationArray.size());
    for (size_t animationIndex = 0; animationIndex < animationArray.size(); ++animationIndex) {
        const auto& animationNode = animationArray[animationIndex];
        const std::string animationName = animationNode.value("name", "glTF_Animation_" + std::to_string(animationIndex));
        auto animation = std::make_shared<Animation>(animationName);

        if (!animationNode.contains("samplers") || !animationNode["samplers"].is_array() ||
            !animationNode.contains("channels") || !animationNode["channels"].is_array()) {
            animations.push_back(animation);
            continue;
        }

        const auto& samplers = animationNode["samplers"];
        for (size_t channelIndex = 0; channelIndex < animationNode["channels"].size(); ++channelIndex) {
            const auto& channel = animationNode["channels"][channelIndex];
            if (!channel.contains("sampler") || !channel.contains("target")) {
                continue;
            }

            const size_t samplerIndex = channel["sampler"].get<size_t>();
            if (samplerIndex >= samplers.size()) {
                throw std::runtime_error("glTF animation sampler index out of range");
            }

            const auto& target = channel["target"];
            if (!target.contains("node") || !target.contains("path")) {
                continue;
            }

            const size_t nodeIndex = target["node"].get<size_t>();
            if (nodeIndex >= nodeAnimationKeys.size()) {
                throw std::runtime_error("glTF animation node target index out of range");
            }

            const auto& sampler = samplers[samplerIndex];
            const std::string interpolation = sampler.value("interpolation", std::string("LINEAR"));
            if (interpolation == "CUBICSPLINE") {
                spdlog::warn("glTF animation '{}' channel {} uses unsupported CUBICSPLINE interpolation; skipping channel", animationName, channelIndex);
                continue;
            }

            AnimationInterpolationMode interpolationMode = AnimationInterpolationMode::Linear;
            try {
                interpolationMode = ParseInterpolationMode(interpolation);
            }
            catch (const std::exception&) {
                spdlog::warn("glTF animation '{}' channel {} uses unsupported interpolation '{}'; skipping channel", animationName, channelIndex, interpolation);
                continue;
            }

            const size_t inputAccessorIndex = sampler.at("input").get<size_t>();
            const size_t outputAccessorIndex = sampler.at("output").get<size_t>();
            auto times = ReadAccessorScalarsAsFloat(gltf, bufferSources, inputAccessorIndex);
            if (times.empty()) {
                continue;
            }

            const std::string nodeKey = nodeAnimationKeys[nodeIndex];
            auto& clip = animation->nodesMap[nodeKey];
            if (!clip) {
                clip = std::make_shared<AnimationClip>();
            }

            const std::string path = target["path"].get<std::string>();
            if (path == "translation") {
                auto values = ReadAccessorVec3AsFloat(gltf, bufferSources, outputAccessorIndex);
                if (values.size() != times.size()) {
                    spdlog::warn("glTF animation '{}' translation channel {} has mismatched input/output counts; skipping channel", animationName, channelIndex);
                    continue;
                }
                for (size_t keyIndex = 0; keyIndex < times.size(); ++keyIndex) {
                    clip->addPositionKeyframe(times[keyIndex], values[keyIndex], interpolationMode);
                }
            }
            else if (path == "rotation") {
                auto values = ReadAccessorVec4AsFloat(gltf, bufferSources, outputAccessorIndex);
                if (values.size() != times.size()) {
                    spdlog::warn("glTF animation '{}' rotation channel {} has mismatched input/output counts; skipping channel", animationName, channelIndex);
                    continue;
                }
                for (size_t keyIndex = 0; keyIndex < times.size(); ++keyIndex) {
                    clip->addRotationKeyframe(times[keyIndex], XMVectorSet(values[keyIndex].x, values[keyIndex].y, values[keyIndex].z, values[keyIndex].w), interpolationMode);
                }
            }
            else if (path == "scale") {
                auto values = ReadAccessorVec3AsFloat(gltf, bufferSources, outputAccessorIndex);
                if (values.size() != times.size()) {
                    spdlog::warn("glTF animation '{}' scale channel {} has mismatched input/output counts; skipping channel", animationName, channelIndex);
                    continue;
                }
                for (size_t keyIndex = 0; keyIndex < times.size(); ++keyIndex) {
                    clip->addScaleKeyframe(times[keyIndex], values[keyIndex], interpolationMode);
                }
            }
            else if (path == "weights") {
                spdlog::warn("glTF animation '{}' channel {} targets morph weights, which are not supported yet; skipping channel", animationName, channelIndex);
            }
        }

        animations.push_back(animation);
    }

    return animations;
}

std::vector<std::shared_ptr<Skeleton>> BuildSkins(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    const std::vector<flecs::entity>& nodeEntities,
    const std::vector<std::string>& nodeAnimationKeys,
    const std::vector<std::shared_ptr<Animation>>& animations)
{
    std::vector<std::shared_ptr<Skeleton>> skeletons;
    if (!gltf.contains("skins") || !gltf["skins"].is_array()) {
        return skeletons;
    }

    const auto& skins = gltf["skins"];
    skeletons.resize(skins.size());
    for (size_t skinIndex = 0; skinIndex < skins.size(); ++skinIndex) {
        const auto& skinNode = skins[skinIndex];
        if (!skinNode.contains("joints") || !skinNode["joints"].is_array() || skinNode["joints"].empty()) {
            spdlog::warn("glTF skin {} has no joints; skipping skin", skinIndex);
            continue;
        }

        std::vector<flecs::entity> jointNodes;
        jointNodes.reserve(skinNode["joints"].size());
        std::vector<XMMATRIX> inverseBindMatrices(skinNode["joints"].size(), XMMatrixIdentity());
        std::vector<std::string> jointAnimationKeys;
        jointAnimationKeys.reserve(skinNode["joints"].size());

        if (skinNode.contains("inverseBindMatrices")) {
            auto loadedMatrices = ReadAccessorMat4AsMatrices(gltf, bufferSources, skinNode["inverseBindMatrices"].get<size_t>());
            if (loadedMatrices.size() != inverseBindMatrices.size()) {
                spdlog::warn("glTF skin {} inverse bind matrix count ({}) does not match joint count ({}); missing entries default to identity",
                    skinIndex,
                    loadedMatrices.size(),
                    inverseBindMatrices.size());
            }

            const size_t matrixCount = std::min(loadedMatrices.size(), inverseBindMatrices.size());
            for (size_t matrixIndex = 0; matrixIndex < matrixCount; ++matrixIndex) {
                inverseBindMatrices[matrixIndex] = loadedMatrices[matrixIndex];
            }
        }

        for (const auto& jointValue : skinNode["joints"]) {
            const size_t jointNodeIndex = jointValue.get<size_t>();
            if (jointNodeIndex >= nodeEntities.size()) {
                throw std::runtime_error("glTF skin joint node index out of range");
            }

            flecs::entity jointEntity = nodeEntities[jointNodeIndex];
            if (!jointEntity.is_alive()) {
                throw std::runtime_error("glTF skin references an invalid joint entity");
            }

            if (!jointEntity.has<AnimationController>()) {
                jointEntity.add<AnimationController>();
            }
            jointEntity.set<Components::AnimationName>({ nodeAnimationKeys[jointNodeIndex] });
            jointNodes.push_back(jointEntity);
            jointAnimationKeys.push_back(nodeAnimationKeys[jointNodeIndex]);
        }

        auto skeleton = std::make_shared<Skeleton>(jointNodes, inverseBindMatrices);
        for (const auto& animation : animations) {
            bool usesSkeleton = false;
            for (const auto& jointKey : jointAnimationKeys) {
                if (animation->nodesMap.contains(jointKey)) {
                    usesSkeleton = true;
                    break;
                }
            }

            if (usesSkeleton) {
                skeleton->AddAnimation(animation);
            }
        }

        skeletons[skinIndex] = skeleton;
    }

    return skeletons;
}

} // namespace

namespace GlTFLoader {

std::shared_ptr<Scene> LoadModel(std::string filePath) {
    try {
        auto extraction = GlTFGeometryExtractor::ExtractAll(filePath);
        const std::filesystem::path sourcePath(filePath);

        auto scene = std::make_shared<Scene>();
        auto defaultMaterial = Material::GetDefaultMaterial();
        GlTFMaterialCache materialCache;
        materialCache.sourceKey = NormalizeSourceKey(sourcePath);
        materialCache.bufferSources = BuildBufferSources(extraction.gltf, sourcePath);
        if (extraction.gltf.contains("materials") && extraction.gltf["materials"].is_array()) {
            materialCache.materialCache.resize(extraction.gltf["materials"].size());
        }

        // Build mesh/primitive structure from glTF JSON
        const size_t meshCount = extraction.gltf.contains("meshes") ? extraction.gltf["meshes"].size() : 0;
        std::vector<std::vector<PrimitiveData>> allMeshes(meshCount);
        std::vector<std::vector<std::shared_ptr<Mesh>>> sharedMeshLists(meshCount);
        std::vector<std::vector<std::optional<PreparedPrimitiveData>>> preparedPrimitives(meshCount);
        for (size_t mi = 0; mi < meshCount; ++mi) {
            const auto& meshNode = extraction.gltf["meshes"][mi];
            if (meshNode.contains("primitives")) {
                allMeshes[mi].resize(meshNode["primitives"].size());
                preparedPrimitives[mi].resize(meshNode["primitives"].size());
            }
        }

        // Build GPU meshes from extracted geometry
        for (auto& ep : extraction.primitives) {
            const auto& primitiveNode = extraction.gltf["meshes"][ep.meshIndex]["primitives"][ep.primitiveIndex];
            auto material = ResolvePrimitiveMaterial(
                extraction.gltf,
                sourcePath,
                materialCache,
                primitiveNode,
                defaultMaterial);
            if (GltfMaterialDebugLoggingEnabled()) {
                const bool hasMaterialIndex = primitiveNode.contains("material");
                spdlog::info(
                    "SARPDBG glTF primitive mesh={} primitive={} gltfMaterial={} resolvedDefault={} materialPtr={}",
                    ep.meshIndex,
                    ep.primitiveIndex,
                    hasMaterialIndex ? primitiveNode["material"].get<int>() : -1,
                    material == defaultMaterial,
                    static_cast<const void*>(material.get()));
            }
            PreparedPrimitiveData preparedPrimitive{
                std::move(ep.result),
                material
            };

            MeshPreprocessResult buildResult = preparedPrimitive.result;
            auto mesh = buildResult.ingest.Build(
                material,
                std::move(buildResult.prebuiltData),
                MeshCpuDataPolicy::ReleaseAfterUpload);
            allMeshes[ep.meshIndex][ep.primitiveIndex].mesh = mesh;
            preparedPrimitives[ep.meshIndex][ep.primitiveIndex] = std::move(preparedPrimitive);
        }

        for (size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
            sharedMeshLists[meshIndex].reserve(allMeshes[meshIndex].size());
            for (const auto& primitive : allMeshes[meshIndex]) {
                if (primitive.mesh != nullptr) {
                    sharedMeshLists[meshIndex].push_back(primitive.mesh);
                }
            }
        }

        const auto nodeAnimationKeys = BuildNodeAnimationKeys(extraction.gltf);
        const auto hierarchy = BuildNodeHierarchy(scene, extraction.gltf, allMeshes);
        const auto animations = ParseAnimations(extraction.gltf, materialCache.bufferSources, nodeAnimationKeys);
        const auto skeletons = BuildSkins(extraction.gltf, materialCache.bufferSources, hierarchy.entities, nodeAnimationKeys, animations);

        std::unordered_map<MeshBindingKey, std::vector<std::shared_ptr<Mesh>>, MeshBindingKeyHasher> skinnedMeshCache;
        auto getMeshesForBinding = [&](size_t meshIndex, int skinIndex) -> const std::vector<std::shared_ptr<Mesh>>& {
            if (meshIndex >= sharedMeshLists.size()) {
                throw std::runtime_error("glTF mesh binding index out of range");
            }

            if (skinIndex < 0) {
                return sharedMeshLists[meshIndex];
            }

            MeshBindingKey bindingKey{ meshIndex, skinIndex };
            auto existing = skinnedMeshCache.find(bindingKey);
            if (existing != skinnedMeshCache.end()) {
                return existing->second;
            }

            if (static_cast<size_t>(skinIndex) >= skeletons.size() || skeletons[skinIndex] == nullptr) {
                spdlog::warn("glTF node references missing skin {}; using static mesh binding instead", skinIndex);
                return skinnedMeshCache.emplace(bindingKey, sharedMeshLists[meshIndex]).first->second;
            }

            std::vector<std::shared_ptr<Mesh>> boundMeshes;
            boundMeshes.reserve(preparedPrimitives[meshIndex].size());
            for (size_t primitiveIndex = 0; primitiveIndex < preparedPrimitives[meshIndex].size(); ++primitiveIndex) {
                const auto& preparedPrimitive = preparedPrimitives[meshIndex][primitiveIndex];
                if (!preparedPrimitive.has_value()) {
                    continue;
                }

                MeshPreprocessResult buildResult = preparedPrimitive->result;
                auto mesh = buildResult.ingest.Build(
                    preparedPrimitive->material,
                    std::move(buildResult.prebuiltData),
                    MeshCpuDataPolicy::ReleaseAfterUpload);

                if ((mesh->GetPerMeshCBData().vertexFlags & VertexFlags::VERTEX_SKINNED) != 0u) {
                    mesh->SetBaseSkin(skeletons[skinIndex]);
                }
                else {
                    spdlog::warn(
                        "glTF node binds skin {} to mesh {} primitive {} without JOINTS_0/WEIGHTS_0 data; leaving that primitive static",
                        skinIndex,
                        meshIndex,
                        primitiveIndex);
                }

                boundMeshes.push_back(mesh);
            }

            return skinnedMeshCache.emplace(bindingKey, std::move(boundMeshes)).first->second;
        };

        bool importedSkins = false;
        if (extraction.gltf.contains("nodes") && extraction.gltf["nodes"].is_array()) {
            const auto& nodes = extraction.gltf["nodes"];
            for (size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
                const auto& node = nodes[nodeIndex];
                if (!node.contains("skin") || !node.contains("mesh")) {
                    continue;
                }

                if (nodeIndex >= hierarchy.entities.size() || !hierarchy.entities[nodeIndex].is_alive()) {
                    continue;
                }

                const size_t skinIndex = node["skin"].get<size_t>();
                if (skinIndex >= skeletons.size()) {
                    throw std::runtime_error("glTF node skin index out of range");
                }

                const int32_t meshIndex = hierarchy.meshIndices[nodeIndex];
                if (meshIndex < 0) {
                    continue;
                }

                SetEntityMeshes(hierarchy.entities[nodeIndex], getMeshesForBinding(static_cast<size_t>(meshIndex), static_cast<int>(skinIndex)));
                importedSkins = true;
            }
        }

        if (importedSkins) {
            scene->ProcessEntitySkins(true);
        }

        return scene;
    }
    catch (const std::exception& e) {
        spdlog::error("GlTFLoader failed for {}: {}", filePath, e.what());
        return nullptr;
    }
}

} // namespace GlTFLoader
