#pragma once

#include <directx/d3d12.h>
#include <wrl.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <filesystem>
#include <optional>
#include <mutex>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <boost/container_hash/hash.hpp>

#include <rhi.h>
#include <OpenRenderGraph/OpenRenderGraph.h>

#pragma warning(push, 0)   // Disable all warnings for dxc header
#include "ThirdParty/DirectX/dxcapi.h"
#pragma warning(pop)
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/PSOFlags.h"
#include "Materials/TechniqueDescriptor.h"
#include "Managers/Singletons/TaskSchedulerManager.h"

using Microsoft::WRL::ComPtr;

struct PSOKey {
    uint64_t psoFlags;
    MaterialCompileFlags materialCompileFlags;
	bool wireframe;

    PSOKey(uint64_t flags, MaterialCompileFlags materialCompileFlags, bool wireframe) : psoFlags(flags), materialCompileFlags(materialCompileFlags), wireframe(wireframe) {}

    bool operator==(const PSOKey& other) const {
        return psoFlags == other.psoFlags && materialCompileFlags == other.materialCompileFlags && wireframe == other.wireframe;
    }
};

namespace std {
    template <>
    struct hash<PSOKey> {
        std::size_t operator()(const PSOKey& key) const noexcept {
            // Combine the hash of psoFlags, materialCompileFlags, and wireframe
			std::size_t seed = 0;

			boost::hash_combine(seed, key.psoFlags);
			boost::hash_combine(seed, key.materialCompileFlags);
			boost::hash_combine(seed, key.wireframe);
            return seed;
        }
    };
}

struct RasterPSOKey {
	MaterialRasterFlags materialRasterFlags;
	UINT psoFlags;
	bool wireframe;
	bool singleView;

	RasterPSOKey(MaterialRasterFlags materialRasterFlags, bool wireframe, bool singleView = false, UINT psoFlags = 0)
        : materialRasterFlags(materialRasterFlags), psoFlags(psoFlags), wireframe(wireframe), singleView(singleView) {}
    bool operator==(const RasterPSOKey& other) const {
        return materialRasterFlags == other.materialRasterFlags && psoFlags == other.psoFlags &&
            wireframe == other.wireframe &&
            singleView == other.singleView;
	}
};

namespace std {
    template <>
    struct hash<RasterPSOKey> {
        std::size_t operator()(const RasterPSOKey& key) const noexcept {
            // Combine the hash of materialRasterFlags and wireframe
            std::size_t seed = 0;

			boost::hash_combine(seed, key.materialRasterFlags);
			boost::hash_combine(seed, key.psoFlags);
			boost::hash_combine(seed, key.wireframe);
			boost::hash_combine(seed, key.singleView);

            return seed;
        }
    };
}

struct ShaderInfo {
    std::wstring filename;
    std::wstring entryPoint;
    std::wstring target;
    ShaderInfo(const std::wstring& file, const std::wstring& entry, const std::wstring& tgt)
        : filename(file), entryPoint(entry), target(tgt) {}
};

struct ShaderLibraryInfo {
    std::wstring filename;
	std::wstring target;
    ShaderLibraryInfo(const std::wstring& file, const std::wstring& tgt) : filename(file), target(tgt) {}
};

struct ShaderInfoBundle {
    ShaderInfoBundle(const std::vector<DxcDefine>& defs, bool debug = false, bool warnings = true) : 
        defines(defs), enableDebugInfo(debug), warningsAsErrors(warnings) {}
	ShaderInfoBundle() = default;
    std::optional<ShaderInfo> vertexShader;
    std::optional<ShaderInfo> pixelShader;
    std::optional<ShaderInfo> amplificationShader;
    std::optional<ShaderInfo> meshShader;
    std::optional<ShaderInfo> computeShader;

    std::vector<DxcDefine> defines;
    bool enableDebugInfo = false;
    bool warningsAsErrors = true;
};

struct ShaderBundle {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
    Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
    Microsoft::WRL::ComPtr<ID3DBlob> amplificationShader;
    Microsoft::WRL::ComPtr<ID3DBlob> meshShader;
    Microsoft::WRL::ComPtr<ID3DBlob> computeShader;
	PipelineResources resourceDescriptorSlots;
	uint64_t resourceIDsHash = 0;
};

struct ShaderLibraryBundle {
    Microsoft::WRL::ComPtr<ID3DBlob> libraryBlob;
    PipelineResources resourceDescriptorSlots;
    uint64_t resourceIDsHash = 0;
};

class PSOManager {
public:
    enum class LivePipelineKind : uint8_t {
        Compute,
        Graphics,
        Mesh,
        RayTracing
    };

    enum class LiveJobState : uint8_t {
        Queued,
        Compiling,
        ReadyToPublish,
        Published,
        Failed
    };

    struct LivePipelineInfo {
        std::string id;
        std::string displayName;
        LivePipelineKind kind = LivePipelineKind::Compute;
        std::vector<std::string> shaderPaths;
        uint64_t activeGeneration = 0;
        uint64_t sourceHash = 0;
        uint64_t bytecodeHash = 0;
        std::string label;
        std::map<std::string, std::string> defineOverrides;
        bool compiling = false;
    };

    struct LiveJobInfo {
        uint64_t id = 0;
        std::string pipelineId;
        LiveJobState state = LiveJobState::Queued;
        uint64_t generation = 0;
        std::string error;
    };

    struct RecompileOptions {
        std::string label;
        std::map<std::wstring, std::wstring> defineOverrides;
    };

    struct PipelineGenerationSnapshot {
        uint64_t epoch = 0;
        std::vector<LivePipelineInfo> pipelines;
        uint64_t digest = 0;
    };

    static PSOManager& GetInstance();

    void initialize();
    void initializeShaderCompiler();
    void Cleanup();

    const PipelineState& GetPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const PipelineState& GetPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const PipelineState& GetMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const PipelineState& GetMeshPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const PipelineState& GetPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const PipelineState& GetMeshPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const PipelineState& GetShadowPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const PipelineState& GetShadowMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const PipelineState& GetVisibilityBufferPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
	const PipelineState& GetVisibilityBufferMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const PipelineState& GetClusterLODRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState& GetClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState& GetClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState& GetClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState& GetClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState& GetClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState& GetClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false, UINT psoFlags = 0);
    const PipelineState& GetClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind);
    const PipelineState& GetClusterLODDeepVisibilityResolvePSO(UINT psoFlags);

    const PipelineState* TryGetClusterLODRasterPSO(
        MaterialRasterFlags materialRasterFlags,
        bool wireframe = false,
        bool singleView = false);
    const PipelineState* TryGetClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState* TryGetClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState* TryGetClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState* TryGetClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState* TryGetClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const PipelineState* TryGetClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false, UINT psoFlags = 0);
    const PipelineState* TryGetClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind);
    const PipelineState* TryGetMaterialEvalPSO(MaterialCompileFlags materialCompileFlags);


    const PipelineState& GetDeferredPSO(UINT psoFlags);

    PipelineState MakeComputePipeline(rhi::PipelineLayoutHandle layout,
        const wchar_t* shaderPath,
        const wchar_t* entryPoint,
        std::vector<DxcDefine> defines = {},
        const char* debugName = nullptr);
	const rhi::Pipeline& ResolvePipeline(const PipelineState& pipeline, BackendInstanceId backendInstance);

    PipelineState RegisterExternalPipeline(
        PipelineState state,
        std::string id,
        std::string displayName,
        LivePipelineKind kind,
        std::function<PipelineState()> rebuild);

    const rhi::PipelineLayout& GetRootSignature();
    const rhi::PipelineLayout& GetComputeRootSignature();
	const rhi::PipelineLayout& GetRootSignature(BackendInstanceId backendInstance);
	const rhi::PipelineLayout& GetComputeRootSignature(BackendInstanceId backendInstance);
    bool RebuildAllPipelines(std::string& error);
    std::vector<LivePipelineInfo> ListPipelines() const;
    std::optional<LiveJobInfo> GetLiveJob(uint64_t jobId) const;
    uint64_t RequestRecompile(const std::string& pipelineId, RecompileOptions options = {});
    uint64_t RequestActivation(const std::string& pipelineId, uint64_t generation);
    void PublishPendingLivePipelines(uint64_t retirementFenceValue);
    void CollectRetiredLivePipelines(uint64_t completedFenceValue);
    uint64_t GetPipelineEpoch() const;
    PipelineGenerationSnapshot GetPipelineGenerationSnapshot() const;
    std::vector<DxcDefine> GetShaderDefines(UINT psoFlags, MaterialCompileFlags materialFlags);
	std::vector<DxcDefine> GetRasterShaderDefines(MaterialRasterFlags materialRasterFlags);
	ShaderBundle CompileShaders(const ShaderInfoBundle& shaderInfoBundle);
	ShaderBundle CompileShaders(const ShaderInfoBundle& shaderInfoBundle, BackendInstanceId backendInstance);
	void PrecompileMaterialEvalShaderArtifact(MaterialCompileFlags materialCompileFlags);
	ShaderLibraryBundle CompileShaderLibrary(const ShaderLibraryInfo& libraryInfo, const std::vector<DxcDefine>& defines = {});

    void GetPreprocessedBlob(
        const std::wstring& filename,
        const std::wstring& entryPoint,
        const std::wstring& target,
        std::vector<DxcDefine> defines,
        Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);

private:
    struct OwnedDefine {
        std::wstring name;
        std::wstring value;
    };

    struct ComputeRecipe {
        rhi::PipelineLayoutHandle layout{};
        std::wstring shaderPath;
        std::wstring entryPoint;
        std::vector<OwnedDefine> defines;
        std::string debugName;
    };

    struct LivePipelineEntry {
        std::string id;
        std::string displayName;
        LivePipelineKind kind = LivePipelineKind::Compute;
        PipelineState state;
        ComputeRecipe computeRecipe;
        std::function<PipelineState()> rebuild;
        std::vector<std::string> shaderPaths;
        bool supportsDefineOverrides = false;
        std::deque<std::shared_ptr<PipelineStatePayload>> generations;
        std::map<uint64_t, std::map<std::string, std::string>> generationDefineOverrides;
        bool compiling = false;
    };

    struct PendingPublication {
        uint64_t jobId = 0;
        std::string pipelineId;
        std::shared_ptr<PipelineStatePayload> payload;
        std::map<std::string, std::string> defineOverrides;
    };

    struct PendingActivation {
        uint64_t jobId = 0;
        std::string pipelineId;
        uint64_t generation = 0;
    };

    struct RetiredPayload {
        uint64_t fenceValue = 0;
        std::shared_ptr<PipelineStatePayload> payload;
    };

    struct ShaderCompileOptions
    {
        std::wstring entryPoint;
        std::wstring target;
        std::vector<DxcDefine> defines;
        bool emitSpirv = false;
        bool enableDebugInfo = false;
        bool warningsAsErrors = true;
    };

    struct SourceData {
        DxcBuffer                  buffer;
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> blob;
    };

    PSOManager() = default;
	rhi::PipelineLayoutPtr m_rootSignature;
	rhi::PipelineLayoutPtr m_peerRootSignature;
	rhi::PipelineLayoutPtr m_computeRootSignature;
	rhi::PipelineLayoutPtr m_peerComputeRootSignature;
    rhi::PipelineLayoutPtr m_debugRootSignature;
    rhi::PipelineLayoutPtr m_environmentConversionRootSignature;

    std::unordered_map<PSOKey, PipelineState> m_psoCache;
    std::unordered_map<PSOKey, PipelineState> m_PPLLPSOCache;
    std::unordered_map<PSOKey, PipelineState> m_meshPSOCache;
    std::unordered_map<PSOKey, PipelineState> m_meshPPLLPSOCache;

    std::unordered_map<PSOKey, PipelineState> m_prePassPSOCache;
    std::unordered_map<PSOKey, PipelineState> m_meshPrePassPSOCache;

    std::unordered_map<PSOKey, PipelineState> m_shadowPSOCache;
	std::unordered_map<PSOKey, PipelineState> m_shadowMeshPSOCache;

    std::unordered_map<PSOKey, PipelineState> m_visibilityBufferPSOCache;
    std::unordered_map<PSOKey, PipelineState> m_visibilityBufferMeshPSOCache;

    std::unordered_map<RasterPSOKey, PipelineState> m_clusterLODRasterPSOCache;
    std::unordered_map<RasterPSOKey, PipelineState> m_clusterLODVirtualShadowRasterPSOCache;
    std::unordered_map<RasterPSOKey, PipelineState> m_clusterLODVirtualShadowReyesRasterPSOCache;
    std::unordered_map<RasterPSOKey, PipelineState> m_clusterLODDeepVisibilityRasterPSOCache;
    std::unordered_map<RasterPSOKey, PipelineState> m_clusterLODAVBOITOccupancyPSOCache;
    std::unordered_map<RasterPSOKey, PipelineState> m_clusterLODAVBOITRasterPSOCache;
    std::unordered_map<RasterPSOKey, PipelineState> m_clusterLODAVBOITShadePSOCache;
    std::unordered_map<uint64_t, PipelineState> m_clusterLODSoftwareRasterPSOCache;
    std::unordered_map<unsigned int, PipelineState> m_clusterLODDeepVisibilityResolvePSOCache;
    std::unordered_map<MaterialCompileFlags, PipelineState> m_materialEvalPSOCache;

    std::unordered_set<RasterPSOKey> m_pendingClusterLODRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODVirtualShadowRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODVirtualShadowReyesRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODDeepVisibilityRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITOccupancyPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITShadePSOs;
    std::unordered_set<uint64_t> m_pendingClusterLODSoftwareRasterPSOs;
    std::unordered_set<MaterialCompileFlags> m_pendingMaterialEvalPSOs;

	std::unordered_map<unsigned int, PipelineState> m_deferredPSOCache;

    ComPtr<IDxcUtils> pUtils;
    ComPtr<IDxcCompiler3> pCompiler;
	ComPtr<ID3D12PipelineState> debugPSO;
    ComPtr<ID3D12PipelineState> environmentConversionPSO;
    mutable std::mutex m_cacheMutex;
    std::atomic<uint64_t> m_asyncPSOGeneration = 0;
    mutable std::mutex m_livePipelineMutex;
    std::unordered_map<std::string, LivePipelineEntry> m_livePipelines;
    std::unordered_map<uint64_t, LiveJobInfo> m_liveJobs;
    std::deque<PendingPublication> m_pendingLivePublications;
    std::deque<PendingActivation> m_pendingLiveActivations;
    std::deque<RetiredPayload> m_retiredLivePayloads;
    std::atomic<uint64_t> m_nextLiveJobId = 1;
    std::atomic<uint64_t> m_pipelineEpoch = 1;

    PipelineState CreatePSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    PipelineState CreatePPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    PipelineState CreateMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    PipelineState CreateMeshPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    PipelineState CreatePrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    PipelineState CreateMeshPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    PipelineState CreateShadowPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    PipelineState CreateShadowMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

	PipelineState CreateVisibilityBufferPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
	PipelineState CreateVisibilityBufferMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    PipelineState CreateClusterLODRasterPSO(
        MaterialRasterFlags materialRasterFlags,
        bool wireframe = false,
        bool singleView = false);
    PipelineState CreateClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    PipelineState CreateClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    PipelineState CreateClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    PipelineState CreateClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    PipelineState CreateClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    PipelineState CreateClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false, UINT psoFlags = 0);
    PipelineState CreateClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind);
    PipelineState CreateClusterLODDeepVisibilityResolvePSO(UINT psoFlags);
    PipelineState CreateMaterialEvalPSO(MaterialCompileFlags materialCompileFlags);

    PipelineState CreateDeferredPSO(UINT psoFlags);
    PipelineState BuildComputePipeline(const ComputeRecipe& recipe, const RecompileOptions* options = nullptr);
	void BuildComputePipelineForBackend(const ComputeRecipe& recipe, const PipelineState& pipeline,
		BackendInstanceId backendInstance);
    PipelineState RegisterComputePipeline(PipelineState state, ComputeRecipe recipe);
    PipelineState RegisterPipeline(
        PipelineState state,
        std::string id,
        std::string displayName,
        LivePipelineKind kind,
        std::function<PipelineState()> rebuild);

    template <typename TCache, typename TPending, typename TKey, typename TFactory>
    const PipelineState* TryGetOrRequestPipelineState(
        TCache PSOManager::* cacheMember,
        TPending PSOManager::* pendingMember,
        const TKey& key,
        std::string taskName,
        TFactory&& factory)
    {
        {
            std::scoped_lock lock(m_cacheMutex);
            auto& cache = this->*cacheMember;
            auto it = cache.find(key);
            if (it != cache.end()) {
                return &it->second;
            }

            auto& pending = this->*pendingMember;
            if (!pending.insert(key).second) {
                return nullptr;
            }
        }

        const uint64_t generation = m_asyncPSOGeneration.load(std::memory_order_acquire);
        const std::string queueTaskName = taskName;
        TaskSchedulerManager::GetInstance().QueueShaderCompileTask(
            queueTaskName,
            [this,
                cacheMember,
                pendingMember,
                key,
                generation,
                taskName = std::move(taskName),
                factory = std::forward<TFactory>(factory)]() mutable {
                try {
                    PipelineState pipelineState = factory();
                    pipelineState = RegisterPipeline(
                        std::move(pipelineState),
                        taskName + ".key=" + std::to_string(std::hash<TKey>{}(key)),
                        taskName,
                        LivePipelineKind::Graphics,
                        [factory]() mutable {
                            return factory();
                        });
                    std::scoped_lock lock(m_cacheMutex);
                    if (generation != m_asyncPSOGeneration.load(std::memory_order_acquire)) {
                        return;
                    }

                    auto& pending = this->*pendingMember;
                    pending.erase(key);
                    auto& cache = this->*cacheMember;
                    cache.emplace(key, std::move(pipelineState));
                }
                catch (...) {
                    std::scoped_lock lock(m_cacheMutex);
                    if (generation != m_asyncPSOGeneration.load(std::memory_order_acquire)) {
                        return;
                    }

                    auto& pending = this->*pendingMember;
                    pending.erase(key);
                }
            });

        return nullptr;
    }

    void CompileShaderForSlot(
        const std::optional<ShaderInfo>& slot,
        const std::vector<DxcDefine>& defines,
		const DxcBuffer& buffer,
        Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
    void CompileShader(const std::wstring& filename, 
        const std::wstring& entryPoint, 
        const std::wstring& target, 
        const DxcBuffer& ppBuffer,
        std::vector<DxcDefine> defines, 
        Microsoft::WRL::ComPtr<ID3DBlob>& shaderBlob);

    void createRootSignature();
    rhi::BlendState GetBlendDesc(MaterialCompileFlags materialCompileFlags);

    void LoadSource(const std::filesystem::path& path, PSOManager::SourceData& sd);

    ComPtr<IDxcIncludeHandler> CreateIncludeHandler();

    std::vector<LPCWSTR> BuildArguments(
        const ShaderCompileOptions& opts,
        const std::filesystem::path& shaderDir,
        std::vector<std::wstring>& ownedArgs);

    ComPtr<IDxcResult> InvokeCompile(
        const DxcBuffer& srcBuffer,
        std::vector<LPCWSTR>& arguments,
        IDxcIncludeHandler* includeHandler,
        const std::wstring& filename,
        const std::wstring& entryPoint,
        const std::wstring& target);

    ComPtr<IDxcBlob> ExtractObject(
        IDxcResult* result,
        const std::wstring& filename,
        bool writeDebugArtifacts);

    void WriteDebugArtifacts(
        IDxcResult* result,
        const std::filesystem::path& outDir,
        const std::wstring& baseName);

    template<typename BlobT>
    void PreprocessShaderSlot(
        const std::optional<ShaderInfo>& slot,
        const std::vector<DxcDefine>& defines,
        Microsoft::WRL::ComPtr<BlobT>& outBlob,
        DxcBuffer& outBuf)
    {
        if (!slot)
            return;

        GetPreprocessedBlob(
            slot->filename,
            slot->entryPoint,
            slot->target,
            defines,
            outBlob
        );

        outBuf.Ptr = outBlob->GetBufferPointer();
        outBuf.Size = outBlob->GetBufferSize();
        outBuf.Encoding = 0;
    }

};

inline PSOManager& PSOManager::GetInstance() {
    static PSOManager instance;
    return instance;
}
