#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapInvalidatePagesPass.h"

#include <vector>

#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "BuiltinResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/RendererComponents.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowInvalidateRootConstants.h"

VirtualShadowMapInvalidatePagesPass::VirtualShadowMapInvalidatePagesPass(
    std::shared_ptr<Buffer> invalidationInputsBuffer,
    std::shared_ptr<Buffer> invalidationCountBuffer,
    std::shared_ptr<Buffer> invalidatedInstancesBitsetBuffer,
    std::shared_ptr<Buffer> clipmapInfoBuffer,
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> directionalPageViewInfoBuffer,
    std::shared_ptr<Buffer> statsBuffer)
    : m_invalidationInputsBuffer(std::move(invalidationInputsBuffer))
    , m_invalidationCountBuffer(std::move(invalidationCountBuffer))
    , m_invalidatedInstancesBitsetBuffer(std::move(invalidatedInstancesBitsetBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_directionalPageViewInfoBuffer(std::move(directionalPageViewInfoBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowInvalidatePagesCSMain",
        {},
        "CLod.VirtualShadow.InvalidatePages.PSO");
    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();

    m_transformChangedQuery = ecsWorld.query_builder<const Components::ObjectDrawInfo>()
        .with<Components::Active>()
        .with<Components::RenderTransformUpdated>()
        .without<Components::SkipShadowPass>()
        .build();
}

void VirtualShadowMapInvalidatePagesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            Builtin::Shadows::CLodCompactShadowCameras,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            m_invalidationInputsBuffer,
            m_invalidationCountBuffer,
            m_clipmapInfoBuffer)
        .WithUnorderedAccess(
            m_pageTableTexture,
            m_dirtyPageFlagsBuffer,
            m_pageMetadataBuffer,
            m_directionalPageViewInfoBuffer,
            m_statsBuffer);

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void VirtualShadowMapInvalidatePagesPass::Setup() {}

void VirtualShadowMapInvalidatePagesPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;

    std::vector<CLodVirtualShadowInvalidationInput> inputs;
    inputs.reserve(1024);
    std::vector<uint32_t> invalidatedInstancesBitset(CLodVirtualShadowMovedInstanceBitWordCount(), 0u);

    auto markInvalidatedInstance = [&invalidatedInstancesBitset](uint32_t drawRecordIndex) {
        if (drawRecordIndex >= CLodVirtualShadowMovedInstanceBitCapacity) {
            return;
        }

        invalidatedInstancesBitset[drawRecordIndex >> 5u] |= 1u << (drawRecordIndex & 31u);
    };

    m_transformChangedQuery.each([&](flecs::entity entity, const Components::ObjectDrawInfo& drawInfo) {
        const bool skinned = entity.has<Components::Skinned>();
        const uint32_t dynamicSkinnedClipmapCount =
            g_clodSkinnedShadowEffectiveDynamicClipmapCount.load(std::memory_order_relaxed);
        const uint32_t clipmapMask = skinned
            ? (dynamicSkinnedClipmapCount >= 32u
                ? 0u
                : (0xFFFFFFFFu << dynamicSkinnedClipmapCount))
            : 0xFFFFFFFFu;
        if (clipmapMask == 0u) {
            return;
        }
        uint32_t flags = 0u;
        flags |= CLodVirtualShadowInvalidationFlagUsePreviousBounds;
        flags |= CLodVirtualShadowInvalidationFlagUseCurrentBounds;

        for (uint32_t drawRecordIndex : drawInfo.instanceDrawRecordIndices) {
            if (inputs.size() >= CLodVirtualShadowMaxInvalidationInputs) {
                break;
            }

            CLodVirtualShadowInvalidationInput input{};
            input.perMeshInstanceBufferIndex = drawRecordIndex;
            input.flags = flags;
            input.clipmapMask = clipmapMask;
            inputs.push_back(input);
            markInvalidatedInstance(drawRecordIndex);
        }
    });

    m_pendingInputCount = static_cast<uint32_t>(inputs.size());
    if (!inputs.empty()) {
        BUFFER_UPLOAD(
            inputs.data(),
            static_cast<uint32_t>(inputs.size() * sizeof(CLodVirtualShadowInvalidationInput)),
            rg::runtime::UploadTarget::FromShared(m_invalidationInputsBuffer),
            0);
    }

    BUFFER_UPLOAD(
        &m_pendingInputCount,
        sizeof(m_pendingInputCount),
        rg::runtime::UploadTarget::FromShared(m_invalidationCountBuffer),
        0);

    BUFFER_UPLOAD(
        invalidatedInstancesBitset.data(),
        static_cast<uint32_t>(invalidatedInstancesBitset.size() * sizeof(uint32_t)),
        rg::runtime::UploadTarget::FromShared(m_invalidatedInstancesBitsetBuffer),
        0);
}

PassReturn VirtualShadowMapInvalidatePagesPass::Execute(PassExecutionContext& executionContext)
{
    if (m_pendingInputCount == 0u) {
        return {};
    }

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_INPUTS_DESCRIPTOR_INDEX] = m_invalidationInputsBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_INPUT_COUNT_DESCRIPTOR_INDEX] = m_invalidationCountBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_clipmapInfoBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_TABLE_DESCRIPTOR_INDEX] = m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_DIRTY_FLAGS_DESCRIPTOR_INDEX] = m_dirtyPageFlagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_METADATA_DESCRIPTOR_INDEX] = m_pageMetadataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = m_directionalPageViewInfoBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_INVALIDATE_STATS_DESCRIPTOR_INDEX] = m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    constexpr uint32_t kThreadsPerGroup = 64u;
    commandList.Dispatch((m_pendingInputCount + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1u, 1u);

    return {};
}

void VirtualShadowMapInvalidatePagesPass::Cleanup() {}
