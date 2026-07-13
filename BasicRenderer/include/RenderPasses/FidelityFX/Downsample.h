#pragma once

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/ComputePass.h"
#include "BuiltinResources.h"
#include "Managers/ViewManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
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

class DownsamplePass : public ComputePass, public IDynamicDeclaredResources {
public:

    DownsamplePass()
    {
        CreateDownsampleComputePSO();
        m_pDownsampleConstants = LazyDynamicStructuredBuffer<spdConstants>::CreateShared(1, "Downsample constants");
    }
    ~DownsamplePass() {
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, Mip{ 0, 1 }))
            .WithUnorderedAccess(Subresources(Builtin::PrimaryCamera::LinearDepthMap, FromMip{ 1 }));
    }

    void Setup() override {
        SyncMapInfos(m_activeDepthMaps);
    }

    void Update(const UpdateExecutionContext& executionContext) override {
        auto* updateContext = executionContext.hostData->Get<UpdateContext>();
        if (!updateContext || !updateContext->viewManager) {
            if (!m_activeDepthMaps.empty()) {
                m_activeDepthMaps.clear();
                m_declaredResourcesChanged = true;
            }
            else {
                m_declaredResourcesChanged = false;
            }
            return;
        }

        auto activeDepthMaps = CollectActiveDepthMaps(*updateContext->viewManager);
        m_declaredResourcesChanged = !HaveSameActiveDepthMaps(m_activeDepthMaps, activeDepthMaps);
        if (m_declaredResourcesChanged) {
            m_activeDepthMaps = std::move(activeDepthMaps);
        }
    }

    bool DeclaredResourcesChanged() const override {
        return m_declaredResourcesChanged;
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;

        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

        // Set the root signature
        commandList.BindLayout(psoManager.GetComputeRootSignature().GetHandle());

        for (auto& [resourceID, mapInfo] : m_perMapInfo) {
            if (!mapInfo.pConstantsBufferView || !mapInfo.sourceMap) {
                continue;
            }

            unsigned int downsampleRootConstants[NumMiscUintRootConstants] = {};
            downsampleRootConstants[UintRootConstant0] = mapInfo.pCounterResource->GetUAVShaderVisibleInfo(0).slot.index;
            downsampleRootConstants[UintRootConstant1] = mapInfo.isArrayLike
                ? mapInfo.sourceMap->GetSRVInfo(SRVViewType::Texture2DArray, 0).slot.index
                : mapInfo.sourceMap->GetSRVInfo(0).slot.index;
            downsampleRootConstants[UintRootConstant2] = m_pDownsampleConstants->GetSRVInfo(0).slot.index;
            downsampleRootConstants[UintRootConstant3] = mapInfo.constantsIndex;

            commandList.PushConstants(
                rhi::ShaderStage::Compute,
                0,
                MiscUintRootSignatureIndex,
                0,
                NumMiscUintRootConstants,
                downsampleRootConstants
            );

            commandList.BindPipeline(
                mapInfo.isArrayLike
                    ? downsampleArrayPSO.GetAPIPipelineState().GetHandle()
                    : downsamplePassPSO.GetAPIPipelineState().GetHandle());

            commandList.Dispatch(
                mapInfo.dispatchThreadGroupCountXY[0],
                mapInfo.dispatchThreadGroupCountXY[1],
                mapInfo.dispatchThreadGroupCountZ);
        }

        return {};
    }

    void Cleanup() override {
        // Cleanup if necessary
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
        uint64_t sourceResourceID;
		std::shared_ptr<PixelBuffer> sourceMap;
		bool isArrayLike;
        unsigned int constantsIndex;
		std::shared_ptr<BufferView> pConstantsBufferView;
		unsigned int dispatchThreadGroupCountXY[2];
        unsigned int dispatchThreadGroupCountZ;
        std::shared_ptr<GloballyIndexedResource> pCounterResource;
        uint64_t sourceBackingGeneration;
    };
	std::unordered_map<uint64_t, PerMapInfo> m_perMapInfo;
    std::unordered_map<uint64_t, std::shared_ptr<PixelBuffer>> m_activeDepthMaps;

    std::shared_ptr<LazyDynamicStructuredBuffer<spdConstants>> m_pDownsampleConstants;

    PipelineState downsamplePassPSO;
	PipelineState downsampleArrayPSO;
    bool m_declaredResourcesChanged = true;

    static std::unordered_map<uint64_t, std::shared_ptr<PixelBuffer>> CollectActiveDepthMaps(ViewManager& viewManager)
    {
        std::unordered_map<uint64_t, std::shared_ptr<PixelBuffer>> activeDepthMaps;

        viewManager.ForEachView([&](uint64_t viewID) {
            auto* view = viewManager.Get(viewID);
            if (!view || !view->gpu.linearDepthMap) {
                return;
            }

            const uint64_t resourceID = view->gpu.linearDepthMap->GetGlobalResourceID();
            activeDepthMaps[resourceID] = view->gpu.linearDepthMap;
        });

        return activeDepthMaps;
    }

    static bool HaveSameActiveDepthMaps(
        const std::unordered_map<uint64_t, std::shared_ptr<PixelBuffer>>& lhs,
        const std::unordered_map<uint64_t, std::shared_ptr<PixelBuffer>>& rhs)
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

    static uint32_t GetSliceCount(const PixelBuffer& map)
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
			m_pDownsampleConstants->Remove(it->second.pConstantsBufferView.get());
			m_perMapInfo.erase(it);
		}
	}

    void CreateOrUpdateMapInfo(uint64_t resourceID, const std::shared_ptr<PixelBuffer>& linearDepthMap) {
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

		for (uint32_t i = 0; i < constants.mips; ++i) {
			constants.mipUavDescriptorIndices[i] = linearDepthMap->GetUAVShaderVisibleInfo(i + 1).slot.index;
		}

        auto constantsView = m_pDownsampleConstants->Add();
        m_pDownsampleConstants->UpdateView(constantsView.get(), &constants);

        PerMapInfo mapInfo = {};
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

        m_perMapInfo[resourceID] = std::move(mapInfo);
    }

    void SyncMapInfos(const std::unordered_map<uint64_t, std::shared_ptr<PixelBuffer>>& activeDepthMaps) {
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
