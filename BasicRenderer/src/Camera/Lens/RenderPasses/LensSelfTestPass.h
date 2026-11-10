#pragma once

#include <memory>

#include "BuiltinResources.h"
#include "Camera/Lens/LensManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "../shaders/Include/lens/lensDataLayout.h"

struct LensSelfTestFrameData {
    uint64_t sequence = 0; // 0: nothing asked for, no dispatch
    const org::PipelineState* pipeline = nullptr;
};

struct LensSelfTestBindings {
    org::DeclaredViewToken output;
};

// lens.selftest: while one is pending, traces the published lens's self-test rays through lensExplicit.hlsli (and, for a
// polynomial lens, its generated code in lensPolynomial.hlsli) into LensManager's output buffer, which LensManager::OnFrame
// reads back after the graph. The pipeline has the published lens's shader defines, so it is chosen per frame and
// compiled on a worker; no dispatch until it exists.
class LensSelfTestPass : public org::TypedRenderGraphPass<LensSelfTestPass, LensSelfTestFrameData, LensSelfTestBindings> {
public:
    explicit LensSelfTestPass(std::shared_ptr<br::lens::LensManager> lens) : m_lens(std::move(lens)) {}

    LensSelfTestBindings Declare(org::PassBuilder& builder) {
        LensSelfTestBindings bindings{};
        builder.ShaderResource(Builtin::RayTracing::LensData);
        bindings.output = builder.UnorderedAccess(m_lens->SelfTestOutput()).View();
        return bindings;
    }

    LensSelfTestFrameData Prepare(const LensSelfTestBindings&, const org::PassPrepareContext&) const {
        LensSelfTestFrameData frame{ m_lens->PendingSelfTest() };
        if (frame.sequence == 0u) return frame;
        const auto published = m_lens->Published();
        frame.pipeline = PSOManager::GetInstance().TryGetComputePipeline(L"Shaders/ClusterLOD/lensSelfTest.hlsl", L"LensSelfTestCS",
            br::lens::LensManager::ShaderDefines(*published), "Lens.SelfTest.PSO");
        if (!frame.pipeline) frame.sequence = 0u;
        return frame;
    }

    static void Record(const LensSelfTestBindings& bindings, const LensSelfTestFrameData& frame, org::PassRecordContext& recording) {
        if (frame.sequence == 0u || !frame.pipeline) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.output);
        constants[1] = static_cast<uint32_t>(frame.sequence);
        recording.BindComputePipeline(*frame.pipeline);
        br::render::PushMiscConstants(recording, constants);
        recording.Commands().Dispatch((LENS_MAX_SELFTEST_RAYS + 63u) / 64u, 1u, 1u);
    }

private:
    std::shared_ptr<br::lens::LensManager> m_lens;
};
