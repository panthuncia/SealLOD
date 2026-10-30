#include <iostream>
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <BasicRenderer/Pipeline/PipelineRecipe.h>
#include "BasicRenderer/Runtime/Detail/DepthHistoryService.h"
#include "Render/RenderGraph/RhiExecution.h"
#include "Render/ShaderAPI.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "Materials/Evaluation/RenderPasses/BuildPixelListPass.h"
#include "Materials/Evaluation/RenderPasses/EvaluateMaterialGroupsPass.h"
#include "Runtime/Publication/PersistentRendererPublication.h"
#include "VirtualGeometry/Culling/RenderPasses/ComputeCullingPass.h"
#include "VirtualGeometry/Rasterization/RenderPasses/ClusterSoftwareRasterizationPass.h"
#include "VirtualGeometry/Culling/PreparedCullingWorkloads.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodPureComputeCullingRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

namespace {

// Command trace of a recording: every command the pass emits, in order, through
// a capturing command list. No device is involved.
struct Trace {
    enum class Kind { Layout, Pipeline, Push, Dispatch, Indirect, Barrier };
    struct Event {
        Kind kind{};
        // Layout/Pipeline: its index. Push/Dispatch: the pipeline bound at the time.
        // Indirect: the argument resource.
        uint32_t handle = 0;
        // Push: the root parameter. Indirect: the command signature. Barrier: the
        // first buffer's access after the barrier.
        uint32_t root = 0;
        uint64_t offset = 0;  // Indirect: the argument offset.
        // Push: the constants. Dispatch: the groups. Barrier: the buffers.
        std::vector<uint32_t> values;
    };
    std::vector<Event> events;
    uint32_t pipeline = 0;

    std::vector<Event> Of(Kind kind) const
    {
        std::vector<Event> result;
        for (const auto& event : events) if (event.kind == kind) result.push_back(event);
        return result;
    }
    std::vector<uint32_t> Handles(Kind kind) const
    {
        std::vector<uint32_t> result;
        for (const auto& event : events) if (event.kind == kind) result.push_back(event.handle);
        return result;
    }
    std::vector<uint32_t> Pipelines() const { return Handles(Kind::Pipeline); }
    // The misc root-constant blocks pushed, optionally only those under one pipeline.
    std::vector<std::vector<uint32_t>> MiscPushes(std::optional<uint32_t> pipeline = std::nullopt) const
    {
        std::vector<std::vector<uint32_t>> result;
        for (const auto& event : events)
            if (event.kind == Kind::Push && event.root == MiscUintRootSignatureIndex
                && (!pipeline || event.handle == *pipeline))
                result.push_back(event.values);
        return result;
    }
    std::vector<std::vector<uint32_t>> DescriptorSlotPushes() const
    {
        std::vector<std::vector<uint32_t>> result;
        for (const auto& event : events)
            if (event.kind == Kind::Push && event.root == org::shaderapi::kResourceDescriptorIndicesRootParameter)
                result.push_back(event.values);
        return result;
    }
};

rhi::CommandList TraceList(Trace& trace)
{
    static const rhi::CommandListVTable table = [] {
        rhi::CommandListVTable t{};
        t.abi_version = rhi::RHI_CL_ABI_MIN;
        t.bindLayout = +[](rhi::CommandList* list, rhi::PipelineLayoutHandle layout) noexcept {
            static_cast<Trace*>(list->impl)->events.push_back({Trace::Kind::Layout, layout.index});
        };
        t.bindPipeline = +[](rhi::CommandList* list, rhi::PipelineHandle pipeline) noexcept {
            auto& trace = *static_cast<Trace*>(list->impl);
            trace.pipeline = pipeline.index;
            trace.events.push_back({Trace::Kind::Pipeline, pipeline.index});
        };
        t.pushConstants = +[](rhi::CommandList* list, rhi::ShaderStage, uint32_t, uint32_t root,
            uint32_t, uint32_t count, const void* values) noexcept {
            auto& trace = *static_cast<Trace*>(list->impl);
            const auto* words = static_cast<const uint32_t*>(values);
            trace.events.push_back({Trace::Kind::Push, trace.pipeline, root, 0, std::vector<uint32_t>(words, words + count)});
        };
        t.dispatch = +[](rhi::CommandList* list, uint32_t x, uint32_t y, uint32_t z) noexcept {
            auto& trace = *static_cast<Trace*>(list->impl);
            trace.events.push_back({Trace::Kind::Dispatch, trace.pipeline, 0, 0, {x, y, z}});
        };
        t.executeIndirect = +[](rhi::CommandList* list, rhi::CommandSignatureHandle signature,
            rhi::ResourceHandle arguments, uint64_t offset, rhi::ResourceHandle, uint64_t, uint32_t count) noexcept {
            static_cast<Trace*>(list->impl)->events.push_back(
                {Trace::Kind::Indirect, arguments.index, signature.index, offset, {count}});
        };
        t.barriers = +[](rhi::CommandList* list, const rhi::BarrierBatch& batch) noexcept {
            Trace::Event event{Trace::Kind::Barrier};
            for (uint32_t i = 0; i < batch.buffers.size; ++i) event.values.push_back(batch.buffers.data[i].buffer.index);
            if (batch.buffers.size) event.root = static_cast<uint32_t>(batch.buffers.data[0].afterAccess);
            static_cast<Trace*>(list->impl)->events.push_back(std::move(event));
        };
        return t;
    }();
    rhi::CommandList list{rhi::CommandListHandle{1, 1}};
    list.vt = &table;
    list.impl = &trace;
    return list;
}

// A pass's declarations built by hand, as the graph would freeze them for one
// frame. Every declared resource publishes a shader-resource, unordered-access
// and constant-buffer view, so recordings may read any of them.
class DeclaredFrame {
public:
    using Kind = org::BindlessViewKind;
    static uint32_t ResourceIndex(const org::DeclaredViewToken& token) { return 1000u + token.use; }
    static uint32_t Descriptor(Kind kind, const org::DeclaredViewToken& token)
    {
        switch (kind) {
        case Kind::UnorderedAccess: return 100u + token.use;
        case Kind::ConstantBuffer: return 300u + token.use;
        default: return 200u + token.use;
        }
    }
    static uint32_t Srv(const org::DeclaredViewToken& token) { return Descriptor(Kind::ShaderResource, token); }
    static uint32_t Uav(const org::DeclaredViewToken& token) { return Descriptor(Kind::UnorderedAccess, token); }

    // Declares one resource with one view; a name makes it what pipeline
    // ResourceDescriptorIndex slots of that name resolve to.
    org::DeclaredViewToken Declare(Kind kind = Kind::UnorderedAccess, const char* name = nullptr)
    {
        using A = rhi::ResourceAccessType;
        const auto use = static_cast<uint32_t>(m_layout->uses.size());
        const uint64_t id = 500u + use;
        const auto access = kind == Kind::UnorderedAccess ? A::UnorderedAccess
            : kind == Kind::ConstantBuffer ? A::ConstantBuffer : A::ShaderResource;
        m_layout->uses.push_back({{}, {access, rhi::ResourceLayout::Common, rhi::ResourceSyncState::All},
            {id, id}, {org::BindlessViewRequest{kind}}});
        if (name) m_layout->namedViews.push_back({org::ResourceIdentifier{name}.hash, use, 0, kind});
        return {m_layout, use, 0};
    }

    org::RecordingContext Context(rhi::CommandList list) const
    {
        std::vector<org::FrozenExecutionBindings::ResourceBinding> resources;
        auto slots = std::make_shared<org::FramePreparationContext::ResourceSlots>();
        auto scope = std::make_shared<org::DeclaredRecordingScope>();
        scope->layout = m_layout;
        for (uint32_t use = 0; use < m_layout->uses.size(); ++use) {
            const org::DeclaredViewToken token{m_layout, use, 0};
            auto views = std::make_shared<org::BindlessResourceViews>();
            for (const auto kind : {Kind::ShaderResource, Kind::UnorderedAccess, Kind::ConstantBuffer})
                views->views.push_back({kind, UINT32_MAX, 0, 0, Slot(Descriptor(kind, token))});
            resources.push_back({rhi::Resource(rhi::ResourceHandle{ResourceIndex(token), 1}), m_owner, views});
            slots->push_back({m_layout->uses[use].binding.globalResourceID, use});
            scope->views.push_back(Slot(Descriptor(m_layout->uses[use].requiredViews[0].kind, token)));
        }
        auto bindings = std::make_shared<const org::FrozenExecutionBindings>(std::move(resources));
        org::RecordingContext base(list, bindings);
        return base.WithDeclaredResources(bindings, slots, scope);
    }

private:
    static rhi::DescriptorSlot Slot(uint32_t index) { return rhi::DescriptorSlot(rhi::DescriptorHeapHandle{3, 1}, index); }
    std::shared_ptr<org::ResourceUseLayout> m_layout = std::make_shared<org::ResourceUseLayout>();
    std::shared_ptr<int> m_owner = std::make_shared<int>(0);
};

// Native objects with handles only; nothing destroys them.
constexpr uint32_t kLayoutBase = 50u;
int g_nativeObject = 0;

rhi::PipelinePtr NativePipeline(uint32_t index)
{
    static const rhi::PipelineVTable table{};
    rhi::Pipeline pipeline{rhi::PipelineHandle{index, 1}};
    pipeline.impl = &g_nativeObject;
    pipeline.vt = &table;
    return rhi::PipelinePtr(rhi::Device{}, pipeline, nullptr);
}

org::PipelineState Pipeline(uint32_t index, org::PipelineResources resources = {})
{
    return org::PipelineState(NativePipeline(index), 0, std::move(resources), {},
        rhi::PipelineLayoutHandle{kLayoutBase + index, 1});
}

std::shared_ptr<const org::PipelineStatePayload> Payload(uint32_t index, org::PipelineResources resources = {})
{
    auto payload = std::make_shared<org::PipelineStatePayload>(NativePipeline(index), 0, std::move(resources),
        1, 0, 0, "TestPipeline" + std::to_string(index));
    payload->layout = {kLayoutBase + index, 1};
    return payload;
}

std::shared_ptr<const rhi::CommandSignaturePtr> Signature(uint32_t index)
{
    static const rhi::CommandSignatureVTable table{};
    rhi::CommandSignature signature{rhi::CommandSignatureHandle{index, 1}};
    signature.impl = &g_nativeObject;
    signature.vt = &table;
    return std::make_shared<const rhi::CommandSignaturePtr>(rhi::Device{}, signature, nullptr);
}

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Exception, class F> bool Rejects(F&& record)
{
    try { record(); }
    catch (const Exception&) { return true; }
    return false;
}

// Compute culling: the culling programs are 1..12 in ComputeCullingPrograms
// order, the dispatch signature 40.
struct CullingFixture {
    DeclaredFrame declared;
    ComputeCullingBindings bindings;

    CullingFixture(bool firstPass, bool windCache)
    {
        auto& b = bindings;
        b.programs = {Pipeline(1), Pipeline(2), Pipeline(3), Pipeline(4), Pipeline(5), Pipeline(6),
            Pipeline(7), Pipeline(8), Pipeline(9), Pipeline(10), Pipeline(11), Pipeline(12), Signature(40)};
        b.visibleClusters = declared.Declare();
        b.visibleClusterTransformIndices = declared.Declare();
        b.visibleClustersCounter = declared.Declare();
        b.swVisibleClustersCounter = declared.Declare();
        b.rasterBucketHistogramCommand = declared.Declare();
        b.telemetry = declared.Declare();
        b.replay = declared.Declare();
        b.replayState = declared.Declare();
        auto& f = b.frontiers;
        for (auto* token : {&f.currentNodes, &f.nextNodes, &f.currentLeaves, &f.nextLeaves, &f.clusters,
                 &f.currentNodeCounter, &f.nextNodeCounter, &f.currentLeafCounter, &f.nextLeafCounter, &f.clusterCounter,
                 &f.nodeDispatchArgs, &f.leafDispatchArgs, &f.clusterDispatchArgs})
            *token = declared.Declare();
        if (windCache) {
            b.windBounds = declared.Declare();
            b.windMembership = declared.Declare(DeclaredFrame::Kind::ShaderResource);
        }
        b.maxVisibleClusters = 4096u;
        b.isFirstPass = firstPass;
    }
};

void TestComputeCullingRecording()
{
    ComputeCullingFrame frame;
    frame.active = true;
    frame.cullingFlags = 0x5u;
    frame.traversalDepth = 1u;
    frame.expansionFactor = 4u;
    frame.rasterBucketCount = 9u;
    frame.viewRasterInfoTable = 61u;
    frame.viewDepthTable = 62u;
    frame.windEntryCount = 123u;
    frame.windGeneration = 77u;
    frame.workloads.push_back({.viewDataIndex = 3, .activeDrawSetIndicesSRVIndex = 4, .activeDrawCount = 5,
        .drawRecordVisibilityGenerationSRVIndex = 6, .drawRecordVisibilityGenerationCount = 8, .shadowCasterClass = 2, .dispatchGridX = 7});
    frame.workloads.push_back({.viewDataIndex = 13, .activeDrawSetIndicesSRVIndex = 14, .activeDrawCount = 15,
        .drawRecordVisibilityGenerationSRVIndex = 16, .drawRecordVisibilityGenerationCount = 18, .shadowCasterClass = 1, .dispatchGridX = 17});

    // Phase 1 with the DynamicWind bounds cache.
    {
        CullingFixture fixture(true, true);
        const auto& b = fixture.bindings;
        Trace trace;
        auto context = fixture.declared.Context(TraceList(trace));
        ComputeCullingPass::Record(b, frame, context);
        Require(trace.Pipelines() == std::vector<uint32_t>{1, 1, 1, 1, 1, 7, 6, 4, 10, 11, 3, 12, 2},
            "phase-1 culling stages changed order");
        Require(trace.Handles(Trace::Kind::Layout) == std::vector<uint32_t>{51, 51, 51, 51, 51, 57, 56, 54, 60, 61, 53, 62, 52},
            "culling programs were bound without their layouts");

        const std::vector<org::DeclaredViewToken> counters{b.frontiers.currentNodeCounter, b.frontiers.nextNodeCounter,
            b.frontiers.currentLeafCounter, b.frontiers.nextLeafCounter, b.frontiers.clusterCounter};
        const auto clears = trace.MiscPushes(1);
        Require(clears.size() == counters.size(), "culling did not clear every frontier counter");
        for (size_t index = 0; index < counters.size(); ++index)
            Require(clears[index][CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(counters[index])
                && clears[index][CLOD_CLEAR_UINT_BUFFER_COUNT] == 1u, "frontier counter clear lost its target");

        // Object culling: one dispatch per workload with exactly that workload's
        // arguments, between the shared prefix and the traversal suffix.
        const auto objectCull = trace.MiscPushes(7);
        std::vector<Trace::Event> objectDispatches;
        for (const auto& dispatch : trace.Of(Trace::Kind::Dispatch))
            if (dispatch.handle == 7) objectDispatches.push_back(dispatch);
        Require(objectCull.size() == frame.workloads.size() && objectDispatches.size() == frame.workloads.size(),
            "object culling did not dispatch once per workload");
        for (size_t index = 0; index < frame.workloads.size(); ++index) {
            const auto& constants = objectCull[index];
            const auto& workload = frame.workloads[index];
            Require(constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT] == workload.activeDrawCount
                && constants[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX] == workload.viewDataIndex
                && constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_SET_SRV_INDEX] == workload.activeDrawSetIndicesSRVIndex
                && constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_SRV_INDEX] == workload.drawRecordVisibilityGenerationSRVIndex
                && constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_COUNT] == workload.drawRecordVisibilityGenerationCount
                && constants[CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS] == workload.shadowCasterClass
                && constants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(b.frontiers.currentNodes)
                && constants[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(b.frontiers.currentNodeCounter),
                "object culling did not use the exact workload arguments");
            Require(objectDispatches[index].values == std::vector<uint32_t>{workload.dispatchGridX, 1, 1},
                "object culling dispatch grid changed");
        }

        // Every stage reading the shared block sees this frame's values and the
        // declared views.
        for (const uint32_t program : {7u, 6u, 4u, 10u, 11u, 3u, 12u}) {
            for (const auto& constants : trace.MiscPushes(program)) {
                Require(constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] == 77u
                    && constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] == 123u
                    && constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(b.windBounds)
                    && constants[CLOD_WG_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(b.windMembership),
                    "DynamicWind bounds cache constants did not reach every culling stage");
                Require(constants[CLOD_WG_FLAGS] == 0x5u && constants[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] == 61u
                    && constants[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX] == 0u
                    && constants[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(b.visibleClusters)
                    && constants[CLOD_WG_HW_WRITE_BASE_COUNTER_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(b.visibleClustersCounter)
                    && constants[CLOD_PC_PHASE2_EXPANSION_FACTOR] == 4u,
                    "shared culling constants lost frame values or declared views");
            }
        }
        Require(trace.MiscPushes(3).front()[CLOD_PC_DISPATCH_THREADS_PER_GROUP] == 64u / frame.expansionFactor,
            "dense cluster culling ignored the expansion factor");

        const auto indirect = trace.Of(Trace::Kind::Indirect);
        Require(indirect.size() == 3
            && indirect[0].handle == DeclaredFrame::ResourceIndex(b.frontiers.nodeDispatchArgs)
            && indirect[1].handle == DeclaredFrame::ResourceIndex(b.frontiers.leafDispatchArgs)
            && indirect[2].handle == DeclaredFrame::ResourceIndex(b.frontiers.clusterDispatchArgs)
            && std::all_of(indirect.begin(), indirect.end(), [](const auto& event) { return event.root == 40u; }),
            "traversal lost its indirect dispatches");
        const auto create = trace.MiscPushes(2);
        Require(create.size() == 1 && create[0][CLOD_CREATE_NUM_RASTER_BUCKETS] == 9u
            && create[0][CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] == 4096u
            && create[0][CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(b.visibleClustersCounter),
            "raster bucket command creation lost its frame values");

        // A later frame records its own values from the same bindings.
        auto next = frame;
        next.windGeneration = 78u;
        next.workloads.pop_back();
        Trace nextTrace;
        auto nextContext = fixture.declared.Context(TraceList(nextTrace));
        ComputeCullingPass::Record(b, next, nextContext);
        Require(nextTrace.MiscPushes(7).size() == 1
            && nextTrace.MiscPushes(7)[0][CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] == 78u,
            "culling recorded stale frame values");
    }

    // Phase 2 without the bounds cache: replay seeds the frontiers instead of
    // object culling, and the cache reads as absent.
    {
        CullingFixture fixture(false, false);
        Trace trace;
        auto context = fixture.declared.Context(TraceList(trace));
        ComputeCullingPass::Record(fixture.bindings, frame, context);
        Require(trace.Pipelines() == std::vector<uint32_t>{1, 1, 1, 1, 1, 5, 8, 5, 9, 3, 12, 6, 4, 10, 11, 3, 12, 2},
            "phase-2 culling stages changed order");
        Require(trace.Of(Trace::Kind::Indirect).size() == 6, "phase-2 culling lost an indirect dispatch");
        for (const auto& constants : trace.MiscPushes(8))
            Require(constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_DESCRIPTOR_INDEX] == 0u
                && constants[CLOD_WG_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] == 0u,
                "an undeclared bounds cache was bound");
        const auto replay = trace.MiscPushes(5);
        Require(replay.size() == 2 && replay[0][CLOD_PC_REPLAY_SOURCE_INDEX] == 0u && replay[1][CLOD_PC_REPLAY_SOURCE_INDEX] == 1u,
            "occlusion replay seeded its sources out of order");
    }

    // A frame without a render snapshot records nothing.
    {
        CullingFixture fixture(true, true);
        auto inactive = frame;
        inactive.active = false;
        Trace trace;
        auto context = fixture.declared.Context(TraceList(trace));
        ComputeCullingPass::Record(fixture.bindings, inactive, context);
        Require(trace.events.empty(), "inactive culling recorded commands");
    }
}

void TestResolverSnapshotLifetime()
{
    std::weak_ptr<const org::ResolverDeclarationState> lifetime;
    std::shared_ptr<const org::ResolverDeclarationState> retained;
    {
        auto group = std::make_shared<org::ResourceGroup>("LifetimeTest");
        ResourceGroupResolver resolver(group);
        auto first = resolver.CaptureDeclarationState();
        Require(first == resolver.CaptureDeclarationState(), "unchanged group must reuse its snapshot");
        auto clone = resolver;
        Require(clone.CaptureDeclarationState() == first, "clones must share the declaration cache");
        group->ClearResources();
        retained = resolver.CaptureDeclarationState();
        Require(retained != first, "group revision must replace the cached snapshot");
        Require(retained->dependencyIdentity == first->dependencyIdentity,
            "dependency identity must stay stable across revisions");
        lifetime = retained;
    }
    Require(!lifetime.expired(), "queued snapshot must outlive its resolver");
    retained.reset();
    Require(lifetime.expired(), "declaration cache must not own itself through dependency identity");
}

void TestPersistentRendererPublicationBindings()
{
    using namespace br::render;
    const PublishedResourceKey key{PublishedFragmentKind::Materials,PublishedResourceUsage::ShaderResource,0,0,13};
    auto makeState = [&](uint32_t handle, uint32_t descriptor, uint64_t capacity) {
        auto state = std::make_shared<PublishedRendererState>();
        auto snapshot = std::make_shared<org::ResourceBindingSnapshot>();
        snapshot->resourceID = 100 + handle;
        snapshot->resource = rhi::Resource(rhi::ResourceHandle{handle,1});
        snapshot->backingGeneration = 1;
        snapshot->allocationOwner = snapshot->descriptorOwner = std::make_shared<const uint32_t>(handle);
        snapshot->description.type = rhi::ResourceType::Buffer;
        snapshot->description.buffer.sizeBytes = capacity;
        auto views = std::make_shared<org::BindlessResourceViews>();
        views->views.push_back({org::BindlessViewKind::ShaderResource,UINT32_MAX,0,0,
            rhi::DescriptorSlot(rhi::DescriptorHeapHandle{3,1},descriptor)});
        snapshot->views = views;
        PublishedResourceSelection selection;
        selection.contentVersion = handle;
        selection.bindingBundle = std::make_shared<const org::PublicationBindingBundle>(
            std::vector<org::PublicationBindingBundle::Snapshot>{snapshot});
        org::compiler::PreparedBackingState initial;
        initial.graphResourceID = snapshot->resourceID; initial.resource = snapshot->resource.GetHandle();
        initial.shape = {1,1,false};
        initial.regions = std::make_shared<const std::vector<org::compiler::PreparedStateRegion>>(
            std::initializer_list<org::compiler::PreparedStateRegion>{{{0,1,0,1},{}}});
        selection.initialStates = std::make_shared<const std::vector<org::compiler::PreparedBackingState>>(
            std::initializer_list<org::compiler::PreparedBackingState>{initial});
        auto shard = std::make_shared<PublishedResourceCatalog::OwnerShard>();
        shard->selections.emplace(key,std::move(selection));
        auto catalog = std::make_shared<PublishedResourceCatalog>();
        catalog->ownerShards[static_cast<size_t>(key.owner)] = std::move(shard);
        state->resourceCatalog = std::move(catalog);
        return state;
    };
    auto first = makeState(23,7,256);
    PreparePersistentRendererPublication({},*first);
    const auto held = first->persistentPublication;
    const auto slot = held->slots.at(key).front();
    auto replacement = makeState(24,31,512);
    PreparePersistentRendererPublication(first,*replacement);
    Require(replacement->persistentPublication->slots.at(key).front() == slot,"producer replacement preserves logical slot");
    Require(replacement->persistentPublication->graph->executable == held->graph->executable,
        "compatible producer capacity/backing/descriptor rotation must not compile");
    Require(held->graph->bindings.At(slot).recording->resource.GetHandle().index == 23
        && replacement->persistentPublication->graph->bindings.At(slot).recording->resource.GetHandle().index == 24,
        "selected publications retain exact producer backing versions");
    auto scalar = std::make_shared<PublishedRendererState>(*replacement);
    PreparePersistentRendererPublication(replacement,*scalar);
    Require(scalar->persistentPublication == replacement->persistentPublication,"unchanged producer shards share the exact root");
    auto invalid = makeState(25,41,512);
    auto catalog = std::make_shared<PublishedResourceCatalog>(*invalid->resourceCatalog);
    auto shard = std::make_shared<PublishedResourceCatalog::OwnerShard>(*catalog->ownerShards[static_cast<size_t>(key.owner)]);
    shard->selections.at(key).initialStates.reset();
    catalog->ownerShards[static_cast<size_t>(key.owner)] = shard; invalid->resourceCatalog = catalog;
    bool rejected = false;
    try { PreparePersistentRendererPublication(replacement,*invalid); } catch (const std::exception&) { rejected = true; }
    Require(rejected && replacement->persistentPublication->graph->bindings.At(slot).recording->resource.GetHandle().index == 24,
        "missing producer seed rejects replacement without changing selection");
}

void TestPublishedCullingInvocationPreparation()
{
    // Real production invocation preparation, with metadata-only buffers and no
    // device, scene loader, shader compilation or singleton manager initialization.
    org::Resource::ScopedECSRegistrationSuppression suppressECS;
    auto indirect = std::make_shared<br::render::PublishedIndirectState>();
    indirect->visibilityGenerations = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal,4096);
    indirect->visibilityGenerationsSRVIndex = 17;
    br::render::PublishedIndirectWorkload workload;
    workload.viewID = 13;
    workload.key.renderPhase = RenderPhase("Opaque");
    workload.key.clodOnly = true;
    workload.activeDrawList = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal,4096);
    workload.activeDrawListSRVIndex = 7;
    workload.count = 513; workload.capacity = 1024;
    indirect->workloads.push_back(workload);
    auto publication = std::make_shared<br::render::PublishedRendererState>();
    publication->epoch = 1;
    publication->indirectWorkloads.payload = br::render::ArtifactPayload::Make<br::render::PublishedIndirectState>(indirect);
    std::array<PreparedViewFrameData,1> views{};
    views[0].id = 13; views[0].primary = true; views[0].cameraBufferIndex = 3;
    auto prepare = [&](const std::shared_ptr<const br::render::PublishedRendererState>& selected) {
        return br::render::PrepareCullingWorkloads(views,selected,RenderPhase("Opaque"),true,false,
            CLodRasterOutputKind::VisibilityBuffer,256,"HeadlessCullingTest");
    };
    const auto first = prepare(publication);
    Require(first.size() == 1 && first[0].activeDrawCount == 513 && first[0].dispatchGridX == 3,
        "selected workload count and dispatch grid");
    Require(first[0].activeDrawSetIndicesSRVIndex == 7 && first[0].drawRecordVisibilityGenerationSRVIndex == 17,
        "exact selected publication descriptors");
    auto replacement = std::make_shared<br::render::PublishedIndirectState>(*indirect);
    replacement->workloads[0].count = 1025;
    replacement->workloads[0].capacity = 2048;
    replacement->workloads[0].activeDrawListSRVIndex = 31;
    replacement->workloads[0].activeDrawList = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal,8192);
    auto next = std::make_shared<br::render::PublishedRendererState>(*publication);
    next->epoch = 2;
    next->indirectWorkloads.payload = br::render::ArtifactPayload::Make<br::render::PublishedIndirectState>(replacement);
    views[0].cameraBufferIndex = 8;
    const auto current = prepare(next);
    const auto held = prepare(publication);
    Require(current.size() == 1 && current[0].activeDrawCount == 1025 && current[0].dispatchGridX == 5
        && current[0].activeDrawSetIndicesSRVIndex == 31,"replacement invocation bindings");
    Require(held.size() == 1 && held[0].activeDrawCount == 513 && held[0].activeDrawSetIndicesSRVIndex == 7
        && held[0].viewDataIndex == 8,"held publication with fresh camera values");
    views[0].id = 999;
    Require(prepare(next).empty(),"absent workloads must not issue dispatches");
}

std::shared_ptr<org::PixelBuffer> MakeHistoryResource()
{
    org::TextureDescription description;
    description.format = rhi::Format::R32_Float;
    description.imageDimensions.push_back({ 16u, 16u, 0u, 0u });
    description.hasSRV = true;
    return org::PixelBuffer::CreateSharedUnmaterialized(description);
}

void TestDepthHistoryReservationOwnership()
{
    auto resource = MakeHistoryResource();
    auto views = std::make_shared<br::render::PreparedViewFamilyState>();
    views->views.push_back({ .id = 17, .linearDepthMap = resource });

    auto service = std::make_unique<br::render::DepthHistoryPublicationService>();
    auto first = service->ReserveDepthHistoryPublication(views, 41);
    auto pending = service->Select(17, resource);
    Require(pending && pending.producerFrameNumber == 41,
        "an accepted producer must immediately become selectable history");
    Require(pending.producerSubmissionID == 0,
        "unsubmitted history must expose an unresolved submission dependency");

    first->Submitted({ .submissionID = 73 });
    auto submitted = service->Select(17, resource);
    Require(submitted && submitted.producerSubmissionID == 73,
        "submission must resolve the selected history dependency");

    auto second = service->ReserveDepthHistoryPublication(views, 42);
    auto selectedSecond = service->Select(17, resource);
    Require(selectedSecond && selectedSecond.producerFrameNumber == 42,
        "the immediate pending predecessor must supersede older published history");
    second->Abandoned(org::AbandonReason::PreparationFailed);
    Require(!selectedSecond,
        "cancelling a producer must invalidate selections already copied by consumers");
    auto restored = service->Select(17, resource);
    Require(restored && restored.producerFrameNumber == 41,
        "cancelling pending history must reveal the last submitted compatible producer");

    auto stale = service->ReserveDepthHistoryPublication(views, 43);
    auto staleSelection = service->Select(17, resource);
    service->Clear();
    Require(!staleSelection, "a generation boundary must invalidate copied history selections");
    stale->Submitted({ .submissionID = 74 });
    Require(!service->Select(17, resource),
        "a delayed callback from an old generation must not republish history");

    auto retainedSubmission = service->ReserveDepthHistoryPublication(views, 44);
    auto retainedSelection = service->Select(17, resource);
    auto retainedCancellation = service->ReserveDepthHistoryPublication(views, 45);
    service.reset();
    retainedSubmission->Submitted({ .submissionID = 75 });
    Require(retainedSelection && retainedSelection.dependency->submissionID == 75,
        "delayed history callbacks must retain their service state");
    retainedCancellation->Abandoned(org::AbandonReason::Shutdown);
}

void TestDemoPreset()
{
    const auto recipe = br::pipeline::MakeBasicRendererDemoPipeline();
    Require(recipe.Validate().valid, "demo recipe must validate");
    Require(!recipe.Contains<br::pipeline::TerrainRvtTechnique>(), "demo recipe must omit RVT");
    Require(
        recipe.Options<br::pipeline::ClusterLodTechnique>().reyes == br::pipeline::ReyesMode::Disabled,
        "demo recipe must disable Reyes");
    Require(!recipe.Contains<br::pipeline::ClusterLodVoxelTechnique>(),
        "demo recipe must omit CLod voxel rasterization");
    Require(!recipe.Contains<br::pipeline::ClusterLodAlphaTechnique>(), "demo recipe must omit CLod alpha");
    Require(recipe.Contains<br::pipeline::ClusterLodShadowTechnique>(), "demo recipe must include CLod shadows");
    Require(recipe.Contains<br::pipeline::GtaoTechnique>(), "demo recipe must include GTAO");
    Require(recipe.Contains<br::pipeline::ClusteredLightingTechnique>(), "demo recipe must include clustered lighting");
    Require(recipe.Contains<br::pipeline::TonemappingTechnique>(), "demo recipe must include tonemapping");
}

void TestSarpPreset()
{
    auto recipe = br::pipeline::MakeSarpPipeline();
    Require(recipe.Validate().valid, "SARP recipe must validate");
    Require(recipe.Contains<br::pipeline::TerrainRvtTechnique>(), "SARP recipe must include RVT");
    Require(
        recipe.Options<br::pipeline::ClusterLodTechnique>().reyes == br::pipeline::ReyesMode::Enabled,
        "SARP recipe must enable Reyes");
    Require(recipe.Contains<br::pipeline::ClusterLodVoxelTechnique>(),
        "SARP recipe must include CLod voxel rasterization");
    Require(recipe.Options<br::pipeline::ClusterLodVoxelTechnique>().workRecordCapacity == (1u << 20),
        "SARP recipe must use the bounded default CLod voxel work capacity");
    Require(recipe.Contains<br::pipeline::ClusterLodAlphaTechnique>(), "SARP recipe must include CLod alpha");
    Require(recipe.Contains<br::pipeline::ClusterLodShadowTechnique>(), "SARP recipe must include CLod shadows");
    Require(recipe.Contains<br::pipeline::CanonicalSurfaceResourcesTechnique>(),
        "SARP recipe must publish canonical surfaces directly");

    recipe.Configure<br::pipeline::ClusterLodVoxelTechnique>({ .workRecordCapacity = 262144u });
    Require(recipe.Options<br::pipeline::ClusterLodVoxelTechnique>().workRecordCapacity == 262144u,
        "CLod voxel capacity must be caller-configurable");
    recipe.Remove<br::pipeline::ClusterLodVoxelTechnique>();
    Require(recipe.Validate().valid && !recipe.Contains<br::pipeline::ClusterLodVoxelTechnique>(),
        "CLod voxel rasterization must be independently removable");
}

void TestGeometryMaterialProducerPreset()
{
    const auto recipe = br::pipeline::MakeGeometryMaterialProducerPipeline();
    Require(recipe.Validate().valid, "geometry/material producer recipe must validate");
    Require(recipe.Contains<br::pipeline::TerrainRvtTechnique>(), "producer must include RVT");
    Require(recipe.Contains<br::pipeline::ClusterLodShadowTechnique>(), "producer must include VSM caster rendering");
    Require(recipe.Contains<br::pipeline::MaterialEvaluationTechnique>(), "producer must evaluate materials");
    Require(recipe.Contains<br::pipeline::CanonicalSurfaceResourcesTechnique>(), "producer must publish canonical surfaces");
    Require(!recipe.Contains<br::pipeline::EnvironmentTechnique>(), "producer must omit environment processing");
    Require(!recipe.Contains<br::pipeline::GtaoTechnique>(), "producer must omit GTAO");
    Require(!recipe.Contains<br::pipeline::ClusteredLightingTechnique>(), "producer must omit light clustering");
    Require(!recipe.Contains<br::pipeline::PrimaryLightingTechnique>(), "producer must omit deferred lighting");
    Require(!recipe.Contains<br::pipeline::ReflectionsTechnique>(), "producer must omit reflections");
    Require(!recipe.Contains<br::pipeline::UpscalingTechnique>(), "producer must omit upscaling");
    Require(!recipe.Contains<br::pipeline::TonemappingTechnique>(), "producer must omit tonemapping");
    Require(!recipe.Contains<br::pipeline::PresentTechnique>(), "producer must omit presentation");
}

void TestInvalidRecipes()
{
    auto missingBinning = br::pipeline::MakeBasicRendererDemoPipeline();
    missingBinning.Add<br::pipeline::TerrainRvtTechnique>();
    missingBinning.Remove<br::pipeline::VisibilityMaterialBinningTechnique>();
    Require(!missingBinning.Validate().valid, "RVT without binning must fail validation");

    auto duplicateExtensions = br::pipeline::MakeBasicRendererDemoPipeline();
    duplicateExtensions.AddExtension("duplicate", [] { return std::unique_ptr<org::IRenderGraphExtension>{}; });
    duplicateExtensions.AddExtension("duplicate", [] { return std::unique_ptr<org::IRenderGraphExtension>{}; });
    Require(!duplicateExtensions.Validate().valid, "duplicate extension ids must fail validation");

    org::TextureDescription environmentDescription;
    environmentDescription.format = rhi::Format::R16G16B16A16_Float;
    environmentDescription.imageDimensions.push_back({ 4u, 4u, 0u, 0u });
    environmentDescription.isCubemap = true;
    environmentDescription.arraySize = 6u;
    environmentDescription.hasSRV = false;
    auto incompatibleEnvironment = org::PixelBuffer::CreateSharedUnmaterialized(environmentDescription);
    auto incompatibleBinding = br::pipeline::MakeBasicRendererDemoPipeline();
    incompatibleBinding.Bindings().Bind(
        br::pipeline::Slots::EnvironmentCubemap,
        incompatibleEnvironment,
        br::pipeline::ResourceBindingContract{
            .format = rhi::Format::R16G16B16A16_Float,
            .width = 4u,
            .height = 4u,
            .requiredViews = static_cast<uint8_t>(br::pipeline::ResourceViewCapability::ShaderResource) });
    Require(!incompatibleBinding.Validate().valid, "a binding missing required views must fail validation");

    auto invalidOrder = br::pipeline::MakeBasicRendererDemoPipeline();
    invalidOrder.Remove<br::pipeline::CanonicalSurfaceResourcesTechnique>();
    invalidOrder.Add<br::pipeline::CanonicalSurfaceResourcesTechnique>();
    Require(!invalidOrder.Validate().valid, "technique dependency order must be validated");

    auto invalidVoxel = br::pipeline::MakeBasicRendererDemoPipeline();
    invalidVoxel.Add<br::pipeline::ClusterLodVoxelTechnique>({ .workRecordCapacity = 0u });
    Require(!invalidVoxel.Validate().valid, "zero-capacity CLod voxel technique must fail validation");
}

void TestPixelListRecording()
{
    // The publication lookup BuildPixelListPass::Prepare turns into the voxel bin.
    br::render::PublishedMaterialState material;
    material.activeCompileFlags = {MaterialCompileFlags::MaterialCompileNone, MaterialCompileFlags::MaterialCompileVoxel};
    material.activeCompileFlagSlots = {3, 7};
    uint32_t bin = UINT32_MAX;
    Require(material.TryGetCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel, bin) && bin == 7,
        "pixel-list voxel material bin lookup");
    material.activeCompileFlagSlots.pop_back();
    bin = UINT32_MAX;
    Require(!material.TryGetCompileFlagsSlot(MaterialCompileFlags::MaterialCompileVoxel, bin) && bin == UINT32_MAX,
        "pixel-list absent material slot must keep the sentinel");

    for (const bool diceQueue : {true, false}) {
        DeclaredFrame declared;
        BuildPixelListBindings bindings{Pipeline(5), declared.Declare(DeclaredFrame::Kind::ShaderResource)};
        if (diceQueue) {
            bindings.reyesDiceQueue = declared.Declare(DeclaredFrame::Kind::ShaderResource);
            bindings.hasReyesDiceQueue = true;
        }
        bindings.patchVisibilityIndexBase = 31;
        Trace trace;
        auto context = declared.Context(TraceList(trace));
        BuildPixelListPass::Record(bindings, {7, 3, 2}, context);
        BuildPixelListPass::Record(bindings, {17, 6, 5}, context);
        BuildPixelListPass::Record(bindings, {UINT32_MAX, 0, 0}, context);
        const auto pushes = trace.MiscPushes(5);
        const auto dispatches = trace.Of(Trace::Kind::Dispatch);
        Require(pushes.size() == 2 && dispatches.size() == 2 && trace.Pipelines() == std::vector<uint32_t>{5, 5},
            "zero-sized pixel-list frame recorded commands");
        Require(pushes[0][VISBUF_VOXEL_MATERIAL_BIN_INDEX] == 7 && dispatches[0].values == std::vector<uint32_t>{3, 2, 1},
            "pixel-list first frame recording");
        Require(pushes[1][VISBUF_VOXEL_MATERIAL_BIN_INDEX] == 17 && dispatches[1].values == std::vector<uint32_t>{6, 5, 1},
            "pixel-list later frame recording");
        Require(pushes[1][VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(bindings.visibleClusters)
            && pushes[1][VISBUF_REYES_PATCH_INDEX_BASE] == 31
            && pushes[1][VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX]
                == (diceQueue ? DeclaredFrame::Srv(bindings.reyesDiceQueue) : UINT32_MAX),
            "pixel-list declared views or absent dice queue");
    }
}

void TestMaterialCommandSelection()
{
    struct SyntheticBindings {
        uint32_t visibleClusters = 777, visibleClusterTransformIndices = 778;
        uint32_t reyesDiceQueue = 779, reyesTessTableConfigs = 780, reyesTessTableVertices = 781, reyesTessTableTriangles = 782;
        bool hasReyesDiceQueue = true, hasReyesTessTables = true;
        uint32_t patchVisibilityIndexBase = 777;
    } bindings;
    EvaluateMaterialGroupsPass::RecordingConfiguration configuration{false,0.25f,3,0.75f};
    const auto constants = EvaluateMaterialGroupsPass::BuildConstants<uint32_t>(bindings,configuration,
        [](uint32_t descriptor) { return descriptor+1000; });
    Require(constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 1777
        && constants[VISBUF_REYES_PATCH_INDEX_BASE] == 777 && constants[VISBUF_REYES_USE_NORMAL_MAPS] == 0
        && constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] == std::bit_cast<uint32_t>(0.25f)
        && constants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] == std::bit_cast<uint32_t>(0.75f),
        "material constants confused descriptors with scalars or lost captured normal configuration");
    bindings.hasReyesDiceQueue = false; bindings.hasReyesTessTables = false;
    const auto missing = EvaluateMaterialGroupsPass::BuildConstants<uint32_t>(bindings,configuration,[](uint32_t value) { return value; });
    Require(missing[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] == UINT32_MAX
        && missing[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX] == UINT32_MAX,
        "material constants failed to preserve unavailable optional bindings");
    br::render::PublishedMaterialState materials;
    materials.activeCompileFlags = {MaterialCompileNone,MaterialCompileTerrain,MaterialCompileNormalMap,MaterialCompileAlphaTest};
    materials.activeCompileFlagSlots = {3,1,7,2};
    materials.compileFlagSlotsUsed = 8;
    struct Entry { MaterialCompileFlags key; uint32_t slot; uint64_t offset; };
    const auto select = [&](bool terrain, OutputType output, uint64_t bytes) {
        std::vector<Entry> entries;
        EvaluateMaterialGroupsPass::VisitCommands(materials,terrain,output,bytes,
            [&](MaterialCompileFlags,MaterialCompileFlags key,uint32_t slot,uint64_t offset) {
                entries.push_back({key,slot,offset});
            });
        return entries;
    };
    constexpr uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
    auto selected = select(true,OutputType::COLOR,8*stride);
    Require(selected.size() == 3 && selected[0].slot == 3 && selected[1].slot == 7 && selected[2].slot == 2,
        "material command selection changed active order or terrain filtering");
    Require(selected[1].offset == 7*stride && (selected[1].key & MaterialCompileMaterialEvalColorOnly) != 0,
        "material selection lost slot offset or color program variant");
    auto bounded = select(false,OutputType::NORMAL,3*stride);
    Require(bounded.size() == 2 && bounded[0].slot == 1 && bounded[1].slot == 2,
        "material argument bounds rejected an exact end or accepted out-of-range commands");
    Require((bounded[0].key & MaterialCompileMaterialEvalColorOnly) == 0,
        "material non-color selection used color-only program");
    materials.compileFlagSlotsUsed = 3;
    materials.activeCompileFlagSlots.pop_back();
    Require(select(false,OutputType::NORMAL,UINT64_MAX).size() == 1,
        "material selection accepted an unavailable or unused slot");
    Require(select(false,OutputType::NORMAL,stride-1).empty(),
        "material selection accepted a truncated argument buffer");
}

void TestMaterialEvaluationRecording()
{
    using Kind = DeclaredFrame::Kind;
    constexpr uint64_t stride = sizeof(MaterialEvaluationIndirectCommand);
    // The programs name a declared input, an RVT slot and an optional undeclared slot.
    const org::PipelineResources reflection{{"Builtin::Test::MaterialInput", "Builtin::Terrain::Rvt::Regions"},
        {"Builtin::Test::MaterialOptional"}};
    for (const bool terrainRvt : {false, true}) {
        DeclaredFrame declared;
        EvaluateMaterialGroupsBindings bindings{};
        bindings.visibleClusters = declared.Declare(Kind::ShaderResource);
        bindings.visibleClusterTransformIndices = declared.Declare(Kind::ShaderResource);
        const auto input = declared.Declare(Kind::ShaderResource, "Builtin::Test::MaterialInput");
        const auto arguments = declared.Declare(Kind::ShaderResource);
        bindings.arguments = arguments.Resource();
        bindings.patchVisibilityIndexBase = 777;
        bindings.terrainRvtEnabled = terrainRvt;
        MaterialEvaluationFrame frame;
        frame.signature = Signature(4);
        frame.steps = {{Payload(8, reflection), 2 * stride}, {Payload(9, reflection), 5 * stride}};
        frame.configuration = {false, 0.25f, 3, 0.75f};
        Trace trace;
        auto context = declared.Context(TraceList(trace));
        if (terrainRvt) {
            // With RVT enabled its slots are mandatory: an undeclared one fails
            // before the pass emits anything.
            Require(Rejects<std::invalid_argument>([&] { EvaluateMaterialGroupsPass::Record(bindings, frame, context); })
                && trace.events.empty(), "material evaluation bound an undeclared RVT resource");
            continue;
        }
        EvaluateMaterialGroupsPass::Record(bindings, frame, context);
        Require(trace.Pipelines() == std::vector<uint32_t>{8, 9}
            && trace.Handles(Trace::Kind::Layout) == std::vector<uint32_t>{58, 59},
            "material evaluation lost a program or its layout");
        const auto slots = trace.DescriptorSlotPushes();
        Require(slots.size() == 2 && slots[0] == std::vector<uint32_t>{DeclaredFrame::Srv(input), UINT32_MAX, UINT32_MAX}
            && slots[1] == slots[0], "material descriptor slots did not resolve declared, absent RVT and optional names");
        const auto pushes = trace.MiscPushes();
        Require(pushes.size() == 2 && pushes[0] == pushes[1], "material steps pushed different constants");
        const auto& constants = pushes[0];
        Require(constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(bindings.visibleClusters)
            && constants[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX]
                == DeclaredFrame::Srv(bindings.visibleClusterTransformIndices)
            && constants[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] == UINT32_MAX
            && constants[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] == UINT32_MAX
            && constants[VISBUF_REYES_PATCH_INDEX_BASE] == 777 && constants[VISBUF_REYES_USE_NORMAL_MAPS] == 0
            && constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] == std::bit_cast<uint32_t>(0.25f)
            && constants[VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS] == 3
            && constants[VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT] == std::bit_cast<uint32_t>(0.75f),
            "material constants lost declared views or the frame's normal configuration");
        const auto indirect = trace.Of(Trace::Kind::Indirect);
        Require(indirect.size() == 2 && indirect[0].offset == 2 * stride && indirect[1].offset == 5 * stride
            && indirect[0].handle == DeclaredFrame::ResourceIndex(arguments) && indirect[1].root == 4,
            "material evaluation lost its argument slots");
        // Each step binds, pushes, then executes.
        std::vector<Trace::Kind> order;
        for (const auto& event : trace.events) order.push_back(event.kind);
        using K = Trace::Kind;
        Require(order == std::vector<K>{K::Layout, K::Pipeline, K::Push, K::Push, K::Indirect,
            K::Layout, K::Pipeline, K::Push, K::Push, K::Indirect}, "material step commands changed order");

        Trace empty;
        auto emptyContext = declared.Context(TraceList(empty));
        EvaluateMaterialGroupsPass::Record(bindings, MaterialEvaluationFrame{}, emptyContext);
        Require(empty.events.empty(), "material evaluation without active programs recorded commands");
    }
}

void TestSoftwareRasterRecording()
{
    using Kind = DeclaredFrame::Kind;
    constexpr uint64_t stride = sizeof(RasterizeClustersCommand);
    const auto declareCommon = [](DeclaredFrame& declared, ClusterSoftwareRasterBindings& bindings) {
        bindings.rasterSignature = Signature(20);
        bindings.cacheDispatchSignature = Signature(21);
        bindings.histogram = declared.Declare(Kind::ShaderResource);
        bindings.visible = declared.Declare(Kind::ShaderResource);
        bindings.transforms = declared.Declare(Kind::ShaderResource);
        bindings.mapping = declared.Declare(Kind::ShaderResource);
        const auto arguments = declared.Declare(Kind::ShaderResource);
        bindings.indirectArgs = arguments.Resource();
        return arguments;
    };

    // Visibility output: one indirect dispatch per bucket with a pipeline.
    {
        DeclaredFrame declared;
        ClusterSoftwareRasterBindings bindings{};
        const auto arguments = declareCommon(declared, bindings);
        ClusterSoftwareRasterFrame frame;
        frame.enabled = true;
        frame.bucketCount = 3;
        frame.viewRasterInfoTable = 66;
        frame.steps = {{Pipeline(30), 0}, {Pipeline(32), 2 * stride}};
        Trace trace;
        auto context = declared.Context(TraceList(trace));
        ClusterSoftwareRasterizationPass::Record(bindings, frame, context);
        Require(trace.Pipelines() == std::vector<uint32_t>{30, 32}, "software raster changed bucket programs");
        const auto indirect = trace.Of(Trace::Kind::Indirect);
        Require(indirect.size() == 2 && indirect[0].offset == 0 && indirect[1].offset == 2 * stride
            && indirect[0].root == 20 && indirect[1].handle == DeclaredFrame::ResourceIndex(arguments),
            "software raster changed bucket argument offsets");
        const auto pushes = trace.MiscPushes();
        Require(pushes.size() == 2 && pushes[0] == pushes[1], "software raster steps pushed different constants");
        Require(pushes[0][CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(bindings.histogram)
            && pushes[0][CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(bindings.mapping)
            && pushes[0][CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] == 66
            && pushes[0][CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] == UINT32_MAX
            && pushes[0][CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] == UINT32_MAX
            && pushes[0][CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] == 0,
            "software raster primary constants lost declared views or missing-cache defaults");

        for (const bool enabled : {false, true}) {
            auto idle = frame;
            idle.enabled = enabled;
            idle.bucketCount = enabled ? 0u : 3u;
            Trace idleTrace;
            auto idleContext = declared.Context(TraceList(idleTrace));
            ClusterSoftwareRasterizationPass::Record(bindings, idle, idleContext);
            Require(idleTrace.events.empty(), "inactive software raster recorded commands");
        }
    }

    // Virtual shadows with the DynamicWind skin cache: clear, build over every
    // bucket, finalize, skin, resolve over every bucket, then rasterize.
    {
        DeclaredFrame declared;
        ClusterSoftwareRasterBindings bindings{};
        declareCommon(declared, bindings);
        bindings.cacheClear = Pipeline(11);
        bindings.cacheBuild = Pipeline(12);
        bindings.cacheFinalize = Pipeline(13);
        bindings.cacheSkin = Pipeline(14);
        bindings.cacheResolve = Pipeline(15);
        bindings.pageTable = declared.Declare();
        bindings.clipmapInfo = declared.Declare(Kind::ShaderResource);
        bindings.physicalPages = declared.Declare();
        bindings.dynamicPages = declared.Declare();
        bindings.telemetry = declared.Declare();
        bindings.skinMapping = declared.Declare();
        bindings.skinHash = declared.Declare();
        bindings.skinPositions = declared.Declare();
        bindings.skinAllocator = declared.Declare();
        bindings.skinWork = declared.Declare();
        bindings.skinArgs = declared.Declare();
        bindings.skinMembership = declared.Declare(Kind::ShaderResource);
        bindings.skinCacheHashEntries = 10;
        bindings.skinCachePositionCapacity = 1024;
        bindings.virtualShadow = bindings.hasTelemetry = bindings.hasSkinCache = true;
        ClusterSoftwareRasterFrame frame;
        frame.enabled = frame.hasSkinCache = true;
        frame.bucketCount = 2;
        frame.skinCacheGeneration = 42;
        frame.pageTableResolution = 128;
        frame.virtualResolution = 16384;
        frame.steps = {{Pipeline(30), 0}, {Pipeline(31), stride}};
        Trace trace;
        auto context = declared.Context(TraceList(trace));
        ClusterSoftwareRasterizationPass::Record(bindings, frame, context);
        Require(trace.Pipelines() == std::vector<uint32_t>{11, 12, 13, 14, 15, 30, 31},
            "software skin cache lost a phase or changed order");
        const auto indirect = trace.Of(Trace::Kind::Indirect);
        Require(indirect.size() == 7 && indirect[0].offset == 0 && indirect[1].offset == stride
            && indirect[2].root == 21 && indirect[2].handle == DeclaredFrame::ResourceIndex(bindings.skinArgs)
            && indirect[3].offset == 0 && indirect[4].offset == stride && indirect[6].offset == stride,
            "software skin cache lost a bucket pass or its dispatch arguments");
        std::vector<uint32_t> barriers;
        for (const auto& barrier : trace.Of(Trace::Kind::Barrier)) barriers.insert(barriers.end(), barrier.values.begin(), barrier.values.end());
        const auto resource = [](const org::DeclaredViewToken& token) { return DeclaredFrame::ResourceIndex(token); };
        Require(barriers == std::vector<uint32_t>{resource(bindings.skinAllocator), resource(bindings.skinHash),
            resource(bindings.skinAllocator), resource(bindings.skinWork), resource(bindings.skinArgs),
            resource(bindings.skinPositions), resource(bindings.skinMapping)},
            "software skin cache lost required synchronization");
        Require(trace.Of(Trace::Kind::Barrier)[4].root == static_cast<uint32_t>(rhi::ResourceAccessType::IndirectArgument),
            "skin dispatch arguments were not made indirect arguments");
        const auto clear = trace.MiscPushes(11);
        Require(clear.size() == 2 && clear[0][CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(bindings.skinAllocator)
            && clear[0][CLOD_CLEAR_UINT_BUFFER_COUNT] == 2u
            && clear[1][CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] == 42u,
            "software skin cache clear or generation constants");
        const auto steps = trace.MiscPushes(30);
        Require(steps.size() == 1 && steps[0][CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] == 42u
            && steps[0][CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(bindings.pageTable)
            && steps[0][CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] == 128u
            && steps[0][CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] == 16384u
            && steps[0][CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT] == 10u
            && steps[0][CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY] == 1024u
            && steps[0][CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] == DeclaredFrame::Uav(bindings.telemetry)
            && steps[0][CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] == DeclaredFrame::Srv(bindings.skinMembership),
            "software shadow raster constants confused scalar shape with declared views");
        Require(trace.Of(Trace::Kind::Dispatch).size() == 2, "software skin cache clear/finalize dispatches");

        // A reserved generation is rejected before anything is recorded.
        for (const uint32_t reserved : {0u, 0x7FFFFFFFu}) {
            auto invalid = frame;
            invalid.skinCacheGeneration = reserved;
            Trace rejected;
            auto rejectedContext = declared.Context(TraceList(rejected));
            Require(Rejects<std::invalid_argument>([&] {
                ClusterSoftwareRasterizationPass::Record(bindings, invalid, rejectedContext); })
                && rejected.events.empty(), "invalid skin-cache invocation partially recorded commands");
        }
    }
}
}

int main()
{
    try {
        TestResolverSnapshotLifetime();
        TestComputeCullingRecording();
        TestPublishedCullingInvocationPreparation();
        TestPersistentRendererPublicationBindings();
        TestPixelListRecording();
        TestMaterialCommandSelection();
        TestMaterialEvaluationRecording();
        TestSoftwareRasterRecording();
        TestDepthHistoryReservationOwnership();
        TestDemoPreset();
        TestSarpPreset();
        TestGeometryMaterialProducerPreset();
        TestInvalidRecipes();
        std::cout << "PipelineRecipeTests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "PipelineRecipeTests failed: " << error.what() << '\n';
        return 1;
    }
}
