#pragma once
#include "Render/GraphExtensions/ClusterLOD/CapturedHierarchicalDispatchCullingInputs.h"
#include "Render/GraphExtensions/ClusterLOD/HierarchicalDispatchCullingPass.h"
#include <unordered_map>
#include <algorithm>
#include <iterator>
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"

namespace br::render {
struct CapturedCullingBindings {
    struct View { org::BindlessViewRequest request; org::persistent::ViewToken token; };
    struct Resource { org::persistent::BindingToken token; std::vector<View> views; };
    std::unordered_map<rhi::ResourceHandle,Resource,rhi::HandleHash<rhi::ResourceHandle>,rhi::HandleEqual<rhi::ResourceHandle>> resources;
    std::unordered_map<const org::ResourceBindingSnapshot*,Resource> snapshotResources;
    std::unordered_map<rhi::PipelineHandle,std::shared_ptr<const org::PipelineStatePayload>,
        rhi::HandleHash<rhi::PipelineHandle>,rhi::HandleEqual<rhi::PipelineHandle>> programs;
    std::unordered_map<org::ResourceIdentifier,org::persistent::ViewToken> reflectedViews;
    std::shared_ptr<const rhi::CommandSignaturePtr> signature;
    void AddResource(const std::shared_ptr<const org::ResourceBindingSnapshot>& snapshot, Resource resource) {
        if (!snapshot) throw std::invalid_argument("Missing captured culling binding snapshot");
        // Physical operands may use any declared representative. Descriptor views
        // must retain their logical snapshot identity even on a shared backing.
        resources.try_emplace(snapshot->resource.GetHandle(),resource);
        if (!snapshotResources.emplace(snapshot.get(),std::move(resource)).second)
            throw std::invalid_argument("Duplicate captured culling binding snapshot");
    }
};
// Publication-worker facade for the full production culling emitter. It lowers
// symbolic values once; recording uses the ordinary production command encoder.
class CapturedCullingCommandSink {
public:
    using Constant = SymbolicComputeConstant;
    explicit CapturedCullingCommandSink(const CapturedCullingBindings& bindings) : m_bindings(bindings) {}
    void BindLayout(rhi::PipelineLayoutHandle) {} // Each owned program supplies its exact layout.
    void BindPipeline(rhi::PipelineHandle pipeline) {
        m_commands.commands.emplace_back(PreparedBindPersistentComputeProgram{m_bindings.programs.at(pipeline)});
    }
    void BindDescriptorIndices(const org::PipelineResources& resources) {
        PreparedPersistentDescriptorIndices descriptors;
        for (const auto& key : resources.mandatoryResourceDescriptorSlots) descriptors.slots.push_back(m_bindings.reflectedViews.at(key));
        for (const auto& key : resources.optionalResourceDescriptorSlots) {
            const auto found = m_bindings.reflectedViews.find(key);
            descriptors.slots.push_back(found == m_bindings.reflectedViews.end() ? std::nullopt : std::optional{found->second});
        }
        m_commands.commands.emplace_back(std::move(descriptors));
    }
    Constant SRVIndex(const CapturedHierarchicalDispatchCullingInputs::EmissionBuffer& resource,
        uint32_t variant = UINT32_MAX, uint32_t mip = 0, uint32_t slice = 0) const {
        return ViewIndex(resource,{org::BindlessViewKind::ShaderResource,variant,mip,slice});
    }
    Constant UAVIndex(const CapturedHierarchicalDispatchCullingInputs::EmissionBuffer& resource,
        uint32_t variant = UINT32_MAX, uint32_t mip = 0, uint32_t slice = 0) const {
        return ViewIndex(resource,{org::BindlessViewKind::UnorderedAccess,variant,mip,slice});
    }
    void PushConstants(rhi::ShaderStage,uint32_t,uint32_t root,uint32_t offset,uint32_t count,const Constant* values) {
        m_commands.commands.emplace_back(BuildSymbolicComputeConstants(root,offset,{values,count}));
    }
    void PushSharedConstants(rhi::ShaderStage,uint32_t,uint32_t root,uint32_t offset,uint32_t count,const Constant* values) {
        m_commands.commands.emplace_back(BuildSymbolicComputeConstants(root,offset,{values,count},
            (uint64_t{1} << CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION)
            | (uint64_t{1} << CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT)));
    }
    void Dispatch(uint32_t x,uint32_t y,uint32_t z) { m_commands.commands.emplace_back(PreparedDispatchGroups{x,y,z}); }
    void Barriers(const rhi::BarrierBatch& barriers) {
        PreparedBufferBarrierBatch result;
        for (const auto& barrier : barriers.buffers) result.barriers.push_back({m_bindings.resources.at(barrier.buffer).token,
            barrier.beforeAccess,barrier.afterAccess,barrier.beforeSync,barrier.afterSync});
        if (!result.barriers.empty()) m_commands.commands.emplace_back(std::move(result));
    }
    void ExecuteIndirect(rhi::CommandSignatureHandle signature,rhi::ResourceHandle arguments,uint64_t offset,
        rhi::ResourceHandle count,uint64_t countOffset,uint32_t maximum) {
        if (!m_bindings.signature || !rhi::HandleEqual<rhi::CommandSignatureHandle>{}(m_bindings.signature->Get().GetHandle(), signature))
            throw std::invalid_argument("Culling signature differs from captured owner");
        PreparedExecuteIndirectCommand command{signature,m_bindings.resources.at(arguments).token,offset,maximum,m_bindings.signature};
        if (count.valid()) command.countBuffer = m_bindings.resources.at(count).token;
        command.countOffset = countOffset; m_commands.commands.emplace_back(std::move(command));
    }
    void ObjectCull(const Constant* constants) {
        if (m_objectOffset) throw std::invalid_argument("Duplicate culling object stage");
        m_objectOffset = m_commands.commands.size();
        m_objectConstants = BuildSymbolicComputeConstants(MiscUintRootSignatureIndex,0,{constants,NumMiscUintRootConstants});
    }
    HierarchicalDispatchCullingRecipe Finish() {
        ValidatePersistentComputeCommands(m_commands);
        HierarchicalDispatchCullingRecipe result;
        result.prefix = std::move(m_commands);
        if (m_objectOffset) {
            result.hasObjectCull = true;
            std::copy(m_objectConstants.values.begin(),m_objectConstants.values.end(),result.objectCullConstants.begin());
            result.objectCullBindingViews = std::move(m_objectConstants.bindingViews);
            auto& commands = result.prefix.commands;
            result.suffix.commands.assign(std::make_move_iterator(commands.begin()+*m_objectOffset),std::make_move_iterator(commands.end()));
            commands.resize(*m_objectOffset);
        }
        return result;
    }
private:
    Constant ViewIndex(const CapturedHierarchicalDispatchCullingInputs::EmissionBuffer& resource,org::BindlessViewRequest request) const {
        if (!resource) throw std::invalid_argument("Missing captured culling view resource");
        const auto& views = m_bindings.snapshotResources.at(resource->snapshot.get()).views;
        const auto found = std::find_if(views.begin(),views.end(),[&](const auto& view) {
            return view.request.kind == request.kind && view.request.variant == request.variant
                && view.request.mip == request.mip && view.request.slice == request.slice;
        });
        if (found == views.end()) throw std::invalid_argument("Missing declared captured culling view");
        return found->token;
    }
    const CapturedCullingBindings& m_bindings;
    PreparedComputeCommandSequence m_commands;
    std::optional<size_t> m_objectOffset;
    PreparedComputeConstants m_objectConstants;
};
}
