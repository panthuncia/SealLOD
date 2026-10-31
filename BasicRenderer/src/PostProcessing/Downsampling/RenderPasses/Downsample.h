#pragma once

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <functional>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/PixelBuffer.h"
#include "Utilities/Utilities.h"

#define A_CPU
#include "../shaders/FidelityFX/ffx_a.h"
#include "../shaders/FidelityFX/ffx_spd.h"

/*
SpdSetup(
outAU2 dispatchThreadGroupCountXY, // CPU side: dispatch thread group count xy
outAU2 workGroupOffset, // GPU side: pass in as constant
outAU2 numWorkGroupsAndMips, // GPU side: pass in as constant
inAU4 rectInfo, // left, top, width, height
ASU1 mips
*/

struct DownsampleMapBindings {
    org::DeclaredViewToken source, counter, constants;
    std::vector<org::DeclaredViewToken> mipUavs;
    std::function<void(std::span<const uint32_t>)> uploadConstants;
    bool isArrayLike = false;
    unsigned int constantsIndex = 0;
    std::array<unsigned int, 3> dispatch{};
};

struct DownsampleBindings {
    org::PipelineState standardPipeline, arrayPipeline;
    std::vector<DownsampleMapBindings> maps;
};

class DownsamplePass : public org::TypedRenderGraphPass<DownsamplePass,
    org::EmptyPassFrameData, DownsampleBindings>, public org::IDynamicDeclaredResources {
public:

    DownsamplePass()
    {
        CreateDownsampleComputePSO();
    }
    ~DownsamplePass() {
    }

    DownsampleBindings Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        SyncMapInfos(m_activeDepthMaps);
        DownsampleBindings bindings;
        bindings.standardPipeline = downsamplePassPSO;
        bindings.arrayPipeline = downsampleArrayPSO;
        bindings.maps.reserve(m_perMapInfo.size());
        for (auto& [resourceID, map] : m_perMapInfo) {
            (void)resourceID;
            const uint32_t variant = map.isArrayLike
                ? static_cast<uint32_t>(org::SRVViewType::Texture2DArray) : UINT32_MAX;
            auto source = declaration.ShaderResource(
                Subresources(map.sourceMap, org::Mip{0, 1}), org::SrvView{variant}).View();
            std::vector<org::UavView> views;
            for (uint32_t i = 0; i < map.constants.mips; ++i) views.push_back({UINT32_MAX, i + 1u});
            auto uavs = declaration.UnorderedAccess(Subresources(map.sourceMap, org::FromMip{1}),
                std::span<const org::UavView>(views));
            DownsampleMapBindings binding{};
            binding.source = source;
            binding.counter = declaration.UnorderedAccess(map.pCounterResource).View();
            binding.constants = declaration.ShaderResource(map.constantsBuffer).View();
            for (uint32_t i = 0; i < views.size(); ++i) binding.mipUavs.push_back(uavs.View(i));
            binding.uploadConstants = [buffer = map.constantsBuffer, view = map.pConstantsBufferView,
                frozen = map.constants](std::span<const uint32_t> indices) mutable {
                for (size_t i = 0; i < indices.size(); ++i) frozen.mipUavDescriptorIndices[i] = indices[i];
                buffer->UpdateView(view.get(), &frozen);
            };
            binding.isArrayLike = map.isArrayLike;
            binding.constantsIndex = map.constantsIndex;
            binding.dispatch = {map.dispatchThreadGroupCountXY[0],
                map.dispatchThreadGroupCountXY[1], map.dispatchThreadGroupCountZ};
            bindings.maps.push_back(std::move(binding));
        }
        return bindings;
    }

    void Update(const org::UpdateExecutionContext& executionContext) override {
        auto* updateContext = executionContext.hostData->Get<UpdateContext>();
        if (!updateContext) {
            if (!m_activeDepthMaps.empty()) {
                m_activeDepthMaps.clear();
                m_declaredResourcesChanged = true;
            }
            else {
                m_declaredResourcesChanged = false;
            }
            return;
        }

        auto activeDepthMaps = CollectActiveDepthMaps(updateContext->Views());
        m_declaredResourcesChanged = !HaveSameActiveDepthMaps(m_activeDepthMaps, activeDepthMaps);
        for (const auto& [resourceID, map] : activeDepthMaps) {
            const auto previous = m_perMapInfo.find(resourceID);
            if (previous == m_perMapInfo.end() ||
                previous->second.sourceBackingGeneration != map->GetBackingGeneration())
                m_declaredResourcesChanged = true;
        }
        if (m_declaredResourcesChanged) {
            m_activeDepthMaps = std::move(activeDepthMaps);
        }
    }

    // Its SPD constants (mip count, dispatch size) are made per declaration, and it redeclares on a resize or when a
    // depth map comes or goes. Building that inline beats a frame or two of old constants on the new texture.
    bool PreparesOnlyDeclaredWork() const noexcept override { return false; }
    bool DeclaredResourcesChanged() const override {
        return m_declaredResourcesChanged;
    }

    // Publishes each map's SPD constants, which carry the bindless indices of its mip UAVs.
    org::EmptyPassFrameData Prepare(const DownsampleBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        for (const auto& map : bindings.maps) {
            std::vector<uint32_t> mipIndices;
            for (const auto& token : map.mipUavs) mipIndices.push_back(preparation.Resolve(token).index);
            map.uploadConstants(mipIndices);
        }
        return {};
    }

    static void Record(const DownsampleBindings& bindings, org::PassRecordContext& context) {
        for (const auto& map : bindings.maps) {
            br::render::MiscConstants constants{};
            constants[UintRootConstant0] = context.Index(map.counter);
            constants[UintRootConstant1] = context.Index(map.source);
            constants[UintRootConstant2] = context.Index(map.constants);
            constants[UintRootConstant3] = map.constantsIndex;
            br::render::DispatchCompute(context, map.isArrayLike ? bindings.arrayPipeline : bindings.standardPipeline,
                constants, map.dispatch[0], map.dispatch[1], map.dispatch[2]);
        }
    }

private:
    struct spdConstants
    {
        uint srcSize[2];
        uint mips;
        uint numWorkGroups;
        
        uint workGroupOffset[2];
        float invInputSize[2];
        
        unsigned int mipUavDescriptorIndices[12];
        uint validSourceSize[2];
        uint pad[2];
    };

    struct PerMapInfo {
        std::shared_ptr<org::LazyDynamicStructuredBuffer<spdConstants>> constantsBuffer;
        uint64_t sourceResourceID;
		std::shared_ptr<org::PixelBuffer> sourceMap;
		bool isArrayLike;
        unsigned int constantsIndex;
		std::shared_ptr<org::BufferView> pConstantsBufferView;
		unsigned int dispatchThreadGroupCountXY[2];
        unsigned int dispatchThreadGroupCountZ;
        std::shared_ptr<org::GloballyIndexedResource> pCounterResource;
        uint64_t sourceBackingGeneration;
        spdConstants constants{};
    };
	std::unordered_map<uint64_t, PerMapInfo> m_perMapInfo;
    std::unordered_map<uint64_t, std::shared_ptr<org::PixelBuffer>> m_activeDepthMaps;


    org::PipelineState downsamplePassPSO;
	org::PipelineState downsampleArrayPSO;
    bool m_declaredResourcesChanged = true;

    static std::unordered_map<uint64_t, std::shared_ptr<org::PixelBuffer>> CollectActiveDepthMaps(
        std::span<const PreparedViewFrameData> views)
    {
        std::unordered_map<uint64_t, std::shared_ptr<org::PixelBuffer>> activeDepthMaps;

        for (const auto& view : views) {
            if (!view.linearDepthMap) continue;
            const uint64_t resourceID = view.linearDepthMap->GetGlobalResourceID();
            activeDepthMaps[resourceID] = view.linearDepthMap;
        }

        return activeDepthMaps;
    }

    static bool HaveSameActiveDepthMaps(
        const std::unordered_map<uint64_t, std::shared_ptr<org::PixelBuffer>>& lhs,
        const std::unordered_map<uint64_t, std::shared_ptr<org::PixelBuffer>>& rhs)
    {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (const auto& [resourceID, depthMap] : lhs) {
            auto rhsIt = rhs.find(resourceID);
            if (rhsIt == rhs.end()) {
                return false;
            }

            const uint64_t lhsGeneration = depthMap ? depthMap->GetBackingGeneration() : 0;
            const uint64_t rhsGeneration = rhsIt->second ? rhsIt->second->GetBackingGeneration() : 0;
            if (lhsGeneration != rhsGeneration) {
                return false;
            }
        }

        return true;
    }

    static uint32_t GetSliceCount(const org::PixelBuffer& map)
    {
        const auto& desc = map.GetDescription();
        if (desc.isCubemap) {
            return 6u * (std::max)(1u, desc.arraySize);
        }
        if (desc.isArray) {
            return (std::max)(1u, desc.arraySize);
        }
        return 1u;
    }

    void RemoveMapInfo(uint64_t resourceID) {
		auto it = m_perMapInfo.find(resourceID);
		if (it != m_perMapInfo.end()) {
			m_perMapInfo.erase(it);
		}
	}

    void CreateOrUpdateMapInfo(uint64_t resourceID, const std::shared_ptr<org::PixelBuffer>& linearDepthMap) {
        if (!linearDepthMap) {
            return;
        }

        const uint64_t generation = linearDepthMap->GetBackingGeneration();
        auto existing = m_perMapInfo.find(resourceID);
        if (existing != m_perMapInfo.end() && existing->second.sourceBackingGeneration == generation) {
            return;
        }

        if (existing != m_perMapInfo.end()) {
            RemoveMapInfo(resourceID);
        }

		const uint32_t paddedWidth = linearDepthMap->GetInternalWidth();
		const uint32_t paddedHeight = linearDepthMap->GetInternalHeight();

        unsigned int workGroupOffset[2];
        unsigned int numWorkGroupsAndMips[2];
        unsigned int rectInfo[4];
        rectInfo[0] = 0;
        rectInfo[1] = 0;
        rectInfo[2] = paddedWidth;
        rectInfo[3] = paddedHeight;

        unsigned int threadGroupCountXY[2];
        SpdSetup(threadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);

        const uint32_t maxGen = 12u;
        numWorkGroupsAndMips[1] = (std::min)(linearDepthMap->GetNumUAVMipLevels() - 1, maxGen);

        spdConstants constants = {};
		constants.srcSize[0] = linearDepthMap->GetInternalWidth();
		constants.srcSize[1] = linearDepthMap->GetInternalHeight();
        constants.validSourceSize[0] = linearDepthMap->GetWidth();
        constants.validSourceSize[1] = linearDepthMap->GetHeight();
        constants.invInputSize[0] = 1.0f / static_cast<float>(paddedWidth);
        constants.invInputSize[1] = 1.0f / static_cast<float>(paddedHeight);
        constants.mips = numWorkGroupsAndMips[1];
        constants.numWorkGroups = numWorkGroupsAndMips[0];
        constants.workGroupOffset[0] = workGroupOffset[0];
        constants.workGroupOffset[1] = workGroupOffset[1];

        // A new backing generation gets a new immutable constants allocation.
        auto constantsBuffer = org::LazyDynamicStructuredBuffer<spdConstants>::CreateShared(1, "Downsample map constants");
        auto constantsView = constantsBuffer->Add();
        constantsBuffer->UpdateView(constantsView.get(), &constants);

        PerMapInfo mapInfo = {};
        mapInfo.constantsBuffer = std::move(constantsBuffer);
        mapInfo.sourceResourceID = resourceID;
        mapInfo.sourceMap = linearDepthMap;
        mapInfo.isArrayLike = linearDepthMap->GetDescription().isArray || linearDepthMap->GetDescription().isCubemap;
        mapInfo.constantsIndex = static_cast<unsigned int>(constantsView.get()->GetOffset() / sizeof(spdConstants));
        mapInfo.pConstantsBufferView = constantsView;
        mapInfo.dispatchThreadGroupCountXY[0] = threadGroupCountXY[0];
        mapInfo.dispatchThreadGroupCountXY[1] = threadGroupCountXY[1];
        mapInfo.dispatchThreadGroupCountZ = GetSliceCount(*linearDepthMap);
        mapInfo.pCounterResource = CreateIndexedStructuredBuffer(1, sizeof(unsigned int) * 6, true);
        mapInfo.sourceBackingGeneration = generation;
        mapInfo.constants = constants;

        m_perMapInfo[resourceID] = std::move(mapInfo);
    }

    void SyncMapInfos(const std::unordered_map<uint64_t, std::shared_ptr<org::PixelBuffer>>& activeDepthMaps) {
        std::vector<uint64_t> stale;
        stale.reserve(m_perMapInfo.size());
        for (const auto& [resourceID, mapInfo] : m_perMapInfo) {
            if (!activeDepthMaps.contains(resourceID)) {
                stale.push_back(resourceID);
            }
        }

        for (uint64_t resourceID : stale) {
            RemoveMapInfo(resourceID);
        }

        for (const auto& [resourceID, depthMap] : activeDepthMaps) {
            CreateOrUpdateMapInfo(resourceID, depthMap);
        }
    }

    void CreateDownsampleComputePSO()
    {
		auto& psoManager = PSOManager::GetInstance();
        auto& layout = psoManager.GetComputeRootSignature();

        // Plain downsample
        downsamplePassPSO = psoManager.MakeComputePipeline(
            layout.GetHandle(),
            L"shaders/downsample.hlsl",
            L"DownsampleCSMain",
            {},                         // no defines
            "DownsampleCS"
        );

        // Array variant (DOWNSAMPLE_ARRAY=1)
        downsampleArrayPSO = psoManager.MakeComputePipeline(
            layout.GetHandle(),
            L"shaders/downsample.hlsl",
            L"DownsampleCSMain",
            { DxcDefine{ L"DOWNSAMPLE_ARRAY", L"1" } },
            "DownsampleCS[Array]"
        );
    }

};
