#include "Render/GraphExtensions/ClusterLOD/CLodStreamingBeginFramePass.h"

#include <algorithm>
#include <tracy/Tracy.hpp>

#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/PassBuilders.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Managers/UploadInstance.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"

CLodStreamingBeginFramePass::CLodStreamingBeginFramePass(
    std::function<org::UploadInstance*()> getUploadInstance,
    std::shared_ptr<org::Buffer> loadCounter,
    std::shared_ptr<org::Buffer> loadRequestKeys,
    std::shared_ptr<org::Buffer> usedGroupsCounter,
    std::shared_ptr<org::Buffer> sourceGroupMismatchCounter,
    std::shared_ptr<org::Buffer> nonResidentBits,
    std::shared_ptr<org::Buffer> activeGroupsBits,
    std::shared_ptr<org::Buffer> runtimeState,
    std::function<bool(std::vector<uint32_t>&, uint32_t&, org::UploadInstance*)> queueNonResidentBitsUpload,
    std::function<bool(std::vector<uint32_t>&, uint32_t&)> getActiveGroupsBitsUpload,
    std::function<void()> scheduleStreamingReadbacks,
    std::function<void()> processStreamingRequests)
    : m_loadCounter(std::move(loadCounter))
    , m_loadRequestKeys(std::move(loadRequestKeys))
    , m_usedGroupsCounter(std::move(usedGroupsCounter))
    , m_sourceGroupMismatchCounter(std::move(sourceGroupMismatchCounter))
    , m_nonResidentBits(std::move(nonResidentBits))
    , m_activeGroupsBits(std::move(activeGroupsBits))
    , m_runtimeState(std::move(runtimeState))
    , m_queueNonResidentBitsUpload(std::move(queueNonResidentBitsUpload))
    , m_getActiveGroupsBitsUpload(std::move(getActiveGroupsBitsUpload))
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
    CLodStreamingBeginFrameBindings bindings{builder.BindUnorderedAccess(m_loadCounter),
        builder.BindUnorderedAccess(m_loadRequestKeys), builder.BindUnorderedAccess(m_usedGroupsCounter)};
    builder.WithUnorderedAccess(m_nonResidentBits, m_activeGroupsBits, m_runtimeState);
    if (m_sourceGroupMismatchCounter) {
        bindings.sourceMismatchCounter = builder.BindUnorderedAccess(m_sourceGroupMismatchCounter);
        bindings.hasSourceMismatchCounter = true;
    }
    return bindings;
}

br::render::PreparedComputeDispatchSequence CLodStreamingBeginFramePass::Prepare(
    const CLodStreamingBeginFrameBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputeDispatchSequence data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    auto program = preparation.CaptureProgramBinding(m_clearUintPipeline);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    auto appendClear = [&](org::ResourceBindingToken token, bool present, uint32_t value, uint32_t count) {
        if (!present || count == 0u) return;
        br::render::PreparedComputeDispatchSequence::Step step{};
        step.constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index;
        step.constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = value;
        step.constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
        step.groupsX = (count + 63u) / 64u;
        data.steps.push_back(step);
    };
    appendClear(bindings.loadCounter, true, 0u, 1u);
    appendClear(bindings.usedGroupsCounter, true, 0u, 1u);
    appendClear(bindings.sourceMismatchCounter, bindings.hasSourceMismatchCounter, 0u, 1u);
    appendClear(bindings.loadRequestKeys, true, 0xffffffffu, CLodStreamingRequestCapacity);
    return data;
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

    uint32_t activeGroupScanCount = 0u;
    {
        ZoneScopedN("CLodStreamingBeginFramePass::UploadActiveGroupsBits");
        const bool activeGroupsBitsUploadPending = m_getActiveGroupsBitsUpload
            && m_getActiveGroupsBitsUpload(m_activeGroupsBitsUploadScratch, activeGroupScanCount);
        if (activeGroupsBitsUploadPending && !m_activeGroupsBitsUploadScratch.empty()) {
            UploadBufferData(
                m_activeGroupsBitsUploadScratch.data(),
                static_cast<uint32_t>(m_activeGroupsBitsUploadScratch.size() * sizeof(uint32_t)),
                org::runtime::UploadTarget::FromShared(m_activeGroupsBits),
                0);
        }
    }

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

void CLodStreamingBeginFramePass::Record(const CLodStreamingBeginFrameBindings&,
    const br::render::PreparedComputeDispatchSequence& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatchSequence(data, recording);
}
