#include "Render/Pipeline/PipelineRecipe.h"

#include "Render/BuiltinResources.h"

namespace br::pipeline {

namespace {
bool HasRequiredView(uint8_t requiredViews, ResourceViewCapability capability)
{
    return (requiredViews & static_cast<uint8_t>(capability)) != 0u;
}

void ValidatePixelBufferBinding(
    ResourceIdentifier id,
    const ResourceBinding& binding,
    PipelineValidationResult& result)
{
    const auto texture = std::dynamic_pointer_cast<PixelBuffer>(binding.resource);
    if (!texture) {
        return;
    }

    const auto& contract = binding.contract;
    const auto& description = texture->GetDescription();
    const auto addError = [&](std::string detail) {
        result.errors.push_back("Incompatible pipeline resource binding " + id.ToString() + ": " + std::move(detail));
    };

    if (contract.format != rhi::Format::Unknown && texture->GetFormat() != contract.format) {
        addError("format does not match the declared contract");
    }
    if (contract.width != 0u && texture->GetWidth() != contract.width) {
        addError("width does not match the declared contract");
    }
    if (contract.height != 0u && texture->GetHeight() != contract.height) {
        addError("height does not match the declared contract");
    }
    if (HasRequiredView(contract.requiredViews, ResourceViewCapability::ShaderResource) && !description.hasSRV) {
        addError("shader-resource view is required");
    }
    if (HasRequiredView(contract.requiredViews, ResourceViewCapability::UnorderedAccess) && !description.hasUAV) {
        addError("unordered-access view is required");
    }
    if (HasRequiredView(contract.requiredViews, ResourceViewCapability::RenderTarget) && !description.hasRTV) {
        addError("render-target view is required");
    }
    if (HasRequiredView(contract.requiredViews, ResourceViewCapability::DepthStencil) && !description.hasDSV) {
        addError("depth-stencil view is required");
    }
}
}

const ResourceBinding* ResourceBindings::Find(ResourceIdentifier id) const
{
    auto it = m_bindings.find(id);
    return it == m_bindings.end() ? nullptr : &it->second;
}

bool ResourceBindings::Contains(ResourceIdentifier id) const
{
    return m_bindings.contains(id);
}

bool PipelineRecipe::Contains(TechniqueId id) const
{
    return std::ranges::any_of(m_techniques, [id](const TechniqueEntry& entry) { return entry.id == id; });
}

PipelineRecipe& PipelineRecipe::AddTechnique(std::shared_ptr<const IRenderTechnique> technique)
{
    if (!technique) {
        throw std::invalid_argument("Cannot add a null render technique");
    }
    const TechniqueId id = technique->Id();
    std::erase_if(m_techniques, [id](const TechniqueEntry& entry) { return entry.id == id; });
    m_techniques.push_back(TechniqueEntry{ id, technique->Options(), std::move(technique) });
    return *this;
}

PipelineRecipe& PipelineRecipe::AddExtension(std::string id, RenderGraphExtensionFactory factory)
{
    m_extensions.emplace_back(std::move(id), std::move(factory));
    return *this;
}

PipelineValidationResult PipelineRecipe::Validate() const
{
    PipelineValidationResult result;
    std::unordered_set<TechniqueId> techniqueIds;
    for (const auto& entry : m_techniques) {
        if (!entry.technique) {
            result.errors.push_back("Pipeline technique has no implementation for id " +
                std::to_string(static_cast<uint32_t>(entry.id)));
        }
        if (!techniqueIds.insert(entry.id).second) {
            result.errors.push_back("Duplicate pipeline technique id " + std::to_string(static_cast<uint32_t>(entry.id)));
        }
    }

    std::unordered_set<std::string> extensionIds;
    for (const auto& [id, factory] : m_extensions) {
        if (id.empty()) {
            result.errors.emplace_back("Pipeline extension id cannot be empty");
        }
        else if (!extensionIds.insert(id).second) {
            result.errors.push_back("Duplicate pipeline extension id: " + id);
        }
        if (!factory) {
            result.errors.push_back("Pipeline extension factory is empty: " + id);
        }
    }

    for (const auto& [id, binding] : m_bindings.Entries()) {
        if (!binding.resource) {
            result.errors.push_back("Pipeline resource binding is null: " + id.ToString());
            continue;
        }
        ValidatePixelBufferBinding(id, binding, result);
    }

    const auto techniqueIndex = [&](TechniqueId id) -> std::optional<size_t> {
        for (size_t index = 0; index < m_techniques.size(); ++index) {
            if (m_techniques[index].id == id) return index;
        }
        return std::nullopt;
    };
    const auto requireBefore = [&](TechniqueId producer, TechniqueId consumer, std::string_view detail) {
        const auto producerIndex = techniqueIndex(producer);
        const auto consumerIndex = techniqueIndex(consumer);
        if (producerIndex && consumerIndex && *producerIndex > *consumerIndex) {
            result.errors.emplace_back(detail);
        }
    };

    if (Contains<TerrainRvtTechnique>() && !Contains<VisibilityMaterialBinningTechnique>()) {
        result.errors.emplace_back("Terrain RVT requires visibility material binning");
    }
    if (Contains<VisibilityMaterialBinningTechnique>()) {
        if (!Contains<ClusterLodTechnique>()) {
            result.errors.emplace_back("Visibility material binning requires the CLod visibility variant");
        }
    }
    if ((Contains<ClusterLodAlphaTechnique>() || Contains<ClusterLodShadowTechnique>()) &&
        !Contains<ClusterLodTechnique>()) {
        result.errors.emplace_back("CLod alpha and shadow techniques require the CLod visibility technique");
    }
    if (Contains<ClusterLodVoxelTechnique>() && !Contains<ClusterLodTechnique>()) {
        result.errors.emplace_back("CLod voxel rasterization requires the CLod visibility technique");
    }
    if (Contains<ClusterLodVoxelTechnique>() &&
        Options<ClusterLodVoxelTechnique>().workRecordCapacity == 0u) {
        result.errors.emplace_back("CLod voxel rasterization work-record capacity must be greater than zero");
    }
    if (Contains<MaterialEvaluationTechnique>() && !Contains<GBufferResourcesTechnique>()) {
        result.errors.emplace_back("Material evaluation requires GBuffer resources");
    }
    if (Contains<CanonicalSurfaceFinalizationTechnique>() && !Contains<MaterialEvaluationTechnique>()) {
        result.errors.emplace_back("Canonical surface finalization requires material evaluation");
    }
    if (Contains<GtaoTechnique>() && !Contains<GBufferResourcesTechnique>()) {
        result.errors.emplace_back("GTAO requires GBuffer resources");
    }
    if (Contains<PrimaryLightingTechnique>() && !Contains<GBufferResourcesTechnique>()) {
        result.errors.emplace_back("Primary lighting requires GBuffer resources");
    }
    if (Contains<ExposureTechnique>() && !Contains<PrimaryLightingTechnique>()) {
        result.errors.emplace_back("Exposure requires primary lighting output");
    }
    if (Contains<UpscalingTechnique>() && !Contains<PrimaryLightingTechnique>()) {
        result.errors.emplace_back("Upscaling requires primary lighting output");
    }
    if (Contains<TonemappingTechnique>() && !Contains<UpscalingTechnique>()) {
        result.errors.emplace_back("Tonemapping currently requires the upscaling output");
    }
    if (Contains<PresentTechnique>() && !Contains<TonemappingTechnique>()) {
        result.errors.emplace_back("Presentation currently requires tonemapping");
    }

    requireBefore(TechniqueId::FrameResources, TechniqueId::GBufferResources,
        "Frame resources must precede GBuffer resources");
    requireBefore(TechniqueId::ClusterLod, TechniqueId::GBufferResources,
        "CLod visibility must precede GBuffer construction");
    requireBefore(TechniqueId::GBufferResources, TechniqueId::VisibilityMaterialBinning,
        "GBuffer resources must precede visibility material binning");
    requireBefore(TechniqueId::MaterialEvaluation, TechniqueId::CanonicalSurfaceFinalization,
        "Material evaluation must precede canonical surface finalization");
    requireBefore(TechniqueId::CanonicalSurfaceFinalization, TechniqueId::PrimaryLighting,
        "Canonical surface finalization must precede primary lighting");
    requireBefore(TechniqueId::VisibilityMaterialBinning, TechniqueId::TerrainRvt,
        "Visibility material binning must precede terrain RVT");
    requireBefore(TechniqueId::VisibilityMaterialBinning, TechniqueId::MaterialEvaluation,
        "Visibility material binning must precede material evaluation");
    requireBefore(TechniqueId::TerrainRvt, TechniqueId::MaterialEvaluation,
        "Terrain RVT must precede material evaluation");
    requireBefore(TechniqueId::GBufferResources, TechniqueId::PrimaryLighting,
        "GBuffer resources must precede primary lighting");
    requireBefore(TechniqueId::Environment, TechniqueId::PrimaryLighting,
        "Environment preparation must precede primary lighting");
    requireBefore(TechniqueId::PrimaryLighting, TechniqueId::Exposure,
        "Primary lighting must precede exposure");
    requireBefore(TechniqueId::PrimaryLighting, TechniqueId::Upscaling,
        "Primary lighting must precede upscaling");
    requireBefore(TechniqueId::Upscaling, TechniqueId::Tonemapping,
        "Upscaling must precede tonemapping");
    requireBefore(TechniqueId::Tonemapping, TechniqueId::Present,
        "Tonemapping must precede presentation");

    result.valid = result.errors.empty();
    return result;
}

namespace Slots {
const ResourceSlot<PixelBuffer> EnvironmentCubemap{ Builtin::Environment::CurrentCubemap };
const ResourceSlot<PixelBuffer> EnvironmentPrefilteredCubemap{ Builtin::Environment::CurrentPrefilteredCubemap };
}

namespace {
PipelineRecipe MakeStandardPipeline(
    bool terrainRvt,
    ReyesMode reyes,
    bool clodAlpha,
    bool clodShadows,
    std::optional<ClusterLodVoxelOptions> clodVoxel)
{
    PipelineRecipe recipe;
    recipe.Add<FrameResourcesTechnique>()
        .Add<BrdfIntegrationTechnique>()
        .Add<EnvironmentTechnique>()
        .Add<ClusterLodTechnique>(ClusterLodOptions{
            .reyes = reyes });
    if (clodAlpha) {
        recipe.Add<ClusterLodAlphaTechnique>(ClusterLodOptions{ .reyes = reyes });
    }
    if (clodShadows) {
        recipe.Add<ClusterLodShadowTechnique>(ClusterLodOptions{ .reyes = reyes });
    }
    if (clodVoxel) {
        recipe.Add<ClusterLodVoxelTechnique>(*clodVoxel);
    }
    recipe.Add<GBufferResourcesTechnique>()
        .Add<VisibilityMaterialBinningTechnique>();
    if (terrainRvt) {
        recipe.Add<TerrainRvtTechnique>();
    }
    recipe.Add<MaterialEvaluationTechnique>()
        .Add<CanonicalSurfaceFinalizationTechnique>()
        .Add<GtaoTechnique>()
        .Add<ClusteredLightingTechnique>()
        .Add<PrimaryLightingTechnique>()
        .Add<ReflectionsTechnique>()
        .Add<ExposureTechnique>()
        .Add<UpscalingTechnique>()
        .Add<BloomTechnique>()
        .Add<TonemappingTechnique>()
        .Add<DebugOutputTechnique>()
        .Add<DebugUiTechnique>()
        .Add<DepthHistoryTechnique>()
        .Add<PresentTechnique>();
    return recipe;
}
}

PipelineRecipe MakeBasicRendererDemoPipeline()
{
    return MakeStandardPipeline(false, ReyesMode::Disabled, true, true, std::nullopt);
}

PipelineRecipe MakeSarpPipeline()
{
    return MakeStandardPipeline(
        true,
        ReyesMode::Enabled,
        true,
        true,
        ClusterLodVoxelOptions{ .workRecordCapacity = 1u << 20 });
}

PipelineRecipe MakeGeometryMaterialProducerPipeline()
{
    PipelineRecipe recipe;
    recipe.Add<FrameResourcesTechnique>()
        .Add<ClusterLodTechnique>(ClusterLodOptions{ .reyes = ReyesMode::Enabled })
        .Add<ClusterLodAlphaTechnique>(ClusterLodOptions{ .reyes = ReyesMode::Enabled })
        .Add<ClusterLodShadowTechnique>(ClusterLodOptions{ .reyes = ReyesMode::Enabled })
        .Add<GBufferResourcesTechnique>()
        .Add<VisibilityMaterialBinningTechnique>()
        .Add<TerrainRvtTechnique>()
        .Add<MaterialEvaluationTechnique>()
        .Add<CanonicalSurfaceFinalizationTechnique>();
    return recipe;
}

} // namespace br::pipeline
