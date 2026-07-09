#include "Render/GraphExtensions/ClusterLOD/CLodStreamingBeginFramePass.h"

#include <algorithm>
#include <cstdlib>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/PassBuilders.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Managers/UploadInstance.h"
#include "BuiltinResources.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"

namespace {
bool SarpClodImportDebugLoggingEnabled() {
    static const bool enabled = [] {
#if defined(_WIN32)
        char* env = nullptr;
        size_t envSize = 0;
        const errno_t err = _dupenv_s(&env, &envSize, "SARP_DEBUG_CLOD_IMPORT");
        const bool result = err == 0 && env != nullptr && env[0] != '\0' && env[0] != '0';
        std::free(env);
        return result;
#else
        const char* env = std::getenv("SARP_DEBUG_CLOD_IMPORT");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
#endif
    }();
    return enabled;
}
}

CLodStreamingBeginFramePass::CLodStreamingBeginFramePass(
    std::function<UploadInstance*()> getUploadInstance,
    std::shared_ptr<Buffer> loadCounter,
    std::shared_ptr<Buffer> loadRequestKeys,
    std::shared_ptr<Buffer> usedGroupsCounter,
    std::shared_ptr<Buffer> sourceGroupMismatchCounter,
    std::shared_ptr<Buffer> nonResidentBits,
    std::shared_ptr<Buffer> activeGroupsBits,
    std::shared_ptr<Buffer> runtimeState,
    std::function<bool(std::vector<uint32_t>&, uint32_t&)> tryConsumeNonResidentBitsUpload,
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
    , m_tryConsumeNonResidentBitsUpload(std::move(tryConsumeNonResidentBitsUpload))
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

void CLodStreamingBeginFramePass::DeclareResourceUsages(ComputePassBuilder* builder) {
    builder->WithUnorderedAccess(m_loadCounter, m_loadRequestKeys, m_usedGroupsCounter, m_nonResidentBits, m_activeGroupsBits, m_runtimeState);
    if (m_sourceGroupMismatchCounter) {
        builder->WithUnorderedAccess(m_sourceGroupMismatchCounter);
    }
}

void CLodStreamingBeginFramePass::Setup() {}

PassReturn CLodStreamingBeginFramePass::Execute(PassExecutionContext& executionContext) {
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;
    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

    auto clearUintBuffer = [&](const std::shared_ptr<Buffer>& buffer, uint32_t value, uint32_t count) {
        if (!buffer || count == 0u) {
            return;
        }

        BindResourceDescriptorIndices(commandList, m_clearUintPipeline.GetResourceDescriptorSlots());
        commandList.BindPipeline(m_clearUintPipeline.GetAPIPipelineState().GetHandle());

        uint32_t clearRootConstants[NumMiscUintRootConstants] = {};
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = buffer->GetUAVShaderVisibleInfo(0).slot.index;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = value;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = count;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch((count + 63u) / 64u, 1u, 1u);
    };

    {
        ZoneScopedN("CLodStreamingBeginFramePass::ClearFeedbackCounters");
        clearUintBuffer(m_loadCounter, 0u, 1u);
        clearUintBuffer(m_usedGroupsCounter, 0u, 1u);
        clearUintBuffer(m_sourceGroupMismatchCounter, 0u, 1u);
    }

    if (m_loadRequestKeys) {
        ZoneScopedN("CLodStreamingBeginFramePass::ClearRequestKeys");
        clearUintBuffer(m_loadRequestKeys, 0xffffffffu, CLodStreamingRequestCapacity);
    }
    return {};
}

void CLodStreamingBeginFramePass::Update(const UpdateExecutionContext& executionContext) {
    ZoneScopedN("CLodStreamingBeginFramePass::Update");

    auto* updateContext = executionContext.hostData ? executionContext.hostData->Get<UpdateContext>() : nullptr;
    if (!updateContext) {
        return;
    }

    // Retire upload-heap pages from completed frames.
    UploadInstance* uploadInstance = m_getUploadInstance ? m_getUploadInstance() : nullptr;
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
        if (SarpClodImportDebugLoggingEnabled()) {
            spdlog::info(
                "SARPDBG CLodBeginFrame activeGroups uploadPending={} scanCount={} uploadWords={}",
                activeGroupsBitsUploadPending ? 1 : 0,
                activeGroupScanCount,
                static_cast<uint32_t>(m_activeGroupsBitsUploadScratch.size()));
        }
        if (activeGroupsBitsUploadPending && !m_activeGroupsBitsUploadScratch.empty()) {
            BUFFER_UPLOAD(
                m_activeGroupsBitsUploadScratch.data(),
                static_cast<uint32_t>(m_activeGroupsBitsUploadScratch.size() * sizeof(uint32_t)),
                rg::runtime::UploadTarget::FromShared(m_activeGroupsBits),
                0);
        }
    }

    {
        ZoneScopedN("CLodStreamingBeginFramePass::UploadRuntimeState");
        CLodStreamingRuntimeState state{};
        state.activeGroupScanCount = activeGroupScanCount;
        state.unloadAfterFrames = 0u;
        state.activeGroupsBitsetWordCount = CLodBitsetWordCount(activeGroupScanCount);
        BUFFER_UPLOAD(
            &state,
            sizeof(CLodStreamingRuntimeState),
            rg::runtime::UploadTarget::FromShared(m_runtimeState),
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
            hasNonResidentBitsUpload = m_tryConsumeNonResidentBitsUpload
                && m_tryConsumeNonResidentBitsUpload(m_nonResidentBitsUploadScratch, nonResidentFirstWord);
        }
        TracyPlot(
            "CLodBeginFrame.NonResidentBits.UploadWords",
            static_cast<int64_t>(hasNonResidentBitsUpload ? m_nonResidentBitsUploadScratch.size() : 0u));
        if (hasNonResidentBitsUpload && !m_nonResidentBitsUploadScratch.empty()) {
            ZoneScopedN("CLodStreamingBeginFramePass::UploadNonResidentBits::UploadData");
            uploadInstance->UploadData(
                m_nonResidentBitsUploadScratch.data(),
                static_cast<uint32_t>(m_nonResidentBitsUploadScratch.size() * sizeof(uint32_t)),
                rg::runtime::UploadTarget::FromShared(m_nonResidentBits),
                nonResidentFirstWord * sizeof(uint32_t));
        }
    }
}

void CLodStreamingBeginFramePass::Cleanup() {}
