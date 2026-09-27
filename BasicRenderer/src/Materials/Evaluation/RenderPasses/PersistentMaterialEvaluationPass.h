#pragma once
#include "Materials/Evaluation/RenderPasses/EvaluateMaterialGroupsPass.h"
#include "RenderPasses/Base/PersistentTypedPass.h"
#include <optional>
#include <memory>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <utility>

// Explicit publication author; installed interfaces retain no author or manager.
class PersistentMaterialEvaluationPass {
public:
    struct ResourceAccess {
        org::persistent::ResourceSlotId slot;
        org::experimental::CompileResourceState state;
        org::experimental::CompileRange range;
        std::optional<org::BindlessViewRequest> view;
    };
    struct GroupAccess {
        org::persistent::ResourceGroupId group;
        org::experimental::CompileResourceState state;
        uint32_t phase = 0;
        std::optional<org::experimental::CompileRange> range;
    };
    struct Configuration {
        uint32_t visibleClusters = 0, visibleClusterTransformIndices = 0, arguments = 0;
        std::optional<uint32_t> diceQueue, tessTableConfigs, tessTableVertices, tessTableTriangles;
        uint32_t patchVisibilityIndexBase = 0;
        bool terrainRvtEnabled = true;
    };
    struct AccessBinding {
        org::persistent::BindingToken resource;
        std::optional<org::persistent::ViewToken> view;
    };
    struct Bindings {
        std::vector<AccessBinding> accesses;
        EvaluateMaterialGroupsPass::PersistentBindings constants;
    };
    struct CapturedProgram {
        MaterialCompileFlags key;
        std::shared_ptr<const org::PipelineStatePayload> program;
        std::vector<std::optional<uint32_t>> descriptorAccesses;
    };
    struct ProgramBuildInputs {
        std::vector<CapturedProgram> programs;
        std::shared_ptr<const rhi::CommandSignaturePtr> signature;
    };
    // Worker-side mapping from immutable program reflection to explicit accesses.
    // Missing bindings remain explicit and are validated by BuildProgramInterface.
    static ProgramBuildInputs MapCapturedPrograms(std::span<const EvaluateMaterialGroupsPass::ProgramVersion> programs,
        const std::unordered_map<org::ResourceIdentifier,uint32_t>& reflectedAccesses,
        std::shared_ptr<const rhi::CommandSignaturePtr> signature) {
        ProgramBuildInputs result; result.signature = std::move(signature);
        for (const auto& input : programs) {
            if (!input.program) throw std::invalid_argument("Captured material program has no owner");
            CapturedProgram program{input.key,input.program,{}};
            const auto map = [&](const auto& descriptors) {
                for (const auto& descriptor : descriptors) {
                    const auto found = reflectedAccesses.find(descriptor);
                    program.descriptorAccesses.push_back(found == reflectedAccesses.end() ? std::nullopt : std::optional{found->second});
                }
            };
            map(input.program->pipelineResources.mandatoryResourceDescriptorSlots);
            map(input.program->pipelineResources.optionalResourceDescriptorSlots);
            result.programs.push_back(std::move(program));
        }
        return result;
    }
    struct InvocationInputs {
        std::shared_ptr<const br::render::PublishedMaterialState> materials;
        bool terrainEvaluation = false;
        uint32_t outputType = OutputType::COLOR;
        uint64_t argumentBufferBytes = 0;
        EvaluateMaterialGroupsPass::RecordingConfiguration normalConfiguration;
    };
    using ProgramInterface = EvaluateMaterialGroupsPass::PersistentProgramInterface;
    using Invocation = EvaluateMaterialGroupsPass::PersistentInvocation;
    PersistentMaterialEvaluationPass(Configuration configuration, std::vector<ResourceAccess> accesses,
        std::vector<GroupAccess> groups = {})
        : m_configuration(std::move(configuration)), m_accesses(std::move(accesses)), m_groups(std::move(groups)) {}
    Bindings Declare(org::persistent::PassDeclaration& declaration) const {
        Bindings bindings; bindings.accesses.reserve(m_accesses.size());
        for (const auto& access : m_accesses) {
            const auto resource = declaration.Resource(access.slot,access.state,access.range);
            bindings.accesses.push_back({resource,access.view ? std::optional{declaration.View(resource,*access.view)} : std::nullopt});
        }
        for (const auto& group : m_groups) declaration.Group(group.group,group.state,group.phase,group.range);
        const auto shaderView = [&](uint32_t index) {
            const auto& binding = bindings.accesses.at(index);
            if (!binding.view || m_accesses.at(index).view->kind != org::BindlessViewKind::ShaderResource)
                throw std::invalid_argument("Material constant requires a declared shader-resource view");
            if (!(m_accesses.at(index).state.access & static_cast<uint64_t>(rhi::ResourceAccessType::ShaderResource)))
                throw std::invalid_argument("Material constant requires shader-resource access");
            return *binding.view;
        };
        auto& constants = bindings.constants;
        constants.visibleClusters = shaderView(m_configuration.visibleClusters);
        constants.visibleClusterTransformIndices = shaderView(m_configuration.visibleClusterTransformIndices);
        constants.arguments = bindings.accesses.at(m_configuration.arguments).resource;
        if (!(m_accesses.at(m_configuration.arguments).state.access & static_cast<uint64_t>(rhi::ResourceAccessType::IndirectArgument)))
            throw std::invalid_argument("Material arguments require indirect access");
        constants.patchVisibilityIndexBase = m_configuration.patchVisibilityIndexBase;
        if (m_configuration.diceQueue) { constants.hasReyesDiceQueue = true; constants.reyesDiceQueue = shaderView(*m_configuration.diceQueue); }
        const unsigned tessCount = bool(m_configuration.tessTableConfigs)+bool(m_configuration.tessTableVertices)+bool(m_configuration.tessTableTriangles);
        if (tessCount && tessCount != 3) throw std::invalid_argument("Material tessellation table declarations are incomplete");
        if (tessCount) {
            constants.hasReyesTessTables = true;
            constants.reyesTessTableConfigs = shaderView(*m_configuration.tessTableConfigs);
            constants.reyesTessTableVertices = shaderView(*m_configuration.tessTableVertices);
            constants.reyesTessTableTriangles = shaderView(*m_configuration.tessTableTriangles);
        }
        return bindings;
    }
    ProgramInterface BuildProgramInterface(const Bindings& bindings, const ProgramBuildInputs& inputs) const {
        std::vector<EvaluateMaterialGroupsPass::CapturedProgram> programs; programs.reserve(inputs.programs.size());
        for (const auto& input : inputs.programs) {
            if (!input.program) throw std::invalid_argument("Material reflection has no program owner");
            const auto& reflection = input.program->pipelineResources;
            const auto mandatory = reflection.mandatoryResourceDescriptorSlots.size();
            if (input.descriptorAccesses.size() != mandatory+reflection.optionalResourceDescriptorSlots.size())
                throw std::invalid_argument("Material reflected access layout differs from program");
            EvaluateMaterialGroupsPass::CapturedProgram program{input.key,input.program,{}};
            for (size_t i = 0; i < input.descriptorAccesses.size(); ++i) {
                const auto access = input.descriptorAccesses[i];
                if (!access) {
                    const bool disabledTerrainRvt = !m_configuration.terrainRvtEnabled && i < mandatory
                        && reflection.mandatoryResourceDescriptorSlots[i].name.starts_with("Builtin::Terrain::Rvt");
                    if (i < mandatory && !disabledTerrainRvt)
                        throw std::invalid_argument("Material program is missing a mandatory declared descriptor");
                    program.descriptorViews.push_back(std::nullopt);
                }
                else {
                    const auto& view = bindings.accesses.at(*access).view;
                    if (!view) throw std::invalid_argument("Material reflection references an access without a view");
                    program.descriptorViews.push_back(*view);
                }
            }
            programs.push_back(std::move(program));
        }
        return EvaluateMaterialGroupsPass::BuildPersistentProgramInterface(bindings.constants,programs,inputs.signature);
    }
    static Invocation PrepareInvocation(const ProgramInterface& program, const Bindings&, const InvocationInputs& inputs) {
        return EvaluateMaterialGroupsPass::PreparePersistentInvocation(program,inputs.materials,inputs.terrainEvaluation,
            inputs.outputType,inputs.argumentBufferBytes,inputs.normalConfiguration);
    }
    static void Record(const ProgramInterface& program, const Bindings&, const Invocation& invocation, org::RecordingContext& context) {
        EvaluateMaterialGroupsPass::RecordPersistent(program,invocation,context);
    }
private:
    Configuration m_configuration;
    std::vector<ResourceAccess> m_accesses;
    std::vector<GroupAccess> m_groups;
};
