#include "VirtualGeometry/Rasterization/RenderPasses/PerViewLinearDepthCopyPass.h"
#include "Render/PassBuilders.h"

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
    bindings.pipeline = m_pso;
    bindings.views.reserve(m_views.size());
    for (const auto& view : m_views) {
        bindings.views.push_back({builder.ShaderResource(view.visibility),
            builder.UnorderedAccess(view.linearDepth), view.width, view.height,
            view.primary, view.projection});
    }
    if (m_writeProjectedDepth) {
        bindings.projectedDepth = builder.UnorderedAccess(
            Builtin::PrimaryCamera::ProjectedDepthTexture);
        bindings.hasProjectedDepth = true;
        bindings.canonicalDeviceDepth = builder.UnorderedAccess(Builtin::Surface::DeviceDepth);
        bindings.hasCanonicalDeviceDepth = true;
    }
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
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

void PerViewLinearDepthCopyPass::Record(const PerViewLinearDepthCopyBindings& bindings,
    org::PassRecordContext& context) {
    context.BindComputePipeline(bindings.pipeline);
    for (const auto& view : bindings.views) {
        br::render::MiscConstants c{};
        c[UintRootConstant0] = context.Index(view.visibility);
        c[UintRootConstant1] = context.Index(view.linearDepth);
        c[UintRootConstant2] = view.width;
        c[UintRootConstant3] = view.height;
        if (view.primary && bindings.hasProjectedDepth) {
            c[UintRootConstant4] = context.Index(bindings.projectedDepth);
            c[UintRootConstant7] = bindings.hasCanonicalDeviceDepth
                ? context.Index(bindings.canonicalDeviceDepth) : 0xFFFFFFFFu;
            c[UintRootConstant5] = view.projection[0];
            c[UintRootConstant6] = view.projection[1];
        } else c[UintRootConstant4] = c[UintRootConstant7] = 0xFFFFFFFFu;
        br::render::PushMiscConstants(context, c);
        context.Commands().Dispatch((view.width + 7u) / 8u, (view.height + 7u) / 8u, 1);
    }
}
