#include "Assets/Import/GlTF/GlTFLoader.h"

#include <algorithm>
#include <variant>
#include <string_view>
#include <functional>
#include <chrono>
#include <atomic>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Assets/Textures/Processing/TextureProcessingManager.h"
#include <BasicRenderer/Assets/TexturePipeline.h>
#include "BasicRenderer/Scene/Animation/Animation.h"
#include "BasicRenderer/Scene/Animation/Skeleton.h"
#include <BasicRenderer/Assets/Import/GlTFGeometryExtractor.h>
#include <BasicRenderer/Assets/Import/ImportProfile.h>
#include <BasicRenderer/Assets/Import/SceneCache.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Assets/Material.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include <BasicRenderer/Scene/VertexFlags.h>
#include "Resources/Sampler.h"
#include "BasicRenderer/Scene/Components.h"
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
    std::shared_ptr<const br::import::ImportProfile> profile;
    // Each image's source, built once: a provided (KTX2) source's provider reads the header once for every texture of
    // the image, and the loader can read its shape before the texture loads.
    std::unordered_map<size_t, br::textures::TextureSource> imageSources;
};

struct SharedMaterialCacheEntry {
    std::weak_ptr<Material> material;
};

std::mutex g_gltfMaterialCacheMutex;
std::unordered_map<std::string, SharedMaterialCacheEntry> g_sharedMaterialCache;

constexpr uint32_t kGlbMagic = 0x46546C67;
constexpr uint32_t kGlbJsonChunkType = 0x4E4F534A;
constexpr uint32_t kGlbBinChunkType = 0x004E4942;
constexpr uint32_t kMaterialTextureMaxAnisotropy = 16;

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
    // KHR_texture_basisu's image is the KTX2 one; "source", if present, is a fallback for loaders without KTX2.
    if (textureNode.contains("extensions") && textureNode["extensions"].contains("KHR_texture_basisu")) {
        return textureNode["extensions"]["KHR_texture_basisu"].at("source").get<size_t>();
    }
    if (textureNode.contains("source")) {
        return textureNode["source"].get<size_t>();
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

std::shared_ptr<org::Sampler> CreateTextureSampler(const br::import::SceneCacheTexture& texture) {
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

    // The glTF defaults (no sampler) are the values above: wrap, linear, linear mips.
    samplerDesc.addressU = ConvertWrapMode(texture.wrapS);
    samplerDesc.addressV = ConvertWrapMode(texture.wrapT);

    const int minFilter = texture.minFilter;
    const int magFilter = texture.magFilter;

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

    return org::Sampler::CreateSampler(samplerDesc);
}

// A texture's glTF sampler values, left at the glTF defaults when it names none.
void ReadGltfSamplerValues(const json& gltf, const json& textureNode, br::import::SceneCacheTexture& texture) {
    if (!textureNode.contains("sampler")) return;
    const auto& samplers = gltf.contains("samplers") ? gltf["samplers"] : json::array();
    const size_t samplerIndex = textureNode["sampler"].get<size_t>();
    if (samplerIndex >= samplers.size()) {
        throw std::runtime_error("glTF sampler index out of range");
    }
    const auto& samplerNode = samplers[samplerIndex];
    texture.wrapS = samplerNode.value<int>("wrapS", texture.wrapS);
    texture.wrapT = samplerNode.value<int>("wrapT", texture.wrapT);
    texture.minFilter = samplerNode.value<int>("minFilter", texture.minFilter);
    texture.magFilter = samplerNode.value<int>("magFilter", texture.magFilter);
}

// A cheap version for a file the loader reads by reference: changes when the file is replaced, without reading it.
std::string GltfFileVersion(const std::filesystem::path& path)
{
    std::error_code sizeError;
    std::error_code timeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    const auto writeTime = std::filesystem::last_write_time(path, timeError);
    if (sizeError || timeError) return "unversioned";
    return "file:" + std::to_string(size) + ":" + std::to_string(writeTime.time_since_epoch().count());
}

// Where a glTF image's encoded bytes are: its own file, a range of the .glb (a bufferView),
// or memory (a data URI or an inline buffer). An image inside the glTF is versioned by it.
br::textures::TextureSource GltfImageSource(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    const GlTFMaterialCache& cache,
    size_t imageIndex)
{
    const auto& image = gltf.at("images").at(imageIndex);
    if (image.contains("uri")) {
        const std::string uri = image["uri"].get<std::string>();
        if (uri.rfind("data:", 0) != 0) {
            const auto imagePath = sourcePath.parent_path() / std::filesystem::path(uri);
            // KTX2 is recognised by its bytes: exporters label it inconsistently (Zorah says image/png).
            if (br::textures::FileLooksLikeKtx2(imagePath)) {
                auto source = br::textures::MakeFileTextureSource(imagePath);
                source.formatHint = "image/ktx2";
                source.version = GltfFileVersion(imagePath);
                source.data = br::textures::ProvidedTextureSource{ br::textures::MakeKtx2TextureMipProvider(
                    br::textures::MakeFileTextureByteSource(imagePath), source.identity) };
                return source;
            }
            return br::textures::MakeFileTextureSource(imagePath);
        }
    }
    br::textures::TextureSource source;
    source.identity = BuildGlTFImageIdentity(gltf, sourcePath, cache.sourceKey, imageIndex);
    source.version = GltfFileVersion(sourcePath);
    source.formatHint = image.value("mimeType", std::string());
    if (image.contains("bufferView")) {
        const auto& bufferView = gltf.at("bufferViews").at(image["bufferView"].get<size_t>());
        const auto& buffer = cache.bufferSources.at(bufferView.at("buffer").get<size_t>());
        const uint64_t offset = bufferView.value<uint64_t>("byteOffset", 0);
        const uint64_t length = bufferView.at("byteLength").get<uint64_t>();
        if (buffer.backing == BufferBacking::FileSpan) {
            if (offset + length > buffer.fileLength) throw std::runtime_error("glTF image bufferView exceeds its buffer");
            if (br::textures::FileLooksLikeKtx2(buffer.filePath, buffer.fileOffset + offset)) {
                source.formatHint = "image/ktx2";
                source.data = br::textures::ProvidedTextureSource{ br::textures::MakeKtx2TextureMipProvider(
                    br::textures::MakeFileTextureByteSource(buffer.filePath, buffer.fileOffset + offset, length), source.identity) };
                return source;
            }
            source.data = br::textures::FileTextureSource{ buffer.filePath, buffer.fileOffset + offset, length };
            return source;
        }
    }
    auto bytes = std::make_shared<const std::vector<uint8_t>>(ReadImageBytes(gltf, sourcePath, cache.bufferSources, imageIndex));
    if (br::textures::LooksLikeKtx2(*bytes)) {
        source.formatHint = "image/ktx2";
        source.data = br::textures::ProvidedTextureSource{ br::textures::MakeKtx2TextureMipProvider(
            br::textures::MakeMemoryTextureByteSource(bytes), source.identity) };
        return source;
    }
    source.data = br::textures::MemoryTextureSource{ std::move(bytes) };
    return source;
}

br::textures::TextureSource CachedGltfImageSource(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    GlTFMaterialCache& cache,
    size_t imageIndex)
{
    auto it = cache.imageSources.find(imageIndex);
    if (it == cache.imageSources.end()) {
        it = cache.imageSources.emplace(imageIndex, GltfImageSource(gltf, sourcePath, cache, imageIndex)).first;
    }
    return it->second;
}

// The shape of a texture whose image is provided (KTX2): read from its header now, before the texture loads. nullopt
// for images only decoding reveals.
std::optional<br::textures::TextureShape> DescribeProvidedGltfTexture(
    const json& gltf,
    const std::filesystem::path& sourcePath,
    GlTFMaterialCache& cache,
    size_t textureIndex)
{
    const auto& textures = gltf.at("textures");
    if (textureIndex >= textures.size()) return std::nullopt;
    const auto source = CachedGltfImageSource(gltf, sourcePath, cache, ResolveTextureImageIndex(textures[textureIndex]));
    const auto* provided = std::get_if<br::textures::ProvidedTextureSource>(&source.data);
    return provided && provided->provider ? provided->provider->DescribeTexture() : std::nullopt;
}

// An image's name for profile rules: its "name", else its URI's stem.
std::string GltfTextureImageName(const json& gltf, size_t textureIndex)
{
    const auto& textures = gltf.at("textures");
    if (textureIndex >= textures.size()) return {};
    const auto& image = gltf.at("images").at(ResolveTextureImageIndex(textures[textureIndex]));
    if (image.contains("name")) return image["name"].get<std::string>();
    if (image.contains("uri")) {
        std::string stem = std::filesystem::path(image["uri"].get<std::string>()).stem().string();
        return stem;
    }
    return {};
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
    // KHR_texture_transform may override the UV set.
    if (textureInfoNode.contains("extensions") && textureInfoNode["extensions"].contains("KHR_texture_transform")) {
        const auto& transform = textureInfoNode["extensions"]["KHR_texture_transform"];
        if (transform.contains("texCoord")) return transform["texCoord"].get<uint32_t>();
    }
    return textureInfoNode.value<uint32_t>("texCoord", 0u);
}

// The texture infos of a material, base colour first: the slot whose KHR_texture_transform the material uses.
std::vector<const json*> MaterialTextureInfos(const json& materialNode) {
    std::vector<const json*> infos;
    if (materialNode.contains("pbrMetallicRoughness")) {
        const auto& pbr = materialNode["pbrMetallicRoughness"];
        for (const char* slot : { "baseColorTexture", "metallicRoughnessTexture" }) {
            if (pbr.contains(slot)) infos.push_back(&pbr[slot]);
        }
    }
    for (const char* slot : { "normalTexture", "occlusionTexture", "emissiveTexture" }) {
        if (materialNode.contains(slot)) infos.push_back(&materialNode[slot]);
    }
    return infos;
}

// One transform per material: the renderer applies it to every texture slot. glTF allows one per slot; a material
// whose slots differ keeps the first slot's and is reported.
MaterialUvTransform ReadMaterialUvTransform(const json& materialNode, const std::string& materialName) {
    std::vector<json> transforms;
    for (const json* info : MaterialTextureInfos(materialNode)) {
        const bool has = info->contains("extensions") && (*info)["extensions"].contains("KHR_texture_transform");
        json transform = has ? (*info)["extensions"]["KHR_texture_transform"] : json::object();
        transform.erase("texCoord");
        transforms.push_back(std::move(transform));
    }
    MaterialUvTransform result;
    if (transforms.empty()) return result;
    for (size_t i = 1; i < transforms.size(); ++i) {
        if (transforms[i] != transforms.front()) {
            spdlog::warn("glTF material '{}': texture slots use different KHR_texture_transforms; all use the first slot's", materialName);
            break;
        }
    }
    const json& transform = transforms.front();
    if (transform.contains("offset")) result.offset = { transform["offset"][0].get<float>(), transform["offset"][1].get<float>() };
    if (transform.contains("scale")) result.scale = { transform["scale"][0].get<float>(), transform["scale"][1].get<float>() };
    result.rotation = transform.value("rotation", 0.0f);
    return result;
}

// The importer's materials in two steps. The describer reads the glTF into a SceneCacheDescription (what a scene cache
// stores); the realizer makes textures and materials from a description. A live import runs both; a cached scene only
// the realizer, so the two make the same requests and materials.
struct GltfSceneDescriber {
    const json& gltf;
    std::filesystem::path sourcePath;
    GlTFMaterialCache& cache;
    br::import::SceneCacheDescription& scene;
    std::unordered_map<size_t, uint32_t> images;
    std::unordered_map<std::string, uint32_t> textures;
    std::unordered_map<size_t, int32_t> materials;

    uint32_t Image(size_t imageIndex) {
        if (const auto it = images.find(imageIndex); it != images.end()) return it->second;
        const auto& image = gltf.at("images").at(imageIndex);
        // The live source decides what the bytes are, so a description says what a live import read.
        const auto source = CachedGltfImageSource(gltf, sourcePath, cache, imageIndex);
        br::import::SceneCacheImage record;
        record.sourceImage = static_cast<uint32_t>(imageIndex);
        record.ktx2 = std::holds_alternative<br::textures::ProvidedTextureSource>(source.data);
        const std::string uri = image.value("uri", std::string());
        if (!uri.empty() && uri.rfind("data:", 0) != 0) {
            record.kind = br::import::SceneCacheImage::Kind::File;
            record.file = uri;
        }
        else if (const auto* file = std::get_if<br::textures::FileTextureSource>(&source.data); file && image.contains("bufferView")) {
            record.kind = br::import::SceneCacheImage::Kind::FileRange;
            record.file = file->path == sourcePath ? std::string() : file->path.lexically_relative(sourcePath.parent_path()).generic_string();
            record.offset = file->offset;
            record.length = file->size;
        }
        else if (record.ktx2 && image.contains("bufferView")) {
            // A KTX2 bufferView streams through its provider; the range is the bufferView's within its buffer file.
            const auto& bufferView = gltf.at("bufferViews").at(image["bufferView"].get<size_t>());
            const auto& buffer = cache.bufferSources.at(bufferView.at("buffer").get<size_t>());
            if (buffer.backing != BufferBacking::FileSpan) throw std::runtime_error("glTF KTX2 image is not in a file");
            record.kind = br::import::SceneCacheImage::Kind::FileRange;
            record.file = buffer.filePath == sourcePath ? std::string() : buffer.filePath.lexically_relative(sourcePath.parent_path()).generic_string();
            record.offset = buffer.fileOffset + bufferView.value<uint64_t>("byteOffset", 0);
            record.length = bufferView.at("byteLength").get<uint64_t>();
        }
        else {
            record.kind = br::import::SceneCacheImage::Kind::Embedded;
            if (scene.notCacheableReason.empty()) {
                scene.notCacheableReason = "image " + std::to_string(imageIndex) + " is embedded (a data URI or an inline buffer)";
            }
        }
        if (record.kind == br::import::SceneCacheImage::Kind::FileRange) {
            record.bufferView = image.at("bufferView").get<uint32_t>();
            record.mimeType = image.value("mimeType", std::string());
        }
        const auto index = static_cast<uint32_t>(scene.images.size());
        scene.images.push_back(std::move(record));
        images.emplace(imageIndex, index);
        return index;
    }

    uint32_t Texture(size_t textureIndex, bool preferSRGB, TextureSemantic semantic, bool preservePackedChannels = false,
        NormalMapConvention normalConvention = NormalMapConvention::DirectX)
    {
        const std::string key = BuildTextureResourceKey(gltf, sourcePath, cache, textureIndex, preferSRGB, semantic, preservePackedChannels, normalConvention);
        if (const auto it = textures.find(key); it != textures.end()) return it->second;
        const auto& textureNode = gltf.at("textures").at(textureIndex);
        br::import::SceneCacheTexture record;
        record.image = Image(ResolveTextureImageIndex(textureNode));
        ReadGltfSamplerValues(gltf, textureNode, record);
        record.preferSRGB = preferSRGB;
        record.semantic = semantic;
        record.preservePackedChannels = preservePackedChannels;
        record.normalConvention = normalConvention;
        const auto index = static_cast<uint32_t>(scene.textures.size());
        scene.textures.push_back(record);
        textures.emplace(key, index);
        return index;
    }

    // The material a primitive uses, or -1 for the default material.
    int32_t PrimitiveMaterial(const json& primitiveNode) {
        if (!primitiveNode.contains("material") || !gltf.contains("materials") || !gltf["materials"].is_array()) return -1;
        return Material(primitiveNode["material"].get<size_t>());
    }

    int32_t Material(size_t materialIndex) {
        if (const auto it = materials.find(materialIndex); it != materials.end()) return it->second;
        const auto& materialNode = gltf.at("materials").at(materialIndex);
        using Slot = br::import::SceneCacheTextureSlot;
        br::import::SceneCacheMaterial record;
        record.sourceMaterial = static_cast<uint32_t>(materialIndex);
        auto setSlot = [&](Slot slot, uint32_t texture) { record.textures[static_cast<size_t>(slot)] = static_cast<int32_t>(texture); };
        auto slotTexture = [&](Slot slot) { return record.textures[static_cast<size_t>(slot)]; };

        MaterialDescription& desc = record.description;
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
                setSlot(Slot::BaseColor, Texture(textureInfo.at("index").get<size_t>(), true, TextureSemantic::BaseColor));
                desc.baseColor.uvSetIndex = ReadTextureUvSetIndex(textureInfo);
            }
            if (pbr.contains("metallicRoughnessTexture")) {
                const auto& textureInfo = pbr["metallicRoughnessTexture"];
                const size_t textureIndex = textureInfo.at("index").get<size_t>();
                const uint32_t texture = Texture(textureIndex, false, TextureSemantic::MetallicRoughness, true);
                setSlot(Slot::Metallic, texture);
                setSlot(Slot::Roughness, texture);
                if (cache.profile && !cache.profile->materialChannels.empty()) {
                    // Channel counts are only known up front for provided (KTX2) images; a rule that needs one cannot match
                    // a decoded image.
                    const auto shape = DescribeProvidedGltfTexture(gltf, sourcePath, cache, textureIndex);
                    const std::uint32_t channels = shape ? shape->desc.channels : 0u;
                    if (const auto* rule = cache.profile->FindMetallicRoughnessRule(channels, GltfTextureImageName(gltf, textureIndex))) {
                        if (rule->metallic) desc.metallic.channels = { *rule->metallic };
                        if (rule->roughness) desc.roughness.channels = { *rule->roughness };
                    }
                }
                const uint32_t uvSetIndex = ReadTextureUvSetIndex(textureInfo);
                desc.metallic.uvSetIndex = uvSetIndex;
                desc.roughness.uvSetIndex = uvSetIndex;
            }
        }

        if (materialNode.contains("normalTexture")) {
            const auto& textureInfo = materialNode["normalTexture"];
            const size_t textureIndex = textureInfo.at("index").get<size_t>();
            // glTF normals are OpenGL (+Y): the shader flips green, so the texture loads as stored and a block-compressed one
            // streams straight from its file instead of being decompressed, flipped and re-encoded.
            setSlot(Slot::Normal, Texture(textureIndex, false, TextureSemantic::Normal, false, NormalMapConvention::DirectX));
            desc.normal.uvSetIndex = ReadTextureUvSetIndex(textureInfo);
            desc.invertNormalGreen = true;
            if (textureInfo.contains("scale")) {
                desc.normal.factor = textureInfo["scale"].get<float>();
            }
            if (const auto shape = DescribeProvidedGltfTexture(gltf, sourcePath, cache, textureIndex); shape && shape->desc.channels == 2u) {
                desc.normal.channels = { 0u, 1u, 4u };
            }
        }

        if (materialNode.contains("occlusionTexture")) {
            const auto& occlusion = materialNode["occlusionTexture"];
            setSlot(Slot::AO, Texture(occlusion.at("index").get<size_t>(), false, TextureSemantic::AO));
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
            setSlot(Slot::Emissive, Texture(textureInfo.at("index").get<size_t>(), true, TextureSemantic::Emissive));
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
            if (slotTexture(Slot::BaseColor) >= 0) {
                record.textures[static_cast<size_t>(Slot::Opacity)] = slotTexture(Slot::BaseColor);
                desc.opacity.uvSetIndex = desc.baseColor.uvSetIndex;
            }
        }
        else {
            desc.diffuseColor.w = 1.0f;
            desc.opacity.factor = 1.0f;
        }

        desc.forceDoubleSided = materialNode.value("doubleSided", false);
        desc.uvTransform = ReadMaterialUvTransform(materialNode, desc.name);

        const auto index = static_cast<int32_t>(scene.materials.size());
        scene.materials.push_back(std::move(record));
        materials.emplace(materialIndex, index);
        return index;
    }
};

// An image's source from its description: what GltfImageSource makes for it, without the glTF.
br::textures::TextureSource RealizeImageSource(
    const br::import::SceneCacheImage& image,
    const std::filesystem::path& sourcePath,
    const std::string& sourceKey)
{
    using Kind = br::import::SceneCacheImage::Kind;
    if (image.kind == Kind::File) {
        const auto imagePath = sourcePath.parent_path() / std::filesystem::path(image.file);
        if (!image.ktx2) return br::textures::MakeFileTextureSource(imagePath);
        auto source = br::textures::MakeFileTextureSource(imagePath);
        source.formatHint = "image/ktx2";
        source.version = GltfFileVersion(imagePath);
        source.data = br::textures::ProvidedTextureSource{ br::textures::MakeKtx2TextureMipProvider(
            br::textures::MakeFileTextureByteSource(imagePath), source.identity) };
        return source;
    }
    if (image.kind == Kind::FileRange) {
        br::textures::TextureSource source;
        source.identity = sourceKey + "#image-bv:" + std::to_string(image.bufferView) + ":" + image.mimeType;
        source.version = GltfFileVersion(sourcePath);
        source.formatHint = image.mimeType;
        const auto file = image.file.empty() ? sourcePath : sourcePath.parent_path() / std::filesystem::path(image.file);
        if (image.ktx2) {
            source.formatHint = "image/ktx2";
            source.data = br::textures::ProvidedTextureSource{ br::textures::MakeKtx2TextureMipProvider(
                br::textures::MakeFileTextureByteSource(file, image.offset, image.length), source.identity) };
            return source;
        }
        source.data = br::textures::FileTextureSource{ file, image.offset, image.length };
        return source;
    }
    throw std::runtime_error("glTF image " + std::to_string(image.sourceImage) + " is embedded in its source, which a scene cache cannot read");
}

struct GltfSceneRealizer {
    const br::import::SceneCacheDescription& scene;
    std::filesystem::path sourcePath;
    std::string sourceKey;
    // A live import's glTF: images come from its sources (each read once, and embedded ones readable).
    const json* liveGltf = nullptr;
    GlTFMaterialCache* live = nullptr;
    std::unordered_map<uint32_t, br::textures::TextureSource> imageSources;
    std::vector<std::shared_ptr<TextureAsset>> textures;
    std::vector<std::shared_ptr<Material>> materials;

    br::textures::TextureSource ImageSource(uint32_t index) {
        const auto& image = scene.images.at(index);
        if (live) return CachedGltfImageSource(*liveGltf, sourcePath, *live, image.sourceImage);
        auto it = imageSources.find(index);
        if (it == imageSources.end()) it = imageSources.emplace(index, RealizeImageSource(image, sourcePath, sourceKey)).first;
        return it->second;
    }

    // A material texture's request, without its sampler (which needs a device).
    br::textures::TextureRequest Request(uint32_t index) {
        const auto& texture = scene.textures.at(index);
        br::textures::TextureRequest request;
        request.source = ImageSource(texture.image);
        request.processing = MakeMaterialTextureProcessingSettings(
            texture.semantic, texture.preferSRGB, {}, texture.preservePackedChannels, texture.normalConvention);
        request.preferSRGB = texture.preferSRGB;
        request.debugName = request.source.identity;
        return request;
    }

    std::shared_ptr<TextureAsset> Texture(uint32_t index) {
        if (textures.size() < scene.textures.size()) textures.resize(scene.textures.size());
        auto& texture = textures.at(index);
        if (!texture) {
            auto request = Request(index);
            request.sampler = CreateTextureSampler(scene.textures[index]);
            texture = br::textures::RequestTexture(std::move(request));
        }
        return texture;
    }

    std::shared_ptr<Material> GetMaterial(int32_t index) {
        if (index < 0) return Material::GetDefaultMaterial();
        if (materials.size() < scene.materials.size()) materials.resize(scene.materials.size());
        auto& material = materials.at(static_cast<size_t>(index));
        if (material) return material;

        const auto& record = scene.materials[static_cast<size_t>(index)];
        const std::string sharedCacheKey = MakeMaterialCacheKey(sourceKey, record.sourceMaterial);
        {
            std::lock_guard<std::mutex> lock(g_gltfMaterialCacheMutex);
            auto sharedIt = g_sharedMaterialCache.find(sharedCacheKey);
            if (sharedIt != g_sharedMaterialCache.end()) {
                if (auto shared = sharedIt->second.material.lock()) {
                    material = shared;
                    return material;
                }
                g_sharedMaterialCache.erase(sharedIt);
            }
        }

        MaterialDescription desc = record.description;
        TextureAndConstant* slots[] = { &desc.baseColor, &desc.metallic, &desc.roughness, &desc.emissive, &desc.opacity, &desc.aoMap, &desc.heightMap, &desc.normal };
        static_assert(std::size(slots) == static_cast<size_t>(br::import::SceneCacheTextureSlot::Count));
        for (size_t slot = 0; slot < record.textures.size(); ++slot) {
            if (record.textures[slot] >= 0) slots[slot]->texture = Texture(static_cast<uint32_t>(record.textures[slot]));
        }
        material = Material::CreateShared(desc);
        {
            std::lock_guard<std::mutex> lock(g_gltfMaterialCacheMutex);
            g_sharedMaterialCache[sharedCacheKey] = SharedMaterialCacheEntry{ material };
        }
        return material;
    }
};

// The key materials and textures of a file share: its path, and the profile it is read under.
std::string MakeGltfSourceKey(const std::filesystem::path& sourcePath, const std::shared_ptr<const br::import::ImportProfile>& profile) {
    std::string key = NormalizeSourceKey(sourcePath);
    if (profile) {
        // Materials read under a profile differ from the same file's without one.
        char profileHash[17];
        std::snprintf(profileHash, sizeof(profileHash), "%016llx", static_cast<unsigned long long>(profile->Hash()));
        key += std::string("#profile:") + profileHash;
    }
    return key;
}

GlTFMaterialCache MakeGltfMaterialCache(const json& gltf, const std::filesystem::path& sourcePath, const br::import::ImportSettings& settings) {
    GlTFMaterialCache cache;
    cache.sourceKey = MakeGltfSourceKey(sourcePath, settings.profile);
    cache.profile = settings.profile;
    cache.bufferSources = BuildBufferSources(gltf, sourcePath);
    return cache;
}

Components::Transform ReadNodeTransform(const json& gltfNode) {
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

    return Components::Transform(Components::Position{ translation }, Components::Rotation{ rotation }, Components::Scale{ scale });
}

XMMATRIX ComposeNodeMatrix(const Components::Transform& transform) {
    return XMMatrixScalingFromVector(transform.scale.scale) * XMMatrixRotationQuaternion(transform.rot.rot) *
        XMMatrixTranslationFromVector(transform.pos.pos);
}

// EXT_mesh_gpu_instancing: the node's mesh drawn once per instance, each instance's TRS applied before the node's own
// global matrix. Empty for a node without the extension.
std::vector<XMFLOAT4X4> ReadNodeInstanceMatrices(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    const json& node,
    const XMMATRIX& nodeGlobal)
{
    std::vector<XMFLOAT4X4> matrices;
    if (!node.contains("extensions") || !node["extensions"].contains("EXT_mesh_gpu_instancing")) return matrices;
    const auto& attributes = node["extensions"]["EXT_mesh_gpu_instancing"].value("attributes", json::object());
    std::vector<XMFLOAT3> translations;
    std::vector<XMFLOAT4> rotations;
    std::vector<XMFLOAT3> scales;
    if (attributes.contains("TRANSLATION")) translations = ReadAccessorVec3AsFloat(gltf, bufferSources, attributes["TRANSLATION"].get<size_t>());
    if (attributes.contains("ROTATION")) rotations = ReadAccessorVec4AsFloat(gltf, bufferSources, attributes["ROTATION"].get<size_t>());
    if (attributes.contains("SCALE")) scales = ReadAccessorVec3AsFloat(gltf, bufferSources, attributes["SCALE"].get<size_t>());
    const size_t count = (std::max)({ translations.size(), rotations.size(), scales.size() });
    if ((!translations.empty() && translations.size() != count) || (!rotations.empty() && rotations.size() != count) ||
        (!scales.empty() && scales.size() != count)) {
        throw std::runtime_error("EXT_mesh_gpu_instancing attributes have different counts");
    }
    matrices.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const XMMATRIX scale = scales.empty() ? XMMatrixIdentity() : XMMatrixScaling(scales[i].x, scales[i].y, scales[i].z);
        const XMMATRIX rotation = rotations.empty() ? XMMatrixIdentity() : XMMatrixRotationQuaternion(XMLoadFloat4(&rotations[i]));
        const XMMATRIX translation = translations.empty() ? XMMatrixIdentity()
            : XMMatrixTranslation(translations[i].x, translations[i].y, translations[i].z);
        XMStoreFloat4x4(&matrices[i], scale * rotation * translation * nodeGlobal);
    }
    return matrices;
}

// The glTF node tree without entities: each node's parent and the default scene's roots.
struct NodeTree {
    std::vector<int32_t> parents;
    std::vector<size_t> roots;
};

NodeTree BuildNodeTree(const json& gltf) {
    NodeTree tree;
    if (!gltf.contains("nodes")) return tree;
    const auto& nodeArray = gltf["nodes"];
    tree.parents.assign(nodeArray.size(), -1);
    for (size_t nodeIndex = 0; nodeIndex < nodeArray.size(); ++nodeIndex) {
        if (!nodeArray[nodeIndex].contains("children")) continue;
        for (const auto& childIndexValue : nodeArray[nodeIndex]["children"]) {
            const size_t childIndex = childIndexValue.get<size_t>();
            if (childIndex >= nodeArray.size()) throw std::runtime_error("Node child index out of range");
            tree.parents[childIndex] = static_cast<int32_t>(nodeIndex);
        }
    }
    if (gltf.contains("scenes") && gltf["scenes"].is_array() && !gltf["scenes"].empty()) {
        const size_t sceneIndex = gltf.value<size_t>("scene", static_cast<size_t>(0));
        if (sceneIndex >= gltf["scenes"].size()) throw std::runtime_error("Default scene index out of range");
        const auto& selectedScene = gltf["scenes"][sceneIndex];
        if (selectedScene.contains("nodes") && selectedScene["nodes"].is_array()) {
            for (const auto& nodeValue : selectedScene["nodes"]) {
                const size_t rootIndex = nodeValue.get<size_t>();
                if (rootIndex >= nodeArray.size()) throw std::runtime_error("Scene root node index out of range");
                tree.roots.push_back(rootIndex);
            }
        }
    }
    if (tree.roots.empty()) {
        for (size_t nodeIndex = 0; nodeIndex < nodeArray.size(); ++nodeIndex) {
            if (tree.parents[nodeIndex] < 0) tree.roots.push_back(nodeIndex);
        }
    }
    return tree;
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

// Base skeletons named by the nodes' animation keys. A joint's parent is its
// parent node when that is a joint; otherwise it is a root whose parent global is its non-joint ancestors' (within the file).
std::vector<std::shared_ptr<Skeleton>> BuildPayloadSkins(
    const json& gltf,
    const std::vector<BufferSource>& bufferSources,
    const NodeTree& tree,
    const std::vector<std::string>& nodeAnimationKeys,
    const std::vector<std::shared_ptr<Animation>>& animations)
{
    std::vector<std::shared_ptr<Skeleton>> skeletons;
    if (!gltf.contains("skins") || !gltf["skins"].is_array()) {
        return skeletons;
    }

    const auto& nodes = gltf["nodes"];
    const auto& skins = gltf["skins"];
    skeletons.resize(skins.size());
    for (size_t skinIndex = 0; skinIndex < skins.size(); ++skinIndex) {
        const auto& skinNode = skins[skinIndex];
        if (!skinNode.contains("joints") || !skinNode["joints"].is_array() || skinNode["joints"].empty()) {
            spdlog::warn("glTF skin {} has no joints; skipping skin", skinIndex);
            continue;
        }

        const size_t jointCount = skinNode["joints"].size();
        std::vector<XMMATRIX> inverseBindMatrices(jointCount, XMMatrixIdentity());
        if (skinNode.contains("inverseBindMatrices")) {
            auto loadedMatrices = ReadAccessorMat4AsMatrices(gltf, bufferSources, skinNode["inverseBindMatrices"].get<size_t>());
            if (loadedMatrices.size() != jointCount) {
                spdlog::warn("glTF skin {} inverse bind matrix count ({}) does not match joint count ({}); missing entries default to identity",
                    skinIndex, loadedMatrices.size(), jointCount);
            }
            for (size_t matrixIndex = 0; matrixIndex < std::min(loadedMatrices.size(), jointCount); ++matrixIndex) {
                inverseBindMatrices[matrixIndex] = loadedMatrices[matrixIndex];
            }
        }

        std::unordered_map<size_t, int32_t> jointOfNode;
        std::vector<size_t> jointNodes;
        jointNodes.reserve(jointCount);
        for (const auto& jointValue : skinNode["joints"]) {
            const size_t jointNodeIndex = jointValue.get<size_t>();
            if (jointNodeIndex >= nodes.size()) {
                throw std::runtime_error("glTF skin joint node index out of range");
            }
            jointOfNode.emplace(jointNodeIndex, static_cast<int32_t>(jointNodes.size()));
            jointNodes.push_back(jointNodeIndex);
        }

        std::vector<std::string> boneNames(jointCount);
        std::vector<int32_t> parentIndices(jointCount, -1);
        std::vector<Components::Transform> restLocalTransforms(jointCount);
        std::vector<XMMATRIX> rootParentGlobals(jointCount, XMMatrixIdentity());
        for (size_t joint = 0; joint < jointCount; ++joint) {
            const size_t nodeIndex = jointNodes[joint];
            boneNames[joint] = nodeAnimationKeys[nodeIndex];
            restLocalTransforms[joint] = ReadNodeTransform(nodes[nodeIndex]);
            const int32_t parent = tree.parents[nodeIndex];
            if (parent < 0) continue;
            if (const auto found = jointOfNode.find(static_cast<size_t>(parent)); found != jointOfNode.end()) {
                parentIndices[joint] = found->second;
                continue;
            }
            XMMATRIX global = XMMatrixIdentity();
            for (int32_t ancestor = parent; ancestor >= 0 && !jointOfNode.contains(static_cast<size_t>(ancestor));
                 ancestor = tree.parents[static_cast<size_t>(ancestor)]) {
                global = global * ComposeNodeMatrix(ReadNodeTransform(nodes[static_cast<size_t>(ancestor)]));
            }
            rootParentGlobals[joint] = global;
        }

        auto skeleton = std::make_shared<Skeleton>(std::move(boneNames), std::move(parentIndices),
            std::move(inverseBindMatrices), std::move(restLocalTransforms), std::move(rootParentGlobals));
        for (const auto& animation : animations) {
            const bool usesSkeleton = std::ranges::any_of(jointNodes, [&](size_t nodeIndex) {
                return animation->nodesMap.contains(nodeAnimationKeys[nodeIndex]);
            });
            if (usesSkeleton) skeleton->AddAnimation(animation);
        }
        skeletons[skinIndex] = std::move(skeleton);
    }

    return skeletons;
}

// A file's GPU meshes and their bindings to skins.
struct GltfMeshes {
    GlTFGeometryExtractor::ExtractionResult extraction;
    GlTFMaterialCache materialCache;
    std::vector<std::vector<PrimitiveData>> allMeshes;
    std::vector<std::vector<std::shared_ptr<Mesh>>> sharedMeshLists;
    std::vector<std::vector<std::optional<PreparedPrimitiveData>>> preparedPrimitives;
    std::unordered_map<MeshBindingKey, std::vector<std::shared_ptr<Mesh>>, MeshBindingKeyHasher> skinnedMeshCache;

    // A mesh's primitives, rebuilt with the skin bound when skinIndex names one.
    const std::vector<std::shared_ptr<Mesh>>& Binding(
        size_t meshIndex, int skinIndex, const std::vector<std::shared_ptr<Skeleton>>& skeletons)
    {
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
    }
};

GltfMeshes BuildGltfMeshes(const std::string& filePath, const br::import::ImportSettings& settings) {
    GltfMeshes result;
    GlTFGeometryExtractor::ExtractOptions extractOptions;
    extractOptions.expectedSourceIdentifier = settings.expectedSourceIdentifier;
    if (settings.profile && !settings.profile->skipMeshes.empty()) {
        const auto profile = settings.profile;
        extractOptions.skipMesh = [profile](size_t, std::string_view name) { return profile->SkipsMesh(name); };
    }
    result.extraction = GlTFGeometryExtractor::ExtractAll(filePath, extractOptions);
    auto& extraction = result.extraction;
    const std::filesystem::path sourcePath(filePath);

    auto& materialCache = result.materialCache;
    materialCache = MakeGltfMaterialCache(extraction.gltf, sourcePath, settings);
    br::import::SceneCacheDescription description;
    GltfSceneDescriber describer{ extraction.gltf, sourcePath, materialCache, description };
    GltfSceneRealizer realizer{ description, sourcePath, materialCache.sourceKey, &extraction.gltf, &materialCache };

    // Build mesh/primitive structure from glTF JSON
    const size_t meshCount = extraction.gltf.contains("meshes") ? extraction.gltf["meshes"].size() : 0;
    auto& allMeshes = result.allMeshes;
    auto& preparedPrimitives = result.preparedPrimitives;
    allMeshes.resize(meshCount);
    result.sharedMeshLists.resize(meshCount);
    preparedPrimitives.resize(meshCount);
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
        auto material = realizer.GetMaterial(describer.PrimitiveMaterial(primitiveNode));
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
        result.sharedMeshLists[meshIndex].reserve(allMeshes[meshIndex].size());
        for (const auto& primitive : allMeshes[meshIndex]) {
            if (primitive.mesh != nullptr) {
                result.sharedMeshLists[meshIndex].push_back(primitive.mesh);
            }
        }
    }
    return result;
}

} // namespace

namespace GlTFLoader {

std::vector<br::textures::TextureRequest> BuildMaterialTextureRequests(
    const nlohmann::json& gltf,
    const std::string& filePath,
    const br::import::ImportSettings& settings)
{
    const std::filesystem::path sourcePath(filePath);
    GlTFMaterialCache cache = MakeGltfMaterialCache(gltf, sourcePath, settings);
    br::import::SceneCacheDescription description;
    GltfSceneDescriber describer{ gltf, sourcePath, cache, description };
    GltfSceneRealizer realizer{ description, sourcePath, cache.sourceKey, &gltf, &cache };
    if (gltf.contains("materials")) {
        for (size_t material = 0; material < gltf["materials"].size(); ++material) (void)describer.Material(material);
    }
    std::vector<br::textures::TextureRequest> requests;
    std::unordered_set<std::string> keys;
    for (uint32_t texture = 0; texture < description.textures.size(); ++texture) {
        auto request = realizer.Request(texture);
        if (keys.insert(br::textures::TextureCacheKey(request)).second) requests.push_back(std::move(request));
    }
    return requests;
}

br::import::SceneCacheDescription DescribeScene(
    const nlohmann::json& gltf,
    const std::string& filePath,
    const br::import::ImportSettings& settings,
    const std::string& sourceIdentifier,
    const std::function<bool(size_t meshIndex, std::string_view meshName)>& includeMesh)
{
    const std::filesystem::path sourcePath(filePath);
    GlTFMaterialCache cache = MakeGltfMaterialCache(gltf, sourcePath, settings);
    br::import::SceneCacheDescription scene;
    scene.sourceIdentifier = sourceIdentifier;
    GltfSceneDescriber describer{ gltf, sourcePath, cache, scene };
    if (gltf.contains("animations") && !gltf["animations"].empty()) scene.notCacheableReason = "the scene is animated";

    // A mesh's primitives, described once however many nodes place it.
    std::unordered_map<size_t, std::vector<uint32_t>> meshPrimitives;
    auto primitivesOf = [&](size_t meshIndex) -> const std::vector<uint32_t>& {
        if (const auto it = meshPrimitives.find(meshIndex); it != meshPrimitives.end()) return it->second;
        std::vector<uint32_t> indices;
        const auto& mesh = gltf.at("meshes").at(meshIndex);
        if (mesh.contains("primitives") && (!includeMesh || includeMesh(meshIndex, mesh.value("name", std::string())))) {
            const auto& primitives = mesh["primitives"];
            for (size_t primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
                const auto format = GlTFGeometryExtractor::DescribePrimitiveFormat(gltf, sourceIdentifier, meshIndex, primitiveIndex);
                br::import::SceneCachePrimitive record;
                record.mesh = static_cast<uint32_t>(meshIndex);
                record.primitive = static_cast<uint32_t>(primitiveIndex);
                record.primPath = format.identity.primPath;
                record.doubleSidedVoxelSourceNormals = format.identity.doubleSidedVoxelSourceNormals;
                record.vertexFlags = format.vertexFlags;
                record.vertexSize = format.vertexSize;
                record.skinningVertexSize = format.skinningVertexSize;
                record.uvSetCount = format.uvSetCount;
                record.material = describer.PrimitiveMaterial(primitives[primitiveIndex]);
                indices.push_back(static_cast<uint32_t>(scene.primitives.size()));
                scene.primitives.push_back(std::move(record));
            }
        }
        return meshPrimitives.emplace(meshIndex, std::move(indices)).first->second;
    };

    // The nodes as LoadImportedAssetFromFile places them.
    if (gltf.contains("nodes")) {
        const auto tree = BuildNodeTree(gltf);
        const auto& nodes = gltf["nodes"];
        std::vector<int> visited(nodes.size(), 0);
        auto visit = [&](auto&& self, size_t nodeIndex, const XMMATRIX& parentGlobal) -> void {
            if (visited[nodeIndex]++) throw std::runtime_error("glTF node graph is not a tree");
            const auto& node = nodes[nodeIndex];
            const XMMATRIX global = ComposeNodeMatrix(ReadNodeTransform(node)) * parentGlobal;
            if (node.contains("mesh")) {
                const size_t meshIndex = node["mesh"].get<size_t>();
                if (node.contains("skin")) {
                    if (scene.notCacheableReason.empty()) scene.notCacheableReason = "node " + std::to_string(nodeIndex) + " is skinned";
                }
                else if (const auto& primitives = primitivesOf(meshIndex); !primitives.empty()) {
                    br::import::SceneCachePart part;
                    part.name = node.value("name", "glTF_Node_" + std::to_string(nodeIndex));
                    XMStoreFloat4x4(&part.localMatrix, global);
                    part.instanceMatrices = ReadNodeInstanceMatrices(gltf, cache.bufferSources, node, global);
                    part.primitives = primitives;
                    scene.parts.push_back(std::move(part));
                }
            }
            if (node.contains("children")) {
                for (const auto& child : node["children"]) self(self, child.get<size_t>(), global);
            }
        };
        for (const size_t root : tree.roots) visit(visit, root, XMMatrixIdentity());
    }

    // A cached scene's textures must be the ones a live import requests: same identity, version and processing.
    if (scene.notCacheableReason.empty()) {
        GltfSceneRealizer liveRealizer{ scene, sourcePath, cache.sourceKey, &gltf, &cache };
        GltfSceneRealizer cachedRealizer{ scene, sourcePath, cache.sourceKey };
        for (uint32_t texture = 0; texture < scene.textures.size(); ++texture) {
            const std::string liveKey = br::textures::TextureCacheKey(liveRealizer.Request(texture));
            const std::string cachedKey = br::textures::TextureCacheKey(cachedRealizer.Request(texture));
            if (liveKey != cachedKey) {
                throw std::runtime_error("scene cache texture " + std::to_string(texture) + " realizes as '" + cachedKey +
                    "', but a live import requests '" + liveKey + "'");
            }
        }
    }
    return scene;
}

std::vector<br::textures::TextureRequest> SceneTextureRequests(
    const br::import::SceneCacheDescription& scene,
    const std::string& sourceLocation,
    const br::import::ImportSettings& settings)
{
    const std::filesystem::path sourcePath(sourceLocation);
    GltfSceneRealizer realizer{ scene, sourcePath, MakeGltfSourceKey(sourcePath, settings.profile) };
    std::vector<br::textures::TextureRequest> requests;
    std::unordered_set<std::string> keys;
    for (uint32_t texture = 0; texture < scene.textures.size(); ++texture) {
        auto request = realizer.Request(texture);
        if (keys.insert(br::textures::TextureCacheKey(request)).second) requests.push_back(std::move(request));
    }
    return requests;
}

std::optional<br::import::ImportedAssetPayload> LoadImportedAssetFromSceneCache(
    const br::import::SceneCacheDescription& scene,
    const std::string& sourceLocation,
    const br::import::ImportSettings& settings,
    bool requireComplete)
{
    try {
        const std::filesystem::path sourcePath(sourceLocation);
        const auto begin = std::chrono::steady_clock::now();
        std::vector<std::optional<ClusterLODPrebuiltData>> prebuilt(scene.primitives.size());
        std::atomic<size_t> missing{ 0 };
        TaskSchedulerManager::GetInstance().ParallelFor("GlTFLoader::LoadSceneCache", scene.primitives.size(), [&](size_t index) {
            const auto& primitive = scene.primitives[index];
            CLodCacheLoader::MeshCacheIdentity identity{};
            identity.sourceIdentifier = scene.sourceIdentifier;
            identity.primPath = primitive.primPath;
            identity.subsetName = "";
            identity.doubleSidedVoxelSourceNormals = primitive.doubleSidedVoxelSourceNormals;
            prebuilt[index] = CLodCacheLoader::TryLoadPrebuilt(identity);
            if (!prebuilt[index]) missing.fetch_add(1, std::memory_order_relaxed);
        });
        const auto cacheReadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        if (missing.load() != 0) {
            if (requireComplete) {
                spdlog::warn("Scene cache for {}: {} of {} primitives have no current CLod data", sourceLocation, missing.load(), scene.primitives.size());
                return std::nullopt;
            }
            spdlog::warn("Scene cache for {}: {} of {} primitives have no current CLod data and are left out (the source is not there to build them)",
                sourceLocation, missing.load(), scene.primitives.size());
        }

        GltfSceneRealizer realizer{ scene, sourcePath, MakeGltfSourceKey(sourcePath, settings.profile) };
        std::vector<std::shared_ptr<Mesh>> meshes(scene.primitives.size());
        for (size_t index = 0; index < scene.primitives.size(); ++index) {
            if (!prebuilt[index]) continue;
            const auto& primitive = scene.primitives[index];
            // As the extractor makes a cached primitive's mesh: its format, and UV sets named but empty (the values are in
            // its CLod pages).
            MeshIngestBuilder ingest(primitive.vertexSize, primitive.skinningVertexSize, primitive.vertexFlags);
            std::vector<MeshUvSetData> uvSets(primitive.uvSetCount);
            for (uint32_t set = 0; set < primitive.uvSetCount; ++set) uvSets[set].name = "TEXCOORD_" + std::to_string(set);
            ingest.SetUvSets(std::move(uvSets));
            meshes[index] = ingest.Build(realizer.GetMaterial(primitive.material), std::move(prebuilt[index]), MeshCpuDataPolicy::ReleaseAfterUpload);
        }

        br::import::ImportedAssetPayload payload;
        std::unordered_set<std::uint64_t> meshIDs;
        for (const auto& cachedPart : scene.parts) {
            br::import::RenderablePartPayload part;
            part.localMatrix = XMLoadFloat4x4(&cachedPart.localMatrix);
            part.name = cachedPart.name;
            part.instanceMatrices = cachedPart.instanceMatrices;
            for (const uint32_t primitive : cachedPart.primitives) {
                const auto& mesh = meshes[primitive];
                if (!mesh) continue;
                part.meshes.push_back(mesh);
                if (meshIDs.insert(mesh->GetGlobalID()).second) payload.meshes.push_back(mesh);
            }
            if (!part.meshes.empty()) payload.parts.push_back(std::move(part));
        }
        spdlog::info("Scene cache for {}: {} parts, {} meshes, {} textures; CLod data read in {:.2f} s, {:.2f} s in all", sourceLocation,
            payload.parts.size(), payload.meshes.size(), scene.textures.size(), cacheReadSeconds,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
        return payload;
    }
    catch (const std::exception& e) {
        spdlog::error("Scene cache for {} failed to load: {}", sourceLocation, e.what());
        return std::nullopt;
    }
}

std::optional<br::import::ImportedAssetPayload> LoadImportedAssetFromFile(const std::string& filePath, const br::import::ImportSettings& settings) {
    try {
        auto meshes = BuildGltfMeshes(filePath, settings);
        const auto& gltf = meshes.extraction.gltf;
        const auto tree = BuildNodeTree(gltf);
        const auto nodeAnimationKeys = BuildNodeAnimationKeys(gltf);
        const auto animations = ParseAnimations(gltf, meshes.materialCache.bufferSources, nodeAnimationKeys);
        const auto skeletons = BuildPayloadSkins(gltf, meshes.materialCache.bufferSources, tree, nodeAnimationKeys, animations);

        br::import::ImportedAssetPayload payload;
        std::unordered_set<std::uint64_t> meshIDs;
        std::uint32_t skinnedShapeIndex = 0;
        if (!gltf.contains("nodes")) return payload;
        const auto& nodes = gltf["nodes"];
        std::vector<int> visited(nodes.size(), 0);
        auto visit = [&](auto&& self, size_t nodeIndex, const XMMATRIX& parentGlobal) -> void {
            if (visited[nodeIndex]++) throw std::runtime_error("glTF node graph is not a tree");
            const auto& node = nodes[nodeIndex];
            const XMMATRIX global = ComposeNodeMatrix(ReadNodeTransform(node)) * parentGlobal;
            if (node.contains("mesh")) {
                const size_t meshIndex = node["mesh"].get<size_t>();
                if (meshIndex >= meshes.sharedMeshLists.size()) throw std::runtime_error("Node mesh index out of range");
                const int skinIndex = node.contains("skin") ? static_cast<int>(node["skin"].get<size_t>()) : -1;
                br::import::RenderablePartPayload part;
                part.localMatrix = global;
                part.name = node.value("name", "glTF_Node_" + std::to_string(nodeIndex));
                part.instanceMatrices = ReadNodeInstanceMatrices(gltf, meshes.materialCache.bufferSources, node, global);
                bool skinned = false;
                for (const auto& mesh : meshes.Binding(meshIndex, skinIndex, skeletons)) {
                    if (!mesh) continue;
                    part.meshes.push_back(mesh);
                    skinned = skinned || mesh->GetBaseSkin() != nullptr;
                    if (meshIDs.insert(mesh->GetGlobalID()).second) payload.meshes.push_back(mesh);
                }
                if (skinned) part.skinnedShapeIndex = skinnedShapeIndex++;
                if (!part.meshes.empty()) payload.parts.push_back(std::move(part));
            }
            if (node.contains("children")) {
                for (const auto& child : node["children"]) self(self, child.get<size_t>(), global);
            }
        };
        for (const size_t root : tree.roots) visit(visit, root, XMMatrixIdentity());
        return payload;
    }
    catch (const std::exception& e) {
        spdlog::error("GlTFLoader payload import failed for {}: {}", filePath, e.what());
        return std::nullopt;
    }
}

} // namespace GlTFLoader
