#pragma once
#include "BuiltinResources.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/Base/PersistentTypedPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "../shaders/PerPassRootConstants/visUtilRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"

struct BuildPixelListBindings {
    org::ResourceBindingToken visibleClusters, reyesDiceQueue;
    bool hasReyesDiceQueue = false;
    uint32_t patchVisibilityIndexBase = 0;
};

struct BuildPixelListInvocation {
    uint32_t voxelMaterialBin = UINT32_MAX;
    uint32_t groupsX = 0, groupsY = 0;
};

class BuildPixelListPass : public org::TypedRenderGraphPass<BuildPixelListPass, BuildPixelListInvocation, BuildPixelListBindings, br::render::PreparedComputeDispatch> {
public:
    explicit BuildPixelListPass(const MaterialEvaluationBuildInputs& inputs)
        : m_visibleClusterResource(inputs.visibleClusters),
          m_reyesDiceQueueResource(inputs.reyesDiceQueue),
          m_patchVisibilityIndexBase(CLodReyesPatchVisibilityIndexBase(inputs.visibleClusterCapacity)) {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/VisUtil.hlsl",
            L"BuildPixelListCS",
            {},
            "BuildPixelListPSO");

        if (!m_visibleClusterResource)
            throw std::invalid_argument("BuildPixelListPass requires the published visible-cluster resource");
    }
    BuildPixelListBindings Declare(org::PassBuilder& b) {
        BuildPixelListBindings bindings{b.BindShaderResource(m_visibleClusterResource)};
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = b.BindShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;

        b.WithShaderResource(Builtin::PrimaryCamera::VisibilityTexture,
                              //Builtin::PrimaryCamera::VisibleClusterTable,
                              Builtin::PerMeshInstanceBuffer,
                              Builtin::InstanceDrawRecordBuffer,
                              Builtin::PerMeshBuffer,
                              Builtin::PerMaterialDataBuffer,
                              "Builtin::VisUtil::MaterialOffsetBuffer")
         .WithUnorderedAccess("Builtin::VisUtil::MaterialWriteCursorBuffer",
                              "Builtin::VisUtil::PixelListBuffer");
		b.WithConstantBuffer(Builtin::PerFrameBuffer)
         .PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    br::render::PreparedComputeDispatch BuildRecipe(const BuildPixelListBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        br::render::PreparedComputeDispatch data{};
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
        auto program = CaptureProgramBinding(preparation, m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(
            bindings.visibleClusters, {org::BindlessViewKind::ShaderResource}).index;
        data.constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
            ? preparation.ResolveView(bindings.reyesDiceQueue, {org::BindlessViewKind::ShaderResource}).index
            : 0xFFFFFFFFu;
        data.constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext&) const {
        return {reinterpret_cast<uintptr_t>(m_pso.PeekPayload())};
    }
    static BuildPixelListInvocation PrepareInvocation(const br::render::PreparedComputeDispatch&,
        const BuildPixelListBindings&, const org::PassPrepareContext& preparation) {
        if (!preparation.preparationData) throw std::logic_error("BuildPixelListPass requires frame context");
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        const auto* render = preparation.preparationData->Get<RenderContext>();
        if (!update && !render) throw std::logic_error("BuildPixelListPass requires frame context");
        BuildPixelListInvocation invocation;
        const auto& published = update ? update->publishedRendererState : render->publishedRendererState;
        const auto materialState = published
            ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        if (materialState) {
            const bool found = materialState->TryGetCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel,invocation.voxelMaterialBin);
            (void)found;
        }
        const auto resolution = update ? update->renderResolution : render->renderResolution;
        invocation.groupsX = (resolution.x + 7u) / 8u;
        invocation.groupsY = (resolution.y + 7u) / 8u;
        return invocation;
    }
    static void Record(const br::render::PreparedComputeDispatch& data, const BuildPixelListInvocation& invocation,
        org::PassRecordContext& recording) {
        auto constants = data.constants;
        constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = invocation.voxelMaterialBin;
        br::render::RecordPreparedComputeDispatchWithInvocation(data,constants,invocation.groupsX,invocation.groupsY,1,recording);
    }

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<org::GloballyIndexedResource> m_reyesDiceQueueResource;
	uint32_t m_patchVisibilityIndexBase = 0u;
};

// Producer-facing author for the persistent execution mode. The caller supplies
// logical slots and an immutable native program; no resolver or manager is
// retained by the selected executable. The legacy pass remains available
// until renderer publications can install persistent graph edits atomically.
class PersistentBuildPixelListPass {
public:
    struct Slots {
        org::persistent::ResourceSlotId visibleClusters;
        std::optional<org::persistent::ResourceSlotId> reyesDiceQueue;
        // Visibility, mesh instances, draw records, meshes, materials, offsets.
        std::array<org::persistent::ResourceSlotId,6> shaderInputs;
        // Material write cursor and pixel list.
        std::array<org::persistent::ResourceSlotId,2> outputs;
        org::persistent::ResourceSlotId perFrame;
        uint32_t patchVisibilityIndexBase = 0;
    };
    struct Bindings {
        org::persistent::ViewToken visibleClusters;
        std::optional<org::persistent::ViewToken> reyesDiceQueue;
        std::array<org::persistent::ViewToken,9> programDescriptors;
        uint32_t patchVisibilityIndexBase = 0;
    };
    struct ProgramBuildInputs {
        br::render::PreparedComputeDispatch native;
        // Reflected root-slot order, indexing Bindings::programDescriptors.
        std::vector<uint32_t> descriptorOrder;
    };
    struct ProgramInterface {
        br::render::PreparedComputeDispatch native;
        std::vector<org::persistent::ViewToken> descriptors;
    };
    using Invocation = BuildPixelListInvocation;
    explicit PersistentBuildPixelListPass(Slots slots) : m_slots(std::move(slots)) {}
    Bindings Declare(org::persistent::PassDeclaration& declaration) const {
        using Access = rhi::ResourceAccessType;
        const auto state = [](Access access, bool write = false) {
            return org::experimental::CompileResourceState{static_cast<uint64_t>(access),0,
                static_cast<uint64_t>(rhi::ResourceSyncState::ComputeShading),write};
        };
        Bindings bindings;
        bindings.patchVisibilityIndexBase = m_slots.patchVisibilityIndexBase;
        bindings.visibleClusters = declaration.View(declaration.Resource(m_slots.visibleClusters,state(Access::ShaderResource)));
        if (m_slots.reyesDiceQueue)
            bindings.reyesDiceQueue = declaration.View(declaration.Resource(*m_slots.reyesDiceQueue,state(Access::ShaderResource)));
        for (size_t i = 0; i < m_slots.shaderInputs.size(); ++i)
            bindings.programDescriptors[i] = declaration.View(declaration.Resource(m_slots.shaderInputs[i],state(Access::ShaderResource)));
        for (size_t i = 0; i < m_slots.outputs.size(); ++i)
            bindings.programDescriptors[6+i] = declaration.View(declaration.Resource(m_slots.outputs[i],state(Access::UnorderedAccess,true)),
                {org::BindlessViewKind::UnorderedAccess});
        bindings.programDescriptors[8] = declaration.View(declaration.Resource(m_slots.perFrame,state(Access::ConstantBuffer)),
            {org::BindlessViewKind::ConstantBuffer});
        return bindings;
    }
    ProgramInterface BuildProgramInterface(const Bindings& bindings, const ProgramBuildInputs& inputs) const {
        const auto& program = inputs.native;
        if (program.program || !program.descriptorIndices.empty())
            throw std::invalid_argument("Persistent pixel-list program requires native bindings and symbolic descriptors");
        if (!program.pipeline.valid() || !program.layout.valid())
            throw std::invalid_argument("Persistent pixel-list program is incomplete");
        if (inputs.descriptorOrder.size() > NumMiscUintRootConstants)
            throw std::invalid_argument("Persistent pixel-list descriptor layout exceeds root capacity");
        ProgramInterface result{program,{}};
        for (auto index : inputs.descriptorOrder) result.descriptors.push_back(bindings.programDescriptors.at(index));
        return result;
    }
    static Invocation PrepareInvocation(const ProgramInterface& program, const Bindings&,
        const org::PassPrepareContext& preparation);
    static void Record(const ProgramInterface& program, const Bindings& bindings,
        const Invocation& invocation, org::RecordingContext& recording);
private:
    Slots m_slots;
};

inline BuildPixelListInvocation PersistentBuildPixelListPass::PrepareInvocation(
    const ProgramInterface& program, const Bindings&, const org::PassPrepareContext& preparation) {
    return BuildPixelListPass::PrepareInvocation(program.native,{},preparation);
}
inline void PersistentBuildPixelListPass::Record(const ProgramInterface& program, const Bindings& bindings,
    const Invocation& invocation, org::RecordingContext& recording) {
    if (!invocation.groupsX || !invocation.groupsY) return;
    std::array<uint32_t,NumMiscUintRootConstants> descriptors;
    if (program.descriptors.size() > descriptors.size()) throw std::out_of_range("Pixel-list descriptor root bounds");
    for (size_t i = 0; i < program.descriptors.size(); ++i) descriptors[i] = recording.Resolve(program.descriptors[i]).index;
    auto constants = program.native.constants;
    constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = recording.Resolve(bindings.visibleClusters).index;
    constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.reyesDiceQueue
        ? recording.Resolve(*bindings.reyesDiceQueue).index : UINT32_MAX;
    constants[VISBUF_REYES_PATCH_INDEX_BASE] = bindings.patchVisibilityIndexBase;
    constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = invocation.voxelMaterialBin;
    auto& commands = recording.Commands();
    br::render::BindPreparedDescriptorHeaps(commands,program.native.resourceHeap,program.native.samplerHeap);
    commands.BindLayout(program.native.layout);
    commands.BindPipeline(program.native.pipeline);
    if (!program.descriptors.empty()) commands.PushConstants(rhi::ShaderStage::Compute,0,org::shaderapi::kResourceDescriptorIndicesRootParameter,
        0,static_cast<uint32_t>(program.descriptors.size()),descriptors.data());
    commands.PushConstants(rhi::ShaderStage::Compute,0,MiscUintRootSignatureIndex,0,NumMiscUintRootConstants,constants.data());
    commands.Dispatch(invocation.groupsX,invocation.groupsY,1);
}
