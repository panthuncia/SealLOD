#pragma once

#include <cstdint>
#include <cstring>   // memcpy
#include <stdexcept>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "../shaders/PerPassRootConstants/debugGridRootConstants.h"

// Uses MiscUintRootSignatureIndex / UintRootConstantN in HLSL.

class DebugGridPass final : public org::TypedRenderGraphPass<DebugGridPass, br::render::PreparedComputeDispatch>
{
public:
    struct Params
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

    DebugGridPass() : DebugGridPass(Params{}) {}
    explicit DebugGridPass(const Params& p)
        : m_params(p)
    {
        CreatePSO();
    }

    void Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
        builder
            ->WithShaderResource(Builtin::CameraBuffer,
                Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }))
            // Needs UAV, since compute will read-modify-write (manual blend)
            .WithUnorderedAccess(Builtin::Color::HDRColorTarget);
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation) {

        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context.textureDescriptorHeap.GetHandle();
        data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[RC_PlaneY] = PackFloat(m_params.planeY);
        data.constants[RC_MinorCellSize] = PackFloat(m_params.minorCellSize);
        data.constants[RC_MajorCellSize] = PackFloat(m_params.majorCellSize);
        data.constants[RC_MinorLineWidth] = PackFloat(m_params.minorLineWidth);
        data.constants[RC_MajorLineWidth] = PackFloat(m_params.majorLineWidth);
        data.constants[RC_AxisHalfWidthWorld] = PackFloat(m_params.axisHalfWidthWorld);
        data.constants[RC_MinorOpacity] = PackFloat(m_params.minorOpacity);
        data.constants[RC_MajorOpacity] = PackFloat(m_params.majorOpacity);
        data.constants[RC_AxisOpacity] = PackFloat(m_params.axisOpacity);
        data.constants[RC_OverallOpacity] = PackFloat(m_params.overallOpacity);
        data.groupsX = (context.renderResolution.x + 7u) / 8u;
        data.groupsY = (context.renderResolution.y + 7u) / 8u;
        return data;
    }

    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
    }

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
