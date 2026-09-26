#pragma once

#include <directx/d3d12.h>
#include <wrl.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <string_view>
#include <filesystem>
#include <optional>
#include <mutex>
#include <shared_mutex>
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
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Pipeline/PSOFlags.h>
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>

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

#include <BasicRenderer/Extensions/ShaderCompilationTypes.h>
#include <BasicRenderer/Diagnostics/PipelineControl.h>

class PSOManager {
public:


    static PSOManager& GetInstance();

    void initialize();
    void initializeShaderCompiler();
    void Cleanup();

    const org::PipelineState& GetPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const org::PipelineState& GetPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const org::PipelineState& GetMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const org::PipelineState& GetMeshPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const org::PipelineState& GetPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const org::PipelineState& GetMeshPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const org::PipelineState& GetShadowPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    const org::PipelineState& GetShadowMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const org::PipelineState& GetVisibilityBufferPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
	const org::PipelineState& GetVisibilityBufferMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    const org::PipelineState& GetClusterLODRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState& GetClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState& GetClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState& GetClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState& GetClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState& GetClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState& GetClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false, UINT psoFlags = 0);
    const org::PipelineState& GetClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind);
    const org::PipelineState& GetClusterLODDeepVisibilityResolvePSO(UINT psoFlags);

    const org::PipelineState* TryGetClusterLODRasterPSO(
        MaterialRasterFlags materialRasterFlags,
        bool wireframe = false,
        bool singleView = false);
    const org::PipelineState* TryGetClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState* TryGetClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState* TryGetClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState* TryGetClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState* TryGetClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState* TryGetClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false, UINT psoFlags = 0);
    const org::PipelineState* TryGetClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind);
    const org::PipelineState* TryGetMaterialEvalPSO(MaterialCompileFlags materialCompileFlags);


    const org::PipelineState& GetDeferredPSO(UINT psoFlags);
    // Pipeline cache entries live until Cleanup(); passes may cache references
    // to them keyed on this generation.
    uint64_t PipelineCacheGeneration() const noexcept { return m_asyncPSOGeneration.load(std::memory_order_acquire); }

    org::PipelineState MakeComputePipeline(rhi::PipelineLayoutHandle layout,
        const wchar_t* shaderPath,
        const wchar_t* entryPoint,
        std::vector<DxcDefine> defines = {},
        const char* debugName = nullptr,
        std::shared_ptr<const void> layoutOwner = {});
	const rhi::Pipeline& ResolvePipeline(const org::PipelineState& pipeline, org::BackendInstanceId backendInstance);

    org::PipelineState RegisterExternalPipeline(
        org::PipelineState state,
        std::string id,
        std::string displayName,
        br::extensions::PipelineKind kind,
        std::function<org::PipelineState()> rebuild);

    std::shared_ptr<const void> CaptureLayoutOwner(rhi::PipelineLayoutHandle layout) const;
    const rhi::PipelineLayout& GetRootSignature();
    const rhi::PipelineLayout& GetComputeRootSignature();
	const rhi::PipelineLayout& GetRootSignature(org::BackendInstanceId backendInstance);
	const rhi::PipelineLayout& GetComputeRootSignature(org::BackendInstanceId backendInstance);
    bool RebuildAllPipelines(std::string& error);
    std::vector<br::diagnostics::LivePipelineInfo> ListPipelines() const;
    std::optional<br::diagnostics::LiveJobInfo> GetLiveJob(uint64_t jobId) const;
    uint64_t RequestRecompile(const std::string& pipelineId, br::diagnostics::RecompileOptions options = {});
    uint64_t RequestActivation(const std::string& pipelineId, uint64_t generation);
	struct PipelineRetirementPoint {
		rhi::Timeline timeline;
		uint64_t value = 0;
	};
    void PublishPendingLivePipelines(std::vector<PipelineRetirementPoint> retirementPoints);
    void CollectRetiredLivePipelines();
	void DrainRetiredLivePipelinesAfterDeviceIdle();
    uint64_t GetPipelineEpoch() const;
    br::diagnostics::PipelineGenerationSnapshot GetPipelineGenerationSnapshot() const;
    std::vector<DxcDefine> GetShaderDefines(UINT psoFlags, MaterialCompileFlags materialFlags);
	std::vector<DxcDefine> GetRasterShaderDefines(MaterialRasterFlags materialRasterFlags);
	ShaderBundle CompileShaders(const ShaderInfoBundle& shaderInfoBundle);
	ShaderBundle CompileShaders(const ShaderInfoBundle& shaderInfoBundle, org::BackendInstanceId backendInstance);
	void PrecompileMaterialEvalShaderArtifact(MaterialCompileFlags materialCompileFlags);
	void PrecompileShaderArtifact(const ShaderVariantRequest& request);
	void PrecompileShaderBundleArtifact(const ShaderInfoBundle& shaderInfoBundle);
	ShaderLibraryBundle CompileShaderLibrary(const ShaderLibraryInfo& libraryInfo, const std::vector<DxcDefine>& defines = {});

    void GetPreprocessedBlob(
        const std::wstring& filename,
        const std::wstring& entryPoint,
        const std::wstring& target,
        std::vector<DxcDefine> defines,
        Microsoft::WRL::ComPtr<ID3DBlob>& outBlob,
        bool emitSpirv = false);

private:
    struct OwnedDefine {
        std::wstring name;
        std::wstring value;
    };

    struct ComputeRecipe {
        rhi::PipelineLayoutHandle layout{};
        std::shared_ptr<const void> layoutOwner;
        std::wstring shaderPath;
        std::wstring entryPoint;
        std::vector<OwnedDefine> defines;
        std::string debugName;
    };

    struct LivePipelineEntry {
        std::string id;
        std::string displayName;
        br::extensions::PipelineKind kind = br::extensions::PipelineKind::Compute;
        org::PipelineState state;
        ComputeRecipe computeRecipe;
        std::function<org::PipelineState()> rebuild;
        std::vector<std::string> shaderPaths;
        bool supportsDefineOverrides = false;
        std::deque<std::shared_ptr<org::PipelineStatePayload>> generations;
        std::map<uint64_t, std::map<std::string, std::string>> generationDefineOverrides;
        bool compiling = false;
    };

    struct PendingPublication {
        uint64_t jobId = 0;
        std::string pipelineId;
        std::shared_ptr<org::PipelineStatePayload> payload;
        std::map<std::string, std::string> defineOverrides;
    };

    struct PendingActivation {
        uint64_t jobId = 0;
        std::string pipelineId;
        uint64_t generation = 0;
    };

    struct RetiredPayload {
		std::vector<PipelineRetirementPoint> completionPoints;
        std::shared_ptr<org::PipelineStatePayload> payload;
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
    struct LayoutGeneration {
        rhi::PipelineLayoutPtr rootSignature, peerRootSignature;
        rhi::PipelineLayoutPtr computeRootSignature, peerComputeRootSignature;
        rhi::PipelineLayoutPtr debugRootSignature, environmentConversionRootSignature;
    };
    std::shared_ptr<LayoutGeneration> m_layoutGeneration = std::make_shared<LayoutGeneration>();

    std::unordered_map<PSOKey, org::PipelineState> m_psoCache;
    std::unordered_map<PSOKey, org::PipelineState> m_PPLLPSOCache;
    std::unordered_map<PSOKey, org::PipelineState> m_meshPSOCache;
    std::unordered_map<PSOKey, org::PipelineState> m_meshPPLLPSOCache;

    std::unordered_map<PSOKey, org::PipelineState> m_prePassPSOCache;
    std::unordered_map<PSOKey, org::PipelineState> m_meshPrePassPSOCache;

    std::unordered_map<PSOKey, org::PipelineState> m_shadowPSOCache;
	std::unordered_map<PSOKey, org::PipelineState> m_shadowMeshPSOCache;

    std::unordered_map<PSOKey, org::PipelineState> m_visibilityBufferPSOCache;
    std::unordered_map<PSOKey, org::PipelineState> m_visibilityBufferMeshPSOCache;

    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODVirtualShadowRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODVirtualShadowReyesRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODDeepVisibilityRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODAVBOITOccupancyPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODAVBOITRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODAVBOITShadePSOCache;
    std::unordered_map<uint64_t, org::PipelineState> m_clusterLODSoftwareRasterPSOCache;
    std::unordered_map<unsigned int, org::PipelineState> m_clusterLODDeepVisibilityResolvePSOCache;
    std::unordered_map<MaterialCompileFlags, org::PipelineState> m_materialEvalPSOCache;

    std::unordered_set<RasterPSOKey> m_pendingClusterLODRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODVirtualShadowRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODVirtualShadowReyesRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODDeepVisibilityRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITOccupancyPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITShadePSOs;
    std::unordered_set<uint64_t> m_pendingClusterLODSoftwareRasterPSOs;
    std::unordered_set<MaterialCompileFlags> m_pendingMaterialEvalPSOs;

	std::unordered_map<unsigned int, org::PipelineState> m_deferredPSOCache;

    ComPtr<IDxcUtils> pUtils;
    ComPtr<IDxcCompiler3> pCompiler;
	ComPtr<ID3D12PipelineState> debugPSO;
    ComPtr<ID3D12PipelineState> environmentConversionPSO;
    mutable std::shared_mutex m_cacheMutex; // shared: cache lookups; exclusive: inserts, rebuilds
    std::atomic<uint64_t> m_asyncPSOGeneration = 0;
    mutable std::mutex m_livePipelineMutex;
    std::unordered_map<std::string, LivePipelineEntry> m_livePipelines;
    std::unordered_map<uint64_t, br::diagnostics::LiveJobInfo> m_liveJobs;
    std::deque<PendingPublication> m_pendingLivePublications;
    std::deque<PendingActivation> m_pendingLiveActivations;
    std::deque<RetiredPayload> m_retiredLivePayloads;
    std::atomic<uint64_t> m_nextLiveJobId = 1;
    std::atomic<uint64_t> m_pipelineEpoch = 1;

    org::PipelineState CreatePSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    org::PipelineState CreatePPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    org::PipelineState CreateMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    org::PipelineState CreateMeshPPLLPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    org::PipelineState CreatePrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    org::PipelineState CreateMeshPrePassPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    org::PipelineState CreateShadowPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
    org::PipelineState CreateShadowMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

	org::PipelineState CreateVisibilityBufferPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);
	org::PipelineState CreateVisibilityBufferMeshPSO(UINT psoFlags, MaterialCompileFlags materialCompileFlags, bool wireframe = false);

    org::PipelineState CreateClusterLODRasterPSO(
        MaterialRasterFlags materialRasterFlags,
        bool wireframe = false,
        bool singleView = false);
    org::PipelineState CreateClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODVirtualShadowReyesRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false, UINT psoFlags = 0);
    org::PipelineState CreateClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind);
    org::PipelineState CreateClusterLODDeepVisibilityResolvePSO(UINT psoFlags);
    org::PipelineState CreateMaterialEvalPSO(MaterialCompileFlags materialCompileFlags);

    org::PipelineState CreateDeferredPSO(UINT psoFlags);
    org::PipelineState BuildComputePipeline(const ComputeRecipe& recipe, const br::diagnostics::RecompileOptions* options = nullptr);
	void BuildComputePipelineForBackend(const ComputeRecipe& recipe, const org::PipelineState& pipeline,
		org::BackendInstanceId backendInstance);
    org::PipelineState RegisterComputePipeline(org::PipelineState state, ComputeRecipe recipe);
    org::PipelineState RegisterPipeline(
        org::PipelineState state,
        std::string id,
        std::string displayName,
        br::extensions::PipelineKind kind,
        std::function<org::PipelineState()> rebuild);

    template <typename TCache, typename TPending, typename TKey, typename TFactory>
    const org::PipelineState* TryGetOrRequestPipelineState(
        TCache PSOManager::* cacheMember,
        TPending PSOManager::* pendingMember,
        const TKey& key,
        std::string_view taskName,
        TFactory&& factory)
    {
        {
            std::shared_lock lock(m_cacheMutex);
            auto& cache = this->*cacheMember;
            auto it = cache.find(key);
            if (it != cache.end()) {
                return &it->second;
            }
        }
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
        // The cache hit above is the per-frame path; only a miss materializes the name.
        const std::string queueTaskName(taskName);
        TaskSchedulerManager::GetInstance().Submit(
            // On-demand variants are explicit static/material publication
            // dependencies. Background priority can starve the final variant
            // behind the streaming work that is waiting for it, preventing
            // scene quiescence even after every other queue has drained.
            TaskLane::Streaming,
            TaskDomain::ShaderCompile,
            queueTaskName,
            [this,
                cacheMember,
                pendingMember,
                key,
                generation,
                taskName = std::string(taskName),
                factory = std::forward<TFactory>(factory)]() mutable {
                try {
                    org::PipelineState pipelineState = factory();
                    pipelineState = RegisterPipeline(
                        std::move(pipelineState),
                        taskName + ".key=" + std::to_string(std::hash<TKey>{}(key)),
                        taskName,
                        br::extensions::PipelineKind::Graphics,
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
                catch (const std::exception& error) {
                    spdlog::error(
                        "Async PSO compilation failed: task='{}' keyHash={} error='{}'",
                        taskName,
                        std::hash<TKey>{}(key),
                        error.what());
                    std::scoped_lock lock(m_cacheMutex);
                    if (generation != m_asyncPSOGeneration.load(std::memory_order_acquire)) {
                        return;
                    }

                    auto& pending = this->*pendingMember;
                    pending.erase(key);
                }
                catch (...) {
                    spdlog::error(
                        "Async PSO compilation failed: task='{}' keyHash={} error='<non-standard exception>'",
                        taskName,
                        std::hash<TKey>{}(key));
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
        bool emitSpirv,
        Microsoft::WRL::ComPtr<ID3DBlob>& outBlob);
    void CompileShader(const std::wstring& filename, 
        const std::wstring& entryPoint, 
        const std::wstring& target, 
        const DxcBuffer& ppBuffer,
        std::vector<DxcDefine> defines,
        bool emitSpirv,
        Microsoft::WRL::ComPtr<ID3DBlob>& shaderBlob);
	ShaderBundle CompileShadersForBackend(const ShaderInfoBundle& info, rhi::Backend backend);

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
        DxcBuffer& outBuf,
        bool emitSpirv)
    {
        if (!slot)
            return;

        GetPreprocessedBlob(
            slot->filename,
            slot->entryPoint,
            slot->target,
            defines,
            outBlob,
            emitSpirv
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
