#pragma once

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
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

struct DownsampleMapBindings {
    org::ResourceBindingToken source, counter, constants;
    bool isArrayLike = false;
    unsigned int constantsIndex = 0;
    std::array<unsigned int, 3> dispatch{};
};

struct DownsampleBindings { std::vector<DownsampleMapBindings> maps; };

class DownsamplePass : public org::TypedRenderGraphPass<DownsamplePass,
    br::render::PreparedComputePipelineSequence, DownsampleBindings>, public org::IDynamicDeclaredResources {
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
        bindings.maps.reserve(m_perMapInfo.size());
        for (auto& [resourceID, map] : m_perMapInfo) {
            (void)resourceID;
            auto source = declaration.BindShaderResource(
                Subresources(map.sourceMap, org::Mip{0, 1}));
            declaration.WithUnorderedAccess(Subresources(map.sourceMap, org::FromMip{1}));
            auto counter = declaration.BindUnorderedAccess(map.pCounterResource);
            auto constants = declaration.BindShaderResource(map.constantsBuffer);
            auto frozenConstants = map.constants;
            for (uint32_t i = 0; i < frozenConstants.mips; ++i) {
                frozenConstants.mipUavDescriptorIndices[i] = declaration.DeclaredBindlessIndex(
                    map.sourceMap, {org::BindlessViewKind::UnorderedAccess,
                        UINT32_MAX, i + 1u, 0u});
            }
            map.constantsBuffer->UpdateView(map.pConstantsBufferView.get(), &frozenConstants);
            bindings.maps.push_back({source, counter, constants, map.isArrayLike,
                map.constantsIndex, {map.dispatchThreadGroupCountXY[0],
                    map.dispatchThreadGroupCountXY[1], map.dispatchThreadGroupCountZ}});
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

    bool DeclaredResourcesChanged() const override {
        return m_declaredResourcesChanged;
    }

    br::render::PreparedComputePipelineSequence Prepare(const DownsampleBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputePipelineSequence data{};
        data.resourceHeap = context.textureDescriptorHeap.GetHandle();
        data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        const auto standard = preparation.CaptureProgramBinding(downsamplePassPSO);
        const auto array = preparation.CaptureProgramBinding(downsampleArrayPSO);
        data.steps.reserve(bindings.maps.size());
        for (const auto& map : bindings.maps) {
            const auto& program = map.isArrayLike ? array : standard;
            br::render::PreparedComputePipelineSequence::Step item{};
            item.program = program.program;
            item.descriptorIndices = program.descriptorIndices;
            item.constants[UintRootConstant0] = preparation.ResolveView(map.counter,
                {org::BindlessViewKind::UnorderedAccess}).index;
            item.constants[UintRootConstant1] = map.isArrayLike
                ? preparation.ResolveView(map.source, {org::BindlessViewKind::ShaderResource,
                    static_cast<uint32_t>(org::SRVViewType::Texture2DArray)}).index
                : preparation.ResolveView(map.source,
                    {org::BindlessViewKind::ShaderResource}).index;
            item.constants[UintRootConstant2] = preparation.ResolveView(map.constants,
                {org::BindlessViewKind::ShaderResource}).index;
            item.constants[UintRootConstant3] = map.constantsIndex;
            item.groupsX = map.dispatch[0];
            item.groupsY = map.dispatch[1];
            item.groupsZ = map.dispatch[2];
            data.steps.push_back(std::move(item));
        }
        return data;
    }

    static void Record(const DownsampleBindings&,
        const br::render::PreparedComputePipelineSequence& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputePipelineSequence(data, recording);
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
