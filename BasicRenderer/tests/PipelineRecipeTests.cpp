#include <iostream>
#include <stdexcept>

#include "Render/Pipeline/PipelineRecipe.h"

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
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
    Require(recipe.Contains<br::pipeline::CanonicalSurfaceFinalizationTechnique>(),
        "SARP recipe must publish canonical surfaces");

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
    Require(recipe.Contains<br::pipeline::CanonicalSurfaceFinalizationTechnique>(), "producer must finalize surfaces");
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
    duplicateExtensions.AddExtension("duplicate", [] { return std::unique_ptr<RenderGraph::IRenderGraphExtension>{}; });
    duplicateExtensions.AddExtension("duplicate", [] { return std::unique_ptr<RenderGraph::IRenderGraphExtension>{}; });
    Require(!duplicateExtensions.Validate().valid, "duplicate extension ids must fail validation");

    TextureDescription environmentDescription;
    environmentDescription.format = rhi::Format::R16G16B16A16_Float;
    environmentDescription.imageDimensions.push_back({ 4u, 4u, 0u, 0u });
    environmentDescription.isCubemap = true;
    environmentDescription.arraySize = 6u;
    environmentDescription.hasSRV = false;
    auto incompatibleEnvironment = PixelBuffer::CreateSharedUnmaterialized(environmentDescription);
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
    invalidOrder.Remove<br::pipeline::GBufferResourcesTechnique>();
    invalidOrder.Add<br::pipeline::GBufferResourcesTechnique>();
    Require(!invalidOrder.Validate().valid, "technique dependency order must be validated");

    auto invalidVoxel = br::pipeline::MakeBasicRendererDemoPipeline();
    invalidVoxel.Add<br::pipeline::ClusterLodVoxelTechnique>({ .workRecordCapacity = 0u });
    Require(!invalidVoxel.Validate().valid, "zero-capacity CLod voxel technique must fail validation");
}
}

int main()
{
    try {
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
