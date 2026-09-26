#pragma once
#include "VirtualGeometry/Culling/RenderPasses/HierarchicalDispatchCullingPass.h"
#include "VirtualGeometry/Culling/RenderPasses/CapturedCullingCommandSink.h"
#include "RenderPasses/Base/PersistentTypedPass.h"
#include <functional>
#include <array>
#include <memory>
#include <span>
#include <BasicTelemetry/Telemetry.h>
#include <utility>
#include <optional>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <span>
#include "../shaders/PerPassRootConstants/clodPureComputeCullingRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

// Producer-facing author for the opt-in persistent graph. Explicit declarations
// are installed once; neither program templates nor invocations keep an author
// pointer, manager pointer, resolver, or frame compiler index.
class PersistentHierarchicalDispatchCullingPass {
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
    struct AccessBinding {
        org::persistent::BindingToken resource;
        std::optional<org::persistent::ViewToken> view;
    };
    struct Bindings { std::vector<AccessBinding> accesses; };
    struct ConstantViewBinding { uint32_t constant = 0, access = 0; };
    struct ProgramBuildInputs {
        // Called on the publication worker and discarded after building. Its
        // result uses symbolic bindings and owns exact native program versions.
        std::function<HierarchicalDispatchCullingRecipe(const Bindings&)> commands;
        std::vector<ConstantViewBinding> objectConstantViews;
    };
    struct CapturedProgramBuildInputs {
        br::render::CapturedHierarchicalDispatchCullingInputs commands;
        HierarchicalDispatchCullingCommandConfiguration configuration;
        // Exact producer snapshots corresponding to the author's explicit accesses.
        // Repeated snapshots combine their declared views; physical aliases remain distinct.
        std::vector<std::shared_ptr<const org::ResourceBindingSnapshot>> accessSnapshots;
        std::unordered_map<org::ResourceIdentifier,uint32_t> reflectedViewAccesses;
    };
    struct Configuration {
        RenderPhase renderPhase;
        bool clodOnlyWorkloads = true, useShadowCascadeViews = false;
        CLodRasterOutputKind rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
    };
    struct ProgramInterface {
        HierarchicalDispatchCullingRecipe commands;
        std::vector<std::pair<uint32_t,org::persistent::ViewToken>> objectConstantViews;
        Configuration configuration;
    };
    struct InvocationInputs {
        std::span<const PreparedViewFrameData> views;
        std::shared_ptr<const br::render::PublishedRendererState> publication;
        uint32_t windCacheGeneration = 0, windCacheEntryCount = 0;
    };
    using Invocation = HierarchicalDispatchCullingInvocation;
    PersistentHierarchicalDispatchCullingPass(Configuration configuration,
        std::vector<ResourceAccess> accesses, std::vector<GroupAccess> groups = {})
        : m_configuration(std::move(configuration)), m_accesses(std::move(accesses)), m_groups(std::move(groups)) {}
    Bindings Declare(org::persistent::PassDeclaration& declaration) const {
        Bindings bindings; bindings.accesses.reserve(m_accesses.size());
        for (const auto& access : m_accesses) {
            const auto resource = declaration.Resource(access.slot,access.state,access.range);
            bindings.accesses.push_back({resource,access.view
                ? std::optional{declaration.View(resource,*access.view)} : std::nullopt});
        }
        for (const auto& group : m_groups) declaration.Group(group.group,group.state,group.phase,group.range);
        return bindings;
    }
    ProgramInterface BuildProgramInterface(const Bindings& bindings, const ProgramBuildInputs& inputs) const {
        if (!inputs.commands) throw std::invalid_argument("Culling program has no command builder");
        return FinalizeProgramInterface(inputs.commands(bindings),bindings,inputs.objectConstantViews);
    }
    ProgramInterface BuildProgramInterface(const Bindings& bindings, const CapturedProgramBuildInputs& inputs) const {
        BT_ZONE_SCOPE("BR.CullingPublication.BuildInterface");
        if (inputs.accessSnapshots.size() != bindings.accesses.size() || bindings.accesses.size() != m_accesses.size())
            throw std::invalid_argument("Captured culling access snapshot layout differs from declaration");
        br::render::CapturedCullingBindings captured;
        struct ResourceBinding {
            std::shared_ptr<const org::ResourceBindingSnapshot> snapshot;
            br::render::CapturedCullingBindings::Resource resource;
        };
        std::unordered_map<const org::ResourceBindingSnapshot*,ResourceBinding> resources;
        for (size_t i = 0; i < bindings.accesses.size(); ++i) {
            const auto& snapshot = inputs.accessSnapshots[i];
            if (!snapshot) throw std::invalid_argument("Captured culling access has no snapshot");
            const auto& binding = bindings.accesses[i];
            auto [found,inserted] = resources.try_emplace(snapshot.get(),
                ResourceBinding{snapshot,{binding.resource,{}}});
            (void)inserted;
            if (m_accesses[i].view) {
                if (!binding.view) throw std::invalid_argument("Captured culling access has no view token");
                found->second.resource.views.push_back({*m_accesses[i].view,*binding.view});
            }
        }
        for (auto& [snapshot,binding] : resources) {
            (void)snapshot;
            captured.AddResource(binding.snapshot,std::move(binding.resource));
        }
        for (const auto& program : inputs.commands.Programs()) {
            if (!program) continue;
            if (!program->pso.Get().GetHandle().valid()) throw std::invalid_argument("Captured culling program has no native pipeline");
            const auto [found,inserted] = captured.programs.emplace(program->pso.Get().GetHandle(),program);
            if (!inserted && found->second != program)
                throw std::invalid_argument("Captured culling pipeline has inconsistent program ownership");
        }
        for (const auto& [key,access] : inputs.reflectedViewAccesses) {
            const auto& view = bindings.accesses.at(access).view;
            if (!view) throw std::invalid_argument("Captured culling reflection references an access without a view");
            captured.reflectedViews.emplace(key,*view);
        }
        captured.signature = inputs.commands.m_pureComputeDispatchCommandSignature;
        return FinalizeProgramInterface(HierarchicalDispatchCullingPass::BuildCapturedRecipe(
            inputs.commands,inputs.configuration,captured),bindings,{});
    }
private:
    ProgramInterface FinalizeProgramInterface(HierarchicalDispatchCullingRecipe commands, const Bindings& bindings,
        std::span<const ConstantViewBinding> objectConstantViews) const {
        ProgramInterface result{std::move(commands),{},m_configuration};
        br::render::ValidatePersistentComputeCommands(result.commands.prefix);
        br::render::ValidatePersistentComputeCommands(result.commands.suffix);
        std::array<bool,NumMiscUintRootConstants> occupied{};
        const auto claim = [&](uint32_t constant) {
            if (constant >= occupied.size() || occupied[constant])
                throw std::invalid_argument("Culling object descriptor constant layout is invalid");
            if (constant == CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION
                || constant == CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT
                || constant == CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT || constant == CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX
                || constant == CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_SET_SRV_INDEX
                || constant == CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_SRV_INDEX
                || constant == CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS)
                throw std::invalid_argument("Culling descriptor overlaps fresh workload constants");
            occupied[constant] = true;
        };
        for (const auto& patch : result.commands.objectCullBindingViews) claim(patch.index);
        for (const auto& patch : objectConstantViews) {
            claim(patch.constant);
            const auto& binding = bindings.accesses.at(patch.access);
            if (!binding.view) throw std::invalid_argument("Culling object descriptor has no declared view");
            result.objectConstantViews.emplace_back(patch.constant,*binding.view);
        }
        return result;
    }
public:
    static Invocation PrepareInvocation(const ProgramInterface& program, const Bindings&, const InvocationInputs& inputs) {
        return HierarchicalDispatchCullingPass::PrepareInvocation(program.commands,HierarchicalDispatchCullingPreparation{
            .views = inputs.views, .publication = inputs.publication, .renderPhase = program.configuration.renderPhase,
            .clodOnlyWorkloads = program.configuration.clodOnlyWorkloads,
            .useShadowCascadeViews = program.configuration.useShadowCascadeViews,
            .rasterOutputKind = program.configuration.rasterOutputKind,
            .windCacheGeneration = inputs.windCacheGeneration, .windCacheEntryCount = inputs.windCacheEntryCount});
    }
    static void Record(const ProgramInterface& program, const Bindings&, const Invocation& invocation, org::RecordingContext& recording) {
        auto constants = program.commands.objectCullConstants;
        for (const auto& [constant,view] : program.objectConstantViews) constants.at(constant) = recording.Resolve(view).index;
        HierarchicalDispatchCullingPass::RecordWithObjectConstants(program.commands,invocation,recording,constants);
    }
private:
    Configuration m_configuration;
    std::vector<ResourceAccess> m_accesses;
    std::vector<GroupAccess> m_groups;
};
