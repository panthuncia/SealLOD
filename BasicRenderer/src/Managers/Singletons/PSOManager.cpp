#include "Managers/Singletons/PSOManager.h"
#include <ORGModuleServices/CompileFlightRegistry.h>

#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <spdlog/spdlog.h>
#include <tree_sitter/api.h>

#include "ShaderArtifactCache.h"
#include "Utilities/Utilities.h"
#include "Utilities/HashMix.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Materials/TechniqueDescriptor.h"
#include "brslHelpers.h"
#include "Render/ShaderAPI.h"

#pragma comment(lib, "dxcompiler.lib")

#define WRITE_DEBUG_FILES BUILD_TYPE == BUILD_TYPE_DEBUG || BUILD_TYPE == BUILD_TYPE_RELEASE_DEBUG

namespace {
    // Bump this whenever the shader compiler argument set changes in a way that affects
    // the generated shader bytecode container, especially debug payload availability.
    constexpr uint64_t kShaderCompilerArgumentFingerprint = 5u;

    // Live optimization jobs must observe source edits immediately. The ordinary
    // runtime path still uses the artifact cache; only the worker servicing an
    // explicit pso.recompile request bypasses cache reads.
    thread_local bool g_bypassShaderArtifactCacheReads = false;

    // BRSL resource arguments must be valid HLSL-like identifiers, but some
    // renderer-neutral graph resources intentionally use URI-style public
    // names. Translate those shader-facing aliases before descriptor lookup.
    // Keep this normalization on cache reads as well: cached artifacts retain
    // the original BRSL identifiers in their PipelineResources metadata.
    ResourceIdentifier ResolveRuntimeResourceIdentifier(std::string_view identifier)
    {
        struct Alias {
            std::string_view shaderIdentifier;
            std::string_view runtimeIdentifier;
        };
        static constexpr Alias aliases[] = {
            { "Builtin::Surface::BaseColorOpacity", "sarp.surface.base-color-opacity" },
            { "Builtin::Surface::NormalRoughness", "sarp.surface.normal-roughness" },
            { "Builtin::Surface::SpecularAo", "sarp.surface.specular-ao" },
            { "Builtin::Surface::Emissive", "sarp.surface.emissive" },
            { "Builtin::Surface::Motion", "sarp.surface.motion" },
            { "Builtin::Surface::DeviceDepth", "sarp.surface.device-depth" },
            { "Builtin::Surface::Identity", "sarp.surface.identity" },
            { "Builtin::Surface::Payload0", "sarp.surface.payload0" },
            { "Builtin::Surface::Payload1", "sarp.surface.payload1" },
            { "Builtin::Surface::Records", "sarp.surface.records" },
        };
        for (const Alias& alias : aliases) {
            if (identifier == alias.shaderIdentifier) {
                return ResourceIdentifier{ alias.runtimeIdentifier };
            }
        }
        return ResourceIdentifier{ identifier };
    }

    void NormalizeRuntimeResourceIdentifiers(PipelineResources& resources)
    {
        const auto normalize = [](std::vector<ResourceIdentifier>& identifiers) {
            for (ResourceIdentifier& identifier : identifiers) {
                identifier = ResolveRuntimeResourceIdentifier(identifier.name);
            }
        };
        normalize(resources.mandatoryResourceDescriptorSlots);
        normalize(resources.optionalResourceDescriptorSlots);
    }

    class ScopedShaderArtifactCacheReadBypass {
    public:
        ScopedShaderArtifactCacheReadBypass()
            : m_previous(g_bypassShaderArtifactCacheReads) {
            g_bypassShaderArtifactCacheReads = true;
        }

        ~ScopedShaderArtifactCacheReadBypass() {
            g_bypassShaderArtifactCacheReads = m_previous;
        }

    private:
        bool m_previous;
    };
}

namespace {
shadercache::BinaryFormat GetShaderBinaryFormat(rhi::Backend backend)
{
    return backend == rhi::Backend::Vulkan
        ? shadercache::BinaryFormat::Spirv
        : shadercache::BinaryFormat::Dxil;
}

bool IsSpirvFormat(shadercache::BinaryFormat binaryFormat)
{
    return binaryFormat == shadercache::BinaryFormat::Spirv;
}

constexpr std::wstring_view kValidationSkipEntryPoints[] = {
    L"ClusterLODBucketMSMain",
};

bool ShouldSkipValidationForEntryPoint(std::wstring_view entryPoint)
{
    for (std::wstring_view skippedEntryPoint : kValidationSkipEntryPoints) {
        if (skippedEntryPoint == entryPoint) {
            return true;
        }
    }

    return false;
}

uint64_t HashBytesStable(const void* data, size_t size)
{
    const uint64_t kOffset = 14695981039346656037ull;
    const uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffset;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    hash ^= static_cast<uint64_t>(size);
    hash *= kPrime;
    return hash;
}

uint64_t HashStringStable(std::string_view value)
{
    return HashBytesStable(value.data(), value.size());
}

uint64_t HashPreprocessedBuffer(const DxcBuffer& buffer)
{
    const char* source = static_cast<const char*>(buffer.Ptr);
    const size_t sourceSize = GetNormalizedShaderSourceSize(source, buffer.Size);
    return HashBytesStable(source, sourceSize);
}

uint64_t HashCanonicalPreprocessedBuffer(const DxcBuffer& buffer)
{
    const char* source = static_cast<const char*>(buffer.Ptr);
    const std::string canonicalSource = CanonicalizePreprocessedShaderSourceForHash(source, buffer.Size);
    return HashBytesStable(canonicalSource.data(), canonicalSource.size());
}

size_t GetCanonicalPreprocessedBufferSize(const DxcBuffer& buffer)
{
    const char* source = static_cast<const char*>(buffer.Ptr);
    return CanonicalizePreprocessedShaderSourceForHash(source, buffer.Size).size();
}

std::string NormalizePathUtf8(const std::filesystem::path& path)
{
    return NormalizeCacheSourcePath(ws2s(path.wstring()));
}

uint64_t BuildBundleIdentityHash(
    const ShaderInfoBundle& info,
    const DxcBuffer& amplificationBuffer,
    const DxcBuffer& meshBuffer,
    const DxcBuffer& pixelBuffer,
    const DxcBuffer& vertexBuffer,
    const DxcBuffer& computeBuffer)
{
    uint64_t seed = 0;
    util::hash_combine_u64(seed, info.enableDebugInfo ? 1u : 0u);
    util::hash_combine_u64(seed, info.warningsAsErrors ? 1u : 0u);

    auto hashSlot = [&](shadercache::BlobKind blobKind, const std::optional<ShaderInfo>& slot, const DxcBuffer& buffer) {
        util::hash_combine_u64(seed, static_cast<uint8_t>(blobKind));
        util::hash_combine_u64(seed, slot.has_value() ? 1u : 0u);
        if (!slot) {
            return;
        }

        util::hash_combine_u64(seed, HashCanonicalPreprocessedBuffer(buffer));
        util::hash_combine_u64(seed, GetCanonicalPreprocessedBufferSize(buffer));
        util::hash_combine_u64(seed, HashStringStable(ws2s(slot->entryPoint)));
        util::hash_combine_u64(seed, HashStringStable(ws2s(slot->target)));
        util::hash_combine_u64(seed, ShouldSkipValidationForEntryPoint(slot->entryPoint) ? 1u : 0u);
    };

    hashSlot(shadercache::BlobKind::Amplification, info.amplificationShader, amplificationBuffer);
    hashSlot(shadercache::BlobKind::Mesh, info.meshShader, meshBuffer);
    hashSlot(shadercache::BlobKind::Vertex, info.vertexShader, vertexBuffer);
    hashSlot(shadercache::BlobKind::Pixel, info.pixelShader, pixelBuffer);
    hashSlot(shadercache::BlobKind::Compute, info.computeShader, computeBuffer);
    return seed;
}

uint64_t BuildLibraryIdentityHash(
    const ShaderLibraryInfo& info,
    const DxcBuffer& preprocessedBuffer)
{
    uint64_t seed = 0;
    util::hash_combine_u64(seed, HashCanonicalPreprocessedBuffer(preprocessedBuffer));
    util::hash_combine_u64(seed, GetCanonicalPreprocessedBufferSize(preprocessedBuffer));
    util::hash_combine_u64(seed, HashStringStable(ws2s(info.target)));
    return seed;
}

struct ShaderCompileFlightKey {
    shadercache::BinaryFormat binaryFormat = shadercache::BinaryFormat::Dxil;
    shadercache::ArtifactKind artifactKind = shadercache::ArtifactKind::Bundle;
    uint64_t identityHash = 0;
    uint64_t buildConfigHash = 0;

    bool operator==(const ShaderCompileFlightKey&) const = default;
};

struct ShaderCompileFlightKeyHash {
    size_t operator()(const ShaderCompileFlightKey& key) const noexcept {
        uint64_t seed = 0;
        util::hash_combine_u64(seed, static_cast<uint8_t>(key.binaryFormat));
        util::hash_combine_u64(seed, static_cast<uint8_t>(key.artifactKind));
        util::hash_combine_u64(seed, key.identityHash);
        util::hash_combine_u64(seed, key.buildConfigHash);
        return static_cast<size_t>(seed);
    }
};

using ShaderCompileFlightRegistry = org::services::CompileFlightRegistry<ShaderCompileFlightKey, ShaderCompileFlightKeyHash>;

ShaderCompileFlightRegistry& GetShaderCompileFlightRegistry()
{
    static ShaderCompileFlightRegistry registry;
    return registry;
}

class ShaderCompileFlightScope {
public:
    explicit ShaderCompileFlightScope(ShaderCompileFlightKey key)
        : m_key(key)
    {
    }

    ~ShaderCompileFlightScope()
    {
        if (m_active) {
            GetShaderCompileFlightRegistry().Complete(m_key);
        }
    }

    ShaderCompileFlightScope(const ShaderCompileFlightScope&) = delete;
    ShaderCompileFlightScope& operator=(const ShaderCompileFlightScope&) = delete;

private:
    ShaderCompileFlightKey m_key;
    bool m_active = true;
};

uint64_t ComputeShaderCacheBuildConfigHash(shadercache::BinaryFormat binaryFormat)
{
    uint64_t seed = 0;
    util::hash_combine_u64(seed, shadercache::kSchemaVersion);
    util::hash_combine_u64(seed, static_cast<uint8_t>(binaryFormat));
    util::hash_combine_u64(seed, kBRSLPreprocessVersion);
    util::hash_combine_u64(seed, kShaderCompilerArgumentFingerprint);
#if WRITE_DEBUG_FILES
    util::hash_combine_u64(seed, 1u);
#else
    util::hash_combine_u64(seed, 0u);
#endif
    util::hash_combine_u64(seed, 1u); // warnings-as-errors is always enabled in the current DXC path

    wchar_t modulePath[MAX_PATH] = {};
    HMODULE dxcompilerModule = GetModuleHandleW(L"dxcompiler.dll");
    if (dxcompilerModule != nullptr && GetModuleFileNameW(dxcompilerModule, modulePath, MAX_PATH) > 0) {
        const std::filesystem::path path(modulePath);
        util::hash_combine_u64(seed, HashStringStable(NormalizePathUtf8(path)));

        std::error_code ec;
        const auto fileSize = std::filesystem::file_size(path, ec);
        if (!ec) {
            util::hash_combine_u64(seed, fileSize);
        }
        const auto lastWrite = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            util::hash_combine_u64(seed, lastWrite.time_since_epoch().count());
        }
    }

    return seed;
}

//void LogFailedShaderSource(
//    const std::wstring& filename,
//    const std::wstring& entryPoint,
//    const std::wstring& target,
//    const DxcBuffer& sourceBuffer)
//{
//    if (sourceBuffer.Ptr == nullptr || sourceBuffer.Size == 0) {
//        return;
//    }
//
//    const char* source = static_cast<const char*>(sourceBuffer.Ptr);
//    const size_t sourceSize = GetNormalizedShaderSourceSize(source, sourceBuffer.Size);
//    spdlog::error(
//        "DXC input dump for failed compile file='{}' entry='{}' target='{}':\n{}",
//        ws2s(filename),
//        ws2s(entryPoint),
//        ws2s(target),
//        std::string(source, sourceSize));
//}

bool CreateBlobFromBytes(
    IDxcUtils* utils,
    const std::vector<std::byte>& bytes,
    Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
{
    if (!utils) {
        return false;
    }

    ComPtr<IDxcBlobEncoding> blobEncoding;
    HRESULT hr = utils->CreateBlob(
        bytes.empty() ? nullptr : bytes.data(),
        static_cast<UINT32>(bytes.size()),
        0,
        blobEncoding.GetAddressOf());
    if (FAILED(hr) || !blobEncoding) {
        return false;
    }

    outBlob.Attach(reinterpret_cast<ID3DBlob*>(blobEncoding.Detach()));
    return true;
}

std::vector<std::byte> CopyBlobBytes(ID3DBlob* blob)
{
    if (!blob) {
        return {};
    }
    const std::byte* begin = static_cast<const std::byte*>(blob->GetBufferPointer());
    return std::vector<std::byte>(begin, begin + blob->GetBufferSize());
}

template <typename TCache, typename TKey, typename TFactory>
const PipelineState& GetOrCreatePipelineState(
    TCache& cache,
    const TKey& key,
    TFactory&& factory)
{
    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, factory()).first;
    }
    return it->second;
}

std::string MakePSOKeyId(std::string_view family, const PSOKey& key)
{
    return std::string(family) + ".flags=" + std::to_string(key.psoFlags) +
        ".material=" + std::to_string(static_cast<uint64_t>(key.materialCompileFlags)) +
        ".wire=" + (key.wireframe ? "1" : "0");
}

std::string MakeRasterPSOKeyId(std::string_view family, const RasterPSOKey& key)
{
    return std::string(family) + ".material=" +
        std::to_string(static_cast<uint64_t>(key.materialRasterFlags)) +
        ".wire=" + (key.wireframe ? "1" : "0");
}

void AppendBundleBlob(
    shadercache::CacheData& cacheData,
    shadercache::BlobKind kind,
    const std::wstring& entryPoint,
    const std::wstring& target,
    ID3DBlob* blob)
{
    if (!blob) {
        return;
    }
    cacheData.blobs.push_back(shadercache::CachedShaderBlob{
        .kind = kind,
        .entryPoint = ws2s(entryPoint),
        .target = ws2s(target),
        .bytecode = CopyBlobBytes(blob),
    });
}

std::optional<ShaderBundle> TryLoadShaderBundleFromCache(
    const shadercache::CacheKey& cacheKey,
    uint64_t buildConfigHash,
    IDxcUtils* utils)
{
    std::optional<shadercache::CacheData> cacheData = shadercache::TryLoad(cacheKey, buildConfigHash);
    if (!cacheData.has_value()) {
        return std::nullopt;
    }

    ShaderBundle bundle;
    bundle.resourceDescriptorSlots = cacheData->resourceDescriptorSlots;
    NormalizeRuntimeResourceIdentifiers(bundle.resourceDescriptorSlots);
    bundle.resourceIDsHash = cacheData->resourceIDsHash;

    for (const shadercache::CachedShaderBlob& blob : cacheData->blobs) {
        Microsoft::WRL::ComPtr<ID3DBlob> reconstructedBlob;
        if (!CreateBlobFromBytes(utils, blob.bytecode, reconstructedBlob)) {
            spdlog::warn("Shader cache blob reconstruction failed; treating as miss.");
            return std::nullopt;
        }

        switch (blob.kind) {
        case shadercache::BlobKind::Vertex:
            bundle.vertexShader = std::move(reconstructedBlob);
            break;
        case shadercache::BlobKind::Pixel:
            bundle.pixelShader = std::move(reconstructedBlob);
            break;
        case shadercache::BlobKind::Amplification:
            bundle.amplificationShader = std::move(reconstructedBlob);
            break;
        case shadercache::BlobKind::Mesh:
            bundle.meshShader = std::move(reconstructedBlob);
            break;
        case shadercache::BlobKind::Compute:
            bundle.computeShader = std::move(reconstructedBlob);
            break;
        default:
            spdlog::warn("Unexpected shader cache blob kind {} in bundle cache.", static_cast<uint32_t>(blob.kind));
            return std::nullopt;
        }
    }

    return bundle;
}

std::optional<ShaderLibraryBundle> TryLoadShaderLibraryFromCache(
    const shadercache::CacheKey& cacheKey,
    uint64_t buildConfigHash,
    IDxcUtils* utils)
{
    std::optional<shadercache::CacheData> cacheData = shadercache::TryLoad(cacheKey, buildConfigHash);
    if (!cacheData.has_value()) {
        return std::nullopt;
    }

    if (cacheData->blobs.size() != 1 || cacheData->blobs.front().kind != shadercache::BlobKind::Library) {
        spdlog::warn("Shader library cache entry is malformed; treating as miss.");
        return std::nullopt;
    }

    ShaderLibraryBundle bundle;
    bundle.resourceDescriptorSlots = cacheData->resourceDescriptorSlots;
    NormalizeRuntimeResourceIdentifiers(bundle.resourceDescriptorSlots);
    bundle.resourceIDsHash = cacheData->resourceIDsHash;
    if (!CreateBlobFromBytes(utils, cacheData->blobs.front().bytecode, bundle.libraryBlob)) {
        spdlog::warn("Shader library cache blob reconstruction failed; treating as miss.");
        return std::nullopt;
    }

    return bundle;
}

shadercache::CacheData BuildBundleCacheData(const ShaderInfoBundle& info, const ShaderBundle& bundle,
    uint64_t buildConfigHash, shadercache::BinaryFormat binaryFormat)
{
    shadercache::CacheData cacheData;
    cacheData.buildConfigHash = buildConfigHash;
    cacheData.binaryFormat = binaryFormat;
    cacheData.artifactKind = shadercache::ArtifactKind::Bundle;
    cacheData.resourceDescriptorSlots = bundle.resourceDescriptorSlots;
    cacheData.resourceIDsHash = bundle.resourceIDsHash;

    AppendBundleBlob(cacheData, shadercache::BlobKind::Vertex, info.vertexShader ? info.vertexShader->entryPoint : L"", info.vertexShader ? info.vertexShader->target : L"", bundle.vertexShader.Get());
    AppendBundleBlob(cacheData, shadercache::BlobKind::Pixel, info.pixelShader ? info.pixelShader->entryPoint : L"", info.pixelShader ? info.pixelShader->target : L"", bundle.pixelShader.Get());
    AppendBundleBlob(cacheData, shadercache::BlobKind::Amplification, info.amplificationShader ? info.amplificationShader->entryPoint : L"", info.amplificationShader ? info.amplificationShader->target : L"", bundle.amplificationShader.Get());
    AppendBundleBlob(cacheData, shadercache::BlobKind::Mesh, info.meshShader ? info.meshShader->entryPoint : L"", info.meshShader ? info.meshShader->target : L"", bundle.meshShader.Get());
    AppendBundleBlob(cacheData, shadercache::BlobKind::Compute, info.computeShader ? info.computeShader->entryPoint : L"", info.computeShader ? info.computeShader->target : L"", bundle.computeShader.Get());
    return cacheData;
}

shadercache::CacheData BuildLibraryCacheData(const ShaderLibraryInfo& info, const ShaderLibraryBundle& bundle,
    uint64_t buildConfigHash, shadercache::BinaryFormat binaryFormat)
{
    shadercache::CacheData cacheData;
    cacheData.buildConfigHash = buildConfigHash;
    cacheData.binaryFormat = binaryFormat;
    cacheData.artifactKind = shadercache::ArtifactKind::Library;
    cacheData.resourceDescriptorSlots = bundle.resourceDescriptorSlots;
    cacheData.resourceIDsHash = bundle.resourceIDsHash;
    AppendBundleBlob(cacheData, shadercache::BlobKind::Library, L"", info.target, bundle.libraryBlob.Get());
    return cacheData;
}

} // namespace

void PSOManager::initialize() {
    createRootSignature();

    initializeShaderCompiler();
}

void PSOManager::initializeShaderCompiler() {
    if (pUtils && pCompiler) {
        return;
    }

    HMODULE dxcompiler = LoadLibrary(L"dxcompiler.dll");
    if (!dxcompiler)
    {

        throw std::runtime_error("Failed to load dxcompiler.dll");
    }

    // Get DxcCreateInstance function
    auto DxcCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(dxcompiler, "DxcCreateInstance"));
    if (!DxcCreateInstance)
    {
        throw std::runtime_error("Failed to get DxcCreateInstance function");
    }
    // Create compiler and library instances
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(pUtils.GetAddressOf()));
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(pCompiler.GetAddressOf()));
}

void PSOManager::Cleanup() {
    std::scoped_lock lock(m_cacheMutex, m_livePipelineMutex);
    m_asyncPSOGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_psoCache.clear();
    m_PPLLPSOCache.clear();
    m_meshPSOCache.clear();
    m_meshPPLLPSOCache.clear();
    m_prePassPSOCache.clear();
    m_meshPrePassPSOCache.clear();
    m_shadowPSOCache.clear();
    m_shadowMeshPSOCache.clear();
    m_visibilityBufferPSOCache.clear();
    m_visibilityBufferMeshPSOCache.clear();
    m_deferredPSOCache.clear();
    m_clusterLODRasterPSOCache.clear();
    m_clusterLODVirtualShadowRasterPSOCache.clear();
    m_clusterLODVirtualShadowReyesRasterPSOCache.clear();
    m_clusterLODDeepVisibilityRasterPSOCache.clear();
    m_clusterLODSoftwareRasterPSOCache.clear();
    m_clusterLODAVBOITOccupancyPSOCache.clear();
    m_clusterLODAVBOITRasterPSOCache.clear();
    m_clusterLODAVBOITShadePSOCache.clear();
    m_clusterLODDeepVisibilityResolvePSOCache.clear();
    m_materialEvalPSOCache.clear();
    m_pendingClusterLODRasterPSOs.clear();
    m_pendingClusterLODVirtualShadowRasterPSOs.clear();
    m_pendingClusterLODVirtualShadowReyesRasterPSOs.clear();
    m_pendingClusterLODDeepVisibilityRasterPSOs.clear();
    m_pendingClusterLODAVBOITOccupancyPSOs.clear();
    m_pendingClusterLODAVBOITRasterPSOs.clear();
    m_pendingClusterLODAVBOITShadePSOs.clear();
    m_pendingClusterLODSoftwareRasterPSOs.clear();
    m_pendingMaterialEvalPSOs.clear();
    m_livePipelines.clear();
    m_liveJobs.clear();
    m_pendingLivePublications.clear();
    m_pendingLiveActivations.clear();
    m_retiredLivePayloads.clear();

    debugPSO.Reset();
    environmentConversionPSO.Reset();
    m_rootSignature.Reset();
	m_peerRootSignature.Reset();
    m_computeRootSignature.Reset();
	m_peerComputeRootSignature.Reset();
    m_debugRootSignature.Reset();
    m_environmentConversionRootSignature.Reset();
    pUtils.Reset();
    pCompiler.Reset();
}

const PipelineState& PSOManager::GetPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_psoCache, key, [&]() {
        return RegisterPipeline(
            CreatePSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.Forward", key),
            "Material.Forward",
            LivePipelineKind::Graphics,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreatePSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetShadowPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_shadowPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateShadowPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.Shadow", key),
            "Material.Shadow",
            LivePipelineKind::Graphics,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreateShadowPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetShadowMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_shadowMeshPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateShadowMeshPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.ShadowMesh", key),
            "Material.ShadowMesh",
            LivePipelineKind::Mesh,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreateShadowMeshPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_prePassPSOCache, key, [&]() {
        return RegisterPipeline(
            CreatePrePassPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.PrePass", key),
            "Material.PrePass",
            LivePipelineKind::Graphics,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreatePrePassPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetMeshPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_meshPrePassPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateMeshPrePassPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.MeshPrePass", key),
            "Material.MeshPrePass",
            LivePipelineKind::Mesh,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreateMeshPrePassPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_PPLLPSOCache, key, [&]() {
        return RegisterPipeline(
            CreatePPLLPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.PPLL", key),
            "Material.PPLL",
            LivePipelineKind::Graphics,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreatePPLLPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_meshPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateMeshPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.Mesh", key),
            "Material.Mesh",
            LivePipelineKind::Mesh,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreateMeshPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetMeshPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_meshPPLLPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateMeshPPLLPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.MeshPPLL", key),
            "Material.MeshPPLL",
            LivePipelineKind::Mesh,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreateMeshPPLLPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetVisibilityBufferPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_visibilityBufferPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateVisibilityBufferPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.Visibility", key),
            "Material.Visibility",
            LivePipelineKind::Graphics,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreateVisibilityBufferPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetVisibilityBufferMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    PSOKey key(psoFlags, materialCompileFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_visibilityBufferMeshPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateVisibilityBufferMeshPSO(psoFlags, materialCompileFlags, wireframe),
            MakePSOKeyId("Material.VisibilityMesh", key),
            "Material.VisibilityMesh",
            LivePipelineKind::Mesh,
            [this, psoFlags, materialCompileFlags, wireframe] {
                return CreateVisibilityBufferMeshPSO(psoFlags, materialCompileFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODRasterPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateClusterLODRasterPSO(materialRasterFlags, wireframe),
            MakeRasterPSOKeyId("CLod.Raster", key),
            "CLod.Raster",
            LivePipelineKind::Mesh,
            [this, materialRasterFlags, wireframe] {
                return CreateClusterLODRasterPSO(materialRasterFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODVirtualShadowRasterPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateClusterLODVirtualShadowRasterPSO(materialRasterFlags, wireframe),
            MakeRasterPSOKeyId("CLod.VirtualShadowRaster", key),
            "CLod.VirtualShadowRaster",
            LivePipelineKind::Mesh,
            [this, materialRasterFlags, wireframe] {
                return CreateClusterLODVirtualShadowRasterPSO(materialRasterFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODVirtualShadowReyesRasterPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateClusterLODVirtualShadowReyesRasterPSO(materialRasterFlags, wireframe),
            MakeRasterPSOKeyId("CLod.VirtualShadowReyesRaster", key),
            "CLod.VirtualShadowReyesRaster",
            LivePipelineKind::Mesh,
            [this, materialRasterFlags, wireframe] {
                return CreateClusterLODVirtualShadowReyesRasterPSO(materialRasterFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODDeepVisibilityRasterPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateClusterLODDeepVisibilityRasterPSO(materialRasterFlags, wireframe),
            MakeRasterPSOKeyId("CLod.DeepVisibilityRaster", key),
            "CLod.DeepVisibilityRaster",
            LivePipelineKind::Mesh,
            [this, materialRasterFlags, wireframe] {
                return CreateClusterLODDeepVisibilityRasterPSO(materialRasterFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODAVBOITOccupancyPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateClusterLODAVBOITOccupancyPSO(materialRasterFlags, wireframe),
            MakeRasterPSOKeyId("CLod.AVBOITOccupancy", key),
            "CLod.AVBOITOccupancy",
            LivePipelineKind::Mesh,
            [this, materialRasterFlags, wireframe] {
                return CreateClusterLODAVBOITOccupancyPSO(materialRasterFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODAVBOITRasterPSOCache, key, [&]() {
        return RegisterPipeline(
            CreateClusterLODAVBOITRasterPSO(materialRasterFlags, wireframe),
            MakeRasterPSOKeyId("CLod.AVBOITRaster", key),
            "CLod.AVBOITRaster",
            LivePipelineKind::Mesh,
            [this, materialRasterFlags, wireframe] {
                return CreateClusterLODAVBOITRasterPSO(materialRasterFlags, wireframe);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe, UINT psoFlags) {
    RasterPSOKey key(materialRasterFlags, wireframe, false, psoFlags);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODAVBOITShadePSOCache, key, [&]() {
        return RegisterPipeline(
            CreateClusterLODAVBOITShadePSO(materialRasterFlags, wireframe, psoFlags),
            MakeRasterPSOKeyId("CLod.AVBOITShade", key),
            "CLod.AVBOITShade",
            LivePipelineKind::Mesh,
            [this, materialRasterFlags, wireframe, psoFlags] {
                return CreateClusterLODAVBOITShadePSO(materialRasterFlags, wireframe, psoFlags);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind) {
    const uint64_t key = static_cast<uint64_t>(materialRasterFlags) |
        (static_cast<uint64_t>(outputKind) << 32u);
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODSoftwareRasterPSOCache, key, [&]() {
        return CreateClusterLODSoftwareRasterPSO(materialRasterFlags, outputKind);
    });
}

const PipelineState& PSOManager::GetDeferredPSO(UINT psoFlags) {
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_deferredPSOCache, psoFlags, [&]() {
        return RegisterPipeline(
            CreateDeferredPSO(psoFlags),
            "Material.Deferred.flags=" + std::to_string(psoFlags),
            "Material.Deferred",
            LivePipelineKind::Graphics,
            [this, psoFlags] {
                return CreateDeferredPSO(psoFlags);
            });
    });
}

const PipelineState& PSOManager::GetClusterLODDeepVisibilityResolvePSO(UINT psoFlags) {
    std::scoped_lock lock(m_cacheMutex);
    return GetOrCreatePipelineState(m_clusterLODDeepVisibilityResolvePSOCache, psoFlags, [&]() {
        return CreateClusterLODDeepVisibilityResolvePSO(psoFlags);
    });
}

const PipelineState* PSOManager::TryGetClusterLODRasterPSO(
    MaterialRasterFlags materialRasterFlags,
    bool wireframe,
    bool singleView) {
    RasterPSOKey key(materialRasterFlags, wireframe, singleView);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODRasterPSOCache,
        &PSOManager::m_pendingClusterLODRasterPSOs,
        key,
        "PSOManager::CompileClusterLODRasterPSO",
        [this, materialRasterFlags, wireframe, singleView]() {
            return CreateClusterLODRasterPSO(materialRasterFlags, wireframe, singleView);
        });
}

const PipelineState* PSOManager::TryGetClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODVirtualShadowRasterPSOCache,
        &PSOManager::m_pendingClusterLODVirtualShadowRasterPSOs,
        key,
        "PSOManager::CompileClusterLODVirtualShadowRasterPSO",
        [this, materialRasterFlags, wireframe]() {
            return CreateClusterLODVirtualShadowRasterPSO(materialRasterFlags, wireframe);
        });
}

const PipelineState* PSOManager::TryGetClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODVirtualShadowReyesRasterPSOCache,
        &PSOManager::m_pendingClusterLODVirtualShadowReyesRasterPSOs,
        key,
        "PSOManager::CompileClusterLODVirtualShadowReyesRasterPSO",
        [this, materialRasterFlags, wireframe]() {
            return CreateClusterLODVirtualShadowReyesRasterPSO(materialRasterFlags, wireframe);
        });
}

const PipelineState* PSOManager::TryGetClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODDeepVisibilityRasterPSOCache,
        &PSOManager::m_pendingClusterLODDeepVisibilityRasterPSOs,
        key,
        "PSOManager::CompileClusterLODDeepVisibilityRasterPSO",
        [this, materialRasterFlags, wireframe]() {
            return CreateClusterLODDeepVisibilityRasterPSO(materialRasterFlags, wireframe);
        });
}

const PipelineState* PSOManager::TryGetClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODAVBOITOccupancyPSOCache,
        &PSOManager::m_pendingClusterLODAVBOITOccupancyPSOs,
        key,
        "PSOManager::CompileClusterLODAVBOITOccupancyPSO",
        [this, materialRasterFlags, wireframe]() {
            return CreateClusterLODAVBOITOccupancyPSO(materialRasterFlags, wireframe);
        });
}

const PipelineState* PSOManager::TryGetClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe) {
    RasterPSOKey key(materialRasterFlags, wireframe);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODAVBOITRasterPSOCache,
        &PSOManager::m_pendingClusterLODAVBOITRasterPSOs,
        key,
        "PSOManager::CompileClusterLODAVBOITRasterPSO",
        [this, materialRasterFlags, wireframe]() {
            return CreateClusterLODAVBOITRasterPSO(materialRasterFlags, wireframe);
        });
}

const PipelineState* PSOManager::TryGetClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe, UINT psoFlags) {
    RasterPSOKey key(materialRasterFlags, wireframe, false, psoFlags);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODAVBOITShadePSOCache,
        &PSOManager::m_pendingClusterLODAVBOITShadePSOs,
        key,
        "PSOManager::CompileClusterLODAVBOITShadePSO",
        [this, materialRasterFlags, wireframe, psoFlags]() {
            return CreateClusterLODAVBOITShadePSO(materialRasterFlags, wireframe, psoFlags);
        });
}

const PipelineState* PSOManager::TryGetClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind) {
    const uint64_t key = static_cast<uint64_t>(materialRasterFlags) |
        (static_cast<uint64_t>(outputKind) << 32u);
    return TryGetOrRequestPipelineState(
        &PSOManager::m_clusterLODSoftwareRasterPSOCache,
        &PSOManager::m_pendingClusterLODSoftwareRasterPSOs,
        key,
        "PSOManager::CompileClusterLODSoftwareRasterPSO",
        [this, materialRasterFlags, outputKind]() {
            return CreateClusterLODSoftwareRasterPSO(materialRasterFlags, outputKind);
        });
}

const PipelineState* PSOManager::TryGetMaterialEvalPSO(MaterialCompileFlags materialCompileFlags) {
    return TryGetOrRequestPipelineState(
        &PSOManager::m_materialEvalPSOCache,
        &PSOManager::m_pendingMaterialEvalPSOs,
        materialCompileFlags,
        "PSOManager::CompileMaterialEvalPSO",
        [this, materialCompileFlags]() {
            return CreateMaterialEvalPSO(materialCompileFlags);
        });
}

PipelineState PSOManager::CreatePSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe)
{
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);
    defines.push_back({ L"USE_MISC_DRAW_ROOT_CONSTANTS", L"1" });
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.vertexShader = { L"shaders/shaders.hlsl", L"VSMain", L"vs_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl", L"PSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = PSOManager::GetInstance().CompileShaders(shaderInfoBundle);
    vsBlob = compiledBundle.vertexShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle()};

    rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(vsBlob.Get()), "VSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel,  rhi::DXIL(psBlob.Get()), "PSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    // Depth: Equal, no writes
    rhi::DepthStencilState ds{};
    ds.depthEnable = true;
    ds.depthWrite = false;                  // D3D12_DEPTH_WRITE_MASK_ZERO
    ds.depthFunc = rhi::CompareOp::Equal;  // D3D12_COMPARISON_FUNC_EQUAL
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};
    if (psoFlags & PSOFlags::PSO_SHADOW) {
        rts.count = 0;
    }
    else {
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16B16A16_Float;
    }
    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soVS),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso; 
	auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create PSO (RHI)");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateShadowPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe)
{
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);
    defines.push_back({ L"USE_MISC_DRAW_ROOT_CONSTANTS", L"1" });

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.vertexShader = { L"shaders/shaders.hlsl", L"VSMain", L"vs_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl", L"PSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    vsBlob = compiledBundle.vertexShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle()};

    rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(vsBlob.Get()), "VSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel,  rhi::DXIL(psBlob.Get()), "PSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    rhi::DepthStencilState ds{};
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};
    rts.count = 1;
    rts.formats[0] = rhi::Format::R32_Float;
    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soVS),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
	auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create Shadow PSO (RHI)");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}


PipelineState PSOManager::CreatePrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe)
{
    auto defines = GetShaderDefines(psoFlags | PSO_PREPASS, materialCompileFlags);
    defines.push_back({ L"USE_MISC_DRAW_ROOT_CONSTANTS", L"1" });

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.vertexShader = { L"shaders/shaders.hlsl", L"VSMain",        L"vs_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl", L"PrepassPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    vsBlob = compiledBundle.vertexShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature(); // PipelineLayoutHandle
    rhi::SubobjLayout soLayout{ layout.GetHandle()};

    rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(vsBlob.Get()), "VSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel,  rhi::DXIL(psBlob.Get()), "PrepassPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    rhi::DepthStencilState ds{}; // defaults: depth test on, write on, LessEqual
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};
    rts.count = 6;
    rts.formats[0] = rhi::Format::R32G32B32A32_Float;   // Normals
    rts.formats[1] = rhi::Format::R16G16_Float;         // Motion vector
    rts.formats[2] = rhi::Format::R32_Float;            // Depth
    rts.formats[3] = rhi::Format::R8G8B8A8_UNorm;       // Albedo
    rts.formats[4] = rhi::Format::R8G8_UNorm;           // Metallic+Roughness
    rts.formats[5] = rhi::Format::R16G16B16A16_Float;   // Emissive

    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soVS),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
	auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create PrePass PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateVisibilityBufferPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe)
{
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.vertexShader = { L"shaders/shaders.hlsl", L"VisibilityBufferVSMain", L"vs_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl", L"VisibilityBufferPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    vsBlob = compiledBundle.vertexShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature(); // PipelineLayoutHandle
    rhi::SubobjLayout soLayout{ layout.GetHandle() };

    rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(vsBlob.Get()), "VisibilityBufferVSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel,  rhi::DXIL(psBlob.Get()), "VisibilityBufferPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    rhi::DepthStencilState ds{}; // defaults: depth test on, write on, LessEqual
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};

    rts.count = 1;
    rts.formats[0] = rhi::Format::R32G32_UInt; // packed visibility

    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soVS),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
	auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create PrePass PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateVisibilityBufferMeshPSO(
    UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);

    Microsoft::WRL::ComPtr<ID3DBlob> asBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.amplificationShader = { L"shaders/amplification.hlsl", L"ASMain", L"as_6_6" };
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl",          L"VisibilityBufferMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl",       L"VisibilityBufferPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    asBlob = compiledBundle.amplificationShader;
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soTask{ rhi::ShaderStage::Task, rhi::DXIL(asBlob.Get()), "ASMain" };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "VisibilityBufferMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "VisibilityBufferPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    rhi::DepthStencilState ds{};
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};
    
    rts.count = 1;
    rts.formats[0] = rhi::Format::R32G32_UInt; // packed visibility

    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV   soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soTask),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create Mesh PrePass PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODRasterPSO(
    MaterialRasterFlags materialRasterFlags, bool wireframe, bool singleView) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);
    if (singleView) {
        defines.push_back({ L"CLOD_RASTER_SINGLE_VIEW", L"1" });
    }

    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"ClusterLODBucketMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/visibilityOutput.hlsl", L"VisibilityBufferPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "ClusterLODBucketMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "VisibilityBufferPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialRasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create Mesh PrePass PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODVirtualShadowRasterPSO(
    MaterialRasterFlags materialRasterFlags, bool wireframe) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);
    defines.push_back({ L"CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW", L"1" });
    defines.push_back({ L"CLOD_VSM_TWO_LAYER_RASTER_VERSION", L"2" });

    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"ClusterLODBucketMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/VirtualShadowOutput.hlsl", L"VirtualShadowBufferPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "ClusterLODBucketMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "VirtualShadowBufferPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialRasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod virtual shadow raster PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODVirtualShadowReyesRasterPSO(
    MaterialRasterFlags materialRasterFlags, bool wireframe) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);
    defines.push_back({ L"CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW", L"1" });
    defines.push_back({ L"CLOD_VSM_TWO_LAYER_RASTER_VERSION", L"2" });

    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"ClusterLODReyesVirtualShadowMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/VirtualShadowOutput.hlsl", L"VirtualShadowBufferPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "ClusterLODReyesVirtualShadowMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "VirtualShadowBufferPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialRasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod Reyes virtual shadow raster PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODDeepVisibilityRasterPSO(
    MaterialRasterFlags materialRasterFlags, bool wireframe) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);

    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"ClusterLODBucketMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/DeepVisibilityOutput.hlsl", L"DeepVisibilityBufferPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "ClusterLODBucketMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "DeepVisibilityBufferPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialRasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod deep visibility raster PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODAVBOITRasterPSO(
    MaterialRasterFlags materialRasterFlags, bool wireframe) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);
    DxcDefine forwardTransparentMacro;
    forwardTransparentMacro.Value = L"1";
    forwardTransparentMacro.Name = L"CLOD_AVBOIT_FORWARD_TRANSPARENT";
    defines.insert(defines.begin(), forwardTransparentMacro);

    DxcDefine separateReyesBatchMacro;
    separateReyesBatchMacro.Value = L"1";
    separateReyesBatchMacro.Name = L"CLOD_AVBOIT_REYES_SEPARATE_BATCH";
    defines.insert(defines.begin(), separateReyesBatchMacro);
    defines.push_back(DxcDefine{ L"CLOD_AVBOIT_LOW_RES_RASTER", L"1" });

    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"ClusterLODBucketMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/AVBOITCapture.hlsl", L"AVBOITCapturePSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "ClusterLODBucketMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "AVBOITCapturePSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialRasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod AVBOIT raster PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODAVBOITOccupancyPSO(
    MaterialRasterFlags materialRasterFlags, bool wireframe) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);
    DxcDefine forwardTransparentMacro;
    forwardTransparentMacro.Value = L"1";
    forwardTransparentMacro.Name = L"CLOD_AVBOIT_FORWARD_TRANSPARENT";
    defines.insert(defines.begin(), forwardTransparentMacro);

    DxcDefine separateReyesBatchMacro;
    separateReyesBatchMacro.Value = L"1";
    separateReyesBatchMacro.Name = L"CLOD_AVBOIT_REYES_SEPARATE_BATCH";
    defines.insert(defines.begin(), separateReyesBatchMacro);

    defines.push_back(DxcDefine{ L"CLOD_AVBOIT_VBOIT_OCCUPANCY_ONLY", L"1" });
    defines.push_back(DxcDefine{ L"CLOD_AVBOIT_LOW_RES_RASTER", L"1" });

    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"ClusterLODBucketMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/AVBOITCapture.hlsl", L"AVBOITCapturePSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "ClusterLODBucketMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "AVBOITCapturePSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialRasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod AVBOIT occupancy PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODAVBOITShadePSO(
    MaterialRasterFlags materialRasterFlags, bool wireframe, UINT psoFlags) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);
    auto lightingDefines = GetShaderDefines(psoFlags, MaterialCompileNone);
    defines.insert(defines.end(), lightingDefines.begin(), lightingDefines.end());
    DxcDefine forwardTransparentMacro;
    forwardTransparentMacro.Value = L"1";
    forwardTransparentMacro.Name = L"CLOD_AVBOIT_FORWARD_TRANSPARENT";
    defines.insert(defines.begin(), forwardTransparentMacro);

    DxcDefine separateReyesBatchMacro;
    separateReyesBatchMacro.Value = L"1";
    separateReyesBatchMacro.Name = L"CLOD_AVBOIT_REYES_SEPARATE_BATCH";
    defines.insert(defines.begin(), separateReyesBatchMacro);

    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"ClusterLODBucketMSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/ClusterLOD/AVBOITShade.hlsl", L"AVBOITShadePSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "ClusterLODBucketMSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "AVBOITShadePSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialRasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState blend{};
    blend.alphaToCoverage = FALSE;
    blend.independentBlend = FALSE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        blend.attachments[i].enable = TRUE;
        blend.attachments[i].srcColor = rhi::BlendFactor::One;
        blend.attachments[i].dstColor = rhi::BlendFactor::One;
        blend.attachments[i].colorOp = rhi::BlendOp::Add;
        blend.attachments[i].srcAlpha = rhi::BlendFactor::One;
        blend.attachments[i].dstAlpha = rhi::BlendFactor::One;
        blend.attachments[i].alphaOp = rhi::BlendOp::Add;
        blend.attachments[i].writeMask = rhi::ColorWriteEnable::All;
    }
    rhi::SubobjBlend soBlend{ blend };

    rhi::DepthStencilState depthState{};
    depthState.depthEnable = true;
    depthState.depthWrite = false;
    depthState.depthFunc = rhi::CompareOp::LessEqual;
    rhi::SubobjDepth soDepth{ depthState };

    rhi::RenderTargets rts{};
    rts.count = 3;
    rts.formats[0] = rhi::Format::R16G16B16A16_Float;
    rts.formats[1] = rhi::Format::R16G16B16A16_Float;
    rts.formats[2] = rhi::Format::R16G16B16A16_Float;
    rhi::SubobjRTVs soRTV{ rts };
    rhi::SubobjDSV soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{ 1, 0 } };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create CLod AVBOIT shading PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind) {
    auto defines = GetRasterShaderDefines(materialRasterFlags);
    if (outputKind == CLodRasterOutputKind::VirtualShadow) {
        defines.push_back({ L"CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW", L"1" });
        defines.push_back({ L"CLOD_VSM_TWO_LAYER_RASTER_VERSION", L"2" });
    }
    return MakeComputePipeline(
        GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/softwareRaster.hlsl",
        L"SWRasterIndirectCSMain",
        defines,
        "CLod_SoftwareRasterIndirectPSO");
}

PipelineState PSOManager::CreatePPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe)
{
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);
    defines.push_back({ L"USE_MISC_DRAW_ROOT_CONSTANTS", L"1" });

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.vertexShader = { L"shaders/shaders.hlsl", L"VSMain", L"vs_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/PPLL.hlsl",    L"PPLLFillPS", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    vsBlob = compiledBundle.vertexShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature(); // PipelineLayoutHandle
    rhi::SubobjLayout soLayout{ layout.GetHandle() };

    rhi::SubobjShader soVS{ rhi::ShaderStage::Vertex, rhi::DXIL(vsBlob.Get()), "VSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel,  rhi::DXIL(psBlob.Get()), "PPLLFillPS" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    rhi::DepthStencilState ds{};
    ds.depthEnable = true;
    ds.depthWrite = false; // D3D12_DEPTH_WRITE_MASK_ZERO
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};
    rts.count = 1;
    rts.formats[0] = (psoFlags & PSOFlags::PSO_SHADOW)
        ? rhi::Format::R32_Float
        : rhi::Format::R16G16B16A16_Float;
    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soVS),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create PPLL PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateMeshPSO(
    UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe)
{
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);

    Microsoft::WRL::ComPtr<ID3DBlob> asBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.amplificationShader = { L"shaders/amplification.hlsl", L"ASMain", L"as_6_6" };
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl",          L"MSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl",       L"PSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    asBlob = compiledBundle.amplificationShader;
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature(); // your PipelineLayoutHandle
    rhi::SubobjLayout soLayout{ layout.GetHandle() };

    rhi::SubobjShader soTask{ rhi::ShaderStage::Task,  rhi::DXIL(asBlob.Get()), "ASMain" };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh,  rhi::DXIL(msBlob.Get()), "MSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "PSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    rhi::DepthStencilState ds{};
    ds.depthEnable = true;
    ds.depthWrite = false;                   // D3D12_DEPTH_WRITE_MASK_ZERO
    ds.depthFunc = rhi::CompareOp::Equal;   // D3D12_COMPARISON_FUNC_EQUAL
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};
    if (psoFlags & PSOFlags::PSO_SHADOW) {
        rts.count = 0;
    }
    else {
        rts.count = 1;
        rts.formats[0] = rhi::Format::R16G16B16A16_Float; // DXGI_FORMAT_R16G16B16A16_FLOAT
    }
    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soTask),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create Mesh PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateShadowMeshPSO(
    UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe)
{
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);

    Microsoft::WRL::ComPtr<ID3DBlob> asBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.amplificationShader = { L"shaders/amplification.hlsl", L"ASMain", L"as_6_6" };
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl",          L"MSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl",       L"PSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    asBlob = compiledBundle.amplificationShader;
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };

    rhi::SubobjShader soTask{ rhi::ShaderStage::Task,  rhi::DXIL(asBlob.Get()), "ASMain" };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh,  rhi::DXIL(msBlob.Get()), "MSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "PSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };


    rhi::DepthStencilState ds{};
    rhi::SubobjDepth soDepth{ ds };

    // Render target formats
    rhi::RenderTargets rts{};
    rts.count = 1;
    rts.formats[0] = rhi::Format::R32_Float;
    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV    soDSV{ rhi::Format::D32_Float }; // DXGI_FORMAT_D32_FLOAT
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soTask),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create Shadow Mesh PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateMeshPrePassPSO(
    UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    auto defines = GetShaderDefines(psoFlags | PSO_PREPASS, materialCompileFlags);

    Microsoft::WRL::ComPtr<ID3DBlob> asBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> msBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;

    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.amplificationShader = { L"shaders/amplification.hlsl", L"ASMain", L"as_6_6" };
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl",          L"MSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/shaders.hlsl",       L"PrepassPSMain", L"ps_6_6" };
    shaderInfoBundle.defines = defines;

    auto compiledBundle = CompileShaders(shaderInfoBundle);
    asBlob = compiledBundle.amplificationShader;
    msBlob = compiledBundle.meshShader;
    psBlob = compiledBundle.pixelShader;

	auto& layout = GetRootSignature();
    rhi::SubobjLayout soLayout{ layout.GetHandle() };
    rhi::SubobjShader soTask{ rhi::ShaderStage::Task, rhi::DXIL(asBlob.Get()), "ASMain" };
    rhi::SubobjShader soMesh{ rhi::ShaderStage::Mesh, rhi::DXIL(msBlob.Get()), "MSMain" };
    rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(psBlob.Get()), "PrepassPSMain" };

    rhi::RasterState rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster soRaster{ rs };

    rhi::BlendState rhiBlend = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend soBlend{ rhiBlend };

    rhi::DepthStencilState ds{};
    rhi::SubobjDepth soDepth{ ds };

    rhi::RenderTargets rts{};
    rts.count = 6;
    rts.formats[0] = rhi::Format::R32G32B32A32_Float;   // Normals
    rts.formats[1] = rhi::Format::R16G16_Float;         // Motion vector
    rts.formats[2] = rhi::Format::R32_Float;            // Depth
    rts.formats[3] = rhi::Format::R8G8B8A8_UNorm;       // Albedo
    rts.formats[4] = rhi::Format::R8G8_UNorm;           // Metallic+Roughness
    rts.formats[5] = rhi::Format::R16G16B16A16_Float;   // Emissive

    rhi::SubobjRTVs soRTV{ rts };

    rhi::SubobjDSV   soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample soSmp{ rhi::SampleDesc{1,0} };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soTask),
        rhi::Make(soMesh),
        rhi::Make(soPS),
        rhi::Make(soRaster),
        rhi::Make(soBlend),
        rhi::Make(soDepth),
        rhi::Make(soRTV),
        rhi::Make(soDSV),
        rhi::Make(soSmp),
    };

	auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create Mesh PrePass PSO");
    }

    return { std::move(pso), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateMeshPPLLPSO(
    UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe) {
    // Define shader macros
    auto defines = GetShaderDefines(psoFlags, materialCompileFlags);

    // Compile shaders
	Microsoft::WRL::ComPtr<ID3DBlob> amplificationShader;
    Microsoft::WRL::ComPtr<ID3DBlob> meshShader;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
    
    ShaderInfoBundle shaderInfoBundle;
    shaderInfoBundle.amplificationShader = { L"shaders/amplification.hlsl", L"ASMain", L"as_6_6" };
    shaderInfoBundle.meshShader = { L"shaders/mesh.hlsl", L"MSMain", L"ms_6_6" };
    shaderInfoBundle.pixelShader = { L"shaders/PPLL.hlsl", L"PPLLFillPS", L"ps_6_6" };
    shaderInfoBundle.defines = defines;
    auto compiledBundle = CompileShaders(shaderInfoBundle);
    amplificationShader = compiledBundle.amplificationShader;
    meshShader = compiledBundle.meshShader;
	pixelShader = compiledBundle.pixelShader;

    auto& layout = GetRootSignature();
    rhi::SubobjLayout    soLayout{ layout.GetHandle() };

    rhi::SubobjShader    soAS{ rhi::ShaderStage::Task, { amplificationShader->GetBufferPointer(), (uint32_t)amplificationShader->GetBufferSize() }, "ASMain" };
    rhi::SubobjShader    soMS{ rhi::ShaderStage::Mesh, { meshShader->GetBufferPointer(),          (uint32_t)meshShader->GetBufferSize() }, "MSMain" };
    rhi::SubobjShader    soPS{ rhi::ShaderStage::Pixel, { pixelShader->GetBufferPointer(),         (uint32_t)pixelShader->GetBufferSize() }, "PPLLFillPS" };

    rhi::RasterState     rs{};
    rs.fill = wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
    rs.cull = (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) ? rhi::CullMode::None : rhi::CullMode::Back;
    rs.frontCCW = true;
    rhi::SubobjRaster    soRaster{ rs };

    rhi::BlendState      bs = GetBlendDesc(materialCompileFlags);
    rhi::SubobjBlend     soBlend{ bs };

    rhi::DepthStencilState ds{};
    ds.depthEnable = true;
    ds.depthWrite = true;
    ds.depthFunc = rhi::CompareOp::LessEqual; // default
    rhi::SubobjDepth     soDepth{ ds };

    rhi::RenderTargets   rts{};
    rts.count = 6;
    rts.formats[0] = rhi::Format::R32G32B32A32_Float;
    rts.formats[1] = rhi::Format::R16G16_Float;
    rts.formats[2] = rhi::Format::R32_Float;
    rts.formats[3] = rhi::Format::R8G8B8A8_UNorm;
    rts.formats[4] = rhi::Format::R8G8_UNorm;
    rts.formats[5] = rhi::Format::R16G16B16A16_Float;

    rhi::SubobjRTVs      soRTV{ rts };

    rhi::SubobjDSV       soDSV{ rhi::Format::D32_Float };
    rhi::SubobjSample    soSmp{ rhi::SampleDesc{1,0} };

    rhi::PipelineStreamItem items[] = {
      rhi::Make(soLayout),
      rhi::Make(soAS), rhi::Make(soMS), rhi::Make(soPS),
      rhi::Make(soRaster), rhi::Make(soBlend), rhi::Make(soDepth),
      rhi::Make(soRTV), rhi::Make(soDSV), rhi::Make(soSmp),
    };
	auto device = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr psoPrepass;

    auto result = device.CreatePipeline(items, (uint32_t)std::size(items), psoPrepass);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create Mesh PrePass PSO");
    }

    return { std::move(psoPrepass), compiledBundle.resourceIDsHash, compiledBundle.resourceDescriptorSlots };
}

PipelineState PSOManager::CreateDeferredPSO(UINT psoFlags)
{
    auto defines = GetShaderDefines(psoFlags, MaterialCompileFlags::MaterialCompileNone);
    defines.push_back({ L"CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE", L"1" });
    PipelineState pso = MakeComputePipeline(
        GetComputeRootSignature().GetHandle(),
        L"shaders/deferred.hlsl",
        L"DeferredCSMain",
        defines,
        "DeferredComputePSO"
    );
    return pso;
}

PipelineState PSOManager::CreateClusterLODDeepVisibilityResolvePSO(UINT psoFlags)
{
    auto defines = GetShaderDefines(psoFlags, MaterialCompileFlags::MaterialCompileNone);
    PipelineState pso = MakeComputePipeline(
        GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/DeepVisibilityResolve.hlsl",
        L"CLodDeepVisibilityResolveCS",
        defines,
        "CLod.DeepVisibilityResolve.PSO");
    return pso;
}

PipelineState PSOManager::CreateMaterialEvalPSO(MaterialCompileFlags materialCompileFlags)
{
    auto shaderDefines = GetShaderDefines(0, materialCompileFlags);
    shaderDefines.push_back({ L"VISUTIL_SPECIALIZED_MATERIAL_EVAL", L"1" });
    shaderDefines.push_back({ L"VISUTIL_USE_COMPACT_MATERIAL_EVAL", L"1" });
    shaderDefines.push_back({ L"VISUTIL_USE_CACHED_VIS_KEY", L"1" });
    shaderDefines.push_back({ L"MATERIAL_EVAL_COMPILED_UV_REQUIREMENTS", L"1" });
    shaderDefines.push_back({ L"CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE", L"1" });
    if (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) {
        shaderDefines.push_back({ L"VISUTIL_DOUBLE_SIDED_GBUFFER_RESOLVE", L"1" });
    }

    spdlog::info(
        "PSOManager: creating material eval PSO shaderKey=0x{:X} defines={}",
        static_cast<uint64_t>(materialCompileFlags),
        shaderDefines.size());

    return MakeComputePipeline(
        GetComputeRootSignature().GetHandle(),
        L"shaders/VisUtilEvaluate.hlsl",
        L"EvaluateMaterialGroupCS",
        std::move(shaderDefines),
        "VisUtil_EvaluateMaterialGroupPSO");
}

void PSOManager::PrecompileMaterialEvalShaderArtifact(MaterialCompileFlags materialCompileFlags)
{
    auto shaderDefines = GetShaderDefines(0, materialCompileFlags);
    shaderDefines.push_back({ L"VISUTIL_SPECIALIZED_MATERIAL_EVAL", L"1" });
    shaderDefines.push_back({ L"VISUTIL_USE_COMPACT_MATERIAL_EVAL", L"1" });
    shaderDefines.push_back({ L"VISUTIL_USE_CACHED_VIS_KEY", L"1" });
    shaderDefines.push_back({ L"MATERIAL_EVAL_COMPILED_UV_REQUIREMENTS", L"1" });
    shaderDefines.push_back({ L"CLOD_VSM_ADAPTIVE_RECEIVER_SCREEN_TRACE", L"1" });
    if (materialCompileFlags & MaterialCompileFlags::MaterialCompileDoubleSided) {
        shaderDefines.push_back({ L"VISUTIL_DOUBLE_SIDED_GBUFFER_RESOLVE", L"1" });
    }

    ShaderInfoBundle shaderInfo;
    shaderInfo.computeShader = { L"shaders/VisUtilEvaluate.hlsl", L"EvaluateMaterialGroupCS", L"cs_6_6" };
    shaderInfo.defines = std::move(shaderDefines);
    // Offline preprocessing has no DeviceManager device, so its runtime backend
    // is intentionally Null. Produce the default renderer-host artifacts.
    CompileShadersForBackend(shaderInfo, rhi::Backend::D3D12);
}

PipelineState PSOManager::MakeComputePipeline(rhi::PipelineLayoutHandle layout,
    const wchar_t* shaderPath,
    const wchar_t* entryPoint,
    std::vector<DxcDefine> defines,
    const char* debugName)
{
    ComputeRecipe recipe;
    recipe.layout = layout;
    recipe.shaderPath = shaderPath;
    recipe.entryPoint = entryPoint;
    recipe.debugName = debugName ? debugName : ws2s(entryPoint);
    recipe.defines.reserve(defines.size());
    for (const DxcDefine& define : defines) {
        recipe.defines.push_back({
            define.Name ? define.Name : L"",
            define.Value ? define.Value : L""
        });
    }
    PipelineState pipeline = BuildComputePipeline(recipe);
    return RegisterComputePipeline(std::move(pipeline), std::move(recipe));
}

PipelineState PSOManager::BuildComputePipeline(const ComputeRecipe& recipe, const RecompileOptions* options)
{
    std::vector<OwnedDefine> ownedDefines = recipe.defines;
    if (options) {
        for (const auto& [name, value] : options->defineOverrides) {
            auto existing = std::ranges::find_if(ownedDefines, [&](const OwnedDefine& define) {
                return define.name == name;
            });
            if (existing == ownedDefines.end()) {
                ownedDefines.push_back({ name, value });
            } else {
                existing->value = value;
            }
        }
    }

    std::vector<DxcDefine> defines;
    defines.reserve(ownedDefines.size());
    for (const OwnedDefine& define : ownedDefines) {
        defines.push_back({ define.name.c_str(), define.value.c_str() });
    }

    ShaderInfoBundle sib;
    sib.computeShader = { recipe.shaderPath, recipe.entryPoint, L"cs_6_6" };
    sib.defines = defines;
    auto compiled = CompileShaders(sib);

    rhi::SubobjLayout soLayout{ recipe.layout };
    rhi::SubobjShader soCS{ rhi::ShaderStage::Compute, rhi::DXIL(compiled.computeShader.Get()), ws2s(recipe.entryPoint) };

    const rhi::PipelineStreamItem items[] = {
        rhi::Make(soLayout),
        rhi::Make(soCS),
    };

    auto dev = DeviceManager::GetInstance().GetDevice();
    rhi::PipelinePtr pso;
    auto result = dev.CreatePipeline(items, (uint32_t)std::size(items), pso);
    if (Failed(result)) {
        throw std::runtime_error("Failed to create compute PSO (RHI)");
    }
    if (!recipe.debugName.empty()) {
        pso->SetName(recipe.debugName.c_str());
    }

    PipelineState out{
        std::move(pso),
        compiled.resourceIDsHash,
        compiled.resourceDescriptorSlots
    };
    auto payload = out.GetPayload();
    payload->bytecodeHash = HashBytesStable(
        compiled.computeShader->GetBufferPointer(),
        compiled.computeShader->GetBufferSize());
    uint64_t sourceHash = HashStringStable(ws2s(recipe.shaderPath));
    util::hash_combine_u64(sourceHash, HashStringStable(ws2s(recipe.entryPoint)));
    for (const OwnedDefine& define : ownedDefines) {
        util::hash_combine_u64(sourceHash, HashStringStable(ws2s(define.name)));
        util::hash_combine_u64(sourceHash, HashStringStable(ws2s(define.value)));
    }
    payload->sourceHash = sourceHash;
    payload->label = options && !options->label.empty() ? options->label : "initial";

    return out;
}

void PSOManager::BuildComputePipelineForBackend(const ComputeRecipe& recipe, const PipelineState& pipeline,
	BackendInstanceId backendInstance) {
	if (backendInstance == BackendInstanceId::Primary || pipeline.HasBackendPipeline(backendInstance)) return;
	std::vector<DxcDefine> defines;
	defines.reserve(recipe.defines.size());
	for (const OwnedDefine& define : recipe.defines) defines.push_back({ define.name.c_str(), define.value.c_str() });
	ShaderInfoBundle sib;
	sib.computeShader = { recipe.shaderPath, recipe.entryPoint, L"cs_6_6" };
	sib.defines = defines;
	auto compiled = CompileShaders(sib, backendInstance);
	rhi::SubobjLayout layout{ GetComputeRootSignature(backendInstance).GetHandle() };
	rhi::SubobjShader shader{ rhi::ShaderStage::Compute, rhi::DXIL(compiled.computeShader.Get()), ws2s(recipe.entryPoint) };
	const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
	rhi::PipelinePtr pso;
	auto device = backendInstance == BackendInstanceId::Primary
		? DeviceManager::GetInstance().GetDevice() : DeviceManager::GetInstance().GetPeerDevice();
	const auto result = device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), pso);
	if (Failed(result)) throw std::runtime_error("Failed to create backend-local compute PSO");
	if (!recipe.debugName.empty()) {
		const std::string name = recipe.debugName + ".backend" + std::to_string(static_cast<uint8_t>(backendInstance));
		pso->SetName(name.c_str());
	}
	pipeline.AttachBackendPipeline(backendInstance, std::move(pso), compiled.resourceIDsHash,
		compiled.resourceDescriptorSlots);
	spdlog::info("PSOManager materialized compute pipeline '{}' for backend instance {}",
		recipe.debugName, static_cast<uint8_t>(backendInstance));
}

const rhi::Pipeline& PSOManager::ResolvePipeline(const PipelineState& pipeline, BackendInstanceId backendInstance) {
	if (backendInstance == BackendInstanceId::Primary) return pipeline.GetAPIPipelineState();
	if (!pipeline.HasBackendPipeline(backendInstance)) {
		std::scoped_lock lock(m_livePipelineMutex);
		for (auto& [id, entry] : m_livePipelines) {
			if (entry.state.GetSlot() == pipeline.GetSlot() && !entry.computeRecipe.shaderPath.empty()) {
				BuildComputePipelineForBackend(entry.computeRecipe, pipeline, backendInstance);
				break;
			}
		}
	}
	return pipeline.GetAPIPipelineState(backendInstance);
}

PipelineState PSOManager::RegisterComputePipeline(PipelineState state, ComputeRecipe recipe)
{
    std::scoped_lock lock(m_livePipelineMutex);
    std::string id = recipe.debugName.empty() ? ws2s(recipe.entryPoint) : recipe.debugName;
    if (const auto existing = m_livePipelines.find(id); existing != m_livePipelines.end()) {
        const ComputeRecipe& registered = existing->second.computeRecipe;
        const bool sameRecipe =
            registered.layout.index == recipe.layout.index &&
            registered.layout.generation == recipe.layout.generation &&
            registered.shaderPath == recipe.shaderPath &&
            registered.entryPoint == recipe.entryPoint &&
            registered.defines.size() == recipe.defines.size() &&
            std::ranges::equal(
                registered.defines,
                recipe.defines,
                {},
                [](const OwnedDefine& define) {
                    return std::pair{ define.name, define.value };
                },
                [](const OwnedDefine& define) {
                    return std::pair{ define.name, define.value };
                });
        if (sameRecipe) {
            return existing->second.state;
        }
        const std::string baseId = id;
        uint32_t suffix = 2;
        while (m_livePipelines.contains(id)) {
            id = baseId + "#" + std::to_string(suffix++);
        }
    }

    auto payload = state.GetPayload();
    LivePipelineEntry entry;
    entry.id = id;
    entry.displayName = recipe.debugName;
    entry.kind = LivePipelineKind::Compute;
    entry.state = state;
    entry.shaderPaths.push_back(ws2s(recipe.shaderPath));
    entry.supportsDefineOverrides = true;
    entry.computeRecipe = std::move(recipe);
    entry.generations.push_back(std::move(payload));
    m_livePipelines.emplace(id, std::move(entry));
    return state;
}

PipelineState PSOManager::RegisterPipeline(
    PipelineState state,
    std::string id,
    std::string displayName,
    LivePipelineKind kind,
    std::function<PipelineState()> rebuild)
{
    std::scoped_lock lock(m_livePipelineMutex);
    const auto slot = state.GetSlot();
    for (const auto& [_, existing] : m_livePipelines) {
        if (existing.state.GetSlot() == slot) {
            return state;
        }
    }
    const std::string baseId = id;
    uint32_t suffix = 2;
    while (m_livePipelines.contains(id)) {
        id = baseId + "#" + std::to_string(suffix++);
    }
    LivePipelineEntry entry;
    entry.id = id;
    entry.displayName = std::move(displayName);
    entry.kind = kind;
    entry.state = state;
    entry.rebuild = std::move(rebuild);
    entry.generations.push_back(state.GetPayload());
    m_livePipelines.emplace(id, std::move(entry));
    return state;
}

PipelineState PSOManager::RegisterExternalPipeline(
    PipelineState state,
    std::string id,
    std::string displayName,
    LivePipelineKind kind,
    std::function<PipelineState()> rebuild)
{
    return RegisterPipeline(
        std::move(state),
        std::move(id),
        std::move(displayName),
        kind,
        std::move(rebuild));
}

std::vector<DxcDefine> PSOManager::GetRasterShaderDefines(MaterialRasterFlags rasterFlags) {
    std::vector<DxcDefine> defines = {};
    defines.push_back({ L"CLOD_ENABLE_SOURCE_GROUP_VALIDATION", L"0" });
    static constexpr const wchar_t* uvCountValues[] = { L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8" };
    defines.push_back({ L"CLOD_FORWARD_UV_SET_COUNT", uvCountValues[GetForwardUvSetCount(rasterFlags)] });
    defines.push_back({ L"CLOD_FORWARD_VERTEX_COLOR", HasForwardVertexColor(rasterFlags) ? L"1" : L"0" });
    defines.push_back({ L"CLOD_FORWARD_GLINT", HasForwardGlint(rasterFlags) ? L"1" : L"0" });
    defines.push_back({ L"CLOD_FORWARD_COAT", HasForwardCoat(rasterFlags) ? L"1" : L"0" });
    defines.push_back({ L"CLOD_FORWARD_FUZZ", HasForwardFuzz(rasterFlags) ? L"1" : L"0" });
    defines.push_back({ L"CLOD_FORWARD_METAL", HasForwardMetal(rasterFlags) ? L"1" : L"0" });
    defines.push_back({ L"CLOD_FORWARD_DIFFUSE_ROUGHNESS", HasForwardDiffuseRoughness(rasterFlags) ? L"1" : L"0" });
    defines.push_back({ L"CLOD_FORWARD_EMISSION", HasForwardEmission(rasterFlags) ? L"1" : L"0" });
    if (rasterFlags & MaterialRasterFlags::MaterialRasterFlagsAlphaTest) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_ALPHA_TEST";
        defines.insert(defines.begin(), macro);
	}
    if (rasterFlags & MaterialRasterFlags::MaterialRasterFlagsDoubleSided) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_DOUBLE_SIDED";
		defines.insert(defines.begin(), macro);
    }
    if (rasterFlags & MaterialRasterFlags::MaterialRasterFlagsGeometricDisplacement) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_GEOMETRIC_DISPLACEMENT";
        defines.insert(defines.begin(), macro);
    }
    if (rasterFlags & MaterialRasterFlags::MaterialRasterFlagsSkinned) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_SKINNED";
        defines.insert(defines.begin(), macro);
    }
    return defines;
}

std::vector<DxcDefine> PSOManager::GetShaderDefines(UINT psoFlags, MaterialCompileFlags materialFlags) {
    std::vector<DxcDefine> defines = {};
    if (materialFlags & MaterialCompileFlags::MaterialCompileDoubleSided) {
		DxcDefine macro;
		macro.Value = L"1";
		macro.Name = L"PSO_DOUBLE_SIDED";
		defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileAlphaTest) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_ALPHA_TEST";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileBlend) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_BLEND";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileBaseColorTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_BASE_COLOR_TEXTURE";
		defines.insert(defines.begin(), macro);
	}
    if (materialFlags & MaterialCompileFlags::MaterialCompileParallax) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_PARALLAX";
        defines.insert(defines.begin(), macro);
	}
    if (materialFlags & MaterialCompileFlags::MaterialCompileHeightFromBaseAlpha) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_HEIGHT_FROM_BASE_ALPHA";
        defines.insert(defines.begin(), macro);
	}
    if (materialFlags & MaterialCompileFlags::MaterialCompileGeometricDisplacement) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_GEOMETRIC_DISPLACEMENT";
        defines.insert(defines.begin(), macro);
	}
    if (materialFlags & MaterialCompileFlags::MaterialCompileNormalMap) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_NORMAL_MAP";
        defines.insert(defines.begin(), macro);
	}
	if (materialFlags & MaterialCompileFlags::MaterialCompileEmissiveTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_EMISSIVE_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileOpacityTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_OPACITY_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileMetallicTexture) {
        DxcDefine macro;
        macro.Value = L"1";
		macro.Name = L"PSO_METALLIC_TEXTURE";
		defines.insert(defines.begin(), macro);
	}
    if (materialFlags & MaterialCompileFlags::MaterialCompileRoughnessTexture) {
		DxcDefine macro;
		macro.Value = L"1";
		macro.Name = L"PSO_ROUGHNESS_TEXTURE";
        defines.insert(defines.begin(), macro);
	}
    if (materialFlags & MaterialCompileFlags::MaterialCompileAOTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_AO_TEXTURE";
        defines.insert(defines.begin(), macro);
	}
    if (materialFlags & MaterialCompileFlags::MaterialCompileOpenPBRCoatColorTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_OPENPBR_COAT_COLOR_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileOpenPBRCoatWeightTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_OPENPBR_COAT_WEIGHT_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileOpenPBRCoatRoughnessTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_OPENPBR_COAT_ROUGHNESS_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileOpenPBRFuzzColorTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_OPENPBR_FUZZ_COLOR_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileOpenPBRFuzzWeightTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_OPENPBR_FUZZ_WEIGHT_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileOpenPBRFuzzRoughnessTexture) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_OPENPBR_FUZZ_ROUGHNESS_TEXTURE";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileMaterialEvalColorOnly) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"VISUTIL_COLOR_ONLY_GBUFFER_EVAL";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileTextureStreaming) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_TEXTURE_STREAMING";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileVoxel) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_CLOD_VOXEL";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileTerrain) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_TERRAIN";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileClodReyesPatch) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_CLOD_REYES_PATCH";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileClodVertexColor) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_CLOD_VERTEX_COLOR";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileClodSkinning) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_CLOD_SKINNING";
        defines.insert(defines.begin(), macro);
    }
    if (materialFlags & MaterialCompileFlags::MaterialCompileTerrainRvtTelemetry) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"TERRAIN_RVT_TELEMETRY";
        defines.insert(defines.begin(), macro);
    }

    if (psoFlags & PSOFlags::PSO_SHADOW) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_SHADOW";
        defines.insert(defines.begin(), macro);
    }
    if (psoFlags & PSOFlags::PSO_IMAGE_BASED_LIGHTING) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_IMAGE_BASED_LIGHTING";
        defines.insert(defines.begin(), macro);
    }
	if (psoFlags & PSOFlags::PSO_CLUSTERED_LIGHTING) {
		DxcDefine macro;
		macro.Value = L"1";
		macro.Name = L"PSO_CLUSTERED_LIGHTING";
		defines.insert(defines.begin(), macro);
	}
	if (psoFlags & PSOFlags::PSO_PREPASS) {
		DxcDefine macro;
		macro.Value = L"1";
		macro.Name = L"PSO_PREPASS";
		defines.insert(defines.begin(), macro);
	}
    if (psoFlags & PSOFlags::PSO_DEFERRED) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_DEFERRED";
        defines.insert(defines.begin(), macro);
	}
    if (!(psoFlags & PSOFlags::PSO_SCREENSPACE_REFLECTIONS)) {
        DxcDefine macro;
        macro.Value = L"1";
        macro.Name = L"PSO_SPECULAR_IBL";
        defines.insert(defines.begin(), macro);
	}

    return defines;
}

void PSOManager::GetPreprocessedBlob(
    const std::wstring& filename,
    const std::wstring& entryPoint,
    const std::wstring& target,
    std::vector<DxcDefine> defines,
    Microsoft::WRL::ComPtr<ID3DBlob>& outBlob,
    bool emitSpirv) {

    auto exePath = std::filesystem::path(GetExePath());
    auto fullPath = exePath / filename;
    auto shaderDir = exePath / L"shaders";

    PSOManager::SourceData srcBuf;
    LoadSource(fullPath, srcBuf);
    auto includeHandler = CreateIncludeHandler();

    ShaderCompileOptions opts;
    opts.entryPoint = entryPoint;
    opts.target = target;
    opts.defines = std::move(defines);
    opts.emitSpirv = emitSpirv;
#if WRITE_DEBUG_FILES
    opts.enableDebugInfo = true;
#endif
    opts.warningsAsErrors = true;

    std::vector<std::wstring> ownedArgs;
    auto args = BuildArguments(opts, shaderDir, ownedArgs);

    args.push_back(L"-P"); // Preprocess only
    auto preProcessedResult = InvokeCompile(
        srcBuf.buffer, args, includeHandler.Get(), filename, entryPoint, target
    );

    preProcessedResult->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(&outBlob), nullptr);
}

void pruneUnusedCodeForSlot(
	std::string& inOutSource,
    const std::optional<ShaderInfo>& slot)
{
    if (!slot)
        return;
    // Prune unused code
    std::string prunedSource = pruneUnusedCode(
        inOutSource.c_str(),
        inOutSource.length(),
        ws2s(slot->entryPoint),
        {}
    );
	inOutSource = std::move(prunedSource);
}

void parseBRSLResourceIdentifiersForSlot(
    const std::optional<ShaderInfo>& slot,
    const std::vector<DxcDefine>& defines,
	const DxcBuffer* preprocessedBuffer,
    std::unordered_set<std::string>& outMandatoryIDs,
    std::unordered_set<std::string>& outOptionalIDs)
{
    if (!slot)
        return;
    
    ParseBRSLResourceIdentifiers(outMandatoryIDs, outOptionalIDs, preprocessedBuffer, ws2s(slot->entryPoint));
}

std::string rewriteResourceDescriptorIndexCallsForSlot(
    const std::optional<ShaderInfo>& slot,
    const std::vector<DxcDefine>& defines,
    Microsoft::WRL::ComPtr<ID3DBlob>& outBlob,
    DxcBuffer& outBuf,
    const std::unordered_map<std::string, std::string>& replacementMap)
{
    if (!slot)
        return "";
    return rewriteResourceDescriptorCalls(
        static_cast<const char*>(outBuf.Ptr),
        outBuf.Size,
        ws2s(slot->entryPoint),
        replacementMap
	);
}

void PSOManager::CompileShaderForSlot(
    const std::optional<ShaderInfo>& slot,
    const std::vector<DxcDefine>& defines,
    const DxcBuffer& buffer,
    bool emitSpirv,
    Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
{
    if (!slot)
        return;
    //DxcBuffer ppBuffer = {};
    //Microsoft::WRL::ComPtr<ID3DBlob> preprocessedBlob;
    //PreprocessShaderSlot(slot, defines, preprocessedBlob, ppBuffer);
    CompileShader(
        slot->filename,
        slot->entryPoint,
        slot->target,
        buffer,
        defines,
        emitSpirv,
        outBlob
    );
}

ShaderLibraryBundle PSOManager::CompileShaderLibrary(const ShaderLibraryInfo& libraryInfo, const std::vector<DxcDefine>& defines) {
	const shadercache::BinaryFormat binaryFormat = GetShaderBinaryFormat(DeviceManager::GetInstance().GetBackend());
	const bool emitSpirv = IsSpirvFormat(binaryFormat);
    Microsoft::WRL::ComPtr<ID3DBlob> outBlob;
    DxcBuffer dxcPreprocessBuff;

	// Preprocess
    GetPreprocessedBlob(
        libraryInfo.filename,
        L"",
        libraryInfo.target,
        defines,
		outBlob,
		emitSpirv
	);

    dxcPreprocessBuff.Ptr = outBlob->GetBufferPointer();
    dxcPreprocessBuff.Size = outBlob->GetBufferSize();
    dxcPreprocessBuff.Encoding = 0;

    std::string debug_shader_string((const char*)dxcPreprocessBuff.Ptr, dxcPreprocessBuff.Size);

    const uint64_t buildConfigHash = ComputeShaderCacheBuildConfigHash(binaryFormat);
    const shadercache::CacheKey cacheKey{
        .binaryFormat = binaryFormat,
        .artifactKind = shadercache::ArtifactKind::Library,
        .identityHash = BuildLibraryIdentityHash(libraryInfo, dxcPreprocessBuff),
    };
    const ShaderCompileFlightKey flightKey{
        .binaryFormat = cacheKey.binaryFormat,
        .artifactKind = cacheKey.artifactKind,
        .identityHash = cacheKey.identityHash,
        .buildConfigHash = buildConfigHash,
    };
    if (g_bypassShaderArtifactCacheReads) {
        spdlog::info(
            "Shader live reload: bypassing library artifact cache identity=0x{:X}",
            cacheKey.identityHash);
    }
    for (;;) {
        if (!g_bypassShaderArtifactCacheReads) {
            if (std::optional<ShaderLibraryBundle> cachedBundle = TryLoadShaderLibraryFromCache(cacheKey, buildConfigHash, pUtils.Get()); cachedBundle.has_value()) {
                return *cachedBundle;
            }
        }
        if (GetShaderCompileFlightRegistry().TryBecomeOwnerOrWait(flightKey)) {
            break;
        }
    }
    ShaderCompileFlightScope flightScope(flightKey);

    // Compile BRSL info
    PreprocessedLibraryResult libPP = PreprocessShaderLibrary(dxcPreprocessBuff);
    if (libPP.diagnostics.usedFallbackRewrite) {
        spdlog::warn(
            "Shader preprocess fallback used for library {}: {}",
            ws2s(libraryInfo.filename),
            FormatShaderPreprocessDiagnostics(libPP.diagnostics));
    }

    const size_t descriptorSlotCount = libPP.mandatoryIDs.size() + libPP.optionalIDs.size();
    if (descriptorSlotCount > org::shaderapi::kNumResourceDescriptorIndicesRootConstants) {
        throw std::runtime_error(
            "Shader library preprocessing requires " + std::to_string(descriptorSlotCount) +
            " descriptor index root constants, but only " +
            std::to_string(org::shaderapi::kNumResourceDescriptorIndicesRootConstants) +
            " are available.");
    }

    DxcBuffer finalBuf = dxcPreprocessBuff;
    finalBuf.Ptr = libPP.finalSource.data();
    finalBuf.Size = libPP.finalSource.size();

    // compile as a library target (lib_6_8) without entry point
    CompileShader(libraryInfo.filename, /*entryPoint*/ L"", libraryInfo.target, finalBuf, defines, emitSpirv, outBlob);

	std::vector<ResourceIdentifier> mandatoryResourceDescriptors;
    for (const auto& idStr : libPP.mandatoryIDs) {
		mandatoryResourceDescriptors.push_back(ResolveRuntimeResourceIdentifier(idStr));
    }
	std::vector<ResourceIdentifier> optionalResourceDescriptors;
    for (const auto& idStr : libPP.optionalIDs) {
        optionalResourceDescriptors.push_back(ResolveRuntimeResourceIdentifier(idStr));
	}

    ShaderLibraryBundle bundle;
	bundle.libraryBlob = outBlob;
    bundle.resourceDescriptorSlots = {mandatoryResourceDescriptors, optionalResourceDescriptors};
	bundle.resourceIDsHash = libPP.resourceIDsHash;

    const shadercache::CacheData cacheData = BuildLibraryCacheData(libraryInfo, bundle, buildConfigHash, binaryFormat);
    shadercache::Save(cacheKey, cacheData);
	return bundle;

}

ShaderBundle PSOManager::CompileShaders(const ShaderInfoBundle& info) {
	return CompileShadersForBackend(info, DeviceManager::GetInstance().GetBackend());
}

ShaderBundle PSOManager::CompileShadersForBackend(const ShaderInfoBundle& info, rhi::Backend backend) {
	if (backend == rhi::Backend::Null)
		throw std::runtime_error("Shader compilation requested for an unavailable backend");
	const shadercache::BinaryFormat binaryFormat = GetShaderBinaryFormat(backend);
	const bool emitSpirv = IsSpirvFormat(binaryFormat);
    if (info.vertexShader && info.meshShader) 
		throw std::runtime_error("Cannot compile both vertex and mesh shaders in the same bundle");
	if (info.computeShader && (info.meshShader || info.amplificationShader || info.vertexShader || info.pixelShader))
		throw std::runtime_error("Cannot compile compute shader with other shader types in the same bundle");

	Microsoft::WRL::ComPtr<ID3DBlob> preprocessedAmplificationShader;
	DxcBuffer amplificationBuffer = {};
	Microsoft::WRL::ComPtr<ID3DBlob> preprocessedMeshShader;
	DxcBuffer meshBuffer = {};
	Microsoft::WRL::ComPtr<ID3DBlob> preprocessedPixelShader;
	DxcBuffer pixelBuffer = {};
	Microsoft::WRL::ComPtr<ID3DBlob> preprocessedVertexShader;
	DxcBuffer vertexBuffer = {};
	Microsoft::WRL::ComPtr<ID3DBlob> preprocessedComputeShader;
	DxcBuffer computeBuffer = {};

    PreprocessShaderSlot(info.amplificationShader, info.defines, preprocessedAmplificationShader, amplificationBuffer, emitSpirv);
    PreprocessShaderSlot(info.meshShader, info.defines, preprocessedMeshShader, meshBuffer, emitSpirv);
    PreprocessShaderSlot(info.pixelShader, info.defines, preprocessedPixelShader, pixelBuffer, emitSpirv);
    PreprocessShaderSlot(info.vertexShader, info.defines, preprocessedVertexShader, vertexBuffer, emitSpirv);
    PreprocessShaderSlot(info.computeShader, info.defines, preprocessedComputeShader, computeBuffer, emitSpirv);

    const uint64_t buildConfigHash = ComputeShaderCacheBuildConfigHash(binaryFormat);
    const shadercache::CacheKey cacheKey{
        .binaryFormat = binaryFormat,
        .artifactKind = shadercache::ArtifactKind::Bundle,
        .identityHash = BuildBundleIdentityHash(
            info,
            amplificationBuffer,
            meshBuffer,
            pixelBuffer,
            vertexBuffer,
            computeBuffer),
    };
    const bool logMaterialEvalCache =
        info.computeShader.has_value()
        && info.computeShader->filename == L"shaders/VisUtilEvaluate.hlsl";
    const ShaderCompileFlightKey flightKey{
        .binaryFormat = cacheKey.binaryFormat,
        .artifactKind = cacheKey.artifactKind,
        .identityHash = cacheKey.identityHash,
        .buildConfigHash = buildConfigHash,
    };
    if (g_bypassShaderArtifactCacheReads) {
        spdlog::info(
            "Shader live reload: bypassing bundle artifact cache identity=0x{:X}",
            cacheKey.identityHash);
    }
    for (;;) {
        if (!g_bypassShaderArtifactCacheReads) {
            if (std::optional<ShaderBundle> cachedBundle = TryLoadShaderBundleFromCache(cacheKey, buildConfigHash, pUtils.Get()); cachedBundle.has_value()) {
            return *cachedBundle;
            }
        }
        if (GetShaderCompileFlightRegistry().TryBecomeOwnerOrWait(flightKey)) {
            break;
        }
    }
    ShaderCompileFlightScope flightScope(flightKey);
    if (logMaterialEvalCache) {
        spdlog::debug("VisUtil material eval shader artifact cache miss; compiling identity=0x{:X}", cacheKey.identityHash);
    }

    auto prepareSlot = [&](const std::optional<ShaderInfo>& slot, const DxcBuffer& buffer)
        -> std::optional<PreparedShaderSource>
        {
            if (!slot) {
                return std::nullopt;
            }

            return PrepareShaderSourceForEntryPoint(buffer, ws2s(slot->entryPoint));
        };

    std::optional<PreparedShaderSource> preparedAmplification = prepareSlot(info.amplificationShader, amplificationBuffer);
    std::optional<PreparedShaderSource> preparedMesh = prepareSlot(info.meshShader, meshBuffer);
    std::optional<PreparedShaderSource> preparedPixel = prepareSlot(info.pixelShader, pixelBuffer);
    std::optional<PreparedShaderSource> preparedVertex = prepareSlot(info.vertexShader, vertexBuffer);
    std::optional<PreparedShaderSource> preparedCompute = prepareSlot(info.computeShader, computeBuffer);

    std::unordered_set<std::string> usedMandatoryIDs;
    std::unordered_set<std::string> usedOptionalIDs;
    auto collectPreparedIDs = [&](const std::optional<PreparedShaderSource>& prepared) {
        if (!prepared.has_value()) {
            return;
        }

        usedMandatoryIDs.insert(prepared->mandatoryIDs.begin(), prepared->mandatoryIDs.end());
        usedOptionalIDs.insert(prepared->optionalIDs.begin(), prepared->optionalIDs.end());
        };

    collectPreparedIDs(preparedAmplification);
    collectPreparedIDs(preparedMesh);
    collectPreparedIDs(preparedPixel);
    collectPreparedIDs(preparedVertex);
    collectPreparedIDs(preparedCompute);

    // A resource referenced through both mandatory and optional code paths is
    // mandatory for the combined pipeline. Keep one canonical root-constant
    // slot; otherwise the optional replacement overwrites the shader macro
    // while both entries remain in the CPU binding array.
    for (const auto& mandatoryID : usedMandatoryIDs) {
        usedOptionalIDs.erase(mandatoryID);
    }

	std::unordered_map<std::string, std::string> replacementMap;
	uint32_t nextIndex = 0;
    ShaderBundle bundle = {};
    std::vector<std::string> usedMandatoryResourceIDsVec(usedMandatoryIDs.begin(), usedMandatoryIDs.end());
    std::vector<std::string> usedOptionalResourceIDsVec(usedOptionalIDs.begin(), usedOptionalIDs.end());
    std::sort(usedMandatoryResourceIDsVec.begin(), usedMandatoryResourceIDsVec.end());
    std::sort(usedOptionalResourceIDsVec.begin(), usedOptionalResourceIDsVec.end());

    const size_t descriptorSlotCount = usedMandatoryResourceIDsVec.size() + usedOptionalResourceIDsVec.size();
    if (descriptorSlotCount > org::shaderapi::kNumResourceDescriptorIndicesRootConstants) {
        throw std::runtime_error(
            "Shader preprocessing requires " + std::to_string(descriptorSlotCount) +
            " descriptor index root constants, but only " +
            std::to_string(org::shaderapi::kNumResourceDescriptorIndicesRootConstants) +
            " are available.");
    }

    for (const std::string& entry : usedMandatoryResourceIDsVec) {
		bundle.resourceDescriptorSlots.mandatoryResourceDescriptorSlots.push_back(ResolveRuntimeResourceIdentifier(entry));
		replacementMap[entry] = "ResourceDescriptorIndex" + std::to_string(nextIndex++);
    }

    for (const std::string& entry : usedOptionalResourceIDsVec) {
        bundle.resourceDescriptorSlots.optionalResourceDescriptorSlots.push_back(ResolveRuntimeResourceIdentifier(entry));
        replacementMap[entry] = "ResourceDescriptorIndex" + std::to_string(nextIndex++);
    }

    auto finalizePreparedSource = [&](
        const std::optional<ShaderInfo>& slot,
        std::optional<PreparedShaderSource>& prepared) -> std::string {
        if (!slot || !prepared.has_value()) {
            return {};
        }

        std::string finalSource = FinalizePreparedShaderSource(*prepared, replacementMap);
        if (prepared->diagnostics.usedFallbackRewrite) {
            spdlog::warn(
                "Shader preprocess fallback used for {} ({}): {}",
                ws2s(slot->entryPoint),
                ws2s(slot->filename),
                FormatShaderPreprocessDiagnostics(prepared->diagnostics));
        }
        return finalSource;
        };

	auto newAmplification = finalizePreparedSource(info.amplificationShader, preparedAmplification);
    auto newMesh = finalizePreparedSource(info.meshShader, preparedMesh);
    auto newPixel = finalizePreparedSource(info.pixelShader, preparedPixel);
    auto newVertex = finalizePreparedSource(info.vertexShader, preparedVertex);
	auto newCompute = finalizePreparedSource(info.computeShader, preparedCompute);

    if (!newAmplification.empty()) {
        amplificationBuffer.Ptr = newAmplification.data();
        amplificationBuffer.Size = newAmplification.size();
	}
    if (!newMesh.empty()) {
        meshBuffer.Ptr = newMesh.data();
        meshBuffer.Size = newMesh.size();
    }
    if (!newPixel.empty()) {
        pixelBuffer.Ptr = newPixel.data();
        pixelBuffer.Size = newPixel.size();
    }
    if (!newVertex.empty()) {
        vertexBuffer.Ptr = newVertex.data();
        vertexBuffer.Size = newVertex.size();
	}
    if (!newCompute.empty()) {
        computeBuffer.Ptr = newCompute.data();
        computeBuffer.Size = newCompute.size();
	}

	CompileShaderForSlot(info.amplificationShader, info.defines, amplificationBuffer, emitSpirv, bundle.amplificationShader);
	CompileShaderForSlot(info.meshShader, info.defines, meshBuffer, emitSpirv, bundle.meshShader);
	CompileShaderForSlot(info.pixelShader, info.defines, pixelBuffer, emitSpirv, bundle.pixelShader);
	CompileShaderForSlot(info.vertexShader, info.defines, vertexBuffer, emitSpirv, bundle.vertexShader);
    CompileShaderForSlot(info.computeShader, info.defines, computeBuffer, emitSpirv, bundle.computeShader);

    std::vector<std::string> combinedIds = {};
	combinedIds.insert(combinedIds.end(), usedMandatoryResourceIDsVec.begin(), usedMandatoryResourceIDsVec.end());
	combinedIds.insert(combinedIds.end(), usedOptionalResourceIDsVec.begin(), usedOptionalResourceIDsVec.end());

	bundle.resourceIDsHash = hash_list(combinedIds);

    const shadercache::CacheData cacheData = BuildBundleCacheData(info, bundle, buildConfigHash, binaryFormat);
    shadercache::Save(cacheKey, cacheData);

	return bundle;
}

ShaderBundle PSOManager::CompileShaders(const ShaderInfoBundle& info, BackendInstanceId backendInstance) {
	const auto& devices = DeviceManager::GetInstance();
	const rhi::Backend backend = backendInstance == BackendInstanceId::Primary
		? devices.GetBackend() : devices.GetPeerBackend();
	if (backend == rhi::Backend::Null) {
		throw std::runtime_error("Shader compilation requested for an unavailable backend instance");
	}
	return CompileShadersForBackend(info, backend);
}

// for string delimiter
std::vector<std::string> split(std::string s, std::string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back(token);
    }

    res.push_back(s.substr(pos_start));
    return res;
}
void PSOManager::CompileShader(
    const std::wstring& filename,
    const std::wstring& entryPoint, 
    const std::wstring& target, 
	const DxcBuffer& ppBuffer,
    std::vector<DxcDefine> defines,
    bool emitSpirv,
    Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
{
    auto exePath = std::filesystem::path(GetExePath());
    auto fullPath = exePath / filename;
    auto shaderDir = exePath / L"shaders";

    ShaderCompileOptions opts;
    opts.entryPoint = entryPoint;
    opts.target = target;
    opts.defines = std::move(defines);
    opts.emitSpirv = emitSpirv;
#if WRITE_DEBUG_FILES
    opts.enableDebugInfo = true;
#endif
    opts.warningsAsErrors = true;

    std::vector<std::wstring> ownedArgs;
    auto args = BuildArguments(opts, shaderDir, ownedArgs);

    ComPtr<IDxcIncludeHandler> includeHandler;
    HRESULT hr = pUtils->CreateDefaultIncludeHandler(&includeHandler);
    if (FAILED(hr)) {
        spdlog::error("Failed to create include handler.");
        ThrowIfFailed(hr);
        return;
    }
    auto result = InvokeCompile(ppBuffer, args, includeHandler.Get(), filename, entryPoint, target);

    auto obj = ExtractObject(result.Get(), filename, opts.enableDebugInfo);
    ThrowIfFailed(obj.As(&outBlob));
}

void PSOManager::LoadSource(const std::filesystem::path& path, PSOManager::SourceData& sd) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Shader source file not found: " + path.string());
    }
        bool emitSpirv = false;
    UINT32 codePage = CP_UTF8;
    ThrowIfFailed(pUtils->LoadFile(
        path.c_str(), &codePage, &sd.blob
    ));

    ValidateUtf8OrThrow(
        std::string_view(
            static_cast<const char*>(sd.blob->GetBufferPointer()),
            sd.blob->GetBufferSize()),
        "Shader source file");

    sd.buffer.Ptr = sd.blob->GetBufferPointer();
    sd.buffer.Size = sd.blob->GetBufferSize();
    sd.buffer.Encoding = 0; // see below
}


ComPtr<IDxcIncludeHandler> PSOManager::CreateIncludeHandler()
{
    ComPtr<IDxcIncludeHandler> handler;
    ThrowIfFailed(pUtils->CreateDefaultIncludeHandler(&handler));
    return handler;
}

std::vector<LPCWSTR> PSOManager::BuildArguments(
    const ShaderCompileOptions& opts,
    const std::filesystem::path& shaderDir,
    std::vector<std::wstring>& ownedArgs)
{
    std::vector<LPCWSTR> args;
    ownedArgs.reserve(opts.defines.size() + 8u);

	if (opts.entryPoint != L"") { // SM 6.8 libraries don't have entry points
        args.push_back(L"-E"); args.push_back(opts.entryPoint.c_str());
    }
    args.push_back(L"-T"); args.push_back(opts.target.c_str());

    args.push_back(L"-D");
    args.push_back(opts.emitSpirv ? L"BASICRENDERER_SHADER_API_VULKAN=1" : L"BASICRENDERER_SHADER_API_DX12=1");

    if (opts.emitSpirv) {
        args.push_back(L"-spirv");
        args.push_back(L"-fvk-use-dx-layout");
        args.push_back(L"-fspv-target-env=vulkan1.3");
        args.push_back(L"-fvk-bind-resource-heap");
        ownedArgs.push_back(std::to_wstring(rhi::VULKAN_RESOURCE_DESCRIPTOR_HEAP_BINDING));
        args.push_back(ownedArgs.back().c_str());
        ownedArgs.push_back(std::to_wstring(rhi::VULKAN_DESCRIPTOR_HEAP_SET));
        args.push_back(ownedArgs.back().c_str());
        args.push_back(L"-fvk-bind-sampler-heap");
        ownedArgs.push_back(std::to_wstring(rhi::VULKAN_SAMPLER_DESCRIPTOR_HEAP_BINDING));
        args.push_back(ownedArgs.back().c_str());
        ownedArgs.push_back(std::to_wstring(rhi::VULKAN_DESCRIPTOR_HEAP_SET));
        args.push_back(ownedArgs.back().c_str());
        args.push_back(L"-fvk-bind-counter-heap");
        ownedArgs.push_back(std::to_wstring(rhi::VULKAN_COUNTER_DESCRIPTOR_HEAP_BINDING));
        args.push_back(ownedArgs.back().c_str());
        ownedArgs.push_back(std::to_wstring(rhi::VULKAN_DESCRIPTOR_HEAP_SET));
        args.push_back(ownedArgs.back().c_str());
    }

    if (opts.warningsAsErrors)
        args.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);

    if (opts.enableDebugInfo) {
        args.push_back(DXC_ARG_DEBUG);
        args.push_back(DXC_ARG_DEBUG_NAME_FOR_SOURCE);
        // Keep source associations in the shader container so GPU-Reshape can resolve
        // filenames/lines without relying solely on external PDB recovery.
        args.push_back(L"-Qembed_debug");
        args.push_back(L"-Qsource_in_debug_module");
        //args.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
    }

    for (auto& def : opts.defines) { 
        args.push_back(L"-D");
        if (def.Value && def.Value[0] != L'\0') {
            ownedArgs.emplace_back(def.Name);
            ownedArgs.back().push_back(L'=');
            ownedArgs.back().append(def.Value);
            args.push_back(ownedArgs.back().c_str());
        }
        else {
            args.push_back(def.Name);
        }
    }

    // always include shaders folder
    args.push_back(L"-I");
    args.push_back(shaderDir.c_str());
    // DXC's default include handler does not reliably preserve the including
    // file's directory for nested includes through an MO2 virtual path.
    // Make the shared include directory explicit so cbuffers.hlsli can resolve
    // siblings such as structs.hlsli.
    ownedArgs.push_back((shaderDir / L"include").wstring());
    args.push_back(L"-I");
    args.push_back(ownedArgs.back().c_str());

    return args;
}

ComPtr<IDxcResult> PSOManager::InvokeCompile(
    const DxcBuffer& src,
    std::vector<LPCWSTR>& arguments,
    IDxcIncludeHandler* includeHandler,
    const std::wstring& filename,
    const std::wstring& entryPoint,
    const std::wstring& target)
{
    ComPtr<IDxcResult> result;
    const HRESULT compileHr = pCompiler->Compile(
        &src,
        arguments.data(),
        (UINT)arguments.size(),
        includeHandler,
        IID_PPV_ARGS(result.GetAddressOf())
    );

    auto formatHResult = [](HRESULT value) {
        std::ostringstream stream;
        stream << "0x"
            << std::uppercase
            << std::hex
            << std::setw(8)
            << std::setfill('0')
            << static_cast<uint32_t>(value);
        return stream.str();
        };

    auto formatArguments = [&]() {
        std::ostringstream stream;
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (i != 0) {
                stream << ' ';
            }

            const std::string argument = arguments[i] ? ws2s(arguments[i]) : "<null>";
            stream << std::quoted(argument);
        }
        return stream.str();
        };

    HRESULT statusQueryHr = E_UNEXPECTED;
    HRESULT compileStatus = E_UNEXPECTED;
    HRESULT diagnosticsHr = S_FALSE;
    bool diagnosticsPresent = false;
    std::string diagnostics;

    if (result) {
        statusQueryHr = result->GetStatus(&compileStatus);

        // GetOutput returns E_INVALIDARG when the requested output kind is absent.
        // Check first so successful, diagnostic-free compiles do not generate a
        // misleading failed HRESULT in an attached debugger.
        diagnosticsPresent = result->HasOutput(DXC_OUT_ERRORS) != FALSE;
        if (diagnosticsPresent) {
            ComPtr<IDxcBlobUtf8> diagnosticsBlob;
            diagnosticsHr = result->GetOutput(
                DXC_OUT_ERRORS,
                IID_PPV_ARGS(diagnosticsBlob.GetAddressOf()),
                nullptr);
            if (SUCCEEDED(diagnosticsHr) && diagnosticsBlob && diagnosticsBlob->GetStringLength() != 0) {
                diagnostics.assign(
                    diagnosticsBlob->GetStringPointer(),
                    diagnosticsBlob->GetStringLength());
            }
        }
    }

    const bool compileFailed =
        FAILED(compileHr)
        || !result
        || FAILED(statusQueryHr)
        || FAILED(compileStatus);
    if (compileFailed) {
        std::ostringstream message;
        message
            << "Shader compilation failed for file='" << ws2s(filename)
            << "', entry='" << ws2s(entryPoint)
            << "', target='" << ws2s(target) << "'. "
            << "IDxcCompiler3::Compile=" << formatHResult(compileHr) << "; ";

        if (result) {
            message
                << "IDxcResult::GetStatus=" << formatHResult(statusQueryHr)
                << "; compile status=" << formatHResult(compileStatus) << "; ";

            if (diagnosticsPresent) {
                message << "DXC_OUT_ERRORS GetOutput=" << formatHResult(diagnosticsHr) << "; ";
            }
            else {
                message << "DXC_OUT_ERRORS=not present; ";
            }
        }
        else {
            message << "IDxcResult=<null>; ";
        }

        message << "arguments=" << formatArguments();
        if (!diagnostics.empty()) {
            message << "\nDXC diagnostics:\n" << diagnostics;
        }

        const std::string failureMessage = message.str();
        spdlog::error("{}", failureMessage);
        throw std::runtime_error(failureMessage);
    }

    if (!diagnostics.empty()) {
        spdlog::warn(
            "Shader compiler diagnostics for file='{}', entry='{}', target='{}':\n{}",
            ws2s(filename),
            ws2s(entryPoint),
            ws2s(target),
            diagnostics);
    }

    return result;
}

ComPtr<IDxcBlob> PSOManager::ExtractObject(
    IDxcResult* result,
    const std::wstring& filename,
    bool writeDebugArtifacts)
{
    ComPtr<IDxcBlob> objectBlob;
    ThrowIfFailed(result->GetOutput(
        DXC_OUT_OBJECT,
        IID_PPV_ARGS(objectBlob.GetAddressOf()),
        nullptr));

    if (writeDebugArtifacts) {
        auto exePath = std::filesystem::path(GetExePath());
        auto outDir = exePath / L"CompiledShaders";
        std::filesystem::create_directories(outDir);

        // derive a base name from the suggested pdb path
        ComPtr<IDxcBlobUtf16> pdbPathBlob;
        ComPtr<IDxcBlob>     pdbBlob;
        const HRESULT pdbHr = result->GetOutput(
            DXC_OUT_PDB,
            IID_PPV_ARGS(pdbBlob.GetAddressOf()),
            pdbPathBlob.GetAddressOf());

        if (SUCCEEDED(pdbHr) && pdbBlob && pdbPathBlob) {
            auto suggested = pdbPathBlob->GetStringPointer();
            auto baseName = std::filesystem::path(suggested).stem().wstring();

            WriteDebugArtifacts(result, outDir, baseName);
        }
        else {
            spdlog::debug("Shader compile result for {} did not include separate PDB output; skipping debug artifact write.", ws2s(filename));
        }
    }

    return objectBlob;
}

void PSOManager::WriteDebugArtifacts(
    IDxcResult* result,
    const std::filesystem::path& outDir,
    const std::wstring& baseName)
{
    // write .bin
    ComPtr<IDxcBlob> pdbBlob;
    ComPtr<IDxcBlob> objBlob;
    ThrowIfFailed(result->GetOutput(
        DXC_OUT_PDB, IID_PPV_ARGS(pdbBlob.GetAddressOf()), nullptr));
    ThrowIfFailed(result->GetOutput(
        DXC_OUT_OBJECT, IID_PPV_ARGS(objBlob.GetAddressOf()), nullptr));

    auto write = [&](const std::filesystem::path& path, IDxcBlob* blob) {
        std::ofstream f(path, std::ios::binary);
        if (!f) spdlog::error("Failed to open {} for writing", path.string());
        else    f.write((char*)blob->GetBufferPointer(), blob->GetBufferSize());
        };

    write(outDir / (baseName + L".bin"), objBlob.Get());
    write(outDir / (baseName + L".pdb"), pdbBlob.Get());
}

void PSOManager::createRootSignature() {

    auto device = DeviceManager::GetInstance().GetDevice();

    rhi::PushConstantRangeDesc pcs[] = {
    { rhi::ShaderStage::All, NumMiscUintRootConstants,          0, 4, rhi::PushConstantRangeType::EmulatedRootConstants },
    { rhi::ShaderStage::All, org::shaderapi::kNumResourceDescriptorIndicesRootConstants, 0, org::shaderapi::kResourceDescriptorIndicesRootParameter, rhi::PushConstantRangeType::EmulatedRootConstants },
    { rhi::ShaderStage::All, org::shaderapi::kNumIndirectCommandSignatureRootConstants, 0, org::shaderapi::kIndirectCommandSignatureRootParameter },
    };

    rhi::SamplerDesc pointClamp = {
        .minFilter = rhi::Filter::Nearest,
        .magFilter = rhi::Filter::Nearest,
        .mipFilter = rhi::MipFilter::Nearest,
        .addressU = rhi::AddressMode::Clamp,
        .addressV = rhi::AddressMode::Clamp,
        .addressW = rhi::AddressMode::Clamp,
    };

    rhi::SamplerDesc linearClamp = {
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipFilter = rhi::MipFilter::Linear,
        .addressU = rhi::AddressMode::Clamp,
        .addressV = rhi::AddressMode::Clamp,
        .addressW = rhi::AddressMode::Clamp,
	};


    rhi::StaticSamplerDesc staticSamplers[] = {
    { .sampler = pointClamp, .visibility = rhi::ShaderStage::All, .set = 0, .binding = 0, .arrayCount = 1 },
    {.sampler = linearClamp, .visibility = rhi::ShaderStage::All, .set = 0, .binding = 1, .arrayCount = 1 }, // fill filter to linear in DX12 map
    };

    auto result = device.CreatePipelineLayout(
        rhi::PipelineLayoutDesc{
            .ranges = {},
            .pushConstants = rhi::Span<rhi::PushConstantRangeDesc>(pcs, std::size(pcs)),
            .staticSamplers = rhi::Span<rhi::StaticSamplerDesc>(staticSamplers, std::size(staticSamplers)),
            .flags = rhi::PipelineLayoutFlags::PF_AllowInputAssembler
        },
        m_rootSignature);

    if (Failed(result) || !m_rootSignature || !m_rootSignature->IsValid()) {
        spdlog::error(
            "Failed to create graphics root signature / pipeline layout: {} ({})",
            rhi::ResultName(result),
            static_cast<uint32_t>(result));
        throw std::runtime_error(
            std::string("Failed to create graphics root signature / pipeline layout: ") +
            rhi::ResultName(result) +
            " (" +
            std::to_string(static_cast<uint32_t>(result)) +
            ")");
    }

    result = device.CreatePipelineLayout(
        rhi::PipelineLayoutDesc{
            .ranges = {},
            .pushConstants = rhi::Span<rhi::PushConstantRangeDesc>(pcs, std::size(pcs)),
            .staticSamplers = rhi::Span<rhi::StaticSamplerDesc>(staticSamplers, std::size(staticSamplers)),
            .flags = rhi::PipelineLayoutFlags::PF_None
        },
        m_computeRootSignature);

    if (Failed(result) || !m_computeRootSignature || !m_computeRootSignature->IsValid()) {
        spdlog::error(
            "Failed to create compute root signature / pipeline layout: {} ({})",
            rhi::ResultName(result),
            static_cast<uint32_t>(result));
        throw std::runtime_error(
            std::string("Failed to create compute root signature / pipeline layout: ") +
            rhi::ResultName(result) +
            " (" +
            std::to_string(static_cast<uint32_t>(result)) +
            ")");
    }

	if (auto peerDevice = DeviceManager::GetInstance().GetPeerDevice()) {
		result = peerDevice.CreatePipelineLayout(
			rhi::PipelineLayoutDesc{
				.ranges = {},
				.pushConstants = rhi::Span<rhi::PushConstantRangeDesc>(pcs, std::size(pcs)),
				.staticSamplers = rhi::Span<rhi::StaticSamplerDesc>(staticSamplers, std::size(staticSamplers)),
				.flags = rhi::PipelineLayoutFlags::PF_AllowInputAssembler },
			m_peerRootSignature);
		if (Failed(result) || !m_peerRootSignature || !m_peerRootSignature->IsValid()) {
			throw std::runtime_error("Failed to create peer graphics pipeline layout");
		}
		result = peerDevice.CreatePipelineLayout(
			rhi::PipelineLayoutDesc{
				.ranges = {},
				.pushConstants = rhi::Span<rhi::PushConstantRangeDesc>(pcs, std::size(pcs)),
				.staticSamplers = rhi::Span<rhi::StaticSamplerDesc>(staticSamplers, std::size(staticSamplers)),
				.flags = rhi::PipelineLayoutFlags::PF_None },
			m_peerComputeRootSignature);
		if (Failed(result) || !m_peerComputeRootSignature || !m_peerComputeRootSignature->IsValid()) {
			throw std::runtime_error("Failed to create peer compute pipeline layout");
		}
		spdlog::info("PSOManager created graphics and compute layouts for the peer backend");
	}
}

const rhi::PipelineLayout& PSOManager::GetRootSignature() {
	if (!m_rootSignature || !m_rootSignature->IsValid()) {
		throw std::runtime_error("Graphics root signature / pipeline layout is not initialized");
	}
    return m_rootSignature.Get();
}

const rhi::PipelineLayout& PSOManager::GetComputeRootSignature() {
	if (!m_computeRootSignature || !m_computeRootSignature->IsValid()) {
		throw std::runtime_error("Compute root signature / pipeline layout is not initialized");
	}
	return m_computeRootSignature.Get();
}

const rhi::PipelineLayout& PSOManager::GetRootSignature(BackendInstanceId backendInstance) {
	if (backendInstance == BackendInstanceId::Primary) return GetRootSignature();
	if (!m_peerRootSignature || !m_peerRootSignature->IsValid()) {
		throw std::runtime_error("Peer graphics pipeline layout is not initialized");
	}
	return m_peerRootSignature.Get();
}

const rhi::PipelineLayout& PSOManager::GetComputeRootSignature(BackendInstanceId backendInstance) {
	if (backendInstance == BackendInstanceId::Primary) return GetComputeRootSignature();
	if (!m_peerComputeRootSignature || !m_peerComputeRootSignature->IsValid()) {
		throw std::runtime_error("Peer compute pipeline layout is not initialized");
	}
	return m_peerComputeRootSignature.Get();
}

bool PSOManager::RebuildAllPipelines(std::string& error) {
    struct RebuildTarget {
        std::string id;
        ComputeRecipe computeRecipe;
        std::function<PipelineState()> rebuild;
        bool isComputeRecipe = false;
    };

    error.clear();
    std::vector<RebuildTarget> targets;
    {
        std::scoped_lock lock(m_cacheMutex, m_livePipelineMutex);
        const auto busy = std::ranges::find_if(m_livePipelines, [](const auto& item) {
            return item.second.compiling;
        });
        if (busy != m_livePipelines.end()) {
            error = "pipeline '" + busy->first + "' is already compiling";
            return false;
        }

        // Invalidate cache-fill jobs which have not published a PipelineState yet.
        // Existing cache entries stay intact because their replaceable slots are
        // rebuilt through m_livePipelines below.
        m_asyncPSOGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_pendingClusterLODRasterPSOs.clear();
        m_pendingClusterLODVirtualShadowRasterPSOs.clear();
        m_pendingClusterLODVirtualShadowReyesRasterPSOs.clear();
        m_pendingClusterLODDeepVisibilityRasterPSOs.clear();
        m_pendingClusterLODAVBOITOccupancyPSOs.clear();
        m_pendingClusterLODAVBOITRasterPSOs.clear();
        m_pendingClusterLODAVBOITShadePSOs.clear();
        m_pendingClusterLODSoftwareRasterPSOs.clear();
        m_pendingMaterialEvalPSOs.clear();

        targets.reserve(m_livePipelines.size());
        for (auto& [id, entry] : m_livePipelines) {
            entry.compiling = true;
            targets.push_back(RebuildTarget{
                .id = id,
                .computeRecipe = entry.computeRecipe,
                .rebuild = entry.rebuild,
                .isComputeRecipe = entry.supportsDefineOverrides
            });
        }
    }

    std::vector<std::pair<std::string, std::shared_ptr<PipelineStatePayload>>> candidates;
    candidates.reserve(targets.size());
    try {
        for (const RebuildTarget& target : targets) {
            PipelineState candidate = target.isComputeRecipe
                ? BuildComputePipeline(target.computeRecipe)
                : target.rebuild();
            auto payload = candidate.GetPayload();
            if (!payload) {
                throw std::runtime_error("pipeline '" + target.id + "' produced no payload");
            }
            candidates.emplace_back(target.id, std::move(payload));
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "unknown shader compilation failure";
    }

    std::scoped_lock lock(m_livePipelineMutex);
    if (!error.empty()) {
        for (const RebuildTarget& target : targets) {
            if (auto entry = m_livePipelines.find(target.id); entry != m_livePipelines.end()) {
                entry->second.compiling = false;
            }
        }
        return false;
    }

    for (auto& [id, payload] : candidates) {
        auto entryIt = m_livePipelines.find(id);
        if (entryIt == m_livePipelines.end()) {
            error = "pipeline '" + id + "' was removed during global rebuild";
            break;
        }
        LivePipelineEntry& entry = entryIt->second;
        uint64_t nextGeneration = 1;
        for (const auto& generation : entry.generations) {
            nextGeneration = std::max(nextGeneration, generation->generation + 1);
        }
        payload->generation = nextGeneration;
        payload->label = "global-rebuild-" + std::to_string(nextGeneration);
    }
    if (!error.empty()) {
        for (const RebuildTarget& target : targets) {
            if (auto entry = m_livePipelines.find(target.id); entry != m_livePipelines.end()) {
                entry->second.compiling = false;
            }
        }
        return false;
    }

    for (auto& [id, payload] : candidates) {
        LivePipelineEntry& entry = m_livePipelines.at(id);
        entry.state.ReplacePayload(payload);
        entry.generations.push_back(payload);
        entry.generationDefineOverrides[payload->generation] = {};
        while (entry.generations.size() > 8) {
            const auto activePayload = entry.state.GetPayload();
            const auto evictionIt = std::ranges::find_if(
                entry.generations,
                [&activePayload](const auto& generation) {
                    return generation != activePayload;
                });
            if (evictionIt == entry.generations.end()) {
                break;
            }
            entry.generationDefineOverrides.erase((*evictionIt)->generation);
            entry.generations.erase(evictionIt);
        }
        entry.compiling = false;
    }
    m_pipelineEpoch.fetch_add(1, std::memory_order_acq_rel);
    spdlog::info("PSOManager: globally rebuilt {} registered pipelines", candidates.size());
    return true;
}

std::vector<PSOManager::LivePipelineInfo> PSOManager::ListPipelines() const
{
    std::scoped_lock lock(m_livePipelineMutex);
    std::vector<LivePipelineInfo> result;
    result.reserve(m_livePipelines.size());
    for (const auto& [_, entry] : m_livePipelines) {
        const auto payload = entry.state.GetPayload();
        LivePipelineInfo info;
        info.id = entry.id;
        info.displayName = entry.displayName;
        info.kind = entry.kind;
        info.shaderPaths = entry.shaderPaths;
        info.activeGeneration = payload ? payload->generation : 0;
        info.sourceHash = payload ? payload->sourceHash : 0;
        info.bytecodeHash = payload ? payload->bytecodeHash : 0;
        info.label = payload ? payload->label : std::string{};
        if (payload) {
            if (const auto definesIt = entry.generationDefineOverrides.find(payload->generation);
                definesIt != entry.generationDefineOverrides.end()) {
                info.defineOverrides = definesIt->second;
            }
        }
        info.compiling = entry.compiling;
        result.push_back(std::move(info));
    }
    std::ranges::sort(result, {}, &LivePipelineInfo::id);
    return result;
}

std::optional<PSOManager::LiveJobInfo> PSOManager::GetLiveJob(uint64_t jobId) const
{
    std::scoped_lock lock(m_livePipelineMutex);
    const auto it = m_liveJobs.find(jobId);
    return it == m_liveJobs.end() ? std::nullopt : std::optional<LiveJobInfo>{ it->second };
}

uint64_t PSOManager::RequestRecompile(const std::string& pipelineId, RecompileOptions options)
{
    ComputeRecipe recipe;
    std::function<PipelineState()> rebuild;
    bool supportsDefineOverrides = false;
    const uint64_t jobId = m_nextLiveJobId.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(m_livePipelineMutex);
        auto entryIt = m_livePipelines.find(pipelineId);
        if (entryIt == m_livePipelines.end()) {
            throw std::runtime_error("Unknown live pipeline '" + pipelineId + "'");
        }
        if (entryIt->second.compiling) {
            throw std::runtime_error("Pipeline '" + pipelineId + "' already has a live compile in progress");
        }
        entryIt->second.compiling = true;
        recipe = entryIt->second.computeRecipe;
        rebuild = entryIt->second.rebuild;
        supportsDefineOverrides = entryIt->second.supportsDefineOverrides;
        if (!supportsDefineOverrides && !options.defineOverrides.empty()) {
            entryIt->second.compiling = false;
            throw std::runtime_error("Define overrides are only supported for directly registered compute pipelines");
        }
        m_liveJobs.emplace(jobId, LiveJobInfo{
            .id = jobId,
            .pipelineId = pipelineId,
            .state = LiveJobState::Queued
        });
    }

    TaskSchedulerManager::GetInstance().Submit(
        TaskLane::Background,
        TaskDomain::ShaderCompile,
        "PSOManager::LiveRecompile::" + pipelineId,
        [this,
            pipelineId,
            jobId,
            recipe = std::move(recipe),
            rebuild = std::move(rebuild),
            supportsDefineOverrides,
            options = std::move(options)]() mutable {
            {
                std::scoped_lock lock(m_livePipelineMutex);
                if (auto it = m_liveJobs.find(jobId); it != m_liveJobs.end()) {
                    it->second.state = LiveJobState::Compiling;
                }
            }
            try {
                ScopedShaderArtifactCacheReadBypass bypassCacheReads;
                PipelineState candidate = supportsDefineOverrides
                    ? BuildComputePipeline(recipe, &options)
                    : rebuild();
                auto payload = candidate.GetPayload();
                if (!options.label.empty()) {
                    payload->label = options.label;
                }
                std::scoped_lock lock(m_livePipelineMutex);
                auto entryIt = m_livePipelines.find(pipelineId);
                if (entryIt == m_livePipelines.end()) {
                    throw std::runtime_error("Pipeline was removed while recompiling");
                }
                uint64_t nextGeneration = 1;
                for (const auto& generation : entryIt->second.generations) {
                    nextGeneration = std::max(nextGeneration, generation->generation + 1);
                }
                payload->generation = nextGeneration;
                payload->label = options.label.empty()
                    ? "generation-" + std::to_string(nextGeneration)
                    : options.label;
                std::map<std::string, std::string> defineOverrides;
                for (const auto& [name, value] : options.defineOverrides) {
                    defineOverrides.emplace(ws2s(name), ws2s(value));
                }
                m_pendingLivePublications.push_back(
                    { jobId, pipelineId, std::move(payload), std::move(defineOverrides) });
                auto& job = m_liveJobs.at(jobId);
                job.state = LiveJobState::ReadyToPublish;
                job.generation = nextGeneration;
            } catch (const std::exception& exception) {
                std::scoped_lock lock(m_livePipelineMutex);
                if (auto entryIt = m_livePipelines.find(pipelineId); entryIt != m_livePipelines.end()) {
                    entryIt->second.compiling = false;
                }
                auto& job = m_liveJobs.at(jobId);
                job.state = LiveJobState::Failed;
                job.error = exception.what();
            } catch (...) {
                std::scoped_lock lock(m_livePipelineMutex);
                if (auto entryIt = m_livePipelines.find(pipelineId); entryIt != m_livePipelines.end()) {
                    entryIt->second.compiling = false;
                }
                auto& job = m_liveJobs.at(jobId);
                job.state = LiveJobState::Failed;
                job.error = "Unknown shader compilation failure";
            }
        });
    return jobId;
}

uint64_t PSOManager::RequestActivation(const std::string& pipelineId, uint64_t generation)
{
    const uint64_t jobId = m_nextLiveJobId.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(m_livePipelineMutex);
    const auto entryIt = m_livePipelines.find(pipelineId);
    if (entryIt == m_livePipelines.end()) {
        throw std::runtime_error("Unknown live pipeline '" + pipelineId + "'");
    }
    if (entryIt->second.compiling) {
        throw std::runtime_error("Pipeline '" + pipelineId + "' has a live compile in progress");
    }
    const bool found = std::ranges::any_of(entryIt->second.generations, [generation](const auto& payload) {
        return payload->generation == generation;
    });
    if (!found) {
        throw std::runtime_error(
            "Pipeline '" + pipelineId + "' has no retained generation " + std::to_string(generation));
    }
    m_liveJobs.emplace(jobId, LiveJobInfo{
        .id = jobId,
        .pipelineId = pipelineId,
        .state = LiveJobState::ReadyToPublish,
        .generation = generation
    });
    m_pendingLiveActivations.push_back({ jobId, pipelineId, generation });
    return jobId;
}

namespace {
bool PipelineResourcesMatch(const PipelineResources& left, const PipelineResources& right)
{
    return left.mandatoryResourceDescriptorSlots == right.mandatoryResourceDescriptorSlots &&
        left.optionalResourceDescriptorSlots == right.optionalResourceDescriptorSlots;
}
}

void PSOManager::PublishPendingLivePipelines(std::vector<PipelineRetirementPoint> retirementPoints)
{
    std::scoped_lock lock(m_livePipelineMutex);
    while (!m_pendingLivePublications.empty()) {
        PendingPublication publication = std::move(m_pendingLivePublications.front());
        m_pendingLivePublications.pop_front();
        auto entryIt = m_livePipelines.find(publication.pipelineId);
        if (entryIt == m_livePipelines.end()) {
            continue;
        }
        LivePipelineEntry& entry = entryIt->second;
        const auto active = entry.state.GetPayload();
        auto& job = m_liveJobs.at(publication.jobId);
        if (!active ||
            active->resourceIDsHash != publication.payload->resourceIDsHash ||
            !PipelineResourcesMatch(active->pipelineResources, publication.payload->pipelineResources)) {
            entry.compiling = false;
            job.state = LiveJobState::Failed;
            job.error = "Shader resource interface changed; use structural shader reload";
            continue;
        }

        auto retired = entry.state.ReplacePayload(publication.payload);
        if (retired) {
			m_retiredLivePayloads.push_back({ retirementPoints, std::move(retired) });
        }
        entry.generations.push_back(publication.payload);
        entry.generationDefineOverrides[publication.payload->generation] =
            std::move(publication.defineOverrides);
        while (entry.generations.size() > 8) {
            const auto activePayload = entry.state.GetPayload();
            const auto evictionIt = std::ranges::find_if(
                entry.generations,
                [&activePayload](const auto& generation) {
                    return generation != activePayload;
                });
            if (evictionIt == entry.generations.end()) {
                break;
            }
            entry.generationDefineOverrides.erase((*evictionIt)->generation);
            entry.generations.erase(evictionIt);
        }
        entry.compiling = false;
        job.state = LiveJobState::Published;
        m_pipelineEpoch.fetch_add(1, std::memory_order_acq_rel);
        spdlog::info(
            "PSOManager: published live pipeline '{}' generation={} label='{}'",
            publication.pipelineId,
            publication.payload->generation,
            publication.payload->label);
    }

    while (!m_pendingLiveActivations.empty()) {
        PendingActivation activation = std::move(m_pendingLiveActivations.front());
        m_pendingLiveActivations.pop_front();
        auto entryIt = m_livePipelines.find(activation.pipelineId);
        if (entryIt == m_livePipelines.end()) {
            continue;
        }
        auto generationIt = std::ranges::find_if(
            entryIt->second.generations,
            [generation = activation.generation](const auto& payload) {
                return payload->generation == generation;
            });
        auto& job = m_liveJobs.at(activation.jobId);
        if (generationIt == entryIt->second.generations.end()) {
            job.state = LiveJobState::Failed;
            job.error = "Requested generation is no longer retained";
            continue;
        }
        auto retired = entryIt->second.state.ReplacePayload(*generationIt);
        if (retired && retired != *generationIt) {
			m_retiredLivePayloads.push_back({ retirementPoints, std::move(retired) });
        }
        job.state = LiveJobState::Published;
        m_pipelineEpoch.fetch_add(1, std::memory_order_acq_rel);
        spdlog::info(
            "PSOManager: activated live pipeline '{}' generation={}",
            activation.pipelineId,
            activation.generation);
    }
}

void PSOManager::CollectRetiredLivePipelines()
{
    std::scoped_lock lock(m_livePipelineMutex);
	std::erase_if(m_retiredLivePayloads, [](RetiredPayload& retired) {
		return std::ranges::all_of(retired.completionPoints, [](PipelineRetirementPoint& point) {
			return point.timeline.IsValid() && point.value != UINT64_MAX &&
				point.timeline.GetCompletedValue() >= point.value;
		});
    });
}

void PSOManager::DrainRetiredLivePipelinesAfterDeviceIdle()
{
	std::scoped_lock lock(m_livePipelineMutex);
	m_retiredLivePayloads.clear();
}

uint64_t PSOManager::GetPipelineEpoch() const
{
    return m_pipelineEpoch.load(std::memory_order_acquire);
}

PSOManager::PipelineGenerationSnapshot PSOManager::GetPipelineGenerationSnapshot() const
{
    PipelineGenerationSnapshot snapshot;
    snapshot.epoch = GetPipelineEpoch();
    snapshot.pipelines = ListPipelines();
    uint64_t digest = 14695981039346656037ull;
    for (const LivePipelineInfo& pipeline : snapshot.pipelines) {
        util::hash_combine_u64(digest, HashStringStable(pipeline.id));
        util::hash_combine_u64(digest, pipeline.activeGeneration);
        util::hash_combine_u64(digest, pipeline.bytecodeHash);
    }
    snapshot.digest = digest;
    return snapshot;
}

rhi::BlendState PSOManager::GetBlendDesc(MaterialCompileFlags materialCompileFlags) {

    if (!(materialCompileFlags & MaterialCompileAlphaTest) && !(materialCompileFlags & MaterialCompileBlend)) {
        rhi::BlendState opaqueBlendDesc = {};
        opaqueBlendDesc.alphaToCoverage = FALSE;
        opaqueBlendDesc.independentBlend = FALSE;

        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            opaqueBlendDesc.attachments[i].enable = FALSE;
            opaqueBlendDesc.attachments[i].writeMask = rhi::ColorWriteEnable::All;
        }
        return opaqueBlendDesc;
    }
    if (materialCompileFlags & MaterialCompileAlphaTest) {
        rhi::BlendState maskBlendDesc = {};
        maskBlendDesc.alphaToCoverage = FALSE;
        maskBlendDesc.independentBlend = FALSE;

        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            maskBlendDesc.attachments[i].enable = FALSE; // No standard blending needed.
            maskBlendDesc.attachments[i].writeMask = rhi::ColorWriteEnable::All;
        }
        return maskBlendDesc;
    }
    if (materialCompileFlags & MaterialCompileBlend) {
        rhi::BlendState blendBlendDesc = {};
        blendBlendDesc.alphaToCoverage = FALSE;
        blendBlendDesc.independentBlend = FALSE;

        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            blendBlendDesc.attachments[i].enable = TRUE;
            blendBlendDesc.attachments[i].srcColor = rhi::BlendFactor::SrcAlpha;
            blendBlendDesc.attachments[i].dstColor = rhi::BlendFactor::InvSrcAlpha;
            blendBlendDesc.attachments[i].colorOp = rhi::BlendOp::Add;
            blendBlendDesc.attachments[i].srcAlpha = rhi::BlendFactor::One;
            blendBlendDesc.attachments[i].dstAlpha = rhi::BlendFactor::Zero;
            blendBlendDesc.attachments[i].alphaOp = rhi::BlendOp::Add;
            blendBlendDesc.attachments[i].writeMask = rhi::ColorWriteEnable::All;
        }
        return blendBlendDesc;
    }

    spdlog::warn("Blend state not set, defaulting to opaque");
    rhi::BlendState opaqueBlendDesc = {};
    opaqueBlendDesc.alphaToCoverage = FALSE;
    opaqueBlendDesc.independentBlend = FALSE;

    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        opaqueBlendDesc.attachments[i].enable = FALSE;
        opaqueBlendDesc.attachments[i].writeMask = rhi::ColorWriteEnable::All;
    }
    return opaqueBlendDesc;
}
