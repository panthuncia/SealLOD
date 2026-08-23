#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <rhi.h>

#include "Render/GraphExtensions/RenderGraphExtensionRegistration.h"
#include "Resources/Resource.h"
#include "Resources/ResourceIdentifier.h"
#include "Resources/PixelBuffer.h"

namespace org { class RenderGraph; }
using org::RenderGraph;

namespace br::pipeline {

enum class ResourceBindingOwnership : uint8_t {
    GraphOwned,
    Imported,
    ImportedOutput,
    Persistent,
    Exported,
};

enum class ResourceViewCapability : uint8_t {
    ShaderResource = 1u << 0u,
    UnorderedAccess = 1u << 1u,
    RenderTarget = 1u << 2u,
    DepthStencil = 1u << 3u,
};

struct ResourceBindingContract {
    ResourceBindingOwnership ownership = ResourceBindingOwnership::Imported;
    rhi::Format format = rhi::Format::Unknown;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint8_t requiredViews = 0u;
    rhi::ResourceAccessType initialAccess = rhi::ResourceAccessType::None;
    rhi::ResourceLayout initialLayout = rhi::ResourceLayout::Undefined;
    rhi::ResourceSyncState initialSync = rhi::ResourceSyncState::None;
    rhi::ResourceAccessType finalAccess = rhi::ResourceAccessType::None;
    rhi::ResourceLayout finalLayout = rhi::ResourceLayout::Undefined;
    rhi::ResourceSyncState finalSync = rhi::ResourceSyncState::None;
};

template<typename TResource>
struct ResourceSlot {
    ResourceIdentifier identifier;

    explicit ResourceSlot(ResourceIdentifier id)
        : identifier(std::move(id)) {}
};

struct ResourceBinding {
    std::shared_ptr<Resource> resource;
    ResourceBindingContract contract;
};

class ResourceBindings {
public:
    template<typename TResource>
    void Bind(
        const ResourceSlot<TResource>& slot,
        std::shared_ptr<TResource> resource,
        ResourceBindingContract contract = {}) {
        static_assert(std::is_base_of_v<Resource, TResource>);
        m_bindings.insert_or_assign(
            slot.identifier,
            ResourceBinding{ std::move(resource), contract });
    }

    template<typename TResource>
    std::shared_ptr<TResource> Find(const ResourceSlot<TResource>& slot) const {
        auto it = m_bindings.find(slot.identifier);
        if (it == m_bindings.end()) {
            return nullptr;
        }
        return std::dynamic_pointer_cast<TResource>(it->second.resource);
    }

    const ResourceBinding* Find(ResourceIdentifier id) const;
    bool Contains(ResourceIdentifier id) const;
    const std::unordered_map<ResourceIdentifier, ResourceBinding>& Entries() const { return m_bindings; }

private:
    std::unordered_map<ResourceIdentifier, ResourceBinding> m_bindings;
};

enum class TechniqueId : uint8_t {
    FrameResources,
    BrdfIntegration,
    Environment,
    ClusterLod,
    ClusterLodAlpha,
    ClusterLodShadow,
    ClusterLodVoxel,
    CanonicalSurfaceResources,
    VisibilityMaterialBinning,
    TerrainRvt,
    TerrainRegionMaterialEvaluation,
    MaterialEvaluation,
    Gtao,
    ClusteredLighting,
    PrimaryLighting,
    Reflections,
    Exposure,
    Upscaling,
    Bloom,
    Tonemapping,
    DebugOutput,
    DebugUi,
    DepthHistory,
    Present,
};

enum class ReyesMode : uint8_t {
    Disabled,
    Enabled,
};

struct EmptyTechniqueOptions {};

struct ClusterLodOptions {
    ReyesMode reyes = ReyesMode::Enabled;
};

struct ClusterLodVoxelOptions {
    // Maximum records in each of the rigid and skinned queues. Each record is
    // currently 68 bytes, so the default bounds the pair to 136 MiB per CLod
    // variant instead of scaling both queues to the full visibility budget.
    uint32_t workRecordCapacity = 1u << 20;
};

struct ReflectionsOptions {
    bool screenSpace = false;
    bool rayTraced = false;
};

using TechniqueOptions = std::variant<EmptyTechniqueOptions, ClusterLodOptions, ClusterLodVoxelOptions, ReflectionsOptions>;

#define BR_DECLARE_PIPELINE_TECHNIQUE(Name, Value) \
    struct Name { \
        using Options = EmptyTechniqueOptions; \
        inline static constexpr TechniqueId Id = TechniqueId::Value; \
    }

BR_DECLARE_PIPELINE_TECHNIQUE(FrameResourcesTechnique, FrameResources);
BR_DECLARE_PIPELINE_TECHNIQUE(BrdfIntegrationTechnique, BrdfIntegration);
BR_DECLARE_PIPELINE_TECHNIQUE(EnvironmentTechnique, Environment);
struct ClusterLodTechnique {
    using Options = ClusterLodOptions;
    inline static constexpr TechniqueId Id = TechniqueId::ClusterLod;
};
struct ClusterLodAlphaTechnique {
    using Options = ClusterLodOptions;
    inline static constexpr TechniqueId Id = TechniqueId::ClusterLodAlpha;
};
struct ClusterLodShadowTechnique {
    using Options = ClusterLodOptions;
    inline static constexpr TechniqueId Id = TechniqueId::ClusterLodShadow;
};
struct ClusterLodVoxelTechnique {
    using Options = ClusterLodVoxelOptions;
    inline static constexpr TechniqueId Id = TechniqueId::ClusterLodVoxel;
};
BR_DECLARE_PIPELINE_TECHNIQUE(CanonicalSurfaceResourcesTechnique, CanonicalSurfaceResources);
BR_DECLARE_PIPELINE_TECHNIQUE(VisibilityMaterialBinningTechnique, VisibilityMaterialBinning);
BR_DECLARE_PIPELINE_TECHNIQUE(TerrainRvtTechnique, TerrainRvt);
BR_DECLARE_PIPELINE_TECHNIQUE(TerrainRegionMaterialEvaluationTechnique, TerrainRegionMaterialEvaluation);
BR_DECLARE_PIPELINE_TECHNIQUE(MaterialEvaluationTechnique, MaterialEvaluation);
BR_DECLARE_PIPELINE_TECHNIQUE(GtaoTechnique, Gtao);
BR_DECLARE_PIPELINE_TECHNIQUE(ClusteredLightingTechnique, ClusteredLighting);
BR_DECLARE_PIPELINE_TECHNIQUE(PrimaryLightingTechnique, PrimaryLighting);
struct ReflectionsTechnique {
    using Options = ReflectionsOptions;
    inline static constexpr TechniqueId Id = TechniqueId::Reflections;
};
BR_DECLARE_PIPELINE_TECHNIQUE(ExposureTechnique, Exposure);
BR_DECLARE_PIPELINE_TECHNIQUE(UpscalingTechnique, Upscaling);
BR_DECLARE_PIPELINE_TECHNIQUE(BloomTechnique, Bloom);
BR_DECLARE_PIPELINE_TECHNIQUE(TonemappingTechnique, Tonemapping);
BR_DECLARE_PIPELINE_TECHNIQUE(DebugOutputTechnique, DebugOutput);
BR_DECLARE_PIPELINE_TECHNIQUE(DebugUiTechnique, DebugUi);
BR_DECLARE_PIPELINE_TECHNIQUE(DepthHistoryTechnique, DepthHistory);
BR_DECLARE_PIPELINE_TECHNIQUE(PresentTechnique, Present);

#undef BR_DECLARE_PIPELINE_TECHNIQUE

struct TechniqueContract {
    std::vector<ResourceIdentifier> requiredInputs;
    std::vector<ResourceIdentifier> optionalInputs;
    std::vector<ResourceIdentifier> outputs;
};

class PipelineBuildContext {
public:
    using TechniqueCallback = std::function<void(TechniqueId, const TechniqueOptions&)>;

    PipelineBuildContext(
        RenderGraph& graph,
        const ResourceBindings& bindings,
        TechniqueCallback buildTechnique = {},
        TechniqueCallback registerExtensions = {})
        : m_graph(graph)
        , m_bindings(bindings)
        , m_buildTechnique(std::move(buildTechnique))
        , m_registerExtensions(std::move(registerExtensions)) {}

    RenderGraph& Graph() const { return m_graph; }
    const ResourceBindings& Bindings() const { return m_bindings; }
    void BuildTechnique(TechniqueId id, const TechniqueOptions& options) const {
        if (!m_buildTechnique) {
            throw std::logic_error("Pipeline build context has no technique builder");
        }
        m_buildTechnique(id, options);
    }
    void RegisterTechniqueExtensions(TechniqueId id, const TechniqueOptions& options) const {
        if (m_registerExtensions) {
            m_registerExtensions(id, options);
        }
    }

    template<typename TResource>
    std::shared_ptr<TResource> Require(const ResourceSlot<TResource>& slot) const {
        auto resource = m_bindings.Find(slot);
        if (!resource) {
            throw std::runtime_error("Required pipeline resource is not bound: " + slot.identifier.ToString());
        }
        return resource;
    }

    template<typename TResource, typename TFactory>
    std::shared_ptr<TResource> GetOrCreateOutput(
        const ResourceSlot<TResource>& slot,
        TFactory&& factory) const {
        auto resource = m_bindings.Find(slot);
        if (!resource) {
            resource = std::invoke(std::forward<TFactory>(factory));
        }
        if (!resource) {
            throw std::runtime_error("Pipeline output factory returned null: " + slot.identifier.ToString());
        }
        m_graph.RegisterResource(slot.identifier, resource);
        return resource;
    }

private:
    RenderGraph& m_graph;
    const ResourceBindings& m_bindings;
    TechniqueCallback m_buildTechnique;
    TechniqueCallback m_registerExtensions;
};

class IRenderTechnique {
public:
    virtual ~IRenderTechnique() = default;
    virtual TechniqueId Id() const = 0;
    virtual const TechniqueOptions& Options() const = 0;
    virtual void Describe(TechniqueContract&) const {}
    virtual void RegisterExtensions(PipelineBuildContext& context) const {
        context.RegisterTechniqueExtensions(Id(), Options());
    }
    virtual void Build(PipelineBuildContext& context) const {
        context.BuildTechnique(Id(), Options());
    }
};

template<typename TTechnique>
class ConfiguredRenderTechnique final : public IRenderTechnique {
public:
    explicit ConfiguredRenderTechnique(typename TTechnique::Options options)
        : m_options(std::move(options)) {}

    TechniqueId Id() const override { return TTechnique::Id; }
    const TechniqueOptions& Options() const override { return m_options; }

private:
    TechniqueOptions m_options;
};

struct TechniqueEntry {
    TechniqueId id = TechniqueId::FrameResources;
    TechniqueOptions options;
    std::shared_ptr<const IRenderTechnique> technique;
};

struct PipelineValidationResult {
    bool valid = true;
    std::vector<std::string> errors;
};

class PipelineRecipe {
public:
    template<typename TTechnique>
    PipelineRecipe& Add(typename TTechnique::Options options = {}) {
        Remove<TTechnique>();
        auto technique = std::make_shared<ConfiguredRenderTechnique<TTechnique>>(std::move(options));
        m_techniques.push_back(TechniqueEntry{ TTechnique::Id, technique->Options(), std::move(technique) });
        return *this;
    }

    PipelineRecipe& AddTechnique(std::shared_ptr<const IRenderTechnique> technique);

    template<typename TTechnique>
    PipelineRecipe& Remove() {
        std::erase_if(m_techniques, [](const TechniqueEntry& entry) { return entry.id == TTechnique::Id; });
        return *this;
    }

    template<typename TTechnique>
    PipelineRecipe& Configure(typename TTechnique::Options options) {
        for (auto& entry : m_techniques) {
            if (entry.id == TTechnique::Id) {
                entry.options = std::move(options);
                entry.technique = std::make_shared<ConfiguredRenderTechnique<TTechnique>>(
                    std::get<typename TTechnique::Options>(entry.options));
                return *this;
            }
        }
        return Add<TTechnique>(std::move(options));
    }

    template<typename TTechnique>
    bool Contains() const {
        return Contains(TTechnique::Id);
    }

    template<typename TTechnique>
    typename TTechnique::Options Options() const {
        for (const auto& entry : m_techniques) {
            if (entry.id == TTechnique::Id) {
                if (const auto* options = std::get_if<typename TTechnique::Options>(&entry.options)) {
                    return *options;
                }
            }
        }
        return {};
    }

    bool Contains(TechniqueId id) const;
    PipelineRecipe& AddExtension(std::string id, RenderGraphExtensionFactory factory);

    ResourceBindings& Bindings() { return m_bindings; }
    const ResourceBindings& Bindings() const { return m_bindings; }
    const std::vector<TechniqueEntry>& Techniques() const { return m_techniques; }
    const std::vector<std::pair<std::string, RenderGraphExtensionFactory>>& Extensions() const { return m_extensions; }
    PipelineValidationResult Validate() const;

private:
    std::vector<TechniqueEntry> m_techniques;
    ResourceBindings m_bindings;
    std::vector<std::pair<std::string, RenderGraphExtensionFactory>> m_extensions;
};

namespace Slots {
    extern const ResourceSlot<PixelBuffer> EnvironmentCubemap;
    extern const ResourceSlot<PixelBuffer> EnvironmentPrefilteredCubemap;
}

PipelineRecipe MakeBasicRendererDemoPipeline();
PipelineRecipe MakeSarpPipeline();
PipelineRecipe MakeGeometryMaterialProducerPipeline();

} // namespace br::pipeline
