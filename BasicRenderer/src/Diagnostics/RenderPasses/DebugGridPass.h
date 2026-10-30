#pragma once

#include <cstdint>
#include <cstring>   // memcpy
#include <stdexcept>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "../shaders/PerPassRootConstants/debugGridRootConstants.h"

// Uses MiscUintRootSignatureIndex / UintRootConstantN in HLSL.

struct DebugGridParams
{
    bool  enabled = true;

    // Grid plane: Y = planeY, grid aligned to world XZ.
    float planeY = 0.0f;

    // World-space cell sizes.
    float minorCellSize = 1.0f;
    float majorCellSize = 10.0f;

    // Line widths expressed as FULL fraction of the cell (0..1).
    // Example: 0.02 => 2% of cell width.
    float minorLineWidth = 0.02f;
    float majorLineWidth = 0.04f;

    // Axis line half-width in WORLD units (single line at X==0 and Z==0).
    float axisHalfWidthWorld = 0.03f;

    // Opacity scalars.
    float minorOpacity = 0.35f;
    float majorOpacity = 0.60f;
    float axisOpacity = 0.90f;

    // Master opacity multiplier for the entire pass.
    float overallOpacity = 1.0f;
};

struct DebugGridBindings {
    org::PipelineState pipeline;
    DebugGridParams params;
};
struct DebugGridFrame {
    uint32_t groupsX = 0, groupsY = 0;
};

class DebugGridPass final : public org::TypedRenderGraphPass<DebugGridPass, DebugGridFrame, DebugGridBindings>
{
public:
    using Params = DebugGridParams;

    DebugGridPass() : DebugGridPass(Params{}) {}
    explicit DebugGridPass(const Params& p)
        : m_params(p)
    {
        CreatePSO();
    }

    DebugGridBindings Declare(org::PassBuilder& declaration);
    DebugGridFrame Prepare(const DebugGridBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const DebugGridBindings& bindings, const DebugGridFrame& frame, org::PassRecordContext& context);

    const Params& GetParams() const { return m_params; }

private:
    org::PipelineState m_pso;

    Params m_params;

    static uint32_t PackFloat(float v)
    {
        uint32_t u;
        static_assert(sizeof(u) == sizeof(v));
        std::memcpy(&u, &v, sizeof(u));
        return u;
    }

    void CreatePSO()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/DebugGrid.hlsl",
            L"DebugGridCSMain",
            {},
            "DebugGridComputePSO"
        );
    }
};

inline DebugGridBindings DebugGridPass::Declare(org::PassBuilder& declaration) {
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    builder->ShaderResource(Builtin::CameraBuffer);
    builder->ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }));
    // Needs UAV, since compute will read-modify-write (manual blend)
    builder->UnorderedAccess(Builtin::Color::HDRColorTarget);
    builder->ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, m_params};
}

inline DebugGridFrame DebugGridPass::Prepare(const DebugGridBindings&, const org::PassPrepareContext& preparation) const {
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    return {(context.renderResolution.x + 7u) / 8u, (context.renderResolution.y + 7u) / 8u};
}

inline void DebugGridPass::Record(const DebugGridBindings& bindings, const DebugGridFrame& frame, org::PassRecordContext& context) {
    const auto& params = bindings.params;
    br::render::MiscConstants constants{};
    constants[RC_PlaneY] = PackFloat(params.planeY);
    constants[RC_MinorCellSize] = PackFloat(params.minorCellSize);
    constants[RC_MajorCellSize] = PackFloat(params.majorCellSize);
    constants[RC_MinorLineWidth] = PackFloat(params.minorLineWidth);
    constants[RC_MajorLineWidth] = PackFloat(params.majorLineWidth);
    constants[RC_AxisHalfWidthWorld] = PackFloat(params.axisHalfWidthWorld);
    constants[RC_MinorOpacity] = PackFloat(params.minorOpacity);
    constants[RC_MajorOpacity] = PackFloat(params.majorOpacity);
    constants[RC_AxisOpacity] = PackFloat(params.axisOpacity);
    constants[RC_OverallOpacity] = PackFloat(params.overallOpacity);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
