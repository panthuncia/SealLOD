#include "Render/GraphExtensions/ClusterLOD/ClearDeepVisibilityPass.h"

#include <vector>

#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

ClearDeepVisibilityPass::ClearDeepVisibilityPass(
    std::shared_ptr<org::Buffer> deepVisibilityCounterBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityOverflowCounterBuffer,
    std::shared_ptr<org::Buffer> deepVisibilityStatsBuffer)
    : m_deepVisibilityCounterBuffer(std::move(deepVisibilityCounterBuffer))
    , m_deepVisibilityOverflowCounterBuffer(std::move(deepVisibilityOverflowCounterBuffer))
    , m_deepVisibilityStatsBuffer(std::move(deepVisibilityStatsBuffer)) {
}

ClearDeepVisibilityBindings ClearDeepVisibilityPass::Declare(org::PassBuilder& declaration)
{
    auto* builder = &declaration;
    builder->WithUnorderedAccess(
        m_deepVisibilityCounterBuffer,
        m_deepVisibilityOverflowCounterBuffer,
        m_deepVisibilityStatsBuffer);
    ClearDeepVisibilityBindings bindings;
    bindings.headPointers.reserve(m_headPointerTextures.size());
    for (auto& texture : m_headPointerTextures)
        bindings.headPointers.push_back(builder->BindUnorderedAccess(texture));
    return bindings;
}



void ClearDeepVisibilityPass::Update(const org::UpdateExecutionContext& executionContext)
{
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    const uint32_t zero = 0u;
    if (m_deepVisibilityCounterBuffer) {
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_deepVisibilityCounterBuffer), 0);
    }
    if (m_deepVisibilityOverflowCounterBuffer) {
        UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_deepVisibilityOverflowCounterBuffer), 0);
    }
    if (m_deepVisibilityStatsBuffer) {
        const CLodDeepVisibilityStats zeroStats{};
        UploadBufferData(&zeroStats, sizeof(CLodDeepVisibilityStats), org::runtime::UploadTarget::FromShared(m_deepVisibilityStatsBuffer), 0);
    }

    std::vector<std::shared_ptr<org::PixelBuffer>> headPointerTextures;
    for (const auto& view : context.Views())
        if (view.visibilityBuffer && view.deepVisibilityHeadPointers)
            headPointerTextures.push_back(view.deepVisibilityHeadPointers);

    m_declaredResourcesChanged = m_headPointerTextures != headPointerTextures;
    m_headPointerTextures = std::move(headPointerTextures);
}

bool ClearDeepVisibilityPass::DeclaredResourcesChanged() const
{
    return m_declaredResourcesChanged;
}

br::render::PreparedResourceClears ClearDeepVisibilityPass::Prepare(
    const ClearDeepVisibilityBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedResourceClears data{};
    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    for (const auto binding : bindings.headPointers) {
        data.clears.push_back({preparation.CaptureResource(binding),
            preparation.CaptureView(binding,
                {org::BindlessViewKind::NonShaderVisibleUnorderedAccess}),
            preparation.CaptureView(binding, {org::BindlessViewKind::UnorderedAccess}),
            0.0f, 0xFFFFFFFFu, false});
    }
    return data;
}

void ClearDeepVisibilityPass::Record(const ClearDeepVisibilityBindings&,
    const br::render::PreparedResourceClears& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedResourceClears(data, recording);
}
