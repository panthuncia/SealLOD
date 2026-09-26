#pragma once

#include <memory>
#include <vector>
#include <span>
#include <stdexcept>
#include <unordered_map>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeCommands.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"
#include "../shaders/PerPassRootConstants/clodRasterizationRootConstants.h"

namespace org { class Buffer; }
namespace org { class ResourceGroup; }

struct ClusterSoftwareRasterFrameData {
    br::render::PreparedComputeIndirectSequence raster;
    bool enabled = false;
    bool hasSkinCache = false;
    uint32_t bucketCount = 0;
    std::array<uint32_t, NumMiscUintRootConstants> cacheConstants{};
    std::array<uint32_t, NumMiscUintRootConstants> clearConstants{};
    org::PreparedProgramBinding clearProgram{}, buildProgram{}, finalizeProgram{}, skinProgram{}, resolveProgram{};
    rhi::CommandSignatureHandle cacheDispatchSignature{};
    org::PreparedResourceReference cacheIndirectArgs{};
    org::PreparedResourceReference cacheAllocator{}, cacheHash{}, cacheWorkRecords{}, cachePositions{}, cacheMapping{};
};
inline void RemapDescriptorIndices(ClusterSoftwareRasterFrameData& data, const org::DescriptorIndexRemap& remap) {
    br::render::RemapDescriptorIndices(data.raster, remap);
    for (auto* program : {&data.clearProgram, &data.buildProgram, &data.finalizeProgram, &data.skinProgram, &data.resolveProgram})
        org::RemapDescriptorIndices(*program, remap);
}

struct ClusterSoftwareRasterBindings {
    org::ResourceBindingToken histogram, visible, transforms, mapping, indirectArgs;
    org::ResourceBindingToken pageTable, clipmapInfo, physicalPages, dynamicPages, telemetry;
    org::ResourceBindingToken skinMapping, skinHash, skinPositions, skinAllocator, skinWork, skinArgs, skinMembership;
    bool virtualShadow = false, hasTelemetry = false, hasSkinCache = false;
};

class ClusterSoftwareRasterizationPass
    : public org::TypedRenderGraphPass<ClusterSoftwareRasterizationPass,
          uint32_t, ClusterSoftwareRasterBindings, ClusterSoftwareRasterFrameData>,
      public org::IDynamicDeclaredResources {
public:
    ClusterSoftwareRasterizationPass(
        std::shared_ptr<org::Buffer> compactedVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> compactedVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsHistogramBuffer,
        std::shared_ptr<org::Buffer> rasterBucketsIndirectArgsBuffer,
        std::shared_ptr<org::Buffer> sortedToUnsortedMappingBuffer,
        CLodRasterOutputKind outputKind,
        std::shared_ptr<org::PixelBuffer> virtualShadowPageTableTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowPhysicalPagesTexture,
        std::shared_ptr<org::PixelBuffer> virtualShadowDynamicPagesTexture,
        std::shared_ptr<org::Buffer> virtualShadowClipmapInfoBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr,
        bool runWhenComputeSWRasterEnabledOnly = false);
    ~ClusterSoftwareRasterizationPass();

    struct CapturedSkinCacheProgram {
        std::shared_ptr<const org::PipelineStatePayload> program;
        std::vector<std::optional<org::persistent::ViewToken>> descriptorViews;
    };
    struct CapturedSkinCacheFrame {
        bool enabled = true, hasSkinCache = true;
        uint32_t bucketCount = 0;
        struct Raster {
            std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
            std::optional<org::persistent::BindingToken> argumentsReference;
        } raster;
        std::array<br::render::SymbolicComputeConstant,NumMiscUintRootConstants> cacheConstants{}, clearConstants{};
        CapturedSkinCacheProgram clearProgram, buildProgram, finalizeProgram, skinProgram, resolveProgram;
        std::shared_ptr<const rhi::CommandSignaturePtr> cacheDispatchSignature;
        org::persistent::BindingToken cacheIndirectArgs, cacheAllocator, cacheHash, cacheWorkRecords, cachePositions, cacheMapping;
    };
    static br::render::PreparedComputeCommandSequence BuildCapturedSkinCacheCommands(const CapturedSkinCacheFrame& frame) {
        struct Sink {
            br::render::PreparedComputeCommandSequence commands;
            uint32_t constantBlocks = 0;
            void Bind(const CapturedSkinCacheProgram& program) {
                if (!program.program) throw std::invalid_argument("Captured software cache program has no owner");
                const auto& reflection = program.program->pipelineResources;
                const auto mandatory = reflection.mandatoryResourceDescriptorSlots.size();
                if (program.descriptorViews.size() != mandatory+reflection.optionalResourceDescriptorSlots.size())
                    throw std::invalid_argument("Captured software cache reflection layout mismatch");
                for (size_t i = 0; i < mandatory; ++i)
                    if (!program.descriptorViews[i]) throw std::invalid_argument("Captured software cache mandatory view is missing");
                commands.commands.emplace_back(br::render::PreparedBindPersistentComputeProgram{program.program});
                if (!program.descriptorViews.empty())
                    commands.commands.emplace_back(br::render::PreparedPersistentDescriptorIndices{program.descriptorViews});
            }
            void Constants(const std::array<br::render::SymbolicComputeConstant,NumMiscUintRootConstants>& values) {
                const auto mask = constantBlocks++ ? uint64_t{1} << CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION : 0;
                commands.commands.emplace_back(br::render::BuildSymbolicComputeConstants(MiscUintRootSignatureIndex,0,values,mask));
            }
            void Dispatch(uint32_t x,uint32_t y,uint32_t z) { commands.commands.emplace_back(br::render::PreparedDispatchGroups{x,y,z}); }
            void Indirect(const std::shared_ptr<const rhi::CommandSignaturePtr>& signature,org::persistent::BindingToken resource,uint64_t offset) {
                if (!signature || !signature->Get().GetHandle().valid()) throw std::invalid_argument("Captured software cache signature is missing");
                commands.commands.emplace_back(br::render::PreparedExecuteIndirectCommand{signature->Get().GetHandle(),resource,offset,1,signature});
            }
            void Barrier(org::persistent::BindingToken resource,rhi::ResourceAccessType after,rhi::ResourceSyncState sync) {
                br::render::PreparedBufferBarrierBatch batch;
                batch.barriers.push_back({resource,rhi::ResourceAccessType::UnorderedAccess,after,rhi::ResourceSyncState::ComputeShading,sync});
                commands.commands.emplace_back(std::move(batch));
            }
            void UavBarrier(org::persistent::BindingToken resource) { Barrier(resource,rhi::ResourceAccessType::UnorderedAccess,rhi::ResourceSyncState::ComputeShading); }
            void IndirectBarrier(org::persistent::BindingToken resource) { Barrier(resource,rhi::ResourceAccessType::IndirectArgument,rhi::ResourceSyncState::ExecuteIndirect); }
        } sink;
        EmitSkinCacheCommands(frame,1,sink);
        br::render::ValidatePersistentComputeCommands(sink.commands);
        return std::move(sink.commands);
    }

    // Shared sequence for live recording and publication-time command emission.
    // Frame operands may be frozen references or stable logical bindings; only
    // the sink knows their physical/program representation.
    template<class Frame, class Sink>
    static void EmitSkinCacheCommands(const Frame& frame, uint32_t generation, Sink& sink) {
        if (!frame.enabled || !frame.hasSkinCache || !frame.bucketCount) return;
        if (!generation || generation >= 0x7FFFFFFFu)
            throw std::invalid_argument("Software skin-cache invocation has a reserved generation");
        if (!frame.raster.argumentsReference)
            throw std::invalid_argument("Software skin-cache invocation has no raster arguments");
        auto constants = frame.cacheConstants;
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] = generation;
        sink.Bind(frame.clearProgram); sink.Constants(frame.clearConstants); sink.Dispatch(1,1,1);
        sink.Constants(constants);
        const auto buckets = [&](const auto& program) {
            sink.Bind(program);
            for (uint32_t bucket = 0; bucket < frame.bucketCount; ++bucket)
                sink.Indirect(frame.raster.commandSignature,*frame.raster.argumentsReference,uint64_t{bucket} * sizeof(RasterizeClustersCommand));
        };
        sink.UavBarrier(frame.cacheAllocator);
        buckets(frame.buildProgram);
        sink.UavBarrier(frame.cacheHash); sink.UavBarrier(frame.cacheAllocator); sink.UavBarrier(frame.cacheWorkRecords);
        sink.Bind(frame.finalizeProgram); sink.Dispatch(1,1,1);
        sink.IndirectBarrier(frame.cacheIndirectArgs);
        sink.Bind(frame.skinProgram); sink.Indirect(frame.cacheDispatchSignature,frame.cacheIndirectArgs,0);
        sink.UavBarrier(frame.cachePositions);
        buckets(frame.resolveProgram);
        sink.UavBarrier(frame.cacheMapping);
    }

    struct ShadowConfiguration {
        uint32_t pageTableResolution = 0, virtualResolution = 0;
        uint32_t skinCacheHashEntries = 0, skinCachePositionCapacity = 0;
        uint32_t pageTableUavVariant = UINT32_MAX;
    };
    template<class Constant, class Bindings, class Srv, class Uav>
    static void ApplyVirtualShadowConstants(std::array<Constant,NumMiscUintRootConstants>& constants,
        const Bindings& bindings, const ShadowConfiguration& configuration, Srv&& srv, Uav&& uav) {
        if (!bindings.virtualShadow) return;
        constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable,configuration.pageTableUavVariant);
        constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
        constants[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX] = uav(bindings.physicalPages,UINT32_MAX);
        constants[CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = uav(bindings.dynamicPages,UINT32_MAX);
        constants[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION] = Constant{configuration.pageTableResolution};
        constants[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_COUNT] = Constant{CLodVirtualShadowMaxSupportedClipmapCount};
        constants[CLOD_RASTER_VIRTUAL_SHADOW_VIRTUAL_RESOLUTION] = Constant{configuration.virtualResolution};
        if (bindings.hasTelemetry) constants[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry,UINT32_MAX);
        if (!bindings.hasSkinCache) return;
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX] = uav(bindings.skinMapping,UINT32_MAX);
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX] = uav(bindings.skinHash,UINT32_MAX);
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT] = Constant{configuration.skinCacheHashEntries};
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION] = Constant{0u};
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX] = uav(bindings.skinPositions,UINT32_MAX);
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY] = Constant{configuration.skinCachePositionCapacity};
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX] = uav(bindings.skinAllocator,UINT32_MAX);
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX] = uav(bindings.skinWork,UINT32_MAX);
        constants[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX] = uav(bindings.skinArgs,UINT32_MAX);
        constants[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX] = srv(bindings.skinMembership);
    }

    template<class Constant, class Bindings, class Resolve>
    static std::array<Constant,NumMiscUintRootConstants> BuildPrimaryConstants(const Bindings& bindings, Resolve&& resolve) {
        std::array<Constant,NumMiscUintRootConstants> constants{};
        for (const auto index : {CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX,
            CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX,
            CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX,
            CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX,CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX})
            constants[index] = Constant{UINT32_MAX};
        constants[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = resolve(bindings.histogram);
        constants[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = resolve(bindings.visible);
        constants[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = resolve(bindings.transforms);
        constants[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX] = Constant{UINT32_MAX}; // Published per preparation.
        constants[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] = resolve(bindings.mapping);
        return constants;
    }

    // Ordered preparation owns the counter; immutable recording data never
    // retains it. Inactive work consumes no generation.
    static uint32_t PrepareInvocationGeneration(bool enabled, bool hasSkinCache, uint32_t bucketCount, uint32_t& counter) {
        if (!enabled || !hasSkinCache || !bucketCount) return 0;
        if (++counter == 0 || counter >= 0x7FFFFFFFu) counter = 1;
        return counter;
    }

    // Device-independent publication selection, shared by live preparation and
    // captured callers. Validate the whole input before invoking any consumer.
    template<class Consumer>
    static void VisitBuckets(uint32_t count, std::span<const MaterialRasterFlags> flags,
        uint64_t argumentBufferBytes, Consumer&& consume) {
        constexpr uint64_t stride = sizeof(RasterizeClustersCommand);
        if (count > flags.size()) throw std::out_of_range("Software raster bucket flags are incomplete");
        if (count > argumentBufferBytes/stride) throw std::out_of_range("Software raster arguments are incomplete");
        for (uint32_t bucket = 0; bucket < count; ++bucket)
            consume(flags[bucket],bucket,uint64_t{bucket} * stride);
    }
    struct ProgramVersion {
        MaterialRasterFlags flags;
        std::shared_ptr<const org::PipelineStatePayload> program;
    };
    template<class Capture>
    static std::vector<ProgramVersion> CaptureProgramVersions(uint32_t count,
        std::span<const MaterialRasterFlags> flags, uint64_t argumentBufferBytes, Capture&& capture) {
        std::vector<ProgramVersion> result;
        std::unordered_map<MaterialRasterFlags,uint32_t> captured;
        VisitBuckets(count,flags,argumentBufferBytes,[&](MaterialRasterFlags key,uint32_t,uint64_t) {
            if (captured.contains(key)) return;
            auto program = capture(key);
            if (!program || !program->pso.Get().GetHandle().valid() || !program->layout.valid() || !program->layoutOwner)
                throw std::runtime_error("Software raster publication program is not ready");
            if (result.size() >= UINT32_MAX) throw std::overflow_error("Software raster programs exhausted");
            captured.emplace(key,static_cast<uint32_t>(result.size()));
            result.push_back({key,std::move(program)});
        });
        return result;
    }

    ClusterSoftwareRasterBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext&) const;
    uint32_t PrepareInvocation(const ClusterSoftwareRasterFrameData&, const ClusterSoftwareRasterBindings&,
        const org::PassPrepareContext&) const;
    ClusterSoftwareRasterFrameData BuildRecipe(const ClusterSoftwareRasterBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ClusterSoftwareRasterFrameData&, const uint32_t&,
        org::PassRecordContext&);

private:
    std::shared_ptr<rhi::CommandSignaturePtr> m_rasterizationCommandSignature;
    std::shared_ptr<rhi::CommandSignaturePtr> m_dynamicWindSkinCacheDispatchCommandSignature;
    org::PipelineState m_dynamicWindSkinCacheBuildPipeline;
    org::PipelineState m_dynamicWindSkinCacheSkinPipeline;
    org::PipelineState m_dynamicWindSkinCacheFinalizePipeline;
    org::PipelineState m_dynamicWindSkinCacheResolvePipeline;
    org::PipelineState m_dynamicWindSkinCacheClearPipeline;
    std::shared_ptr<org::Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsHistogramBuffer;
    std::shared_ptr<org::Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_sortedToUnsortedMappingBuffer;
    // The per-view table the shader reads; it embeds the visibility UAVs, so
    // it is published during preparation from the frame's bindings.
    CLodViewRasterInfoTable ViewRasterInfoTable(const org::PassPrepareContext&) const;
    org::PreparedTablePublisher m_viewRasterInfoPublisher{"CLod Software Raster View Raster Info"};
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowPhysicalPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_virtualShadowDynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_virtualShadowClipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheMappingBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheHashBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCachePositionsBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheAllocatorBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheWorkRecordsBuffer;
    std::shared_ptr<org::Buffer> m_dynamicWindSkinCacheIndirectArgsBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    CLodRasterOutputKind m_outputKind = CLodRasterOutputKind::VisibilityBuffer;
    std::vector<std::shared_ptr<org::PixelBuffer>> m_visibilityBuffers;
    bool m_declaredResourcesChanged = true;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
    uint32_t m_dynamicWindSkinCacheHashEntryCount = 0u;
    uint32_t m_dynamicWindSkinCachePositionCapacity = 0u;
    mutable uint32_t m_dynamicWindSkinCacheGeneration = 1u;
};
