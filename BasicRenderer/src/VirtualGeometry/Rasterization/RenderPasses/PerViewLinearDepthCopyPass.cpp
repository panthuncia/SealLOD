#include "VirtualGeometry/Rasterization/RenderPasses/PerViewLinearDepthCopyPass.h"
#include "Render/PassBuilders.h"

#include "Scene/Views/ViewManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/PixelBuffer.h"
#include "Utilities/Utilities.h"

#include <algorithm>
#include <tuple>

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
            builder.UnorderedAccess(view.linearDepth), view.visibility, view.primary});
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
    // Only what the declaration depends on: which buffers, and which view is primary. Size and projection go in frame
    // data.
    const auto declares = [](const std::vector<ViewSnapshot>& list) {
        std::vector<std::tuple<org::PixelBuffer*, org::PixelBuffer*, bool>> keys;
        for (const auto& view : list) keys.emplace_back(view.visibility.get(), view.linearDepth.get(), view.primary);
        return keys;
    };
    m_declaredResourcesChanged = declares(views) != declares(m_views);
    m_views = std::move(views);
}

PerViewLinearDepthCopyFrame PerViewLinearDepthCopyPass::Prepare(const PerViewLinearDepthCopyBindings& bindings,
    const org::PassPrepareContext&) const {
    PerViewLinearDepthCopyFrame frame;
    frame.views.resize(bindings.views.size());
    for (size_t i = 0; i < bindings.views.size(); ++i) {
        const auto found = std::find_if(m_views.begin(), m_views.end(),
            [&](const ViewSnapshot& view) { return view.visibility == bindings.views[i].source; });
        if (found == m_views.end()) continue;
        frame.views[i] = {found->width, found->height, found->projection};
    }
    return frame;
}

bool PerViewLinearDepthCopyPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

void PerViewLinearDepthCopyPass::Record(const PerViewLinearDepthCopyBindings& bindings,
    const PerViewLinearDepthCopyFrame& frame, org::PassRecordContext& context) {
    context.BindComputePipeline(bindings.pipeline);
    for (size_t i = 0; i < bindings.views.size(); ++i) {
        const auto& view = bindings.views[i];
        const auto& now = frame.views[i];
        if (!now.width || !now.height) continue;
        br::render::MiscConstants c{};
        c[UintRootConstant0] = context.Index(view.visibility);
        c[UintRootConstant1] = context.Index(view.linearDepth);
        c[UintRootConstant2] = now.width;
        c[UintRootConstant3] = now.height;
        if (view.primary && bindings.hasProjectedDepth) {
            c[UintRootConstant4] = context.Index(bindings.projectedDepth);
            c[UintRootConstant7] = bindings.hasCanonicalDeviceDepth
                ? context.Index(bindings.canonicalDeviceDepth) : 0xFFFFFFFFu;
            c[UintRootConstant5] = now.projection[0];
            c[UintRootConstant6] = now.projection[1];
        } else c[UintRootConstant4] = c[UintRootConstant7] = 0xFFFFFFFFu;
        br::render::PushMiscConstants(context, c);
        context.Commands().Dispatch((now.width + 7u) / 8u, (now.height + 7u) / 8u, 1);
    }
}
