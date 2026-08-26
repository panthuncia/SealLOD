#include <iostream>
#include <stdexcept>

#include "Mesh/VertexFlags.h"
#include "Render/DrawWorkload.h"
#include "Render/MaterialCompileFlagsSlotRegistry.h"

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool HasFlag(MaterialCompileFlags flags, MaterialCompileFlags flag)
{
    return (static_cast<uint64_t>(flags) & static_cast<uint64_t>(flag)) != 0u;
}

PerMaterialCB DisplacedMaterialData(float minHeight = 0.0f, float maxHeight = 1.0f)
{
    PerMaterialCB data{};
    data.geometricDisplacementEnabled = 1u;
    data.geometricDisplacementMin = minHeight;
    data.geometricDisplacementMax = maxHeight;
    return data;
}

void TestVariantComposition()
{
    const PerMaterialCB ordinary{};
    auto variants = ComposeMaterialEvalVariantSet(MaterialCompileNone, 0u, true, ordinary);
    Require(!variants.hasDistinctReyes && variants.reyes == variants.regular,
        "ordinary CLod material must alias Reyes to its regular variant");

    variants = ComposeMaterialEvalVariantSet(
        MaterialCompileBaseColorTexture,
        VertexFlags::VERTEX_SKINNED,
        true,
        ordinary);
    Require(HasFlag(variants.regular, MaterialCompileClodSkinning),
        "skinned material must use the skinned evaluation variant");
    Require(!variants.hasDistinctReyes && variants.reyes == variants.regular,
        "ordinary skinned material must not create a Reyes variant");

    const auto displaced = DisplacedMaterialData();
    variants = ComposeMaterialEvalVariantSet(
        MaterialCompileGeometricDisplacement,
        0u,
        true,
        displaced);
    Require(variants.hasDistinctReyes && HasFlag(variants.reyes, MaterialCompileClodReyesPatch),
        "displaced CLod material must create a distinct Reyes variant");

    variants = ComposeMaterialEvalVariantSet(
        MaterialCompileGeometricDisplacement,
        VertexFlags::VERTEX_SKINNED,
        true,
        displaced);
    Require(variants.hasDistinctReyes &&
        HasFlag(variants.regular, MaterialCompileClodSkinning) &&
        HasFlag(variants.reyes, MaterialCompileClodSkinning),
        "displaced skinned material must preserve skinning in both variants");

    variants = ComposeMaterialEvalVariantSet(
        MaterialCompileGeometricDisplacement,
        0u,
        false,
        displaced);
    Require(!variants.hasDistinctReyes, "non-CLod mesh must not create a Reyes variant");

    variants = ComposeMaterialEvalVariantSet(
        MaterialCompileGeometricDisplacement,
        0u,
        true,
        DisplacedMaterialData(1.0f, 1.0f));
    Require(!variants.hasDistinctReyes, "zero-range displacement must not create a Reyes variant");

    variants = ComposeMaterialEvalVariantSet(
        static_cast<MaterialCompileFlags>(
            MaterialCompileGeometricDisplacement | MaterialCompileHeightFromBaseAlpha),
        0u,
        true,
        displaced);
    Require(!variants.hasDistinctReyes,
        "base-alpha displacement must remain on the non-Reyes material path");

    variants = ComposeMaterialEvalVariantSet(
        MaterialCompileTerrain,
        0u,
        true,
        displaced);
    Require(variants.hasDistinctReyes, "displaced terrain must create a Reyes variant");
}

void TestSlotLifecycle()
{
    MaterialCompileFlagsSlotRegistry registry;
    const auto voxel = registry.Acquire(MaterialCompileVoxel);
    Require(voxel.slot != 0u && registry.GetUsageCount(MaterialCompileVoxel) == 1u,
        "voxel slot must be pinned by an acquisition");

    const auto first = registry.Acquire(MaterialCompileClodSkinning);
    const auto shared = registry.Acquire(MaterialCompileClodSkinning, 2u);
    Require(first.slot == shared.slot, "shared variant acquisitions must reuse the same slot");
    Require(registry.GetUsageCount(MaterialCompileClodSkinning) == 3u,
        "shared variant acquisitions must accumulate references");
    Require(registry.GetActiveFlags().size() == 2u,
        "only voxel and the shared variant should be active");

    Require(registry.Release(MaterialCompileClodSkinning, 2u),
        "partial release must succeed");
    Require(registry.GetUsageCount(MaterialCompileClodSkinning) == 1u,
        "partial release must retain the variant");
    Require(registry.Release(MaterialCompileClodSkinning),
        "final release must succeed");

    unsigned int releasedSlot = 0u;
    Require(registry.TryGet(MaterialCompileClodSkinning, releasedSlot) && releasedSlot == first.slot,
        "final release must preserve the interned GPU-visible variant mapping");
    Require(registry.GetUsageCount(MaterialCompileClodSkinning) == 0u,
        "final release must drop the CPU ownership count");
    Require(!registry.Release(MaterialCompileClodSkinning),
        "over-release must be rejected");

    const auto reacquired = registry.Acquire(MaterialCompileClodSkinning);
    Require(reacquired.slot == first.slot,
        "reacquiring a released variant must retain its lifetime-stable slot");
    const auto distinct = registry.Acquire(MaterialCompileNormalMap);
    Require(distinct.slot != first.slot, "an interned slot must never be reused for different flags");
    Require(registry.GetUsageCount(MaterialCompileVoxel) == 1u,
        "new variants must not disturb the pinned voxel variant");
}
}

int main()
{
    try {
        TestVariantComposition();
        TestSlotLifecycle();
        std::cout << "MaterialEvalVariantTests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MaterialEvalVariantTests failed: " << error.what() << '\n';
        return 1;
    }
}
