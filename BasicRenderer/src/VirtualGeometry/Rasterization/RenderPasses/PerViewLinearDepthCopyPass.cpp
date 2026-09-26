#include "VirtualGeometry/Rasterization/RenderPasses/PerViewLinearDepthCopyPass.h"

#include "Scene/Views/ViewManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/PixelBuffer.h"
#include "Utilities/Utilities.h"

PerViewLinearDepthCopyPass::PerViewLinearDepthCopyPass(bool writeProjectedDepth)
    : m_writeProjectedDepth(writeProjectedDepth) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/canonicalSurface.hlsl",
        L"PerViewPrimaryDepthCopyCS",
        {},
        "PerViewPrimaryDepthCopyPSO");
}

PerViewLinearDepthCopyBindings PerViewLinearDepthCopyPass::Declare(org::PassBuilder& builder) {
    PerViewLinearDepthCopyBindings bindings{};
    bindings.views.reserve(m_views.size());
    for (const auto& view : m_views) {
        bindings.views.push_back({builder.BindShaderResource(view.visibility),
            builder.BindUnorderedAccess(view.linearDepth), view.width, view.height,
            view.primary, view.projection});
    }
    if (m_writeProjectedDepth) {
        bindings.projectedDepth = builder.BindUnorderedAccess(
            Builtin::PrimaryCamera::ProjectedDepthTexture);
        bindings.hasProjectedDepth = true;
        bindings.canonicalDeviceDepth = builder.BindUnorderedAccess(Builtin::Surface::DeviceDepth);
        bindings.hasCanonicalDeviceDepth = true;
    }
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

void PerViewLinearDepthCopyPass::Initialize() {
}

void PerViewLinearDepthCopyPass::Update(const org::UpdateExecutionContext& executionContext) {
    const auto* context = executionContext.hostData->Get<UpdateContext>();
    std::vector<ViewSnapshot> views;
    for (const auto& view : context->Views()) {
        if (!view.visibilityBuffer || !view.linearDepthMap) continue;
        const auto& projection = view.cameraInfo.unjitteredProjection;
        views.push_back({view.visibilityBuffer, view.linearDepthMap,
            view.visibilityBuffer->GetWidth(), view.visibilityBuffer->GetHeight(),
            view.primary,
            {as_uint(DirectX::XMVectorGetZ(projection.r[2])),
                as_uint(DirectX::XMVectorGetZ(projection.r[3]))}});
    }
    m_declaredResourcesChanged = views != m_views;
    m_views = std::move(views);
}

bool PerViewLinearDepthCopyPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

PerViewLinearDepthCopyPreparedData PerViewLinearDepthCopyPass::Prepare(
    const PerViewLinearDepthCopyBindings& bindings,
    const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    PreparedData data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    data.program = preparation.CaptureProgram(m_pso);
    for (const auto& view : bindings.views) {
        PreparedView item{};
        item.constants.resize(NumMiscUintRootConstants);
        auto& c = item.constants;
        c[UintRootConstant0] = preparation.ResolveView(view.visibility,
            {org::BindlessViewKind::ShaderResource}).index;
        c[UintRootConstant1] = preparation.ResolveView(view.linearDepth,
            {org::BindlessViewKind::UnorderedAccess}).index;
        c[UintRootConstant2] = view.width;
        c[UintRootConstant3] = view.height;
        if (m_writeProjectedDepth && view.primary && bindings.hasProjectedDepth) {
            c[UintRootConstant4] = preparation.ResolveView(bindings.projectedDepth,
                {org::BindlessViewKind::UnorderedAccess}).index;
            c[UintRootConstant7] = bindings.hasCanonicalDeviceDepth
                ? preparation.ResolveView(bindings.canonicalDeviceDepth,
                    {org::BindlessViewKind::UnorderedAccess}).index : 0xFFFFFFFFu;
            c[UintRootConstant5] = view.projection[0];
            c[UintRootConstant6] = view.projection[1];
        } else c[UintRootConstant4] = c[UintRootConstant7] = 0xFFFFFFFFu;
        item.groupsX = (c[UintRootConstant2] + 7u) / 8u;
        item.groupsY = (c[UintRootConstant3] + 7u) / 8u;
        data.views.push_back(std::move(item));
    }
    return data;
}

void PerViewLinearDepthCopyPass::Record(const PerViewLinearDepthCopyBindings&,
    const PreparedData& data, org::PassRecordContext& recording) {
    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
    commands.BindLayout(data.layout);
    commands.BindPipeline(recording.Resolve(data.program));
    for (const auto& view : data.views) {
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
            NumMiscUintRootConstants, view.constants.data());
        commands.Dispatch(view.groupsX, view.groupsY, 1);
    }
}

