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
#include <set>
#include <memory>
#include <boost/container_hash/hash.hpp>

#include <rhi.h>
#include "Render/PipelineState.h"
#include "Render/QueueKind.h"

#pragma warning(push, 0)   // Disable all warnings for dxc header
#include "ThirdParty/DirectX/dxcapi.h"
#pragma warning(pop)
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Pipeline/PSOFlags.h>
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>

using Microsoft::WRL::ComPtr;


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

// A DXR state object made from one library, with the compute root signature as its global layout. D3D12 only.
struct RayTracingPipelineRecipe {
    struct Shader {
        rhi::ShaderStage stage{};
        std::string exportName;
    };
    struct HitGroup {
        std::string name;
        std::string closestHit;
        std::string anyHit;
    };
    std::wstring libraryPath;
    std::wstring target = L"lib_6_10";
    std::vector<std::pair<std::wstring, std::wstring>> defines;
    // Each ray generation and miss shader becomes a group of its own, named after its export.
    std::vector<Shader> shaders;
    std::vector<HitGroup> hitGroups;
    uint32_t maxPayloadBytes = 0;
    uint32_t maxAttributeBytes = 8;
    uint32_t maxRecursionDepth = 1;
    rhi::RayTracingPipelineFlags flags = static_cast<rhi::RayTracingPipelineFlags>(
        rhi::RTPipeline_AllowClusteredGeometry | rhi::RTPipeline_SkipProceduralPrimitives);
    std::string debugName;

    // Every group, in the order the state object numbers them. CLodRtShaderTable finds shader identifiers through this.
    std::vector<std::string> GroupNames() const;
};

namespace org::services { class ShaderIncludeTracker; }
class PSOManagerShaderBuild;

class PSOManager {
public:


    static PSOManager& GetInstance();

    void initialize();
    void initializeShaderCompiler();
    void Cleanup();

    const org::PipelineState& GetClusterLODRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    const org::PipelineState& GetClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
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
    // On-demand variants (TryGetOrRequestPipelineState) requested and not yet compiled, queued ones included.
    uint32_t PendingAsyncPipelineCount() const noexcept { return m_asyncPSOsPending.load(std::memory_order_acquire); }
    // Resolves through the shader store every D3D12 bundle runs have recorded (the compile manifest beside the store),
    // dropping those that no longer compile. For an offline tool after the shader sources change. Returns how
    // many compiled; progress gets (done, total).
    std::size_t PrecompileRecordedShaderBundles(const std::function<void(std::size_t, std::size_t)>& progress = {});

    // Compiles and creates the state object on the calling thread, unless the same recipe was made before. Throws when the
    // primary backend isn't D3D12. It has no peer-backend build: ResolvePipeline leaves it absent, so its pass must stay on
    // the primary device.
    org::PipelineState MakeRayTracingPipeline(RayTracingPipelineRecipe recipe);
    // The same, on a shader-compile worker: returns null until the state object exists. Creating one can stall for long
    // enough that a frame must not wait on it.
    const org::PipelineState* TryGetRayTracingPipeline(const RayTracingPipelineRecipe& recipe);
    // A compute pipeline on the compute root signature, compiled on a shader-compile worker on first request: null until it
    // exists. For a pass whose defines follow runtime state (the lens self-test's generated polynomial include).
    const org::PipelineState* TryGetComputePipeline(const std::wstring& shaderPath, const std::wstring& entryPoint,
        const std::vector<std::pair<std::wstring, std::wstring>>& defines, const std::string& debugName);

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
	// For org::PipelineLibrary's builds. A reload reads the source files again before asking the shader store.
	ShaderBundle CompileShadersForPipelineLibrary(const ShaderInfoBundle& shaderInfoBundle, org::BackendInstanceId backendInstance, bool bypassCache);
	void PrecompileMaterialEvalShaderArtifact(MaterialCompileFlags materialCompileFlags);
	void PrecompileShaderArtifact(const ShaderVariantRequest& request);
	void PrecompileShaderBundleArtifact(const ShaderInfoBundle& shaderInfoBundle);
	ShaderLibraryBundle CompileShaderLibrary(const ShaderLibraryInfo& libraryInfo, const std::vector<DxcDefine>& defines = {});

    // A device rejected bytecode the shader store returned (ShaderBundle::storeKey*): the artifact is deleted, so the
    // next request builds it from source. False if the bytecode was not from the store.
    static bool DiscardStoredShaders(uint64_t storeKeyHigh, uint64_t storeKeyLow);

private:
    // The permutation caches' compute pipelines (deferred shading, software raster, ...), primary device only. Passes declare
    // theirs through org::RenderPassBuilder::Pipeline instead.
    org::PipelineState MakeComputePipeline(rhi::PipelineLayoutHandle layout,
        const wchar_t* shaderPath,
        const wchar_t* entryPoint,
        std::vector<DxcDefine> defines = {},
        const char* debugName = nullptr,
        std::shared_ptr<const void> layoutOwner = {},
        const wchar_t* target = L"cs_6_6");
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
        std::wstring target = L"cs_6_6";
    };

    struct LivePipelineEntry {
        std::string id;
        std::string displayName;
        br::extensions::PipelineKind kind = br::extensions::PipelineKind::Compute;
        org::PipelineState state;
        ComputeRecipe computeRecipe;
        std::function<org::PipelineState()> rebuild;
        // Ray tracing pipelines: the recipe rebuilt with define overrides (added, or replacing a define of the same name).
        std::function<org::PipelineState(const std::map<std::wstring, std::wstring>&)> rebuildWithDefines;
        std::vector<std::string> shaderPaths;
        uint64_t rayTracingRecipeHash = 0;
        bool supportsDefineOverrides = false;
        std::deque<std::shared_ptr<org::PipelineStatePayload>> generations;
        std::map<uint64_t, std::map<std::string, std::string>> generationDefineOverrides;
        bool compiling = false;
    };

    // The pipelines passes declared by description live in org::PipelineLibrary, which also builds and retains them. These
    // are what the live-pipeline surface adds on top of it.
    struct LibraryPipelineJobs {
        std::set<std::string> compiling;
        std::vector<uint64_t> awaitingPublish;
        std::map<std::string, std::map<uint64_t, std::map<std::string, std::string>>> defineOverrides;
    };
    LibraryPipelineJobs m_libraryJobs; // Guarded by m_livePipelineMutex.

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

    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODVirtualShadowRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODDeepVisibilityRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODAVBOITOccupancyPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODAVBOITRasterPSOCache;
    std::unordered_map<RasterPSOKey, org::PipelineState> m_clusterLODAVBOITShadePSOCache;
    std::unordered_map<uint64_t, org::PipelineState> m_clusterLODSoftwareRasterPSOCache;
    std::unordered_map<unsigned int, org::PipelineState> m_clusterLODDeepVisibilityResolvePSOCache;
    std::unordered_map<MaterialCompileFlags, org::PipelineState> m_materialEvalPSOCache;
    std::unordered_map<uint64_t, org::PipelineState> m_rayTracingPSOCache;
    std::unordered_map<uint64_t, org::PipelineState> m_onDemandComputePSOCache;

    std::unordered_set<RasterPSOKey> m_pendingClusterLODRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODVirtualShadowRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODDeepVisibilityRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITOccupancyPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITRasterPSOs;
    std::unordered_set<RasterPSOKey> m_pendingClusterLODAVBOITShadePSOs;
    std::unordered_set<uint64_t> m_pendingClusterLODSoftwareRasterPSOs;
    std::unordered_set<MaterialCompileFlags> m_pendingMaterialEvalPSOs;
    std::unordered_set<uint64_t> m_pendingRayTracingPSOs;
    std::unordered_set<uint64_t> m_pendingOnDemandComputePSOs;

	std::unordered_map<unsigned int, org::PipelineState> m_deferredPSOCache;

    ComPtr<IDxcUtils> pUtils;
    ComPtr<IDxcCompiler3> pCompiler;
	ComPtr<ID3D12PipelineState> debugPSO;
    ComPtr<ID3D12PipelineState> environmentConversionPSO;
    mutable std::shared_mutex m_cacheMutex; // shared: cache lookups; exclusive: inserts, rebuilds
    std::atomic<uint64_t> m_asyncPSOGeneration = 0;
    std::atomic<uint32_t> m_asyncPSOsPending = 0;
    mutable std::mutex m_livePipelineMutex;
    std::unordered_map<std::string, LivePipelineEntry> m_livePipelines;
    std::unordered_map<uint64_t, br::diagnostics::LiveJobInfo> m_liveJobs;
    std::deque<PendingPublication> m_pendingLivePublications;
    std::deque<PendingActivation> m_pendingLiveActivations;
    std::deque<RetiredPayload> m_retiredLivePayloads;
    std::atomic<uint64_t> m_nextLiveJobId = 1;
    std::atomic<uint64_t> m_pipelineEpoch = 1;

    org::PipelineState CreateClusterLODRasterPSO(
        MaterialRasterFlags materialRasterFlags,
        bool wireframe = false,
        bool singleView = false);
    org::PipelineState CreateClusterLODVirtualShadowRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODDeepVisibilityRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODAVBOITOccupancyPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODAVBOITRasterPSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false);
    org::PipelineState CreateClusterLODAVBOITShadePSO(MaterialRasterFlags materialRasterFlags, bool wireframe = false, UINT psoFlags = 0);
    org::PipelineState CreateClusterLODSoftwareRasterPSO(MaterialRasterFlags materialRasterFlags, CLodRasterOutputKind outputKind);
    org::PipelineState CreateClusterLODDeepVisibilityResolvePSO(UINT psoFlags);
    org::PipelineState CreateMaterialEvalPSO(MaterialCompileFlags materialCompileFlags);

    org::PipelineState CreateDeferredPSO(UINT psoFlags);
    org::PipelineState BuildComputePipeline(const ComputeRecipe& recipe, const br::diagnostics::RecompileOptions* options = nullptr);
    org::PipelineState RegisterComputePipeline(org::PipelineState state, ComputeRecipe recipe);
    org::PipelineState BuildRayTracingPipeline(const RayTracingPipelineRecipe& recipe);
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
        m_asyncPSOsPending.fetch_add(1, std::memory_order_acq_rel);
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
                struct PendingRelease {
                    std::atomic<uint32_t>& pending;
                    ~PendingRelease() { pending.fetch_sub(1, std::memory_order_acq_rel); }
                } pendingRelease{ m_asyncPSOsPending };
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

    void CompileShader(const std::wstring& filename, 
        const std::wstring& entryPoint, 
        const std::wstring& target, 
        const DxcBuffer& ppBuffer,
        std::vector<DxcDefine> defines,
        bool emitSpirv,
        Microsoft::WRL::ComPtr<ID3DBlob>& shaderBlob);
	ShaderBundle CompileShadersForBackend(const ShaderInfoBundle& info, rhi::Backend backend);
	ShaderLibraryBundle CompileShaderLibraryForBackend(const ShaderLibraryInfo& libraryInfo, const std::vector<DxcDefine>& defines, rhi::Backend backend);

    void createRootSignature();
    rhi::BlendState GetBlendDesc(MaterialCompileFlags materialCompileFlags);

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

    void DumpCompiledShader(
        const std::wstring& filename,
        const std::wstring& entryPoint,
        const DxcBuffer& source,
        const std::vector<LPCWSTR>& args,
        IDxcBlob* object,
        bool emitSpirv);

    void WriteDebugArtifacts(
        IDxcResult* result,
        const std::filesystem::path& outDir,
        const std::wstring& baseName);

    // DXC preprocessing of one root, reading the root and every include through the shader store's tracker.
    Microsoft::WRL::ComPtr<ID3DBlob> PreprocessShaderSource(
        const std::wstring& filename,
        const std::wstring& entryPoint,
        const std::wstring& target,
        const std::vector<DxcDefine>& defines,
        bool emitSpirv,
        org::services::ShaderIncludeTracker& includes);

    friend class PSOManagerShaderBuild;
};

inline PSOManager& PSOManager::GetInstance() {
    static PSOManager instance;
    return instance;
}
