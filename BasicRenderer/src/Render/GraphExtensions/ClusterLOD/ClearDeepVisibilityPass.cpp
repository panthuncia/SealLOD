#include "Render/GraphExtensions/ClusterLOD/ClearDeepVisibilityPass.h"

#include <vector>

#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

ClearDeepVisibilityPass::ClearDeepVisibilityPass(
    std::shared_ptr<Buffer> deepVisibilityCounterBuffer,
    std::shared_ptr<Buffer> deepVisibilityOverflowCounterBuffer,
    std::shared_ptr<Buffer> deepVisibilityStatsBuffer)
    : m_deepVisibilityCounterBuffer(std::move(deepVisibilityCounterBuffer))
    , m_deepVisibilityOverflowCounterBuffer(std::move(deepVisibilityOverflowCounterBuffer))
    , m_deepVisibilityStatsBuffer(std::move(deepVisibilityStatsBuffer)) {
}

void ClearDeepVisibilityPass::DeclareResourceUsages(RenderPassBuilder* builder)
{
    builder->WithUnorderedAccess(
        m_deepVisibilityCounterBuffer,
        m_deepVisibilityOverflowCounterBuffer,
        m_deepVisibilityStatsBuffer);
    for (auto& texture : m_headPointerTextures) {
        builder->WithUnorderedAccess(texture);
    }
}

void ClearDeepVisibilityPass::Setup()
{
}

void ClearDeepVisibilityPass::Update(const UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    const uint32_t zero = 0u;
    if (m_deepVisibilityCounterBuffer) {
        BUFFER_UPLOAD(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_deepVisibilityCounterBuffer), 0);
    }
    if (m_deepVisibilityOverflowCounterBuffer) {
        BUFFER_UPLOAD(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_deepVisibilityOverflowCounterBuffer), 0);
    }
    if (m_deepVisibilityStatsBuffer) {
        const CLodDeepVisibilityStats zeroStats{};
        BUFFER_UPLOAD(&zeroStats, sizeof(CLodDeepVisibilityStats), org::runtime::UploadTarget::FromShared(m_deepVisibilityStatsBuffer), 0);
    }

    std::vector<std::shared_ptr<PixelBuffer>> headPointerTextures;
    context.viewManager->ForEachView([&](uint64_t viewID) {
        auto* view = context.viewManager->Get(viewID);
        if (!view || !view->gpu.visibilityBuffer) {
            return;
        }

        auto headPointers = context.viewManager->EnsureCLodDeepVisibilityHeadPointers(viewID);
        if (headPointers) {
            headPointerTextures.push_back(std::move(headPointers));
        }
    });

    m_declaredResourcesChanged = m_headPointerTextures != headPointerTextures;
    m_headPointerTextures = std::move(headPointerTextures);
}

bool ClearDeepVisibilityPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

PassReturn ClearDeepVisibilityPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

    rhi::UavClearUint headPointerClearValue{};
    headPointerClearValue.v[0] = 0xFFFFFFFFu;
    headPointerClearValue.v[1] = 0xFFFFFFFFu;
    headPointerClearValue.v[2] = 0xFFFFFFFFu;
    headPointerClearValue.v[3] = 0xFFFFFFFFu;

    for (auto& texture : m_headPointerTextures) {
        rhi::UavClearInfo clearInfo{};
        clearInfo.cpuVisible = texture->GetUAVNonShaderVisibleInfo(0).slot;
        clearInfo.shaderVisible = texture->GetUAVShaderVisibleInfo(0).slot;
        clearInfo.resource = texture->GetAPIResource();
        commandList.ClearUavUint(clearInfo, headPointerClearValue);
    }

    return {};
}

void ClearDeepVisibilityPass::Cleanup()
{
}
