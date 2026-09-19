#include <iostream>
#include <algorithm>
#include <stdexcept>

#include "Render/Pipeline/PipelineRecipe.h"
#include "Render/DepthHistoryService.h"
#include "Render/RenderGraph/ExperimentalRhiExecution.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "RenderPasses/PreparedComputeCommands.h"
#include "RenderPasses/VisUtil/BuildPixelListPass.h"
#include "RenderPasses/VisUtil/EvaluateMaterialGroupsPass.h"
#include "RenderPasses/VisUtil/PersistentMaterialEvaluationPass.h"
#include "Render/PersistentRendererPublication.h"
#include "Render/GraphExtensions/ClusterLOD/HierarchicalDispatchCullingPass.h"
#include "Render/GraphExtensions/ClusterLOD/PersistentHierarchicalDispatchCullingPass.h"
#include "Render/GraphExtensions/ClusterLOD/CapturedHierarchicalDispatchCullingInputs.h"
#include "Render/GraphExtensions/ClusterLOD/CapturedCullingCommandSink.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterizationPass.h"
#include "Render/GraphExtensions/ClusterLOD/PreparedCullingWorkloads.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodPureComputeCullingRootConstants.h"
#include "../shaders/PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"

namespace {
struct RecordedConstants {
    std::vector<uint32_t> values;
    std::vector<std::vector<uint32_t>> history;
    uint32_t dispatches = 0;
    std::array<uint32_t,3> groups{};
    rhi::PipelineHandle pipeline{};
    rhi::PipelineLayoutHandle layout{};
    rhi::ResourceHandle barrierResource{}, indirectResource{}, workGraphBacking{}, workGraphInput{};
    rhi::ResourceHandle indirectCountBuffer{};
    uint64_t indirectOffset = 0;
    uint64_t indirectCountOffset = 0;
    uint32_t indirectCount = 0;
    rhi::WorkGraphHandle workGraph{};
    bool initializeBacking = false;
};

void TestInvocationConstantRecording()
{
    RecordedConstants recorded;
    rhi::CommandListVTable table{};
    table.abi_version = rhi::RHI_CL_ABI_MIN;
    table.pushConstants = +[](rhi::CommandList* commands, rhi::ShaderStage, uint32_t, uint32_t,
        uint32_t, uint32_t count, const void* values) noexcept {
        auto& state = *static_cast<RecordedConstants*>(commands->impl);
        const auto* words = static_cast<const uint32_t*>(values);
        state.values.assign(words, words + count);
        state.history.push_back(state.values);
    };
    table.dispatch = +[](rhi::CommandList* commands, uint32_t, uint32_t, uint32_t) noexcept {
        ++static_cast<RecordedConstants*>(commands->impl)->dispatches;
    };
    rhi::CommandList commands{rhi::CommandListHandle{1,1}};
    commands.vt = &table;
    commands.impl = &recorded;
    auto bindings = std::make_shared<const org::FrozenExecutionBindings>(
        std::vector<org::FrozenExecutionBindings::ResourceBinding>{});
    org::RecordingContext context(commands, bindings);
    br::render::PreparedComputeCommandSequence recipe;
    recipe.commands.emplace_back(br::render::PreparedComputeConstants{7, 0, {10,20,30}, uint64_t{1} << 1});
    recipe.commands.emplace_back(br::render::PreparedDispatchGroups{2,1,1});
    const std::array<uint32_t,3> first{0,41,0}, second{0,42,0};
    br::render::RecordPreparedComputeCommands(recipe, context, nullptr, {}, first);
    if (recorded.values != std::vector<uint32_t>{10,41,30}) throw std::runtime_error("first invocation constants");
    br::render::RecordPreparedComputeCommands(recipe, context, nullptr, {}, second);
    if (recorded.values != std::vector<uint32_t>{10,42,30} || recorded.dispatches != 2)
        throw std::runtime_error("fresh invocation constants and dispatches");
    if (std::get<br::render::PreparedComputeConstants>(recipe.commands.front()).values != std::vector<uint32_t>{10,20,30})
        throw std::runtime_error("recording mutated immutable recipe constants");
    bool invalidInvocationRejected = false;
    try { br::render::RecordPreparedComputeCommands(recipe, context); }
    catch (const std::out_of_range&) { invalidInvocationRejected = true; }
    if (!invalidInvocationRejected) throw std::runtime_error("missing invocation constant was accepted");

    // Exact object-cull arguments stay fresh between the immutable prefix and
    // suffix; recording needs no pass object or live scene manager.
    recorded.history.clear();
    HierarchicalDispatchCullingRecipe culling;
    culling.hasObjectCull = true;
    culling.objectCullConstants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] = 101;
    culling.prefix.commands.emplace_back(br::render::PreparedComputeConstants{7,0,{11}});
    culling.suffix.commands.emplace_back(br::render::PreparedComputeConstants{7,0,{99}});
    HierarchicalDispatchCullingInvocation invocation;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] = 77;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] = 123;
    invocation.workloads.push_back({3,4,5,6,2,7,1,1});
    invocation.workloads.push_back({13,14,15,16,1,17,1,1});
    HierarchicalDispatchCullingPass::Record(culling, invocation, context);
    if (recorded.history.size() != 4 || recorded.history.front() != std::vector<uint32_t>{11}
        || recorded.history.back() != std::vector<uint32_t>{99})
        throw std::runtime_error("culling invocation escaped its prefix/suffix ordering");
    for (size_t index = 0; index < invocation.workloads.size(); ++index) {
        const auto& constants = recorded.history[index + 1];
        const auto& workload = invocation.workloads[index];
        if (constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT] != workload.activeDrawCount
            || constants[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX] != workload.viewDataIndex
            || constants[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_SET_SRV_INDEX] != workload.activeDrawSetIndicesSRVIndex
            || constants[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_SRV_INDEX] != workload.drawRecordVisibilityGenerationSRVIndex
            || constants[CLOD_PC_OBJECT_CULL_SHADOW_CASTER_CLASS] != workload.shadowCasterClass
            || constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] != 77
            || constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] != 123
            || constants[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX] != 101)
            throw std::runtime_error("culling invocation did not use exact current arguments");
    }
}

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
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
        org::experimental::PreparedBackingState initial;
        initial.graphResourceID = snapshot->resourceID; initial.resource = snapshot->resource.GetHandle();
        initial.shape = {1,1,false};
        initial.regions = std::make_shared<const std::vector<org::experimental::PreparedStateRegion>>(
            std::initializer_list<org::experimental::PreparedStateRegion>{{{0,1,0,1},{}}});
        selection.initialStates = std::make_shared<const std::vector<org::experimental::PreparedBackingState>>(
            std::initializer_list<org::experimental::PreparedBackingState>{initial});
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

void TestPersistentDescriptorCommandRecording()
{
    org::persistent::GraphProgram program;
    auto edit = program.BeginEdit();
    auto owner = std::make_shared<const uint32_t>(1);
    auto snapshot = std::make_shared<org::ResourceBindingSnapshot>();
    snapshot->resource = rhi::Resource(rhi::ResourceHandle{11,1});
    snapshot->backingGeneration = 1; snapshot->allocationOwner = owner; snapshot->descriptorOwner = owner;
    auto views = std::make_shared<org::BindlessResourceViews>();
    views->views.push_back({org::BindlessViewKind::ShaderResource,UINT32_MAX,0,0,
        rhi::DescriptorSlot(rhi::DescriptorHeapHandle{3,1},7)});
    snapshot->views = views;
    org::persistent::BindingVersion binding;
    binding.identity = (uint64_t{1} << 32) | 11; binding.backingRevision = 1;
    binding.owner = owner; binding.recording = snapshot;
    const auto slot = edit.AddResource(binding.shape,std::move(binding));
    const auto pass = edit.AddPass({});
    const auto resource = edit.Declare(pass,slot,{},
        {static_cast<uint64_t>(rhi::ResourceAccessType::ShaderResource)
            | static_cast<uint64_t>(rhi::ResourceAccessType::UnorderedAccess)
            | static_cast<uint64_t>(rhi::ResourceAccessType::IndirectArgument),0,1,true});
    const auto view = edit.DeclareView(resource,{});
    org::experimental::CompileWorkspace workspace; std::atomic_bool cancelled{false};
    auto held = edit.Build(workspace,cancelled);
    Require(program.Install(held), "persistent descriptor bootstrap");
    br::render::PreparedComputeCommandSequence commands;
    auto nativeOwner = std::make_shared<const uint32_t>(3);
    std::weak_ptr<const uint32_t> nativeLifetime = nativeOwner;
    auto nativeProgram = std::make_shared<org::PipelineStatePayload>(
        rhi::PipelinePtr(rhi::Device{},rhi::Pipeline(rhi::PipelineHandle{8,1}),nullptr),0,org::PipelineResources{});
    nativeProgram->layout = {9,1}; nativeProgram->layoutOwner = nativeOwner;
    commands.commands.emplace_back(br::render::PreparedBindPersistentComputeProgram{nativeProgram});
    br::render::PreparedBufferBarrierBatch barriers;
    barriers.barriers.push_back({resource,rhi::ResourceAccessType::UnorderedAccess,rhi::ResourceAccessType::IndirectArgument,
        rhi::ResourceSyncState::ComputeShading,rhi::ResourceSyncState::ExecuteIndirect});
    commands.commands.emplace_back(std::move(barriers));
    commands.commands.emplace_back(br::render::PreparedExecuteIndirectCommand{{4,1},resource,64,3,nativeOwner,resource,8});
    auto workGraph = std::make_shared<const rhi::WorkGraphPtr>(
        rhi::Device{},rhi::WorkGraph(rhi::WorkGraphHandle{5,1}),nullptr);
    commands.commands.emplace_back(br::render::PreparedSetWorkGraph{workGraph,resource,false});
    commands.commands.emplace_back(br::render::PreparedWorkGraphGpuDispatch{resource,16});
    commands.commands.emplace_back(br::render::PreparedPersistentDescriptorIndices{{view,std::nullopt}});
    nativeOwner.reset(); nativeProgram.reset(); workGraph.reset();
    br::render::ValidatePersistentComputeCommands(commands);
    Require(!nativeLifetime.expired(), "immutable commands lost native owners");
    RecordedConstants recorded;
    rhi::CommandListVTable table{}; table.abi_version = rhi::RHI_CL_ABI_MIN;
    table.bindPipeline = +[](rhi::CommandList* list,rhi::PipelineHandle pipeline) noexcept {
        static_cast<RecordedConstants*>(list->impl)->pipeline = pipeline;
    };
    table.bindLayout = +[](rhi::CommandList* list,rhi::PipelineLayoutHandle layout) noexcept {
        static_cast<RecordedConstants*>(list->impl)->layout = layout;
    };
    table.barriers = +[](rhi::CommandList* list,const rhi::BarrierBatch& batch) noexcept {
        if (batch.buffers.size) static_cast<RecordedConstants*>(list->impl)->barrierResource = batch.buffers.data[0].buffer;
    };
    table.executeIndirect = +[](rhi::CommandList* list,rhi::CommandSignatureHandle,rhi::ResourceHandle arguments,
        uint64_t offset,rhi::ResourceHandle countBuffer,uint64_t countOffset,uint32_t count) noexcept {
        auto& recorded = *static_cast<RecordedConstants*>(list->impl);
        recorded.indirectResource = arguments; recorded.indirectOffset = offset; recorded.indirectCount = count;
        recorded.indirectCountBuffer = countBuffer; recorded.indirectCountOffset = countOffset;
    };
    table.setWorkGraph = +[](rhi::CommandList* list,const rhi::WorkGraphHandle& graph,const rhi::ResourceHandle& backing,bool initialize) noexcept {
        auto& recorded = *static_cast<RecordedConstants*>(list->impl);
        recorded.workGraph = graph; recorded.workGraphBacking = backing; recorded.initializeBacking = initialize;
    };
    table.dispatchWorkGraph = +[](rhi::CommandList* list,const rhi::WorkGraphDispatchDesc& dispatch) noexcept {
        static_cast<RecordedConstants*>(list->impl)->workGraphInput = dispatch.multiNodeGpuInput.inputBuffer;
    };
    table.pushConstants = +[](rhi::CommandList* commands, rhi::ShaderStage, uint32_t, uint32_t,
        uint32_t, uint32_t count, const void* values) noexcept {
        auto& recorded = *static_cast<RecordedConstants*>(commands->impl);
        const auto* words = static_cast<const uint32_t*>(values);
        recorded.values.assign(words,words + count);
        recorded.history.push_back(recorded.values);
    };
    rhi::CommandList list{rhi::CommandListHandle{1,1}}; list.vt = &table; list.impl = &recorded;
    auto oldContext = org::RecordingContext::FromPersistentBindings(list,held);
    br::render::RecordPreparedComputeCommands(commands,oldContext);
    Require(recorded.values == std::vector<uint32_t>{7,UINT32_MAX}, "old publication descriptor indices");
    Require(recorded.pipeline.index == 8 && recorded.layout.index == 9, "owned persistent program binding");
    Require(recorded.barrierResource.index == 11 && recorded.indirectResource.index == 11
        && recorded.workGraphBacking.index == 11 && recorded.workGraphInput.index == 11,
        "persistent commands did not resolve held backing");
    Require(recorded.indirectOffset == 64 && recorded.indirectCount == 3 && recorded.workGraph.index == 5
        && recorded.indirectCountBuffer.index == 11 && recorded.indirectCountOffset == 8,
        "persistent command structure changed");
    {
        // Exercise the actual renderer's material recording adapter, including
        // scalar rotation while descriptors and commands stay unchanged.
        br::render::PreparedComputeIndirectSequence recipe;
        recipe.layout = {9,1}; recipe.commandSignature = {4,1}; recipe.arguments = {11,1};
        br::render::PreparedComputeIndirectSequence::Step step;
        step.pipeline = {8,1};
        step.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = 777;
        step.constants[VISBUF_REYES_PATCH_INDEX_BASE] = 777;
        recipe.steps.push_back(step);
        MaterialEvaluationInvocation first, second;
        first.constants[VISBUF_REYES_USE_NORMAL_MAPS] = 0;
        first.constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(0.25f);
        second.constants[VISBUF_REYES_USE_NORMAL_MAPS] = 1;
        second.constants[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] = std::bit_cast<uint32_t>(0.75f);
        EvaluateMaterialGroupsPass::Record(recipe,first,oldContext);
        Require(recorded.values[VISBUF_REYES_USE_NORMAL_MAPS] == 0
            && recorded.values[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] == std::bit_cast<uint32_t>(0.25f),
            "live material adapter lost first invocation values");
        EvaluateMaterialGroupsPass::Record(recipe,second,oldContext);
        Require(recorded.values[VISBUF_REYES_USE_NORMAL_MAPS] == 1
            && recorded.values[VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT] == std::bit_cast<uint32_t>(0.75f)
            && recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 777
            && recorded.values[VISBUF_REYES_PATCH_INDEX_BASE] == 777
            && recipe.steps.front().constants == step.constants,
            "live material adapter mutated immutable bindings or retained stale scalars");
        const auto recordedCount = recorded.history.size();
        bool rejected = false;
        try { br::render::RecordPreparedComputeIndirectSequence(recipe,oldContext,{}, {},uint64_t{1} << 1); }
        catch (const std::out_of_range&) { rejected = true; }
        Require(rejected && recorded.history.size() == recordedCount,
            "incomplete indirect invocation modified the command list");
    }
    EvaluateMaterialGroupsPass::PersistentBindings materialBindings;
    materialBindings.visibleClusters = view; materialBindings.visibleClusterTransformIndices = view;
    materialBindings.arguments = resource; materialBindings.patchVisibilityIndexBase = 7;
    auto capturedMaterialProgram = std::get<br::render::PreparedBindPersistentComputeProgram>(commands.commands.front()).program;
    std::array<EvaluateMaterialGroupsPass::CapturedProgram,1> materialPrograms{{
        {GetMaterialEvaluationShaderKey(MaterialCompileNone),capturedMaterialProgram,{}}}};
    auto materialInterface = EvaluateMaterialGroupsPass::BuildPersistentProgramInterface(materialBindings,materialPrograms,
        std::make_shared<const rhi::CommandSignaturePtr>(rhi::Device{},rhi::CommandSignature(rhi::CommandSignatureHandle{4,1}),nullptr));
    auto materials = std::make_shared<br::render::PublishedMaterialState>();
    materials->activeCompileFlags = {MaterialCompileNone}; materials->activeCompileFlagSlots = {2}; materials->compileFlagSlotsUsed = 3;
    EvaluateMaterialGroupsPass::RecordingConfiguration normalConfiguration{false,0.25f,3,0.75f};
    auto materialInvocation = EvaluateMaterialGroupsPass::PreparePersistentInvocation(materialInterface,materials,false,
        OutputType::NORMAL,3*sizeof(MaterialEvaluationIndirectCommand),normalConfiguration);
    EvaluateMaterialGroupsPass::RecordPersistent(materialInterface,materialInvocation,oldContext);
    Require(recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 7
        && recorded.values[VISBUF_REYES_PATCH_INDEX_BASE] == 7 && recorded.values[VISBUF_REYES_USE_NORMAL_MAPS] == 0
        && recorded.indirectResource.index == 11 && recorded.indirectOffset == 2*sizeof(MaterialEvaluationIndirectCommand),
        "persistent material preparation lost descriptors, scalar configuration or active argument slot");
    auto rotation = program.BeginEdit(); auto next = held->bindings.At(slot);
    auto replacement = std::make_shared<org::ResourceBindingSnapshot>(*next.recording);
    auto replacementViews = std::make_shared<org::BindlessResourceViews>(*replacement->views);
    replacementViews->views[0].descriptor.index = 19;
    replacement->resource = rhi::Resource(rhi::ResourceHandle{12,1}); replacement->backingGeneration = 2;
    next.identity = (uint64_t{1} << 32) | 12; next.backingRevision = 2;
    replacement->views = replacementViews; next.recording = replacement;
    rotation.ReplaceBinding(slot,std::move(next));
    auto selected = rotation.Build(workspace,cancelled);
    Require(program.Install(selected), "descriptor replacement installation");
    Require(selected->executable == held->executable, "descriptor replacement rebuilt executable");
    auto context = org::RecordingContext::FromPersistentBindings(list,selected);
    br::render::RecordPreparedComputeCommands(commands,context);
    Require(recorded.values == std::vector<uint32_t>{19,UINT32_MAX}, "current publication descriptor indices");
    Require(recorded.barrierResource.index == 12 && recorded.indirectResource.index == 12 && recorded.indirectCountBuffer.index == 12
        && recorded.workGraphBacking.index == 12 && recorded.workGraphInput.index == 12,
        "persistent command template retained previous backing");
    br::render::RecordPreparedComputeCommands(commands,context,nullptr,true);
    Require(recorded.initializeBacking, "fresh work-graph initialization effect was ignored");
    br::render::RecordPreparedComputeCommands(commands,oldContext);
    Require(recorded.values == std::vector<uint32_t>{7,UINT32_MAX}, "held descriptor version changed");
    Require(recorded.indirectResource.index == 11 && !recorded.initializeBacking, "held command bindings changed");
    normalConfiguration.useNormalMaps = true;
    auto changedMaterialInvocation = EvaluateMaterialGroupsPass::PreparePersistentInvocation(materialInterface,materials,false,
        OutputType::NORMAL,3*sizeof(MaterialEvaluationIndirectCommand),normalConfiguration);
    EvaluateMaterialGroupsPass::RecordPersistent(materialInterface,changedMaterialInvocation,context);
    Require(recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 19
        && recorded.values[VISBUF_REYES_PATCH_INDEX_BASE] == 7 && recorded.values[VISBUF_REYES_USE_NORMAL_MAPS] == 1
        && recorded.indirectResource.index == 12,
        "persistent material interface retained old bindings or ignored scalar-only invocation changes");
    EvaluateMaterialGroupsPass::RecordPersistent(materialInterface,materialInvocation,oldContext);
    Require(recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 7 && recorded.indirectResource.index == 11,
        "held material invocation lost selected binding versions");
    const auto recordedBeforeInvalidInvocation = recorded.history.size();
    for (uint32_t failure = 0; failure < 3; ++failure) {
        auto invalidInvocation = materialInvocation;
        if (failure == 0) invalidInvocation.commands.push_back({UINT32_MAX,0});
        if (failure == 1) invalidInvocation.argumentBufferBytes = invalidInvocation.commands[0].argumentsOffset;
        if (failure == 2) invalidInvocation.materials.reset();
        bool accepted = true;
        try { EvaluateMaterialGroupsPass::RecordPersistent(materialInterface,invalidInvocation,oldContext); }
        catch (const std::exception&) { accepted = false; }
        Require(!accepted && recorded.history.size() == recordedBeforeInvalidInvocation,
            "invalid material invocation partially mutated native recording");
    }
    materialInterface = {}; materialPrograms[0].program.reset(); capturedMaterialProgram.reset();
    auto oversized = commands;
    std::get<br::render::PreparedPersistentDescriptorIndices>(oversized.commands.back()).slots.resize(65);
    bool rejected = false;
    try { br::render::RecordPreparedComputeCommands(oversized,context); }
    catch (const std::out_of_range&) { rejected = true; }
    Require(rejected, "persistent descriptor root bounds");
    auto invalid = commands;
    std::get<br::render::PreparedPersistentDescriptorIndices>(invalid.commands.back()).slots[0] = org::persistent::ViewToken{};
    rejected = false;
    try { br::render::RecordPreparedComputeCommands(invalid,context); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "invalid descriptor token accepted");
    br::render::PreparedComputeCommandSequence rootConstants;
    rootConstants.commands.emplace_back(br::render::PreparedComputeConstants{
        7,3,{10,20,30},uint64_t{1} << 1,{{0,view},{2,std::nullopt}}});
    br::render::ValidatePersistentComputeCommands(rootConstants);
    const std::array<uint32_t,3> scalars{0,41,0}, changedScalars{0,42,0};
    br::render::RecordPreparedComputeCommands(rootConstants,oldContext,nullptr,{},scalars);
    Require(recorded.values == std::vector<uint32_t>{7,41,UINT32_MAX},"held descriptor and invocation constant patches");
    br::render::RecordPreparedComputeCommands(rootConstants,context,nullptr,{},changedScalars);
    Require(recorded.values == std::vector<uint32_t>{19,42,UINT32_MAX},"current descriptor and invocation constant patches");
    Require(std::get<br::render::PreparedComputeConstants>(rootConstants.commands[0]).values == std::vector<uint32_t>{10,20,30},
        "descriptor patch mutated immutable constant template");
    auto badConstantLayout = rootConstants;
    std::get<br::render::PreparedComputeConstants>(badConstantLayout.commands[0]).bindingViews.push_back({1,view});
    rejected = false;
    try { br::render::ValidatePersistentComputeCommands(badConstantLayout); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected,"descriptor patch overlapped invocation scalar");
    badConstantLayout = rootConstants;
    std::get<br::render::PreparedComputeConstants>(badConstantLayout.commands[0]).bindingViews.push_back({0,view});
    rejected = false;
    try { br::render::ValidatePersistentComputeCommands(badConstantLayout); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected,"duplicate descriptor constant patch accepted");
    badConstantLayout = rootConstants;
    std::get<br::render::PreparedComputeConstants>(badConstantLayout.commands[0]).bindingViews[0].index = 3;
    rejected = false;
    try { br::render::RecordPreparedComputeCommands(badConstantLayout,context,nullptr,{},scalars); }
    catch (const std::out_of_range&) { rejected = true; }
    Require(rejected,"out-of-range descriptor constant patch accepted");
    const std::array<br::render::SymbolicComputeConstant,4> symbolic{uint32_t{7},view,std::nullopt,uint32_t{123}};
    auto copiedSymbolic = symbolic; copiedSymbolic[3] = uint32_t{456};
    br::render::PreparedComputeCommandSequence symbolicCommands;
    symbolicCommands.commands.emplace_back(br::render::BuildSymbolicComputeConstants(7,5,copiedSymbolic));
    br::render::ValidatePersistentComputeCommands(symbolicCommands);
    br::render::RecordPreparedComputeCommands(symbolicCommands,oldContext);
    Require(recorded.values == std::vector<uint32_t>{7,7,UINT32_MAX,456},"symbolic constant construction lost descriptor origin");
    br::render::RecordPreparedComputeCommands(symbolicCommands,context);
    Require(recorded.values == std::vector<uint32_t>{7,19,UINT32_MAX,456},"scalar equal to old descriptor was remapped");
    auto legacyResource = commands;
    std::get<br::render::PreparedExecuteIndirectCommand>(legacyResource.commands[2]).arguments = org::PreparedResourceReference{};
    rejected = false;
    try { br::render::ValidatePersistentComputeCommands(legacyResource); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "persistent build accepted compiler resource index");
    auto unownedSignature = commands;
    std::get<br::render::PreparedExecuteIndirectCommand>(unownedSignature.commands[2]).signatureOwner.reset();
    rejected = false;
    try { br::render::ValidatePersistentComputeCommands(unownedSignature); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "persistent build accepted unowned indirect signature");
    auto cpuRecords = commands;
    cpuRecords.commands.emplace_back(br::render::PreparedWorkGraphCpuDispatch{0,4,{std::byte{1}}});
    rejected = false;
    try { br::render::ValidatePersistentComputeCommands(cpuRecords); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "persistent build accepted one-frame CPU records");
    auto physicalAddress = commands;
    physicalAddress.commands.emplace_back(br::render::PreparedComputeAddressConstants{0,0,{0,0},{{123,0,1}}});
    rejected = false;
    try { br::render::ValidatePersistentComputeCommands(physicalAddress); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "persistent build accepted captured physical address");
    commands.commands.clear(); oversized.commands.clear(); invalid.commands.clear();
    legacyResource.commands.clear(); unownedSignature.commands.clear();
    cpuRecords.commands.clear(); physicalAddress.commands.clear();
    Require(nativeLifetime.expired(), "persistent command versions retained historical native owners");
}

void TestPixelListInvocationPreparation()
{
    struct HostData final : org::IHostExecutionData {
        UpdateContext update;
        const void* TryGet(std::type_index type) const noexcept override {
            return type == std::type_index(typeid(UpdateContext)) ? &update : nullptr;
        }
    } host;
    auto material = std::make_shared<br::render::PublishedMaterialState>();
    material->activeCompileFlags = {MaterialCompileFlags::MaterialCompileVoxel};
    material->activeCompileFlagSlots = {7};
    auto publication = std::make_shared<br::render::PublishedRendererState>();
    publication->materials.payload = br::render::ArtifactPayload::Make<br::render::PublishedMaterialState>(material);
    host.update.publishedRendererState = publication;
    host.update.renderResolution = {17,9};
    org::FramePreparationContext preparation; preparation.preparationData = &host;
    br::render::PreparedComputeDispatch recipe;
    recipe.layout = rhi::PipelineLayoutHandle{1,1}; recipe.pipeline = rhi::PipelineHandle{2,1};
    recipe.descriptorIndices = {99,98};
    recipe.constants[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = 91;
    recipe.constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] = 11;
    const auto first = BuildPixelListPass::PrepareInvocation(recipe,{},preparation);
    Require(first.voxelMaterialBin == 7 && first.groupsX == 3 && first.groupsY == 2,
        "pixel-list first invocation");
    auto changedMaterial = std::make_shared<br::render::PublishedMaterialState>(*material);
    changedMaterial->activeCompileFlagSlots = {17};
    auto changed = std::make_shared<br::render::PublishedRendererState>(*publication);
    changed->materials.revision = 2;
    changed->materials.payload = br::render::ArtifactPayload::Make<br::render::PublishedMaterialState>(changedMaterial);
    host.update.publishedRendererState = changed; host.update.renderResolution = {48,33};
    const auto second = BuildPixelListPass::PrepareInvocation(recipe,{},preparation);
    Require(second.voxelMaterialBin == 17 && second.groupsX == 6 && second.groupsY == 5,
        "pixel-list current invocation");
    RecordedConstants recorded;
    rhi::CommandListVTable table{}; table.abi_version = rhi::RHI_CL_ABI_MIN;
    table.bindLayout = +[](rhi::CommandList* list,rhi::PipelineLayoutHandle layout) noexcept {
        static_cast<RecordedConstants*>(list->impl)->layout = layout;
    };
    table.bindPipeline = +[](rhi::CommandList* list,rhi::PipelineHandle pipeline) noexcept {
        static_cast<RecordedConstants*>(list->impl)->pipeline = pipeline;
    };
    table.pushConstants = +[](rhi::CommandList* list, rhi::ShaderStage, uint32_t, uint32_t,
        uint32_t, uint32_t count, const void* values) noexcept {
        auto& state = *static_cast<RecordedConstants*>(list->impl);
        const auto* words = static_cast<const uint32_t*>(values);
        state.values.assign(words,words + count); state.history.push_back(state.values);
    };
    table.dispatch = +[](rhi::CommandList* list,uint32_t x,uint32_t y,uint32_t z) noexcept {
        auto& state = *static_cast<RecordedConstants*>(list->impl);
        ++state.dispatches; state.groups = {x,y,z};
    };
    rhi::CommandList list{rhi::CommandListHandle{1,1}}; list.vt = &table; list.impl = &recorded;
    auto bindings = std::make_shared<const org::FrozenExecutionBindings>(
        std::vector<org::FrozenExecutionBindings::ResourceBinding>{});
    org::RecordingContext recording(list,bindings);
    BuildPixelListPass::Record(recipe,first,recording);
    Require(recorded.values[VISBUF_VOXEL_MATERIAL_BIN_INDEX] == 7 && recorded.groups == std::array<uint32_t,3>{3,2,1},
        "pixel-list held invocation recording");
    BuildPixelListPass::Record(recipe,second,recording);
    Require(recorded.values[VISBUF_VOXEL_MATERIAL_BIN_INDEX] == 17 && recorded.groups == std::array<uint32_t,3>{6,5,1},
        "pixel-list fresh invocation recording");
    Require(recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 91
        && recipe.constants[VISBUF_VOXEL_MATERIAL_BIN_INDEX] == 11 && recipe.descriptorIndices == std::vector<unsigned>{99,98},
        "pixel-list invocation mutated stable bindings");
    host.update.renderResolution = {0,0}; host.update.publishedRendererState.reset();
    const auto empty = BuildPixelListPass::PrepareInvocation(recipe,{},preparation);
    Require(empty.voxelMaterialBin == UINT32_MAX, "pixel-list absent material sentinel");
    BuildPixelListPass::Record(recipe,empty,recording);
    Require(recorded.dispatches == 2 && recorded.history.size() == 4, "zero pixel-list invocation recorded commands");
    preparation.preparationData = nullptr;
    bool rejected = false;
    try { BuildPixelListPass::PrepareInvocation(recipe,{},preparation); } catch (const std::logic_error&) { rejected = true; }
    Require(rejected, "pixel-list missing context");

    org::persistent::GraphProgram graph;
    auto edit = graph.BeginEdit();
    std::array<org::persistent::ResourceSlotId,10> slots;
    for (uint32_t i = 0; i < slots.size(); ++i) {
        auto owner = std::make_shared<const uint32_t>(i);
        auto snapshot = std::make_shared<org::ResourceBindingSnapshot>();
        snapshot->resource = rhi::Resource(rhi::ResourceHandle{i+1,1});
        snapshot->backingGeneration = 1; snapshot->allocationOwner = owner; snapshot->descriptorOwner = owner;
        auto views = std::make_shared<org::BindlessResourceViews>();
        const auto kind = i == 9 ? org::BindlessViewKind::ConstantBuffer :
            (i >= 7 ? org::BindlessViewKind::UnorderedAccess : org::BindlessViewKind::ShaderResource);
        views->views.push_back({kind,UINT32_MAX,0,0,rhi::DescriptorSlot(rhi::DescriptorHeapHandle{3,1},20+i)});
        snapshot->views = views;
        org::persistent::BindingVersion binding;
        binding.identity = (uint64_t{1} << 32) | (i+1); binding.backingRevision = 1;
        binding.shape = {1,1,false}; binding.owner = owner; binding.recording = snapshot;
        slots[i] = edit.AddResource(binding.shape,binding);
    }
    PersistentBuildPixelListPass::Slots inputs;
    inputs.visibleClusters = slots[0]; inputs.perFrame = slots[9]; inputs.patchVisibilityIndexBase = 31;
    for (uint32_t i = 0; i < 6; ++i) inputs.shaderInputs[i] = slots[i+1];
    inputs.outputs = {slots[7],slots[8]};
    PersistentBuildPixelListPass author(inputs);
    PersistentBuildPixelListPass::ProgramBuildInputs programInputs;
    programInputs.native = recipe; programInputs.native.descriptorIndices.clear();
    programInputs.descriptorOrder = {8,2,6,0};
    using Executable = org::persistent::TypedPassExecutable<PersistentBuildPixelListPass>;
    const auto executable = Executable::Build(edit,{},author,programInputs);
    org::experimental::CompileWorkspace workspace; std::atomic_bool cancelled{false};
    auto held = edit.Build(workspace,cancelled);
    Require(graph.Install(held), "persistent pixel-list bootstrap");
    host.update.publishedRendererState = publication; host.update.renderResolution = {17,9};
    preparation.preparationData = &host;
    auto oldPacket = executable.PrepareInvocation(*held,preparation);
    auto oldRecording = org::RecordingContext::FromPersistentBindings(list,held);
    oldPacket.Record(oldRecording);
    Require(recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 20
        && recorded.values[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] == UINT32_MAX
        && recorded.values[VISBUF_REYES_PATCH_INDEX_BASE] == 31,
        "persistent pixel-list symbolic bindings");
    Require(recorded.history[recorded.history.size()-2] == std::vector<uint32_t>{29,23,27,21},
        "persistent pixel-list reflected descriptor ordering");
    auto rotation = graph.BeginEdit(); auto replacementBinding = held->bindings.At(slots[0]);
    auto replacementSnapshot = std::make_shared<org::ResourceBindingSnapshot>(*replacementBinding.recording);
    auto replacementViews = std::make_shared<org::BindlessResourceViews>(*replacementSnapshot->views);
    replacementViews->views[0].descriptor.index = 101;
    replacementSnapshot->views = replacementViews; replacementBinding.recording = replacementSnapshot;
    rotation.ReplaceBinding(slots[0],replacementBinding);
    auto current = rotation.Build(workspace,cancelled);
    Require(graph.Install(current) && current->executable == held->executable,
        "persistent pixel-list descriptor rotation rebuilt executable");
    auto currentPacket = executable.PrepareInvocation(*current,preparation);
    auto currentRecording = org::RecordingContext::FromPersistentBindings(list,current);
    currentPacket.Record(currentRecording);
    Require(recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 101,
        "persistent pixel-list current descriptor");
    auto heldPacket = executable.PrepareInvocation(*held,preparation);
    heldPacket.Record(oldRecording);
    Require(recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 20,
        "persistent pixel-list held descriptor changed");
    auto programEdit = graph.BeginEdit();
    programInputs.native.pipeline = rhi::PipelineHandle{4,1};
    programInputs.native.layout = rhi::PipelineLayoutHandle{5,1};
    programInputs.descriptorOrder = {0,8};
    const auto rebuilt = executable.RebuildProgramInterface(programEdit,author,programInputs);
    auto programPublication = programEdit.Build(workspace,cancelled);
    Require(graph.Install(programPublication) && programPublication->executable == current->executable,
        "pixel-list program replacement rebuilt logical executable");
    auto programPacket = rebuilt.PrepareInvocation(*programPublication,preparation);
    auto programRecording = org::RecordingContext::FromPersistentBindings(list,programPublication);
    programPacket.Record(programRecording);
    Require(recorded.pipeline.index == programInputs.native.pipeline.index
        && recorded.pipeline.generation == programInputs.native.pipeline.generation
        && recorded.layout.index == programInputs.native.layout.index
        && recorded.layout.generation == programInputs.native.layout.generation
        && recorded.history[recorded.history.size()-2] == std::vector<uint32_t>{21,29},
        "pixel-list replacement program/layout");
    auto previousPacket = executable.PrepareInvocation(*current,preparation);
    previousPacket.Record(currentRecording);
    Require(recorded.pipeline.index == recipe.pipeline.index && recorded.pipeline.generation == recipe.pipeline.generation
        && recorded.layout.index == recipe.layout.index && recorded.layout.generation == recipe.layout.generation,
        "held pixel-list program/layout changed");
    auto failedEdit = graph.BeginEdit();
    auto invalidProgram = programInputs; invalidProgram.descriptorOrder = {9};
    rejected = false;
    try { rebuilt.RebuildProgramInterface(failedEdit,author,invalidProgram); }
    catch (const std::out_of_range&) { rejected = true; }
    Require(rejected, "pixel-list invalid reflection slot accepted");
    rejected = false;
    try { failedEdit.Build(workspace,cancelled); }
    catch (const std::exception&) { rejected = true; }
    Require(rejected && graph.Select() == programPublication,
        "pixel-list failed program preparation changed selection");
}

void TestFullCapturedCullingEmission()
{
    using namespace br::render;
    org::Resource::ScopedECSRegistrationSuppression suppressECS;
    org::persistent::GraphProgram graph;
    auto edit = graph.BeginEdit();
    const auto pass = edit.AddPass({});
    const auto owner = std::make_shared<const uint32_t>(1);
    CapturedHierarchicalDispatchCullingInputs inputs;
    CapturedCullingBindings bindings;
    std::vector<org::persistent::ResourceSlotId> slots;
    std::vector<PersistentHierarchicalDispatchCullingPass::ResourceAccess> accesses;
    std::vector<std::shared_ptr<const org::ResourceBindingSnapshot>> accessSnapshots;
    const std::array resources = {
        &CapturedHierarchicalDispatchCullingInputs::m_dynamicWindBoundsCacheBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_dynamicWindVisibleMembershipBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_histogramIndirectCommand,
        &CapturedHierarchicalDispatchCullingInputs::m_occlusionReplayBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_occlusionReplayStateBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pageJobVisibleClustersCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_phase1VisibleClustersCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeClusterCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeClusterDispatchArgsBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeClusterFrontierBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeCurrentLeafCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeCurrentLeafFrontierBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeCurrentNodeCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeCurrentNodeFrontierBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeLeafDispatchArgsBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeNextLeafCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeNextLeafFrontierBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeNextNodeCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeNextNodeFrontierBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeNodeDispatchArgsBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_skinnedVoxelRasterWorkCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_swVisibleClustersCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_swWriteBaseCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_viewDepthSrvIndicesBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_viewRasterInfoBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_visibleClusterTransformIndicesBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_visibleClustersBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_visibleClustersCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_voxelRasterWorkCounterBuffer,
        &CapturedHierarchicalDispatchCullingInputs::m_workGraphTelemetryBuffer,
    };
    for (const auto field : resources) {
        const uint32_t index = static_cast<uint32_t>(slots.size());
        auto snapshot = std::make_shared<org::ResourceBindingSnapshot>();
        const uint32_t physical = index+1 == resources.size() ? 100 : 100+index;
        snapshot->resource = rhi::Resource(rhi::ResourceHandle{physical,1});
        snapshot->backingGeneration = 1; snapshot->allocationOwner = owner; snapshot->descriptorOwner = owner;
        auto views = std::make_shared<org::BindlessResourceViews>();
        views->views.push_back({org::BindlessViewKind::ShaderResource,UINT32_MAX,0,0,
            rhi::DescriptorSlot(rhi::DescriptorHeapHandle{3,1},200+2*index)});
        views->views.push_back({org::BindlessViewKind::UnorderedAccess,UINT32_MAX,0,0,
            rhi::DescriptorSlot(rhi::DescriptorHeapHandle{3,1},201+2*index)});
        snapshot->views = views;
        org::persistent::BindingVersion binding;
        binding.identity = (uint64_t{1} << 32) | physical; binding.backingRevision = 1;
        binding.owner = owner; binding.recording = snapshot;
        auto initialState = std::make_shared<org::experimental::PreparedBackingState>();
        initialState->graphResourceID = index+1; initialState->resource = snapshot->resource.GetHandle();
        initialState->shape = binding.shape;
        initialState->regions = std::make_shared<const std::vector<org::experimental::PreparedStateRegion>>(
            std::initializer_list<org::experimental::PreparedStateRegion>{{{},
                {static_cast<uint64_t>(rhi::ResourceAccessType::UnorderedAccess),0,
                    static_cast<uint64_t>(rhi::ResourceSyncState::ComputeShading),true}}});
        binding.admission = initialState;
        const auto slot = edit.AddResource(binding.shape,binding); slots.push_back(slot);
        const auto token = edit.Declare(pass,slot,{},
            {static_cast<uint64_t>(rhi::ResourceAccessType::ShaderResource)
                | static_cast<uint64_t>(rhi::ResourceAccessType::UnorderedAccess)
                | static_cast<uint64_t>(rhi::ResourceAccessType::IndirectArgument),0,1,true});
        CapturedCullingBindings::Resource resource{token,{}};
        for (const auto kind : {org::BindlessViewKind::ShaderResource,org::BindlessViewKind::UnorderedAccess}) {
            const org::BindlessViewRequest request{kind,UINT32_MAX,0,0};
            resource.views.push_back({request,edit.DeclareView(token,request)});
            accesses.push_back({slot,{static_cast<uint64_t>(rhi::ResourceAccessType::ShaderResource)
                | static_cast<uint64_t>(rhi::ResourceAccessType::UnorderedAccess)
                | static_cast<uint64_t>(rhi::ResourceAccessType::IndirectArgument),0,1,true},{},request});
            accessSnapshots.push_back(snapshot);
        }
        bindings.AddResource(snapshot,std::move(resource));
        inputs.*field = std::make_shared<const CapturedCullingResource>(CapturedCullingResource{snapshot});
    }
    const org::ResourceIdentifier mandatoryView("Headless.Culling.RequiredView");
    const org::ResourceIdentifier optionalView("Headless.Culling.MissingOptionalView");
    auto program = std::make_shared<org::PipelineStatePayload>(
        rhi::PipelinePtr(rhi::Device{},rhi::Pipeline(rhi::PipelineHandle{8,1}),nullptr),0,
        org::PipelineResources{{mandatoryView},{optionalView}});
    program->layout = {9,1}; program->layoutOwner = owner;
    const std::array programs = {
        &CapturedHierarchicalDispatchCullingInputs::m_clearPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_createCommandPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeBuildDispatchArgsPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeBuildDualDispatchArgsPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeBuildReplayDispatchArgsPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeClearTraversalCountersPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeClusterPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeDenseClusterPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeLeafPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeObjectCullPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeReplayClustersPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeReplayNodesPipelineState,
        &CapturedHierarchicalDispatchCullingInputs::m_pureComputeTraversePipelineState,
    };
    for (const auto field : programs) (inputs.*field).payload = program;
    bindings.programs.emplace(program->pso.Get().GetHandle(),program);
    bindings.reflectedViews.emplace(mandatoryView,
        bindings.snapshotResources.at((inputs.*resources[0])->snapshot.get()).views[0].token);
    bindings.signature = std::make_shared<const rhi::CommandSignaturePtr>(
        rhi::Device{},rhi::CommandSignature(rhi::CommandSignatureHandle{4,1}),nullptr);
    inputs.m_pureComputeDispatchCommandSignature = bindings.signature;
    inputs.m_activeTraversalDepth = 3;
    inputs.m_maxVisibleClusters = 200; // Deliberately also a descriptor index.
    inputs.m_voxelRasterWorkCapacity = 1024;
    HierarchicalDispatchCullingCommandConfiguration configuration;
    configuration.telemetry = true; configuration.occlusion = true;
    configuration.rasterBucketCount = 4;
    const auto recipe = HierarchicalDispatchCullingPass::BuildCapturedRecipe(inputs,configuration,bindings);
    Require(recipe.hasObjectCull && !recipe.objectCullBindingViews.empty()
        && recipe.prefix.commands.size()+recipe.suffix.commands.size() > 100,
        "full captured culling emitter did not produce traversal commands");
    ValidatePersistentComputeCommands(recipe.prefix); ValidatePersistentComputeCommands(recipe.suffix);
    PersistentHierarchicalDispatchCullingPass author({.renderPhase = RenderPhase("Opaque")},std::move(accesses));
    PersistentHierarchicalDispatchCullingPass::CapturedProgramBuildInputs capturedBuild{
        inputs,configuration,std::move(accessSnapshots),{{mandatoryView,0}}};
    const auto executable = org::persistent::TypedPassExecutable<PersistentHierarchicalDispatchCullingPass>::Build(
        edit,{},author,capturedBuild);
    org::experimental::CompileWorkspace workspace; std::atomic_bool cancelled{false};
    auto held = edit.Build(workspace,cancelled); Require(graph.Install(held),"full culling bootstrap");
    struct Trace {
        std::vector<std::vector<uint32_t>> constants;
        std::vector<uint32_t> barriers, indirect;
        std::vector<uint32_t> programs, layouts;
        uint32_t dispatches = 0;
    };
    Trace trace;
    rhi::CommandListVTable table{}; table.abi_version = rhi::RHI_CL_ABI_MIN;
    table.bindLayout = +[](rhi::CommandList* list,rhi::PipelineLayoutHandle layout) noexcept {
        static_cast<Trace*>(list->impl)->layouts.push_back(layout.index);
    };
    table.bindPipeline = +[](rhi::CommandList* list,rhi::PipelineHandle program) noexcept {
        static_cast<Trace*>(list->impl)->programs.push_back(program.index);
    };
    table.pushConstants = +[](rhi::CommandList* list,rhi::ShaderStage,uint32_t,uint32_t,uint32_t,
        uint32_t count,const void* data) noexcept {
        if (!count) return;
        const auto* words = static_cast<const uint32_t*>(data);
        static_cast<Trace*>(list->impl)->constants.emplace_back(words,words+count);
    };
    table.dispatch = +[](rhi::CommandList* list,uint32_t,uint32_t,uint32_t) noexcept {
        ++static_cast<Trace*>(list->impl)->dispatches;
    };
    table.barriers = +[](rhi::CommandList* list,const rhi::BarrierBatch& batch) noexcept {
        for (const auto& barrier : batch.buffers) static_cast<Trace*>(list->impl)->barriers.push_back(barrier.buffer.index);
    };
    table.executeIndirect = +[](rhi::CommandList* list,rhi::CommandSignatureHandle,rhi::ResourceHandle arguments,
        uint64_t,rhi::ResourceHandle,uint64_t,uint32_t) noexcept {
        static_cast<Trace*>(list->impl)->indirect.push_back(arguments.index);
    };
    rhi::CommandList list{rhi::CommandListHandle{1,1}}; list.vt = &table; list.impl = &trace;
    HierarchicalDispatchCullingInvocation invocation;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] = 11;
    invocation.constants[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_ENTRY_COUNT] = 20;
    invocation.workloads.push_back({3,4,5,6,0,1,1,1});
    auto record = [&](const auto& publication) {
        trace = {};
        auto context = org::RecordingContext::FromPersistentBindings(list,publication);
        HierarchicalDispatchCullingPass::Record(recipe,invocation,context);
        return trace;
    };
    const auto initial = record(held);
    auto indirectPublication = std::make_shared<PublishedIndirectState>();
    indirectPublication->visibilityGenerations = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal,4096);
    indirectPublication->visibilityGenerationsSRVIndex = 6;
    PublishedIndirectWorkload active;
    active.viewID = 13; active.key.renderPhase = RenderPhase("Opaque"); active.key.clodOnly = true;
    active.activeDrawList = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal,4096);
    active.activeDrawListSRVIndex = 4; active.count = 5; active.capacity = 64;
    indirectPublication->workloads.push_back(active);
    auto rendererPublication = std::make_shared<PublishedRendererState>();
    rendererPublication->indirectWorkloads.payload = ArtifactPayload::Make<PublishedIndirectState>(indirectPublication);
    std::array<PreparedViewFrameData,1> views{};
    views[0].id = 13; views[0].primary = true; views[0].cameraBufferIndex = 3;
    PersistentHierarchicalDispatchCullingPass::InvocationInputs freshInputs{views,rendererPublication,11,20};
    const auto recordPacket = [&](const auto& publication) {
        trace = {};
        auto packet = executable.PrepareInvocation(*publication,freshInputs);
        auto context = org::RecordingContext::FromPersistentBindings(list,publication);
        packet.Record(context);
        return trace;
    };
    const auto typedInitial = recordPacket(held);
    Require(typedInitial.constants == initial.constants && typedInitial.barriers == initial.barriers
        && typedInitial.indirect == initial.indirect && typedInitial.dispatches == initial.dispatches,
        "typed captured culling construction changed production commands");
    org::persistent::SynchronousAdmission admission;
    const std::array<org::experimental::ExecutionTimelinePoint,1> queues{{{1,0}}};
    const std::array<org::persistent::FrameProducerWait,1> producerWaits{{{executable.Id(),{99,7}}}};
    const std::array<org::persistent::FrameIncomingState,1> incomingStates{{{slots.back(),
        (inputs.*resources.back())->snapshot->resource.GetHandle(),
        std::make_shared<const std::vector<org::experimental::PreparedStateRegion>>(
            std::initializer_list<org::experimental::PreparedStateRegion>{{{},
                {static_cast<uint64_t>(rhi::ResourceAccessType::CopyDest),0,
                    static_cast<uint64_t>(rhi::ResourceSyncState::Copy),true}}}),{98,6}}}};
    struct EmptyContributor {
        static void Record(const uint32_t&,org::RecordingContext&) {}
    };
    std::vector<org::PreparedPass> heldInvocations(held->executable->executionLayout->placements.size());
    const auto& heldPasses = held->executable->graph->structure->passes;
    heldInvocations.at(heldPasses.at(pass.index).preparedPassIndex) = org::PreparedPass::FromTyped<EmptyContributor>(uint32_t{0},{});
    heldInvocations.at(heldPasses.at(executable.Id().index).preparedPassIndex) = executable.PrepareInvocation(*held,freshInputs);
    auto pendingHeldFrame = std::make_shared<org::experimental::PendingPersistentFrame>(1,held,std::move(heldInvocations));
    std::vector<org::PreparedPass> queuedInvocations(held->executable->executionLayout->placements.size());
    queuedInvocations.at(heldPasses.at(pass.index).preparedPassIndex) = org::PreparedPass::FromTyped<EmptyContributor>(uint32_t{0},{});
    queuedInvocations.at(heldPasses.at(executable.Id().index).preparedPassIndex) = executable.PrepareInvocation(*held,freshInputs);
    auto queuedFrame = std::make_shared<org::experimental::PendingPersistentFrame>(3,held,std::move(queuedInvocations));
    auto heldAdmission = admission.Prepare(pendingHeldFrame->Publication(),queues,producerWaits,incomingStates);
    auto sealedHeldFrame = pendingHeldFrame->Seal(heldAdmission);
    bool duplicateSealAccepted = true;
    try { pendingHeldFrame->Seal(heldAdmission); }
    catch (const std::logic_error&) { duplicateSealAccepted = false; }
    Require(!duplicateSealAccepted,"pending culling frame transferred invocation effects twice");
    pendingHeldFrame.reset();
    const auto cullingBatch = held->executable->executionLayout->placements.at(executable.Id().index).batch;
    Require(sealedHeldFrame->incomingWaits.at(cullingBatch).size() == 2
        && sealedHeldFrame->incomingWaits.at(cullingBatch)[0] == org::experimental::ExecutionTimelinePoint(99,7),
        "sealed culling frame lost invocation-specific producer wait");
    Require(sealedHeldFrame->initialStates->at(0).authoritativeIncoming
        && sealedHeldFrame->initialStates->at(0).regions == incomingStates[0].regions,
        "sealed culling frame lost authoritative incoming state root");
    std::vector<org::experimental::OwnedRecordingList> heldRecordings;
    for (uint32_t batch = 0; batch < held->executable->graph->batches.size(); ++batch)
        heldRecordings.push_back(org::experimental::BuildPersistentRecordingList(sealedHeldFrame,batch));
    Require(initial.constants[0] == std::vector<uint32_t>{200,UINT32_MAX},"captured culling reflected view layout mismatch");
    Require(initial.constants[1][CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] == 201+2*(resources.size()-1),
        "captured culling selected a different logical descriptor on shared backing");
    Require(initial.dispatches > 10 && initial.barriers.size() > 20 && initial.indirect.size() >= 3,
        "full captured culling recording skipped traversal work");
    auto rotation = graph.BeginEdit();
    for (const auto slot : slots) {
        auto binding = held->bindings.At(slot);
        auto snapshot = std::make_shared<org::ResourceBindingSnapshot>(*binding.recording);
        snapshot->resource = rhi::Resource(rhi::ResourceHandle{snapshot->resource.GetHandle().index+1000,1});
        ++snapshot->backingGeneration;
        auto views = std::make_shared<org::BindlessResourceViews>(*snapshot->views);
        for (auto& view : views->views) view.descriptor.index += 1000;
        snapshot->views = views; binding.recording = snapshot;
        auto replacedState = std::make_shared<org::experimental::PreparedBackingState>(*binding.admission);
        replacedState->resource = snapshot->resource.GetHandle(); binding.admission = replacedState;
        binding.identity += 1000; ++binding.backingRevision;
        rotation.ReplaceBinding(slot,std::move(binding));
    }
    auto current = rotation.Build(workspace,cancelled); Require(graph.Install(current),"full culling rotation");
    Require(current->executable == held->executable,"full culling backing rotation rebuilt declarations");
    const auto replacement = record(current);
    const auto typedReplacement = recordPacket(current);
    Require(typedReplacement.constants == replacement.constants && typedReplacement.barriers == replacement.barriers
        && typedReplacement.indirect == replacement.indirect,"typed captured culling did not use replacement binding fragments");
    Require(replacement.dispatches == initial.dispatches && replacement.indirect.size() == initial.indirect.size()
        && replacement.barriers.size() == initial.barriers.size(),"binding rotation changed command structure");
    for (size_t i = 0; i < initial.barriers.size(); ++i)
        Require(replacement.barriers[i] == initial.barriers[i]+1000,"full culling barrier retained old backing");
    for (size_t i = 0; i < initial.indirect.size(); ++i)
        Require(replacement.indirect[i] == initial.indirect[i]+1000,"full culling indirect retained old backing");
    const auto heldAgain = record(held);
    Require(heldAgain.constants == initial.constants && heldAgain.barriers == initial.barriers
        && heldAgain.indirect == initial.indirect,"full culling held frame changed after rotation");
    Require(replacement.constants != initial.constants,"full culling descriptor constants failed to rotate");
    std::vector<std::vector<bool>> descriptorWords;
    const auto classify = [&](const PreparedComputeCommandSequence& sequence) {
        for (const auto& command : sequence.commands) {
            if (const auto* constants = std::get_if<PreparedComputeConstants>(&command)) {
                descriptorWords.emplace_back(constants->values.size(),false);
                for (const auto& patch : constants->bindingViews)
                    descriptorWords.back().at(patch.index) = patch.view.has_value();
            } else if (const auto* reflected = std::get_if<PreparedPersistentDescriptorIndices>(&command)) {
                if (reflected->slots.empty()) continue;
                descriptorWords.emplace_back();
                for (const auto& view : reflected->slots) descriptorWords.back().push_back(view.has_value());
            }
        }
    };
    classify(recipe.prefix);
    descriptorWords.emplace_back(NumMiscUintRootConstants,false);
    for (const auto& patch : recipe.objectCullBindingViews)
        descriptorWords.back().at(patch.index) = patch.view.has_value();
    classify(recipe.suffix);
    Require(descriptorWords.size() == initial.constants.size(),"full culling constant trace shape mismatch");
    for (size_t command = 0; command < descriptorWords.size(); ++command) {
        Require(initial.constants[command].size() == descriptorWords[command].size(),"full culling root layout changed");
        for (size_t word = 0; word < descriptorWords[command].size(); ++word)
            Require(replacement.constants[command][word] == initial.constants[command][word]
                + (descriptorWords[command][word] ? 1000u : 0u),
                "full culling rotation changed a scalar or failed to replace a descriptor");
    }
    inputs.m_isFirstPass = false;
    const auto replayRecipe = HierarchicalDispatchCullingPass::BuildCapturedRecipe(inputs,configuration,bindings);
    Require(!replayRecipe.hasObjectCull && replayRecipe.suffix.commands.empty()
        && !replayRecipe.prefix.commands.empty(),"captured replay phase retained object-cull stage");
    auto programEdit = graph.BeginEdit(); auto changedProgram = capturedBuild;
    auto replacementProgram = std::make_shared<org::PipelineStatePayload>(
        rhi::PipelinePtr(rhi::Device{},rhi::Pipeline(rhi::PipelineHandle{9,1}),nullptr),0,
        org::PipelineResources{{mandatoryView},{optionalView}},2);
    replacementProgram->layout = {10,1}; replacementProgram->layoutOwner = owner;
    for (const auto field : programs) (changedProgram.commands.*field).payload = replacementProgram;
    const auto newExecutable = executable.RebuildProgramInterface(programEdit,author,changedProgram);
    auto selectedProgram = programEdit.Build(workspace,cancelled);
    Require(graph.Install(selectedProgram) && selectedProgram->executable == current->executable,
        "compatible culling program replacement rebuilt scheduling");
    trace = {};
    auto newPacket = newExecutable.PrepareInvocation(*selectedProgram,freshInputs);
    auto newContext = org::RecordingContext::FromPersistentBindings(list,selectedProgram);
    newPacket.Record(newContext);
    Require(!trace.programs.empty() && std::all_of(trace.programs.begin(),trace.programs.end(),[](uint32_t value) { return value == 9; })
        && std::all_of(trace.layouts.begin(),trace.layouts.end(),[](uint32_t value) { return value == 10; })
        && trace.constants == typedReplacement.constants,"culling replacement did not select exact program/layout version");
    const auto oldProgramTrace = recordPacket(held);
    Require(oldProgramTrace.programs == initial.programs && oldProgramTrace.layouts == initial.layouts,
        "held culling program/layout changed after replacement");
    sealedHeldFrame.reset();
    trace = {};
    for (const auto& recording : heldRecordings) {
        Require(recording.publication == held && recording.frame && !recording.bindings,
            "owned culling recording lost sealed publication root");
        auto context = org::RecordingContext::FromPersistentBindings(list,recording.publication);
        for (const auto& invocation : recording.passes) invocation.Record(context);
    }
    Require(trace.constants == initial.constants && trace.programs == initial.programs && trace.layouts == initial.layouts
        && trace.barriers == initial.barriers && trace.indirect == initial.indirect,
        "sealed culling invocation changed while publications rotated");
    admission.Abandon(heldAdmission);
    heldRecordings.clear();
    auto queuedAdmission = admission.Prepare(queuedFrame->Publication(),queues);
    auto wrongPublicationAdmission = queuedAdmission;
    wrongPublicationAdmission.publication = selectedProgram;
    bool retargetedPacket = true;
    try { queuedFrame->Seal(wrongPublicationAdmission); }
    catch (const std::invalid_argument&) { retargetedPacket = false; }
    Require(!retargetedPacket,"pending culling packet accepted a different publication");
    auto incompleteAdmission = queuedAdmission;
    incompleteAdmission.backings.clear();
    bool incompletePacket = true;
    try { queuedFrame->Seal(incompleteAdmission); }
    catch (const std::invalid_argument&) { incompletePacket = false; }
    Require(!incompletePacket,"pending culling packet accepted incomplete admission");
    auto queuedSeal = queuedFrame->Seal(queuedAdmission);
    auto queuedContext = org::RecordingContext::FromPersistentBindings(list,queuedSeal->publication);
    trace = {};
    queuedSeal->passes.at(heldPasses.at(executable.Id().index).preparedPassIndex).Record(queuedContext);
    Require(trace.constants == typedInitial.constants && trace.programs == typedInitial.programs,
        "pre-admission culling packet lost selected descriptor or program versions during publication rotation");
    admission.Abandon(queuedAdmission); queuedFrame.reset(); queuedSeal.reset();
    auto cancellationPublication = std::make_shared<PublishedRendererState>(*rendererPublication);
    std::weak_ptr<const PublishedRendererState> cancellationPin = cancellationPublication;
    auto cancellationInputs = freshInputs; cancellationInputs.publication = cancellationPublication;
    std::vector<org::PreparedPass> cancelledInvocations(selectedProgram->executable->executionLayout->placements.size());
    cancelledInvocations.at(heldPasses.at(pass.index).preparedPassIndex) =
        org::PreparedPass::FromTyped<EmptyContributor>(uint32_t{0},{});
    cancelledInvocations.at(heldPasses.at(executable.Id().index).preparedPassIndex) =
        newExecutable.PrepareInvocation(*selectedProgram,cancellationInputs);
    auto cpuConsumer = cancelledInvocations.at(heldPasses.at(executable.Id().index).preparedPassIndex);
    auto cancellationSeal = std::make_shared<org::experimental::PendingPersistentFrame>(
        2,selectedProgram,std::move(cancelledInvocations));
    auto cpuFrame = cancellationSeal; // Preparation tasks retain the pending packet until they join.
    cancellationInputs.publication.reset(); cancellationPublication.reset();
    Require(!cancellationPin.expired(),"sealed culling frame failed to pin semantic publication");
    cancellationSeal.reset();
    Require(!cpuConsumer.IsConsumed() && !cancellationPin.expired(),
        "culling frame abandoned before final CPU seal owner joined");
    cpuFrame.reset();
    Require(cpuConsumer.IsConsumed() && cancellationPin.expired(),
        "cancelled culling frame did not abandon invocation or release terminal semantic ownership");
    bool recordedAfterCancellation = true;
    try { cpuConsumer.Record(newContext); }
    catch (const std::logic_error&) { recordedAfterCancellation = false; }
    Require(!recordedAfterCancellation,"cancelled culling invocation remained recordable");
    cpuConsumer = {};
    Require(cancellationPin.expired(),"cancelled culling semantic publication survived final CPU consumer");
    for (uint32_t failure = 0; failure < 4; ++failure) {
        auto failedEdit = graph.BeginEdit(); auto invalid = capturedBuild;
        if (failure == 0) invalid.accessSnapshots.pop_back();
        if (failure == 1) invalid.accessSnapshots[0].reset();
        if (failure == 2) invalid.reflectedViewAccesses.clear();
        if (failure == 3) invalid.configuration.expansionFactor = 0;
        bool rejected = false;
        try { executable.RebuildProgramInterface(failedEdit,author,invalid); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected && graph.Select() == selectedProgram,"failed typed culling build changed selection");
        rejected = false;
        try { failedEdit.Build(workspace,cancelled); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected,"failed typed culling build left selectable transaction");
    }
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
    // Install the real culling invocation/recording path into a persistent
    // executable. Commands reference declared logical bindings, never compiler
    // indices, and template construction is counted independently of invocation.
    org::persistent::GraphProgram graph; auto edit = graph.BeginEdit();
    auto snapshot = std::make_shared<org::ResourceBindingSnapshot>();
    snapshot->resource = rhi::Resource(rhi::ResourceHandle{71,1}); snapshot->backingGeneration = 1;
    auto nativeOwner = std::make_shared<const uint32_t>(1);
    snapshot->allocationOwner = nativeOwner; snapshot->descriptorOwner = nativeOwner;
    auto descriptorViews = std::make_shared<org::BindlessResourceViews>();
    descriptorViews->views.push_back({org::BindlessViewKind::ShaderResource,UINT32_MAX,0,0,
        rhi::DescriptorSlot(rhi::DescriptorHeapHandle{3,1},7)});
    snapshot->views = descriptorViews;
    org::persistent::BindingVersion binding; binding.identity = (uint64_t{1} << 32) | 71;
    binding.backingRevision = 1; binding.owner = snapshot; binding.recording = snapshot;
    const auto slot = edit.AddResource(binding.shape,binding);
    auto nativeProgram = std::make_shared<org::PipelineStatePayload>(
        rhi::PipelinePtr(rhi::Device{},rhi::Pipeline(rhi::PipelineHandle{8,1}),nullptr),0,org::PipelineResources{});
    nativeProgram->layout = {9,1}; nativeProgram->layoutOwner = nativeOwner;
    PersistentHierarchicalDispatchCullingPass author({.renderPhase = RenderPhase("Opaque")},
        {{slot,{static_cast<uint64_t>(rhi::ResourceAccessType::ShaderResource)
            | static_cast<uint64_t>(rhi::ResourceAccessType::IndirectArgument),0,1,false},{},org::BindlessViewRequest{}}});
    size_t templateBuilds = 0;
    br::render::CapturedCullingBindings capturedBindings;
    PersistentHierarchicalDispatchCullingPass::Bindings declaredBindings;
    PersistentHierarchicalDispatchCullingPass::ProgramBuildInputs build;
    build.commands = [&](const auto& bindings) {
        ++templateBuilds;
        declaredBindings = bindings;
        if (capturedBindings.snapshotResources.empty())
            capturedBindings.AddResource(snapshot,
                br::render::CapturedCullingBindings::Resource{bindings.accesses[0].resource,
                    {{org::BindlessViewRequest{},*bindings.accesses[0].view}}});
        capturedBindings.programs.emplace(nativeProgram->pso.Get().GetHandle(),nativeProgram);
        HierarchicalDispatchCullingRecipe result; result.hasObjectCull = true;
        result.prefix.commands.emplace_back(br::render::PreparedBindPersistentComputeProgram{nativeProgram});
        result.suffix.commands.emplace_back(br::render::PreparedExecuteIndirectCommand{
            {4,1},bindings.accesses[0].resource,64,1,nativeOwner});
        return result;
    };
    build.objectConstantViews = {{CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX,0}};
    auto executable = org::persistent::TypedPassExecutable<PersistentHierarchicalDispatchCullingPass>::Build(edit,{},author,build);
    org::experimental::CompileWorkspace workspace; std::atomic_bool cancelled{false};
    auto heldGraph = edit.Build(workspace,cancelled); Require(graph.Install(heldGraph),"persistent culling bootstrap");
    views[0].id = 13;
    PersistentHierarchicalDispatchCullingPass::InvocationInputs inputs{views,publication,3,4};
    RecordedConstants recorded;
    rhi::CommandListVTable table{}; table.abi_version = rhi::RHI_CL_ABI_MIN;
    table.bindPipeline = +[](rhi::CommandList* list,rhi::PipelineHandle value) noexcept {
        static_cast<RecordedConstants*>(list->impl)->pipeline = value;
    };
    table.bindLayout = +[](rhi::CommandList* list,rhi::PipelineLayoutHandle value) noexcept {
        static_cast<RecordedConstants*>(list->impl)->layout = value;
    };
    table.pushConstants = +[](rhi::CommandList* list,rhi::ShaderStage,uint32_t,uint32_t,uint32_t,uint32_t count,const void* data) noexcept {
        auto& result = *static_cast<RecordedConstants*>(list->impl);
        const auto* values = static_cast<const uint32_t*>(data); result.values.assign(values,values+count);
    };
    table.dispatch = +[](rhi::CommandList* list,uint32_t x,uint32_t y,uint32_t z) noexcept {
        auto& result = *static_cast<RecordedConstants*>(list->impl); ++result.dispatches; result.groups = {x,y,z};
    };
    table.executeIndirect = +[](rhi::CommandList* list,rhi::CommandSignatureHandle,rhi::ResourceHandle arguments,
        uint64_t,rhi::ResourceHandle,uint64_t,uint32_t) noexcept {
        static_cast<RecordedConstants*>(list->impl)->indirectResource = arguments;
    };
    rhi::CommandList list{rhi::CommandListHandle{1,1}}; list.vt = &table; list.impl = &recorded;
    auto packet = executable.PrepareInvocation(*heldGraph,inputs);
    auto recording = org::RecordingContext::FromPersistentBindings(list,heldGraph); packet.Record(recording);
    Require(recorded.groups[0] == 9 && recorded.values[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT] == 513
        && recorded.values[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] == 7
        && recorded.values[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] == 3,"persistent culling initial invocation");
    auto rotation = graph.BeginEdit(); auto rotatedBinding = heldGraph->bindings.At(slot);
    auto rotatedSnapshot = std::make_shared<org::ResourceBindingSnapshot>(*snapshot);
    auto rotatedViews = std::make_shared<org::BindlessResourceViews>(*descriptorViews); rotatedViews->views[0].descriptor.index = 19;
    rotatedSnapshot->resource = rhi::Resource(rhi::ResourceHandle{72,1}); rotatedSnapshot->backingGeneration = 2;
    rotatedSnapshot->views = rotatedViews; rotatedBinding.recording = rotatedSnapshot;
    br::render::CapturedHierarchicalDispatchCullingInputs capturedInputs;
    capturedInputs.m_visibleClustersBuffer = std::make_shared<const br::render::CapturedCullingResource>(
        br::render::CapturedCullingResource{snapshot});
    capturedInputs.m_clearPipelineState.payload = nativeProgram;
    const auto workerInputs = capturedInputs;
    capturedInputs.m_visibleClustersBuffer = std::make_shared<const br::render::CapturedCullingResource>(
        br::render::CapturedCullingResource{rotatedSnapshot});
    Require(workerInputs.m_visibleClustersBuffer->GetAPIResource().GetHandle().index == 71
        && capturedInputs.m_visibleClustersBuffer->GetAPIResource().GetHandle().index == 72,
        "captured worker culling resource followed producer replacement");
    Require(workerInputs.m_clearPipelineState.GetAPIPipelineState().GetHandle().index == 8,
        "captured worker culling program lost its native version");
    br::render::CapturedCullingCommandSink capturedSink(capturedBindings);
    capturedSink.BindPipeline(workerInputs.m_clearPipelineState.GetAPIPipelineState().GetHandle());
    std::array<br::render::SymbolicComputeConstant,NumMiscUintRootConstants> symbolicObject{};
    symbolicObject[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] =
        capturedSink.SRVIndex(workerInputs.m_visibleClustersBuffer);
    capturedSink.ObjectCull(symbolicObject.data());
    capturedSink.Dispatch(1,1,1);
    const auto capturedRecipe = capturedSink.Finish();
    HierarchicalDispatchCullingInvocation invocation;
    invocation.workloads.push_back({});
    Require(capturedRecipe.hasObjectCull && capturedRecipe.objectCullBindingViews.size() == 1,
        "captured sink lost symbolic object binding");
    HierarchicalDispatchCullingPass::RecordWithObjectConstants(capturedRecipe,invocation,recording,{});
    Require(recorded.values[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] == 7,
        "captured sink did not resolve selected object descriptor");
    rotatedBinding.identity = (uint64_t{1} << 32) | 72; rotatedBinding.backingRevision = 2;
    rotation.ReplaceBinding(slot,rotatedBinding);
    auto currentGraph = rotation.Build(workspace,cancelled); Require(graph.Install(currentGraph),"persistent culling rotation");
    Require(currentGraph->executable == heldGraph->executable,"culling replacement rebuilt executable");
    inputs.publication = next; inputs.windCacheGeneration = 11;
    auto currentPacket = executable.PrepareInvocation(*currentGraph,inputs);
    auto currentRecording = org::RecordingContext::FromPersistentBindings(list,currentGraph); currentPacket.Record(currentRecording);
    Require(recorded.groups[0] == 17 && recorded.values[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT] == 1025
        && recorded.values[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] == 19
        && recorded.values[CLOD_WG_DYNAMIC_WIND_BOUNDS_CACHE_GENERATION] == 11 && recorded.indirectResource.index == 72,
        "persistent culling fresh values and selected bindings");
    HierarchicalDispatchCullingPass::RecordWithObjectConstants(capturedRecipe,invocation,currentRecording,{});
    Require(recorded.values[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] == 19,
        "captured sink retained a previous physical descriptor");
    inputs.publication = publication;
    auto heldPacket = executable.PrepareInvocation(*heldGraph,inputs); heldPacket.Record(recording);
    Require(recorded.indirectResource.index == 71 && recorded.values[CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX] == 7,
        "held culling binding version changed");
    Require(templateBuilds == 1,"culling templates rebuilt during invocations or binding replacement");
    auto invalidLayout = graph.BeginEdit(); auto badBuild = build;
    badBuild.objectConstantViews = {{CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT,0}};
    bool rejected = false;
    try { executable.RebuildProgramInterface(invalidLayout,author,badBuild); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected,"culling descriptor overwrote fresh workload scalar");
    rejected = false;
    try { invalidLayout.Build(workspace,cancelled); }
    catch (const std::exception&) { rejected = true; }
    Require(rejected && graph.Select() == currentGraph,"failed culling program changed selection");
    for (const uint32_t constant : {static_cast<uint32_t>(CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT),
        static_cast<uint32_t>(NumMiscUintRootConstants),
        static_cast<uint32_t>(CLOD_PC_OBJECT_CULL_INVALIDATION_COUNT_SRV_INDEX)}) {
        auto invalidCaptured = build;
        invalidCaptured.commands = [&,constant](const auto& declared) {
            auto result = build.commands(declared);
            result.objectCullBindingViews.push_back({constant,declared.accesses[0].view});
            return result;
        };
        rejected = false;
        try { author.BuildProgramInterface(declaredBindings,invalidCaptured); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected,"captured culling patch bypassed object layout validation");
    }
    std::weak_ptr<const br::render::PublishedRendererState> semanticPin;
    auto isolated = std::make_shared<br::render::PublishedRendererState>(*publication); semanticPin = isolated;
    inputs.publication = isolated;
    auto pendingInvocation = executable.PrepareInvocation(*currentGraph,inputs);
    inputs.publication.reset(); isolated.reset();
    Require(!semanticPin.expired(),"culling invocation lost selected semantic publication");
    pendingInvocation.Abandon(org::AbandonReason::PreparationFailed);
    pendingInvocation = {};
    Require(semanticPin.expired(),"abandoned culling invocation retained publication");
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
    duplicateExtensions.AddExtension("duplicate", [] { return std::unique_ptr<org::RenderGraph::IRenderGraphExtension>{}; });
    duplicateExtensions.AddExtension("duplicate", [] { return std::unique_ptr<org::RenderGraph::IRenderGraphExtension>{}; });
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
    const auto symbolic = EvaluateMaterialGroupsPass::BuildConstants<br::render::SymbolicComputeConstant>(bindings,configuration,
        [](uint32_t) { return org::persistent::ViewToken{}; });
    const auto patches = br::render::BuildSymbolicComputeConstants(MiscUintRootSignatureIndex,0,symbolic);
    Require(patches.bindingViews.size() == 6 && patches.values[VISBUF_REYES_PATCH_INDEX_BASE] == 777,
        "material symbolic constants lost stable views or converted an equal-valued scalar");
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

void TestTypedMaterialEvaluationPreparation()
{
    org::persistent::GraphProgram graph; auto edit = graph.BeginEdit();
    auto owner = std::make_shared<const uint32_t>(1);
    auto snapshot = std::make_shared<org::ResourceBindingSnapshot>();
    snapshot->resource = rhi::Resource(rhi::ResourceHandle{21,1}); snapshot->backingGeneration = 1;
    snapshot->allocationOwner = owner; snapshot->descriptorOwner = owner;
    auto views = std::make_shared<org::BindlessResourceViews>();
    views->views.push_back({org::BindlessViewKind::ShaderResource,UINT32_MAX,0,0,rhi::DescriptorSlot({3,1},17)});
    snapshot->views = views;
    org::persistent::BindingVersion binding;
    binding.identity = (uint64_t{1} << 32) | 21; binding.backingRevision = 1; binding.owner = owner; binding.recording = snapshot;
    const auto slot = edit.AddResource(binding.shape,binding);
    const org::experimental::CompileResourceState access{
        static_cast<uint64_t>(rhi::ResourceAccessType::ShaderResource)|static_cast<uint64_t>(rhi::ResourceAccessType::IndirectArgument),0,1,false};
    auto author = std::make_unique<PersistentMaterialEvaluationPass>(PersistentMaterialEvaluationPass::Configuration{},
        std::vector<PersistentMaterialEvaluationPass::ResourceAccess>{{slot,access,{},org::BindlessViewRequest{}}});
    auto pipeline = std::make_shared<org::PipelineStatePayload>(
        rhi::PipelinePtr(rhi::Device{},rhi::Pipeline(rhi::PipelineHandle{8,1}),nullptr),0,org::PipelineResources{});
    pipeline->layout = {9,1}; pipeline->layoutOwner = owner;
    pipeline->pipelineResources.mandatoryResourceDescriptorSlots = {"Builtin::Required"};
    pipeline->pipelineResources.optionalResourceDescriptorSlots = {"Builtin::Optional"};
    br::render::PublishedMaterialState captureMaterials;
    captureMaterials.activeCompileFlags = {MaterialCompileNone,MaterialCompileNone};
    captureMaterials.activeCompileFlagSlots = {0,1}; captureMaterials.compileFlagSlotsUsed = 2;
    uint32_t captures = 0;
    auto captured = EvaluateMaterialGroupsPass::CaptureProgramVersions(captureMaterials,false,OutputType::NORMAL,
        2*sizeof(MaterialEvaluationIndirectCommand),[&](MaterialCompileFlags) { ++captures; return pipeline; });
    Require(captures == 1 && captured.size() == 1 && captured[0].program == pipeline,
        "material producer recaptured a shared shader key or lost exact program ownership");
    bool missingProgramAccepted = true;
    try { EvaluateMaterialGroupsPass::CaptureProgramVersions(captureMaterials,false,OutputType::NORMAL,
        2*sizeof(MaterialEvaluationIndirectCommand),[](MaterialCompileFlags) { return std::shared_ptr<const org::PipelineStatePayload>{}; }); }
    catch (const std::runtime_error&) { missingProgramAccepted = false; }
    Require(!missingProgramAccepted,"material producer accepted an unready publication program");
    auto signature = std::make_shared<const rhi::CommandSignaturePtr>(
        rhi::Device{},rhi::CommandSignature(rhi::CommandSignatureHandle{4,1}),nullptr);
    auto programs = PersistentMaterialEvaluationPass::MapCapturedPrograms(captured,{{org::ResourceIdentifier{"Builtin::Required"},0}},signature);
    Require(programs.programs[0].descriptorAccesses.size() == 2 && programs.programs[0].descriptorAccesses[0] == 0
        && !programs.programs[0].descriptorAccesses[1],"material reflection mapping lost access zero or optional absence");
    auto executable = org::persistent::TypedPassExecutable<PersistentMaterialEvaluationPass>::Build(edit,{},*author,programs);
    author.reset();
    org::experimental::CompileWorkspace workspace; std::atomic_bool cancelled{false};
    auto selected = edit.Build(workspace,cancelled); Require(graph.Install(selected),"typed material publication bootstrap failed");
    auto materials = std::make_shared<br::render::PublishedMaterialState>();
    materials->activeCompileFlags = {MaterialCompileNone}; materials->activeCompileFlagSlots = {1}; materials->compileFlagSlotsUsed = 2;
    PersistentMaterialEvaluationPass::InvocationInputs inputs{materials,false,OutputType::NORMAL,2*sizeof(MaterialEvaluationIndirectCommand),{false,0.25f,3,0.75f}};
    auto packet = executable.PrepareInvocation(*selected,inputs);
    RecordedConstants recorded; rhi::CommandListVTable table{}; table.abi_version = rhi::RHI_CL_ABI_MIN;
    table.bindPipeline = +[](rhi::CommandList* list,rhi::PipelineHandle handle) noexcept { static_cast<RecordedConstants*>(list->impl)->pipeline = handle; };
    table.bindLayout = +[](rhi::CommandList* list,rhi::PipelineLayoutHandle handle) noexcept { static_cast<RecordedConstants*>(list->impl)->layout = handle; };
    table.pushConstants = +[](rhi::CommandList* list,rhi::ShaderStage,uint32_t,uint32_t,uint32_t,uint32_t count,const void* data) noexcept {
        const auto* values = static_cast<const uint32_t*>(data); static_cast<RecordedConstants*>(list->impl)->values.assign(values,values+count);
    };
    table.executeIndirect = +[](rhi::CommandList* list,rhi::CommandSignatureHandle,rhi::ResourceHandle resource,uint64_t offset,
        rhi::ResourceHandle,uint64_t,uint32_t count) noexcept {
        auto& trace = *static_cast<RecordedConstants*>(list->impl); trace.indirectResource = resource; trace.indirectOffset = offset; trace.indirectCount = count;
    };
    rhi::CommandList commands{rhi::CommandListHandle{1,1}}; commands.vt = &table; commands.impl = &recorded;
    auto context = org::RecordingContext::FromPersistentBindings(commands,selected); packet.Record(context);
    Require(recorded.pipeline.index == 8 && recorded.layout.index == 9 && recorded.indirectResource.index == 21
        && recorded.indirectOffset == sizeof(MaterialEvaluationIndirectCommand) && recorded.indirectCount == 1
        && recorded.values[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] == 17,
        "typed material invocation lost immutable program, binding or active-entry data after author destruction");
    for (uint32_t failure = 0; failure < 3; ++failure) {
        auto rejectedEdit = graph.BeginEdit();
        PersistentMaterialEvaluationPass::Configuration invalidConfiguration;
        auto invalidAccess = access;
        if (failure == 0) invalidConfiguration.tessTableConfigs = 0;
        if (failure == 1) invalidAccess.access = static_cast<uint64_t>(rhi::ResourceAccessType::ShaderResource);
        if (failure == 2) invalidAccess.access = static_cast<uint64_t>(rhi::ResourceAccessType::IndirectArgument);
        PersistentMaterialEvaluationPass invalidAuthor(invalidConfiguration,{{slot,invalidAccess,{},org::BindlessViewRequest{}}});
        bool accepted = true;
        try { org::persistent::TypedPassExecutable<PersistentMaterialEvaluationPass>::Build(rejectedEdit,{},invalidAuthor,programs); }
        catch (const std::invalid_argument&) { accepted = false; }
        Require(!accepted && graph.Select() == selected,"invalid material declarations changed selected publication");
    }
    const auto reflectedProgram = [&](org::PipelineResources reflection) {
        auto replacement = std::make_shared<org::PipelineStatePayload>(
            rhi::PipelinePtr(rhi::Device{},rhi::Pipeline(rhi::PipelineHandle{10,1}),nullptr),0,std::move(reflection),2);
        replacement->layout = {11,1}; replacement->layoutOwner = owner;
        return replacement;
    };
    PersistentMaterialEvaluationPass replacementAuthor({},{{slot,access,{},org::BindlessViewRequest{}}});
    for (uint32_t failure = 0; failure < 3; ++failure) {
        org::PipelineResources reflection;
        reflection.mandatoryResourceDescriptorSlots = {failure == 1 ? "Builtin::Terrain::Rvt::Regions" : "Builtin::Required"};
        auto invalidPrograms = programs; invalidPrograms.programs[0].program = reflectedProgram(std::move(reflection));
        if (failure != 2) invalidPrograms.programs[0].descriptorAccesses = {std::nullopt};
        auto rejectedEdit = graph.BeginEdit(); bool accepted = true;
        try { executable.RebuildProgramInterface(rejectedEdit,replacementAuthor,invalidPrograms); }
        catch (const std::invalid_argument&) { accepted = false; }
        Require(!accepted && graph.Select() == selected,"missing material reflection changed selected publication");
    }
    for (bool disabledRvt : {false,true}) {
        org::PipelineResources reflection;
        if (disabledRvt) reflection.mandatoryResourceDescriptorSlots = {"Builtin::Terrain::Rvt::Regions"};
        else reflection.optionalResourceDescriptorSlots = {"Builtin::Optional"};
        auto replacements = programs; replacements.programs[0].program = reflectedProgram(std::move(reflection));
        replacements.programs[0].descriptorAccesses = {std::nullopt};
        PersistentMaterialEvaluationPass::Configuration configuration; configuration.terrainRvtEnabled = !disabledRvt;
        PersistentMaterialEvaluationPass readyAuthor(configuration,{{slot,access,{},org::BindlessViewRequest{}}});
        auto replacementEdit = graph.BeginEdit();
        auto replacementExecutable = executable.RebuildProgramInterface(replacementEdit,readyAuthor,replacements);
        auto publication = replacementEdit.Build(workspace,cancelled);
        Require(graph.Install(publication) && publication->executable == selected->executable,
            "compatible material program replacement rebuilt graph scheduling");
        auto replacementPacket = replacementExecutable.PrepareInvocation(*publication,inputs);
        auto replacementContext = org::RecordingContext::FromPersistentBindings(commands,publication);
        replacementPacket.Record(replacementContext);
        Require(recorded.pipeline.index == 10 && recorded.layout.index == 11 && recorded.indirectResource.index == 21,
            "optional or disabled-RVT material replacement lost program or resource versions");
        executable = replacementExecutable;
    }
}

void TestSoftwareRasterBucketSelection()
{
    struct PrimaryBindings {
        uint32_t histogram = 10, visible = 11, transforms = 12, viewInfo = 13, mapping = 14;
        uint32_t pageTable = 15, clipmapInfo = 16, physicalPages = 17, dynamicPages = 18, telemetry = 19;
        uint32_t skinMapping = 20, skinHash = 21, skinPositions = 22, skinAllocator = 23, skinWork = 24, skinArgs = 25, skinMembership = 26;
        bool virtualShadow = true, hasTelemetry = true, hasSkinCache = true;
    } primary;
    const auto constants = ClusterSoftwareRasterizationPass::BuildPrimaryConstants<uint32_t>(primary,[](uint32_t value) { return value+100; });
    Require(constants[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] == 110
        && constants[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] == 113
        && constants[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] == UINT32_MAX,
        "software raster primary constants lost selected descriptors or missing-cache defaults");
    auto symbolic = ClusterSoftwareRasterizationPass::BuildPrimaryConstants<br::render::SymbolicComputeConstant>(primary,
        [](uint32_t) { return org::persistent::ViewToken{}; });
    Require(br::render::BuildSymbolicComputeConstants(MiscUintRootSignatureIndex,0,symbolic).bindingViews.size() == 5,
        "software raster primary constants lost stable view provenance");
    ClusterSoftwareRasterizationPass::ApplyVirtualShadowConstants(symbolic,primary,{128,16384,10,1024,7},
        [](uint32_t) { return org::persistent::ViewToken{}; },[](uint32_t,uint32_t) { return org::persistent::ViewToken{}; });
    const auto shadowConstants = br::render::BuildSymbolicComputeConstants(MiscUintRootSignatureIndex,0,symbolic);
    Require(shadowConstants.bindingViews.size() == 17
        && shadowConstants.values[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT] == 10
        && shadowConstants.values[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] == 128,
        "software raster shadow constants confused scalar shape with descriptor bindings");
    auto numeric = constants; uint32_t pageVariant = UINT32_MAX;
    ClusterSoftwareRasterizationPass::ApplyVirtualShadowConstants(numeric,primary,{128,16384,10,1024,7},
        [](uint32_t value) { return value+100; },[&](uint32_t value,uint32_t variant) {
            if (value == primary.pageTable) pageVariant = variant;
            return value+200;
        });
    Require(pageVariant == 7 && numeric[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] == 215
        && numeric[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] == 126,
        "software raster shadow constants lost full-array UAV or membership SRV selection");
    auto nativeOwner = std::make_shared<const uint32_t>(1);
    auto pipeline = std::make_shared<org::PipelineStatePayload>(
        rhi::PipelinePtr(rhi::Device{},rhi::Pipeline(rhi::PipelineHandle{8,1}),nullptr),0,org::PipelineResources{});
    pipeline->layout = {9,1}; pipeline->layoutOwner = nativeOwner;
    const std::array<MaterialRasterFlags,2> sharedFlags{MaterialRasterFlagsNone,MaterialRasterFlagsNone};
    uint32_t captures = 0;
    const auto programs = ClusterSoftwareRasterizationPass::CaptureProgramVersions(2,sharedFlags,2*sizeof(RasterizeClustersCommand),
        [&](MaterialRasterFlags) { ++captures; return pipeline; });
    Require(captures == 1 && programs.size() == 1 && programs[0].program == pipeline,
        "software raster producer lost exact versions or recaptured a shared program");
    bool acceptedUnready = true;
    try { ClusterSoftwareRasterizationPass::CaptureProgramVersions(2,sharedFlags,2*sizeof(RasterizeClustersCommand),
        [](MaterialRasterFlags) { return std::shared_ptr<const org::PipelineStatePayload>{}; }); }
    catch (const std::runtime_error&) { acceptedUnready = false; }
    Require(!acceptedUnready,"software raster producer accepted an unready program");
    uint32_t generation = 41;
    Require(ClusterSoftwareRasterizationPass::PrepareInvocationGeneration(false,true,3,generation) == 0
        && ClusterSoftwareRasterizationPass::PrepareInvocationGeneration(true,false,3,generation) == 0
        && ClusterSoftwareRasterizationPass::PrepareInvocationGeneration(true,true,0,generation) == 0 && generation == 41,
        "inactive software raster work consumed a skin-cache generation");
    const auto heldGeneration = ClusterSoftwareRasterizationPass::PrepareInvocationGeneration(true,true,3,generation);
    Require(heldGeneration == 42 && ClusterSoftwareRasterizationPass::PrepareInvocationGeneration(true,true,3,generation) == 43
        && heldGeneration == 42,"fresh software raster invocations changed a held generation");
    for (uint32_t boundary : {0x7FFFFFFEu,UINT32_MAX}) {
        generation = boundary;
        Require(ClusterSoftwareRasterizationPass::PrepareInvocationGeneration(true,true,1,generation) == 1,
            "software raster generation wrap used a reserved or zero generation");
    }
    const std::array<MaterialRasterFlags,3> flags{MaterialRasterFlagsNone,MaterialRasterFlagsAlphaTest,MaterialRasterFlagsSkinned};
    constexpr uint64_t stride = sizeof(RasterizeClustersCommand);
    std::vector<uint32_t> selected; std::vector<uint64_t> offsets;
    ClusterSoftwareRasterizationPass::VisitBuckets(3,flags,3*stride,
        [&](MaterialRasterFlags flag,uint32_t bucket,uint64_t offset) {
            Require(flag == flags[bucket],"software raster selected the wrong program flags");
            selected.push_back(bucket); offsets.push_back(offset);
        });
    Require(selected == std::vector<uint32_t>{0,1,2} && offsets == std::vector<uint64_t>{0,stride,2*stride},
        "software raster changed bucket order or indirect argument offsets");
    for (bool missingFlags : {false,true}) {
        uint32_t callbacks = 0; bool accepted = true;
        try { ClusterSoftwareRasterizationPass::VisitBuckets(missingFlags ? 4 : 3,flags,missingFlags ? 4*stride : 3*stride-1,
            [&](MaterialRasterFlags,uint32_t,uint64_t) { ++callbacks; }); }
        catch (const std::out_of_range&) { accepted = false; }
        Require(!accepted && !callbacks,"incomplete software raster publication partially built commands");
    }
    ClusterSoftwareRasterizationPass::VisitBuckets(0,{},0,
        [](MaterialRasterFlags,uint32_t,uint64_t) { throw std::logic_error("empty raster selected work"); });
}

void TestSoftwareSkinCacheEmission()
{
    struct Frame {
        bool enabled = true, hasSkinCache = true; uint32_t bucketCount = 2;
        struct Raster { uint32_t commandSignature = 1; std::optional<uint32_t> argumentsReference = 2; } raster;
        std::array<uint32_t,NumMiscUintRootConstants> cacheConstants{}, clearConstants{};
        uint32_t clearProgram = 1, buildProgram = 2, finalizeProgram = 3, skinProgram = 4, resolveProgram = 5;
        uint32_t cacheAllocator = 10, cacheHash = 11, cacheWorkRecords = 12, cacheIndirectArgs = 13, cachePositions = 14, cacheMapping = 15;
        uint32_t cacheDispatchSignature = 2;
    } frame;
    struct Sink {
        std::vector<uint32_t> programs, barriers;
        std::vector<uint64_t> offsets;
        uint32_t generation = 0, dispatches = 0, indirectBarrier = 0;
        void Bind(uint32_t value) { programs.push_back(value); }
        void Constants(const std::array<uint32_t,NumMiscUintRootConstants>& values) { generation = values[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION]; }
        void Dispatch(uint32_t,uint32_t,uint32_t) { ++dispatches; }
        void Indirect(uint32_t,uint32_t,uint64_t offset) { offsets.push_back(offset); }
        void UavBarrier(uint32_t value) { barriers.push_back(value); }
        void IndirectBarrier(uint32_t value) { indirectBarrier = value; }
    } sink;
    ClusterSoftwareRasterizationPass::EmitSkinCacheCommands(frame,42,sink);
    Require(sink.programs == std::vector<uint32_t>{1,2,3,4,5} && sink.dispatches == 2 && sink.offsets.size() == 5
        && sink.offsets[1] == sizeof(RasterizeClustersCommand) && sink.offsets[4] == sizeof(RasterizeClustersCommand)
        && sink.barriers == std::vector<uint32_t>{10,11,10,12,14,15} && sink.indirectBarrier == 13
        && sink.generation == 42 && frame.cacheConstants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] == 0,
        "software skin-cache emission lost required phases, synchronization or fresh generation isolation");
    for (bool missingArguments : {false,true}) {
        Sink rejectedSink; auto invalid = frame;
        if (missingArguments) invalid.raster.argumentsReference.reset();
        bool accepted = true;
        try { ClusterSoftwareRasterizationPass::EmitSkinCacheCommands(invalid,missingArguments ? 42 : 0,rejectedSink); }
        catch (const std::invalid_argument&) { accepted = false; }
        Require(!accepted && rejectedSink.programs.empty(),"invalid skin-cache invocation partially emitted commands");
    }
}

int main()
{
    try {
        TestResolverSnapshotLifetime();
        TestInvocationConstantRecording();
        TestFullCapturedCullingEmission();
        TestPublishedCullingInvocationPreparation();
        TestPersistentDescriptorCommandRecording();
        TestPersistentRendererPublicationBindings();
        TestPixelListInvocationPreparation();
        TestMaterialCommandSelection();
        TestTypedMaterialEvaluationPreparation();
        TestSoftwareRasterBucketSelection();
        TestSoftwareSkinCacheEmission();
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
