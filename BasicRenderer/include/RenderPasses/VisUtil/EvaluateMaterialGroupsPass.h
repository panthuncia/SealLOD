#pragma once
#include <vector>
#include <array>
#include <optional>
#include <unordered_map>
#include <span>
#include <cstdint>
#include <bit>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/MaterialManager.h"
#include "Render/RenderContext.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/IndirectCommand.h"
#include "Render/OutputTypes.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/ShaderVariantRequestService.h"
#include "Render/MaterialEvaluationBuildInputs.h"
#include "Resources/Buffers/PagePool.h"
#include "Resources/Buffers/DynamicBufferBase.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "RenderPasses/PreparedComputeCommands.h"

struct EvaluateMaterialGroupsBindings {
    org::ResourceBindingToken visibleClusters, visibleClusterTransformIndices;
    org::ResourceBindingToken reyesDiceQueue, reyesTessTableConfigs;
    org::ResourceBindingToken reyesTessTableVertices, reyesTessTableTriangles;
    bool hasReyesDiceQueue = false, hasReyesTessTables = false;
    uint32_t patchVisibilityIndexBase = 0;
};

struct MaterialEvaluationInvocation {
    std::array<uint32_t,NumMiscUintRootConstants> constants{};
};

class EvaluateMaterialGroupsPass : public org::TypedRenderGraphPass<EvaluateMaterialGroupsPass,
    MaterialEvaluationInvocation, EvaluateMaterialGroupsBindings, br::render::PreparedComputeIndirectSequence> {
public:
    EvaluateMaterialGroupsPass(MaterialEvaluationBuildInputs inputs, bool terrainRvtEnabled)
        : m_inputs(std::move(inputs)), m_terrainRvtEnabled(terrainRvtEnabled) {
        if (!m_inputs.IsValid()) throw std::invalid_argument("EvaluateMaterialGroupsPass requires captured build inputs");

        m_visibleClusterResource = m_inputs.visibleClusters;
        m_visibleClusterTransformIndicesResource = m_inputs.visibleClusterTransformIndices;
        m_reyesDiceQueueResource = m_inputs.reyesDiceQueue;
        m_reyesTessTableConfigsResource = m_inputs.reyesTessTableConfigs;
        m_reyesTessTableVerticesResource = m_inputs.reyesTessTableVertices;
        m_reyesTessTableTrianglesResource = m_inputs.reyesTessTableTriangles;
        m_patchVisibilityIndexBase = CLodReyesPatchVisibilityIndexBase(m_inputs.visibleClusterCapacity);
        m_slabResourceGroup = m_inputs.clodSlabResources;
    }

    EvaluateMaterialGroupsBindings Declare(org::PassBuilder& builder) {
        EvaluateMaterialGroupsBindings bindings{};
        bindings.visibleClusters = builder.BindShaderResource(m_visibleClusterResource);
        bindings.visibleClusterTransformIndices = builder.BindShaderResource(m_visibleClusterTransformIndicesResource);
        if (m_reyesDiceQueueResource) {
            bindings.reyesDiceQueue = builder.BindShaderResource(m_reyesDiceQueueResource);
            bindings.hasReyesDiceQueue = true;
        }
        if (m_reyesTessTableConfigsResource && m_reyesTessTableVerticesResource && m_reyesTessTableTrianglesResource) {
            bindings.reyesTessTableConfigs = builder.BindShaderResource(m_reyesTessTableConfigsResource);
            bindings.reyesTessTableVertices = builder.BindShaderResource(m_reyesTessTableVerticesResource);
            bindings.reyesTessTableTriangles = builder.BindShaderResource(m_reyesTessTableTrianglesResource);
            bindings.hasReyesTessTables = true;
        }
        bindings.patchVisibilityIndexBase = m_patchVisibilityIndexBase;
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* b = &builder;

        if (m_slabResourceGroup) {
            b->WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
        }

        b->WithShaderResource("Builtin::VisUtil::PixelListBuffer",
            Builtin::PrimaryCamera::VisibilityTexture,
            Builtin::PrimaryCamera::LinearDepthMap,
            //Builtin::PrimaryCamera::VisibleClusterTable,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerObjectBuffer,
            Builtin::PerMeshBuffer,
            Builtin::CameraBuffer,
            Builtin::NormalMatrixBuffer,
            Builtin::SkeletonResources::InverseBindMatrices,
            Builtin::SkeletonResources::BoneTransforms,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::PerMaterialDataBuffer,
            "Builtin::PerMaterialEvalDataBuffer",
            Builtin::Terrain::Sets,
            Builtin::Terrain::Layers,
            Builtin::Terrain::StochasticLayers,
            Builtin::Terrain::LayerRefs,
            Builtin::Terrain::Regions,
            Builtin::Terrain::WeightBlocks,
            Builtin::Terrain::TextureGroup,
            Builtin::Material::TextureStreamingMetadataBuffer,
            Builtin::CLod::Offsets,
			Builtin::CLod::GroupChunks,
			Builtin::CLod::Groups,
            Builtin::CLod::GroupPageMap,
            Builtin::CLod::MeshMetadata,
            Builtin::CLod::AssemblyTransforms,
            Builtin::CLod::AssemblyBoneRemaps,
            Builtin::CLod::AssemblyBoneRemapIndices,
            Builtin::SkeletonResources::InverseSkinMatrices,
            Builtin::PerMaterialOpenPBRDataBuffer)
            .WithUnorderedAccess(Builtin::Surface::BaseColorOpacity,
                Builtin::Surface::NormalRoughness,
                Builtin::Surface::SpecularAo,
                Builtin::Surface::Emissive,
                Builtin::Surface::Motion,
                Builtin::Surface::Payload0,
                Builtin::Surface::Payload1,
                Builtin::Surface::Identity,
                Builtin::Surface::Records,
                Builtin::DebugVisualization,
				Builtin::Material::TextureStreamingFeedbackBuffer)
    	.WithConstantBuffer(Builtin::PerFrameBuffer);

        if (m_terrainRvtEnabled) {
            b->WithShaderResource(
                Builtin::Terrain::RvtInfo,
                Builtin::Terrain::RvtClipInfos,
                Builtin::Terrain::RvtPageTable,
                Builtin::Terrain::RvtPageKeys,
                Builtin::Terrain::RvtPhysicalPageOwner,
                Builtin::Terrain::RvtPhysicalPageAtlas,
                Builtin::Terrain::RvtHeightResidentCache,
                Builtin::Terrain::RvtHeightAtlas,
                Builtin::Terrain::RvtAlbedoAtlas,
                Builtin::Terrain::RvtNormalAtlas,
                Builtin::Terrain::RvtMaterialAtlas)
                .WithUnorderedAccess(
                    Builtin::Terrain::RvtRequestMasks,
                    Builtin::Terrain::RvtRequestList,
                    Builtin::Terrain::RvtCounters,
                    Builtin::Terrain::RvtStats);
        }
        b->WithIndirectArguments("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
        return bindings;
    }

    void Initialize() {
        m_materialEvalCmds = m_resourceRegistryView->RequestPtr<Resource>("Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
    }

    // Shared production selection over an immutable publication. Program capture
    // is supplied by the caller; this numeric phase needs no managers or device.
    template<class Consumer>
    static void VisitCommands(const br::render::PublishedMaterialState& materials,
        bool terrainEvaluation, uint32_t outputType, uint64_t argumentBufferBytes, Consumer&& consume) {
        constexpr uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        for (std::size_t index = 0; index < materials.activeCompileFlags.size(); ++index) {
            const auto flags = materials.activeCompileFlags[index];
            if (terrainEvaluation && (flags & MaterialCompileFlags::MaterialCompileTerrain) != 0) continue;
            if (index >= materials.activeCompileFlagSlots.size()) continue;
            const uint32_t slot = materials.activeCompileFlagSlots[index];
            if (slot >= materials.compileFlagSlotsUsed) continue;
            const uint64_t offset = uint64_t{slot} * stride;
            if (offset > argumentBufferBytes || stride > argumentBufferBytes-offset) continue;
            auto key = GetMaterialEvaluationShaderKey(flags);
            if (outputType == OutputType::COLOR) key |= MaterialCompileFlags::MaterialCompileMaterialEvalColorOnly;
            consume(flags,key,slot,offset);
        }
    }

    struct RecordingConfiguration {
        bool useNormalMaps = true;
        float terrainNormalBlend = 0;
        uint32_t terrainNormalMipBias = 0;
        float objectNormalMapBlend = 0;
    };
    struct PersistentBindings {
        org::persistent::ViewToken visibleClusters, visibleClusterTransformIndices;
        org::persistent::ViewToken reyesDiceQueue, reyesTessTableConfigs, reyesTessTableVertices, reyesTessTableTriangles;
        bool hasReyesDiceQueue = false, hasReyesTessTables = false;
        uint32_t patchVisibilityIndexBase = 0;
        org::persistent::BindingToken arguments;
    };
    struct CapturedProgram {
        MaterialCompileFlags key;
        std::shared_ptr<const org::PipelineStatePayload> program;
        std::vector<std::optional<org::persistent::ViewToken>> descriptorViews;
    };
    struct ProgramVersion {
        MaterialCompileFlags key;
        std::shared_ptr<const org::PipelineStatePayload> program;
    };
    // Producer-only capture. All required active programs must be ready before
    // the replacement is published; workers retain these exact versions.
    template<class Capture>
    static std::vector<ProgramVersion> CaptureProgramVersions(const br::render::PublishedMaterialState& materials,
        bool terrainEvaluation, uint32_t outputType, uint64_t argumentBufferBytes, Capture&& capture) {
        std::vector<ProgramVersion> result;
        std::unordered_map<MaterialCompileFlags,uint32_t> captured;
        VisitCommands(materials,terrainEvaluation,outputType,argumentBufferBytes,
            [&](MaterialCompileFlags,MaterialCompileFlags key,uint32_t,uint64_t) {
                if (captured.contains(key)) return;
                auto program = capture(key);
                if (!program || !program->pso.Get().GetHandle().valid() || !program->layout.valid() || !program->layoutOwner)
                    throw std::runtime_error("Material publication program is not ready");
                if (result.size() >= UINT32_MAX) throw std::overflow_error("Material publication programs exhausted");
                captured.emplace(key,static_cast<uint32_t>(result.size()));
                result.push_back({key,std::move(program)});
            });
        return result;
    }
    std::vector<ProgramVersion> CaptureProgramVersions(const br::render::PublishedMaterialState& materials,
        bool terrainEvaluation, uint32_t outputType, uint64_t argumentBufferBytes) const {
        return CaptureProgramVersions(materials,terrainEvaluation,outputType,argumentBufferBytes,
            [&](MaterialCompileFlags key) -> std::shared_ptr<const org::PipelineStatePayload> {
                const auto* program = m_inputs.pipelines->TryGetMaterialEvalPSO(key);
                return program ? program->GetPayload() : nullptr;
            });
    }
    struct PersistentProgramInterface {
        std::vector<br::render::PreparedComputeCommandSequence> programs;
        std::unordered_map<MaterialCompileFlags,uint32_t> programByKey;
        std::shared_ptr<const rhi::CommandSignaturePtr> signature;
        org::persistent::BindingToken arguments;
    };
    struct PersistentInvocation {
        struct Command { uint32_t program; uint64_t argumentsOffset; };
        std::shared_ptr<const br::render::PublishedMaterialState> materials;
        std::vector<Command> commands;
        uint64_t argumentBufferBytes = 0;
        std::array<uint32_t,NumMiscUintRootConstants> constants{};
    };
    static constexpr uint64_t NormalConfigurationMask =
        (uint64_t{1} << VISBUF_REYES_USE_NORMAL_MAPS) | (uint64_t{1} << VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT)
        | (uint64_t{1} << VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS) | (uint64_t{1} << VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT);
    static PersistentProgramInterface BuildPersistentProgramInterface(const PersistentBindings& bindings,
        std::span<const CapturedProgram> programs, std::shared_ptr<const rhi::CommandSignaturePtr> signature) {
        BT_ZONE_SCOPE("BR.MaterialEvaluation.BuildPersistentInterface");
        PersistentProgramInterface result; result.arguments = bindings.arguments; result.signature = std::move(signature);
        if (!programs.empty() && (!result.signature || !result.signature->Get().GetHandle().valid()))
            throw std::invalid_argument("Material program interface has no owned command signature");
        const auto constants = BuildConstants<br::render::SymbolicComputeConstant>(bindings,{},
            [](org::persistent::ViewToken token) { return token; });
        for (const auto& program : programs) {
            if (result.programs.size() >= UINT32_MAX) throw std::overflow_error("Material program interface exhausted");
            if (!result.programByKey.emplace(program.key,static_cast<uint32_t>(result.programs.size())).second)
                throw std::invalid_argument("Material program interface has duplicate shader keys");
            if (!program.program) throw std::invalid_argument("Material program interface has no program owner");
            const auto& reflection = program.program->pipelineResources;
            if (program.descriptorViews.size() != reflection.mandatoryResourceDescriptorSlots.size()+reflection.optionalResourceDescriptorSlots.size())
                throw std::invalid_argument("Material program descriptor layout differs from reflection");
            br::render::PreparedComputeCommandSequence commands;
            commands.commands.emplace_back(br::render::PreparedBindPersistentComputeProgram{program.program});
            if (!program.descriptorViews.empty())
                commands.commands.emplace_back(br::render::PreparedPersistentDescriptorIndices{program.descriptorViews});
            commands.commands.emplace_back(br::render::BuildSymbolicComputeConstants(MiscUintRootSignatureIndex,0,constants,NormalConfigurationMask));
            br::render::ValidatePersistentComputeCommands(commands);
            result.programs.push_back(std::move(commands));
        }
        return result;
    }
    static PersistentInvocation PreparePersistentInvocation(const PersistentProgramInterface& program,
        std::shared_ptr<const br::render::PublishedMaterialState> materials, bool terrainEvaluation,
        uint32_t outputType, uint64_t argumentBufferBytes, const RecordingConfiguration& configuration) {
        BT_ZONE_SCOPE("BR.MaterialEvaluation.PreparePersistentInvocation");
        PersistentInvocation result; result.materials = std::move(materials); result.argumentBufferBytes = argumentBufferBytes;
        if (!result.materials) return result;
        result.commands.reserve(result.materials->activeCompileFlags.size());
        VisitCommands(*result.materials,terrainEvaluation,outputType,argumentBufferBytes,
            [&](MaterialCompileFlags,MaterialCompileFlags key,uint32_t,uint64_t offset) {
                const auto found = program.programByKey.find(key);
                if (found != program.programByKey.end()) result.commands.push_back({found->second,offset});
            });
        if (result.commands.empty()) return result;
        PersistentBindings unused;
        result.constants = BuildConstants<uint32_t>(unused,configuration,[](org::persistent::ViewToken) { return 0u; });
        return result;
    }
    static void RecordPersistent(const PersistentProgramInterface& program, const PersistentInvocation& invocation,
        org::RecordingContext& recording) {
        if (invocation.commands.empty()) return;
        if (!program.signature || !program.signature->Get().GetHandle().valid())
            throw std::invalid_argument("Material recording lost command signature ownership");
        if (!invocation.materials) throw std::invalid_argument("Material recording lost semantic publication ownership");
        constexpr uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
        // Validate the fresh command list before mutating the native recorder.
        // Stable interfaces were validated at publication; no resource scan occurs.
        for (const auto& command : invocation.commands) {
            if (command.program >= program.programs.size()) throw std::out_of_range("Material invocation program index exceeds interface");
            if (command.argumentsOffset % stride || command.argumentsOffset > invocation.argumentBufferBytes
                || stride > invocation.argumentBufferBytes-command.argumentsOffset)
                throw std::out_of_range("Material invocation arguments exceed selected buffer");
        }
        const auto arguments = recording.Resolve(program.arguments).GetHandle();
        for (const auto& command : invocation.commands) {
            br::render::RecordPreparedComputeCommands(program.programs.at(command.program),recording,nullptr,{},invocation.constants);
            recording.Commands().ExecuteIndirect(program.signature->Get().GetHandle(),arguments,command.argumentsOffset,{},0,1);
        }
    }
    static RecordingConfiguration CaptureRecordingConfiguration() {
        return {CLodReyesUseNormalMaps(),CLodReyesTerrainNormalBlend(),CLodReyesTerrainNormalMipBias(),CLodReyesObjectNormalMapBlend()};
    }
    template<class Constant, class Bindings, class Resolve>
    static std::array<Constant,NumMiscUintRootConstants> BuildConstants(
        const Bindings& bindings, const RecordingConfiguration& configuration, Resolve&& resolve) {
        std::array<Constant,NumMiscUintRootConstants> constants{};
        constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = resolve(bindings.visibleClusters);
        constants[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = resolve(bindings.visibleClusterTransformIndices);
        constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = bindings.hasReyesDiceQueue
            ? Constant{resolve(bindings.reyesDiceQueue)} : Constant{0xFFFFFFFFu};
        constants[VISBUF_REYES_PATCH_INDEX_BASE] = Constant{bindings.patchVisibilityIndexBase};
        constants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? Constant{resolve(bindings.reyesTessTableConfigs)} : Constant{0xFFFFFFFFu};
        constants[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? Constant{resolve(bindings.reyesTessTableVertices)} : Constant{0xFFFFFFFFu};
        constants[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] = bindings.hasReyesTessTables
            ? Constant{resolve(bindings.reyesTessTableTriangles)} : Constant{0xFFFFFFFFu};
        constants[VISBUF_REYES_USE_NORMAL_MAPS] = Constant{configuration.useNormalMaps ? 1u : 0u};
        constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = Constant{std::bit_cast<uint32_t>(configuration.terrainNormalBlend)};
        constants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] = Constant{configuration.terrainNormalMipBias};
        constants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] = Constant{std::bit_cast<uint32_t>(configuration.objectNormalMapBlend)};
        return constants;
    }


    br::render::PreparedComputeIndirectSequence BuildRecipe(const EvaluateMaterialGroupsBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        const auto materialState = context->publishedRendererState
            ? context->publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        if (!materialState) return {};
        br::render::PreparedComputeIndirectSequence data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.commandSignature = preparation.CaptureCommandSignature(m_inputs.commandSignatures->CaptureMaterialEvaluationCommandSignature());
        data.argumentsReference = preparation.CaptureResource(m_materialEvalCmds->GetGlobalResourceID());
        const bool terrainEvaluation = context->terrainRegionMaterialEvaluationEnabled;
        const auto outputType = context->outputType;
        const auto* buffer = dynamic_cast<BufferBase*>(m_materialEvalCmds);
        std::optional<std::array<uint32_t,NumMiscUintRootConstants>> constants;
        VisitCommands(*materialState,terrainEvaluation,outputType,buffer ? buffer->GetBufferSize() : UINT64_MAX,
            [&](MaterialCompileFlags, MaterialCompileFlags shaderKey, uint32_t, uint64_t argOffset) {
            const PipelineState* pso = m_inputs.pipelines->TryGetMaterialEvalPSO(shaderKey);
            if (!pso) return;
            if (!constants) constants = BuildConstants<uint32_t>(bindings,{},
                [&](org::ResourceBindingToken token) { return preparation.ResolveView(token,{org::BindlessViewKind::ShaderResource}).index; });
            auto capture = preparation;
            capture.captureDescriptorIndices = [this, &preparation](const PipelineResources& resources) {
                return CaptureMaterialResourceDescriptorIndices(resources, preparation);
            };
            auto program = capture.CaptureProgramBinding(*pso);
            br::render::PreparedComputeIndirectSequence::Step step{};
            step.program = program.program;
            step.descriptorIndices = std::move(program.descriptorIndices);
            step.constants = *constants;
            step.argumentsOffset = argOffset; data.steps.push_back(std::move(step));
        });
        return data;
    }

    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        const auto materialState = context->publishedRendererState
            ? context->publishedRendererState->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
        std::vector<uint64_t> revision{static_cast<uint64_t>(context->outputType),
            context->terrainRegionMaterialEvaluationEnabled};
        // Material values are shader inputs. Only command membership, slots,
        // and program replacement affect this recipe's recording structure.
        revision.push_back(materialState ? materialState->compileFlagSlotsUsed : 0u);
        revision.push_back(materialState ? materialState->activeCompileFlags.size() : 0u);
        revision.push_back(materialState ? materialState->activeCompileFlagSlots.size() : 0u);
        if (materialState) for (const auto flags : materialState->activeCompileFlags) {
            auto key = GetMaterialEvaluationShaderKey(flags);
            if (context->outputType == OutputType::COLOR) key |= MaterialCompileFlags::MaterialCompileMaterialEvalColorOnly;
            const auto* pso = m_inputs.pipelines->TryGetMaterialEvalPSO(key);
            revision.push_back(static_cast<uint64_t>(flags));
            revision.push_back(reinterpret_cast<uintptr_t>(pso ? pso->PeekPayload() : nullptr));
        }
        if (materialState) for (const auto slot : materialState->activeCompileFlagSlots)
            revision.push_back(slot);
        return revision;
    }
    MaterialEvaluationInvocation PrepareInvocation(const br::render::PreparedComputeIndirectSequence& recipe,
        const EvaluateMaterialGroupsBindings&, const org::PassPrepareContext&) const {
        if (recipe.steps.empty()) return {};
        // Resource operands belong to the immutable recording interface. These
        // four scalar values are fresh even when that interface is unchanged.
        EvaluateMaterialGroupsBindings unused;
        return {BuildConstants<uint32_t>(unused,CaptureRecordingConfiguration(),
            [](org::ResourceBindingToken) { return 0u; })};
    }
    static void Record(const br::render::PreparedComputeIndirectSequence& recipe, const MaterialEvaluationInvocation& invocation,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeIndirectSequence(recipe, recording,{},invocation.constants,NormalConfigurationMask);
    }

    void ShutdownPass() {
        m_slabResourceGroup.reset();
        m_materialEvalCmds = nullptr;
    }

private:
    MaterialEvaluationBuildInputs m_inputs;
    // Registry descriptor indices embedded in the recipe must go through the
    // preparation capture hook: the recipe revision only tracks indices it saw
    // captured, so a material publication that rotates these buffers would
    // otherwise leave a stale recipe (everything renders black).
    std::vector<unsigned int> CaptureMaterialResourceDescriptorIndices(const PipelineResources& resources,
        const org::PassPrepareContext& preparation) const {
        auto resolve = [&](const ResourceIdentifier& binding, bool optional) {
            return preparation.captureDescriptorIndex ? preparation.captureDescriptorIndex(binding, optional)
                : m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(binding, optional);
        };
        std::vector<unsigned int> indices;
        indices.reserve(resources.mandatoryResourceDescriptorSlots.size() + resources.optionalResourceDescriptorSlots.size());
        for (const auto& binding : resources.mandatoryResourceDescriptorSlots) {
            const bool allowMissing = !m_terrainRvtEnabled && binding.name.starts_with("Builtin::Terrain::Rvt");
            indices.push_back(resolve(binding, allowMissing));
        }
        for (const auto& binding : resources.optionalResourceDescriptorSlots)
            indices.push_back(resolve(binding, true));
        return indices;
    }

    bool m_terrainRvtEnabled = false;
    Resource* m_materialEvalCmds;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<GloballyIndexedResource> m_visibleClusterResource;
    std::shared_ptr<GloballyIndexedResource> m_visibleClusterTransformIndicesResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesDiceQueueResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableConfigsResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableVerticesResource;
    std::shared_ptr<GloballyIndexedResource> m_reyesTessTableTrianglesResource;
    uint32_t m_patchVisibilityIndexBase = 0u;
};
