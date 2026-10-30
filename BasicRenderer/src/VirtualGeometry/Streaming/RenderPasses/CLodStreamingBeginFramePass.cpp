#include "VirtualGeometry/Streaming/RenderPasses/CLodStreamingBeginFramePass.h"

#include <algorithm>
#include <tracy/Tracy.hpp>

#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Managers/UploadInstance.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"

CLodStreamingBeginFramePass::CLodStreamingBeginFramePass(
    std::function<org::UploadInstance*()> getUploadInstance,
    std::shared_ptr<org::Buffer> loadCounter,
    std::shared_ptr<org::Buffer> loadRequestKeys,
    std::shared_ptr<org::Buffer> usedGroupsCounter,
    std::shared_ptr<org::Buffer> sourceGroupMismatchCounter,
    std::shared_ptr<org::Buffer> runtimeState,
    std::function<bool(std::vector<uint32_t>&, uint32_t&, org::UploadInstance*)> queueNonResidentBitsUpload,
    std::function<uint32_t(const UpdateContext&)> getActiveGroupScanCount,
    std::function<void()> scheduleStreamingReadbacks,
    std::function<void()> processStreamingRequests)
    : m_loadCounter(std::move(loadCounter))
    , m_loadRequestKeys(std::move(loadRequestKeys))
    , m_usedGroupsCounter(std::move(usedGroupsCounter))
    , m_sourceGroupMismatchCounter(std::move(sourceGroupMismatchCounter))
    , m_runtimeState(std::move(runtimeState))
    , m_queueNonResidentBitsUpload(std::move(queueNonResidentBitsUpload))
    , m_getActiveGroupScanCount(std::move(getActiveGroupScanCount))
    , m_scheduleStreamingReadbacks(std::move(scheduleStreamingReadbacks))
    , m_processStreamingRequests(std::move(processStreamingRequests))
    , m_getUploadInstance(std::move(getUploadInstance))
{
    m_clearUintPipeline = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain",
        {},
        "CLodStreamingBeginFrameClearUint");
}

CLodStreamingBeginFrameBindings CLodStreamingBeginFramePass::Declare(org::PassBuilder& builder) {
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    CLodStreamingBeginFrameBindings bindings{m_clearUintPipeline, builder.UnorderedAccess(m_loadCounter),
        builder.UnorderedAccess(m_loadRequestKeys), builder.UnorderedAccess(m_usedGroupsCounter)};
    builder.UnorderedAccess(m_runtimeState);
    if (m_sourceGroupMismatchCounter) {
        bindings.sourceMismatchCounter = builder.UnorderedAccess(m_sourceGroupMismatchCounter);
        bindings.hasSourceMismatchCounter = true;
    }
    return bindings;
}

void CLodStreamingBeginFramePass::Update(const org::UpdateExecutionContext& executionContext) {
    ZoneScopedN("CLodStreamingBeginFramePass::Update");

    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    if (!updateContext) {
        return;
    }

    // Retire upload-heap pages from completed frames.
    org::UploadInstance* uploadInstance = m_getUploadInstance ? m_getUploadInstance() : nullptr;
    if (uploadInstance) {
        ZoneScopedN("CLodStreamingBeginFramePass::ProcessDeferredReleases");
        uploadInstance->ProcessDeferredReleases(static_cast<uint8_t>(executionContext.frameIndex));
    }

    if (m_scheduleStreamingReadbacks) {
        ZoneScopedN("CLodStreamingBeginFramePass::PollReadbacks");
        m_scheduleStreamingReadbacks();
    }
    if (m_processStreamingRequests) {
        ZoneScopedN("CLodStreamingBeginFramePass::ProcessStreamingRequests");
        m_processStreamingRequests();
    }

    // Bounded by the capacity of the residency bitset this frame's published
    // state binds, so the scan never reaches past it.
    const uint32_t activeGroupScanCount = m_getActiveGroupScanCount ? m_getActiveGroupScanCount(*updateContext) : 0u;

    {
        ZoneScopedN("CLodStreamingBeginFramePass::UploadRuntimeState");
        CLodStreamingRuntimeState state{};
        state.activeGroupScanCount = activeGroupScanCount;
        state.unloadAfterFrames = 0u;
        state.activeGroupsBitsetWordCount = CLodBitsetWordCount(activeGroupScanCount);
        UploadBufferData(
            &state,
            sizeof(CLodStreamingRuntimeState),
            org::runtime::UploadTarget::FromShared(m_runtimeState),
            0);
    }

    // nonResidentBits stays on UploadInstance so it arrives in the same copy
    // batch as slab page data — residency is never advertised before data lands.
    if (!uploadInstance) return;

    uint32_t nonResidentFirstWord = 0u;
    {
        ZoneScopedN("CLodStreamingBeginFramePass::UploadNonResidentBits");
        bool hasNonResidentBitsUpload = false;
        {
            ZoneScopedN("CLodStreamingBeginFramePass::UploadNonResidentBits::Consume");
            hasNonResidentBitsUpload = m_queueNonResidentBitsUpload
                && m_queueNonResidentBitsUpload(
                    m_nonResidentBitsUploadScratch,
                    nonResidentFirstWord,
                    uploadInstance);
        }
        TracyPlot(
            "CLodBeginFrame.NonResidentBits.UploadWords",
            static_cast<int64_t>(hasNonResidentBitsUpload ? m_nonResidentBitsUploadScratch.size() : 0u));
    }
}

void CLodStreamingBeginFramePass::Record(const CLodStreamingBeginFrameBindings& bindings,
    org::PassRecordContext& context) {
    context.BindComputePipeline(bindings.clearUint);
    const auto clear = [&](const org::DeclaredViewToken& token, uint32_t value, uint32_t count) {
        br::render::MiscConstants constants{};
        constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = context.Index(token);
        constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = value;
        constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
        br::render::PushMiscConstants(context, constants);
        context.Commands().Dispatch((count + 63u) / 64u, 1, 1);
    };
    clear(bindings.loadCounter, 0u, 1u);
    clear(bindings.usedGroupsCounter, 0u, 1u);
    if (bindings.hasSourceMismatchCounter) clear(bindings.sourceMismatchCounter, 0u, 1u);
    clear(bindings.loadRequestKeys, 0xffffffffu, CLodStreamingRequestCapacity);
}
