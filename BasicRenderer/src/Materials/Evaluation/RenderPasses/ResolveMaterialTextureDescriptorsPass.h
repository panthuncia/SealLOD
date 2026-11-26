#pragma once
#include <functional>
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"
#include "Runtime/Settings/SettingsManager.h"
#include "../shaders/PerPassRootConstants/materialTextureResolveConstants.h"

inline constexpr const char* kMaterialResolvedTextureDescriptorsResource = "Builtin::Material::ResolvedTextureDescriptors";

struct ResolveMaterialTextureDescriptorsBindings {
    br::PipelineState pipeline;
};
struct ResolveMaterialTextureDescriptorsFrame {
    uint32_t groups = 0;
    bool operator==(const ResolveMaterialTextureDescriptorsFrame&) const = default;
};

// Writes, for each row of the material evaluation table, the descriptor each texture slot samples this frame
// (materialTextureResolve.hlsl), so material evaluation reads one row by material index instead of chaining through each
// texture's streaming row per pixel. Runs every frame before EvaluateMaterialGroupsPass, one thread a table row.
//
// TODO: rework texture streaming so material evaluation's descriptors are current without this pass: publish the resolved
// descriptor with the material (or patch the material evaluation table) when a streamed image's descriptor changes, as
// part of the streaming update, and drop this pass and its table.
class ResolveMaterialTextureDescriptorsPass : public org::TypedRenderGraphPass<ResolveMaterialTextureDescriptorsPass,
    ResolveMaterialTextureDescriptorsFrame, ResolveMaterialTextureDescriptorsBindings> {
public:
    ResolveMaterialTextureDescriptorsBindings Declare(org::PassBuilder& b) {
        b.ShaderResource("Builtin::PerMaterialEvalDataBuffer", Builtin::Material::TextureStreamingMetadataBuffer);
        b.ConstantBuffer(Builtin::PerFrameBuffer); // The texture image table's index (materialTextureBinding.hlsli).
        b.UnorderedAccess(kMaterialResolvedTextureDescriptorsResource);
        b.PreferQueue(org::QueueKind::Compute);
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"shaders/materialTextureResolve.hlsl",
            L"ResolveMaterialTextureDescriptorsCS", "VisUtil_ResolveMaterialTextureDescriptorsPSO"));
        return {m_pipeline};
    }

    // Kept recordings (M6 step 7): the dispatch covers the table's capacity setting, taken once per run
    // (RenderGraphPass::TakeFrameValues) and read by Prepare. The table's contents and its image table index reach the shader
    // through GPU memory (the per-frame buffer), so they need no revision.
    void TakeFrameValues(const org::UpdateExecutionContext&) override { m_takenCapacity = m_capacity(); }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        br::render::AppendRevision(out, m_takenCapacity);
    }

    ResolveMaterialTextureDescriptorsFrame Prepare(const ResolveMaterialTextureDescriptorsBindings&,
        const org::PassPrepareContext&) const {
        // The shader stops at the evaluation table's length, read on the GPU, so the dispatch covers the table's capacity.
        const uint32_t rows = std::max(MATERIAL_RESOLVED_TEXTURE_MIN_CAPACITY, m_takenCapacity);
        return {(rows + MATERIAL_RESOLVED_TEXTURE_RESOLVE_GROUP_SIZE - 1u) / MATERIAL_RESOLVED_TEXTURE_RESOLVE_GROUP_SIZE};
    }

    static void Record(const ResolveMaterialTextureDescriptorsBindings& bindings, const ResolveMaterialTextureDescriptorsFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groups);
    }

private:
    std::function<uint32_t()> m_capacity =
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(MaterialResolvedTextureCapacitySettingName);
    br::PipelineState m_pipeline;
    uint32_t m_takenCapacity = 0;
};
