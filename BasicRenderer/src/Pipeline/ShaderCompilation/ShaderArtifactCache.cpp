#include "Pipeline/ShaderCompilation/ShaderArtifactCache.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/CachePaths.h>
#include "Utilities/HashMix.h"

namespace shadercache {
namespace {

std::wstring GetCacheDirectory(BinaryFormat binaryFormat)
{
    switch (binaryFormat) {
    case BinaryFormat::Spirv:
        return L"shaders/spirv";
    case BinaryFormat::Dxil:
    default:
        return L"shaders/dxil";
    }
}

template<typename T>
void WritePod(std::vector<std::byte>& out, const T& value)
{
    const std::byte* ptr = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

template<typename T>
bool ReadPod(const std::vector<std::byte>& in, size_t& offset, T& out)
{
    if (offset + sizeof(T) > in.size()) {
        return false;
    }
    std::memcpy(&out, in.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

void WriteBytes(std::vector<std::byte>& out, const std::vector<std::byte>& value)
{
    const uint64_t length = static_cast<uint64_t>(value.size());
    WritePod(out, length);
    out.insert(out.end(), value.begin(), value.end());
}

bool ReadBytes(const std::vector<std::byte>& in, size_t& offset, std::vector<std::byte>& out)
{
    uint64_t length = 0;
    if (!ReadPod(in, offset, length)) {
        return false;
    }
    if (length > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }
    const size_t size = static_cast<size_t>(length);
    if (offset + size > in.size()) {
        return false;
    }
    out.assign(in.begin() + offset, in.begin() + offset + size);
    offset += size;
    return true;
}

void WriteString(std::vector<std::byte>& out, const std::string& value)
{
    const uint64_t length = static_cast<uint64_t>(value.size());
    WritePod(out, length);
    const std::byte* ptr = reinterpret_cast<const std::byte*>(value.data());
    out.insert(out.end(), ptr, ptr + value.size());
}

bool ReadString(const std::vector<std::byte>& in, size_t& offset, std::string& out)
{
    uint64_t length = 0;
    if (!ReadPod(in, offset, length)) {
        return false;
    }
    if (length > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }
    const size_t size = static_cast<size_t>(length);
    if (offset + size > in.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(in.data() + offset), size);
    offset += size;
    return true;
}

void WriteResourceIdentifiers(std::vector<std::byte>& out, const std::vector<org::ResourceIdentifier>& ids)
{
    const uint64_t count = static_cast<uint64_t>(ids.size());
    WritePod(out, count);
    for (const org::ResourceIdentifier& id : ids) {
        WriteString(out, id.ToString());
    }
}

bool ReadResourceIdentifiers(const std::vector<std::byte>& in, size_t& offset, std::vector<org::ResourceIdentifier>& out)
{
    uint64_t count = 0;
    if (!ReadPod(in, offset, count)) {
        return false;
    }
    if (count > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        std::string text;
        if (!ReadString(in, offset, text)) {
            return false;
        }
        out.emplace_back(text);
    }
    return true;
}

void WriteCacheData(std::vector<std::byte>& out, const CacheData& data)
{
    WritePod(out, data.schemaVersion);
    WritePod(out, data.buildConfigHash);
    WritePod(out, data.binaryFormat);
    WritePod(out, data.artifactKind);
    WritePod(out, data.resourceIDsHash);

    WriteResourceIdentifiers(out, data.resourceDescriptorSlots.mandatoryResourceDescriptorSlots);
    WriteResourceIdentifiers(out, data.resourceDescriptorSlots.optionalResourceDescriptorSlots);

    const uint64_t blobCount = static_cast<uint64_t>(data.blobs.size());
    WritePod(out, blobCount);
    for (const CachedShaderBlob& blob : data.blobs) {
        WritePod(out, blob.kind);
        WriteString(out, blob.entryPoint);
        WriteString(out, blob.target);
        WriteBytes(out, blob.bytecode);
    }
}

bool ReadCacheData(const std::vector<std::byte>& in, CacheData& out)
{
    size_t offset = 0;
    if (!ReadPod(in, offset, out.schemaVersion)) {
        return false;
    }
    if (out.schemaVersion != kSchemaVersion) {
        return false;
    }
    if (!ReadPod(in, offset, out.buildConfigHash)) {
        return false;
    }
    if (!ReadPod(in, offset, out.binaryFormat)) {
        return false;
    }
    if (!ReadPod(in, offset, out.artifactKind)) {
        return false;
    }
    if (!ReadPod(in, offset, out.resourceIDsHash)) {
        return false;
    }
    if (!ReadResourceIdentifiers(in, offset, out.resourceDescriptorSlots.mandatoryResourceDescriptorSlots)) {
        return false;
    }
    if (!ReadResourceIdentifiers(in, offset, out.resourceDescriptorSlots.optionalResourceDescriptorSlots)) {
        return false;
    }

    uint64_t blobCount = 0;
    if (!ReadPod(in, offset, blobCount)) {
        return false;
    }
    if (blobCount > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }
    out.blobs.clear();
    out.blobs.reserve(static_cast<size_t>(blobCount));
    for (uint64_t i = 0; i < blobCount; ++i) {
        CachedShaderBlob blob;
        if (!ReadPod(in, offset, blob.kind)) {
            return false;
        }
        if (!ReadString(in, offset, blob.entryPoint)) {
            return false;
        }
        if (!ReadString(in, offset, blob.target)) {
            return false;
        }
        if (!ReadBytes(in, offset, blob.bytecode)) {
            return false;
        }
        out.blobs.push_back(std::move(blob));
    }

    return offset == in.size();
}

std::vector<std::byte> ReadFileBytes(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open shader cache file: " + ws2s(path));
    }

    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size);
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("Failed to read shader cache file: " + ws2s(path));
    }
    return bytes;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<std::byte>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return file.good();
}
std::mutex& GetMutexForKey(const std::wstring& cachePath)
{
    static std::mutex tableMutex;
    static std::unordered_map<std::wstring, std::unique_ptr<std::mutex>> mutexTable;

    std::lock_guard<std::mutex> lock(tableMutex);
    auto& mutexPtr = mutexTable[cachePath];
    if (!mutexPtr) {
        mutexPtr = std::make_unique<std::mutex>();
    }
    return *mutexPtr;
}

} // namespace

std::wstring BuildCacheFileName(const CacheKey& key, uint64_t buildConfigHash)
{
    uint64_t seed = 0;
    util::hash_combine_u64(seed, static_cast<uint8_t>(key.binaryFormat));
    util::hash_combine_u64(seed, static_cast<uint8_t>(key.artifactKind));
    util::hash_combine_u64(seed, buildConfigHash);
    util::hash_combine_u64(seed, key.identityHash);

    std::wstringstream fileName;
    fileName << (key.artifactKind == ArtifactKind::Bundle ? L"shader_bundle_" : L"shader_library_")
             << std::hex << seed << L".bin";
    return fileName.str();
}

std::optional<CacheData> TryLoad(const CacheKey& key, uint64_t expectedBuildConfigHash)
{
    const std::wstring fileName = BuildCacheFileName(key, expectedBuildConfigHash);
    const std::wstring cachePath = GetCacheFilePath(fileName, GetCacheDirectory(key.binaryFormat));
    if (!std::filesystem::exists(cachePath)) {
        return std::nullopt;
    }

    try {
        CacheData data;
        if (!ReadCacheData(ReadFileBytes(cachePath), data)) {
            spdlog::warn("Shader cache file '{}' is invalid; treating as miss.", ws2s(cachePath));
            return std::nullopt;
        }
        if (data.buildConfigHash != expectedBuildConfigHash || data.binaryFormat != key.binaryFormat || data.artifactKind != key.artifactKind) {
            return std::nullopt;
        }
        return data;
    }
    catch (const std::exception& ex) {
        spdlog::warn("Failed to load shader cache '{}': {}", ws2s(cachePath), ex.what());
        return std::nullopt;
    }
}

bool Save(const CacheKey& key, const CacheData& data)
{
    if (data.schemaVersion != kSchemaVersion || data.binaryFormat != key.binaryFormat || data.artifactKind != key.artifactKind) {
        return false;
    }

    const std::wstring fileName = BuildCacheFileName(key, data.buildConfigHash);
    const std::wstring cachePath = GetCacheFilePath(fileName, GetCacheDirectory(key.binaryFormat));
    std::lock_guard<std::mutex> lock(GetMutexForKey(cachePath));

    std::vector<std::byte> bytes;
    WriteCacheData(bytes, data);
    if (!WriteFileBytes(cachePath, bytes)) {
        spdlog::warn("Failed to write shader cache file '{}'.", ws2s(cachePath));
        return false;
    }
    return true;
}

} // namespace shadercache