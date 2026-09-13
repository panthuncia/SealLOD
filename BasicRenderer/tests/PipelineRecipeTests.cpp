#include <iostream>
#include <stdexcept>

#include "Render/Pipeline/PipelineRecipe.h"
#include "Render/DepthHistoryService.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"

namespace {
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
        auto group = std::make_shared<ResourceGroup>("LifetimeTest");
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
    invalidOrder.Remove<br::pipeline::CanonicalSurfaceResourcesTechnique>();
    invalidOrder.Add<br::pipeline::CanonicalSurfaceResourcesTechnique>();
    Require(!invalidOrder.Validate().valid, "technique dependency order must be validated");

    auto invalidVoxel = br::pipeline::MakeBasicRendererDemoPipeline();
    invalidVoxel.Add<br::pipeline::ClusterLodVoxelTechnique>({ .workRecordCapacity = 0u });
    Require(!invalidVoxel.Validate().valid, "zero-capacity CLod voxel technique must fail validation");
}
}

int main()
{
    try {
        TestResolverSnapshotLifetime();
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
