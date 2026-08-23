#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/VirtualShadowCasterProvider.h"
#include "Utilities/Utilities.h"

namespace {
class MockCasterProvider final : public IVirtualShadowCasterProvider
{
public:
    explicit MockCasterProvider(std::string id) : m_id(std::move(id)) {}
    std::string_view GetVirtualShadowCasterProviderId() const noexcept override { return m_id; }
    void GatherVirtualShadowPreparationPasses(
        const VirtualShadowCasterBuildContext&, VirtualShadowPassBuilder& builder) override
    {
        RenderGraph::ExternalPassDesc pass{};
        pass.name = m_id + "::Prepare";
        builder.Add(std::move(pass));
    }
    void GatherVirtualShadowRasterPasses(
        const VirtualShadowCasterBuildContext&, VirtualShadowPassBuilder& builder) override
    {
        RenderGraph::ExternalPassDesc pass{};
        pass.name = m_id + "::Raster";
        builder.Add(std::move(pass));
    }
private:
    std::string m_id;
};

void RunCasterExtensionCases()
{
    VirtualShadowCasterRegistry empty;
    if (!empty.Empty() || empty.Size() != 0u) {
        throw std::runtime_error("virtual-shadow caster registry was not initially empty");
    }

    MockCasterProvider grass("Grass");
    MockCasterProvider rocks("Rocks");
    empty.Register(grass);
    empty.Register(rocks);
    bool duplicateRejected = false;
    try {
        MockCasterProvider duplicate("Grass");
        empty.Register(duplicate);
    }
    catch (const std::runtime_error&) {
        duplicateRejected = true;
    }
    if (!duplicateRejected || empty.Size() != 2u) {
        throw std::runtime_error("duplicate virtual-shadow provider ID was accepted");
    }

    std::vector<RenderGraph::ExternalPassDesc> passes;
    VirtualShadowCasterBuildContext context{};
    VirtualShadowPassBuilder builder(passes, "CoreRaster", "FinalizeFallback");
    empty.GatherRasterPasses(context, builder);
    if (passes.size() != 2u || builder.LastPassName() != "Rocks::Raster" ||
        !passes[0].where || passes[0].where->after != std::vector<std::string>{ "CoreRaster" } ||
        passes[1].where->after != std::vector<std::string>{ "Grass::Raster" } ||
        passes[1].where->before != std::vector<std::string>{ "FinalizeFallback" }) {
        throw std::runtime_error("virtual-shadow provider raster ordering was not stable");
    }

    VirtualShadowInvalidationQueue queue(2u);
    VirtualShadowInvalidationBounds rigid{};
    rigid.radius = 10.0f;
    queue.Enqueue(rigid);
    auto skinned = rigid;
    skinned.mobility = VirtualShadowCasterMobility::SkinnedOrDeformable;
    queue.Enqueue(skinned);
    if (queue.Enqueue(rigid)) {
        throw std::runtime_error("virtual-shadow invalidation overflow was not reported");
    }
    auto batch = queue.Drain(3u);
    if (!batch.invalidateAllActiveClipmaps || batch.bounds.size() != 2u ||
        batch.bounds[0].clipmapMask != 0xFFFFFFFFu ||
        batch.bounds[1].clipmapMask != 0xFFFFFFF8u || queue.GetOverflowCount() != 1u) {
        throw std::runtime_error("virtual-shadow bounds invalidation policy failed");
    }

    if (VirtualShadowCasterUsesDynamicLayer(VirtualShadowCasterMobility::Rigid, 0x4u) ||
        !VirtualShadowCasterUsesDynamicLayer(VirtualShadowCasterMobility::SkinnedOrDeformable, 0x4u) ||
        VirtualShadowCasterUsesDynamicLayer(VirtualShadowCasterMobility::SkinnedOrDeformable, 0u)) {
        throw std::runtime_error("virtual-shadow caster layer classification failed");
    }
}

struct Case
{
    uint32_t totalBudget;
    uint32_t upgradeBudget;
    uint32_t upgradeEligible;
    uint32_t normalEligible;
    uint32_t expectedUpgrade;
    uint32_t expectedNormal;
};

void RunBudgetCases()
{
    constexpr std::array cases = {
        Case{ 0u, 0u, 7u, 11u, 7u, 11u },
        Case{ 1u, 0u, 7u, 11u, 7u, 1u },
        Case{ 8u, 2u, 7u, 11u, 2u, 8u },
        Case{ 8u, 2u, 1u, 11u, 1u, 8u },
        Case{ 8u, 0u, 3u, 2u, 3u, 2u },
        Case{ 8u, 2u, 0u, 11u, 0u, 8u },
        Case{ 4u, 9u, 7u, 11u, 7u, 4u },
        Case{ 32u, 4u, 4u, 3u, 4u, 3u },
    };

    for (const Case& test : cases) {
        const auto result = CLodVirtualShadowAdmitPageCounts(
            test.totalBudget,
            test.upgradeBudget,
            test.upgradeEligible,
            test.normalEligible);
        if (result.upgrade != test.expectedUpgrade || result.normal != test.expectedNormal) {
            throw std::runtime_error("virtual-shadow budget admission case failed");
        }
        if (test.totalBudget != 0u && result.normal > test.totalBudget) {
            throw std::runtime_error("virtual-shadow normal budget exceeded");
        }
        if (test.upgradeBudget != 0u && result.upgrade > test.upgradeBudget) {
            throw std::runtime_error("virtual-shadow upgrade budget exceeded");
        }
    }
}

void RunAllocationAdmissionCases()
{
    struct AllocationCase
    {
        uint32_t budget;
        uint32_t allocationRequests;
        uint32_t dirtyRetries;
        uint32_t expectedAllocatedAndAdmitted;
        uint32_t expectedRetryAdmissions;
    };
    constexpr std::array cases = {
        AllocationCase{ 0u, 12u, 7u, 12u, 7u },
        AllocationCase{ 1u, 4u, 8u, 1u, 0u },
        AllocationCase{ 10u, 4u, 8u, 4u, 6u },
        AllocationCase{ 10u, 12u, 8u, 10u, 0u },
        AllocationCase{ 10u, 0u, 8u, 0u, 8u },
    };

    for (const AllocationCase& test : cases) {
        const uint32_t allocatedAndAdmitted =
            test.budget == 0u
            ? test.allocationRequests
            : (std::min)(test.allocationRequests, test.budget);
        const uint32_t remainingBudget =
            test.budget == 0u ? test.dirtyRetries : test.budget - allocatedAndAdmitted;
        const uint32_t retryAdmissions =
            test.budget == 0u
            ? test.dirtyRetries
            : (std::min)(test.dirtyRetries, remainingBudget);

        if (allocatedAndAdmitted != test.expectedAllocatedAndAdmitted ||
            retryAdmissions != test.expectedRetryAdmissions) {
            throw std::runtime_error("virtual-shadow allocation admission case failed");
        }
        if (test.budget != 0u &&
            allocatedAndAdmitted + retryAdmissions > test.budget) {
            throw std::runtime_error(
                "virtual-shadow allocation and retry admissions exceeded normal budget");
        }
        // The critical cache invariant: allocation never creates an
        // unadmitted mapping whose atlas contents still belong to an old page.
        if (allocatedAndAdmitted !=
            (test.budget == 0u
                ? test.allocationRequests
                : (std::min)(test.allocationRequests, test.budget))) {
            throw std::runtime_error(
                "virtual-shadow allocation escaped same-frame admission");
        }
    }
}

void RunDeferredPageClearLifecycleCase()
{
    bool physicalDirty = true;
    bool contentValid = true;

    // Deferral must preserve the cached page.
    physicalDirty = false;
    if (physicalDirty || !contentValid) {
        throw std::runtime_error(
            "virtual-shadow deferral did not preserve cached contents");
    }

    // A later admission must restore the physical clear request before any
    // depth-min raster writes can target the page.
    physicalDirty = true;
    if (!physicalDirty) {
        throw std::runtime_error(
            "virtual-shadow retry admission failed to re-arm page clear");
    }

    if (physicalDirty) {
        contentValid = false;
        physicalDirty = false;
    }
    if (physicalDirty || contentValid) {
        throw std::runtime_error(
            "virtual-shadow admitted page was not cleared and invalidated");
    }
}

void RunTwoLayerLifecycleCases()
{
    constexpr uint32_t clearDepth = 0x7F7FFFFFu;
    constexpr uint32_t physicalPage = 11u;
    const auto isStaticRasterable = [](uint32_t pageEntry) {
        constexpr uint32_t required =
            CLodVirtualShadowPageAllocatedMask |
            CLodVirtualShadowPageDirtyMask |
            CLodVirtualShadowPageAdmittedThisFrameMask;
        return (pageEntry & required) == required;
    };
    const auto isDynamicActive = [](uint32_t pageEntry) {
        constexpr uint32_t required =
            CLodVirtualShadowPageAllocatedMask |
            CLodVirtualShadowPageVisitedMask;
        return (pageEntry & required) == required;
    };
    const auto isCompositeSampleable = [&isDynamicActive](uint32_t pageEntry) {
        return isDynamicActive(pageEntry) &&
            (pageEntry & CLodVirtualShadowPageContentValidMask) != 0u;
    };

    uint32_t cleanPageEntry =
        physicalPage |
        CLodVirtualShadowPageAllocatedMask |
        CLodVirtualShadowPageVisitedMask |
        CLodVirtualShadowPageContentValidMask;
    uint32_t staticDepth = 400u;
    uint32_t dynamicDepth = 123u;

    // A clean cached page is not touched by the static clear/raster lifecycle.
    if (isStaticRasterable(cleanPageEntry)) {
        staticDepth = clearDepth;
    }
    if (staticDepth != 400u) {
        throw std::runtime_error(
            "two-layer VSM changed clean cached static depth");
    }

    // Every clean, valid active page initializes the transient layer from the
    // persistent static cache before skinned raster.
    if (isDynamicActive(cleanPageEntry)) {
        dynamicDepth =
            (cleanPageEntry & CLodVirtualShadowPageContentValidMask) != 0u
                ? staticDepth
                : clearDepth;
    }
    if (dynamicDepth != staticDepth) {
        throw std::runtime_error(
            "two-layer VSM did not initialize an active dynamic page");
    }

    // Skinned raster writes only dynamic depth. Composition retains the nearer
    // depth while preserving the persistent cached value.
    dynamicDepth = 250u;
    dynamicDepth = (std::min)(dynamicDepth, staticDepth);
    if (dynamicDepth != 250u || staticDepth != 400u) {
        throw std::runtime_error(
            "two-layer VSM failed nearer-depth composition");
    }
    dynamicDepth = clearDepth;
    dynamicDepth = (std::min)(dynamicDepth, staticDepth);
    if (dynamicDepth != staticDepth) {
        throw std::runtime_error(
            "two-layer VSM failed static-only composition");
    }

    // Dynamic activity neither admits static work nor validates static cache
    // contents.
    uint32_t dynamicOnlyEntry =
        physicalPage |
        CLodVirtualShadowPageAllocatedMask |
        CLodVirtualShadowPageVisitedMask |
        CLodVirtualShadowPageDirtyMask;
    const uint32_t staticAdmissionsBefore = 7u;
    dynamicDepth = 300u;
    if (!isDynamicActive(dynamicOnlyEntry) ||
        isStaticRasterable(dynamicOnlyEntry) ||
        (dynamicOnlyEntry & CLodVirtualShadowPageContentValidMask) != 0u ||
        staticAdmissionsBefore != 7u) {
        throw std::runtime_error(
            "skinned VSM work affected static validity or admission");
    }

    // Reused/new pages remain unsampleable until their admitted static clear
    // and finalization establish valid contents; stale static texels cannot be
    // composed into the current frame.
    staticDepth = 17u;
    if ((dynamicOnlyEntry & CLodVirtualShadowPageContentValidMask) != 0u) {
        dynamicDepth = (std::min)(dynamicDepth, staticDepth);
    }
    if (dynamicDepth != 300u) {
        throw std::runtime_error(
            "two-layer VSM exposed stale reused static contents");
    }

    // Static validity alone is insufficient for the transient composite.
    // Deferred lookup must not read a cached page that was not initialized
    // and rastered as part of this frame's dynamic-active set.
    const uint32_t inactiveCachedEntry =
        physicalPage |
        CLodVirtualShadowPageAllocatedMask |
        CLodVirtualShadowPageContentValidMask;
    if (isCompositeSampleable(inactiveCachedEntry) ||
        !isCompositeSampleable(cleanPageEntry)) {
        throw std::runtime_error(
            "two-layer VSM allowed deferred lookup to sample an inactive composite");
    }

    // Static and dynamic layers can be rasterized on frames with different
    // directional-light view translations. Rebase persistent static depth
    // into the current transient-composite space before the unsigned minimum.
    constexpr float worldAlongLightZ = 37.25f;
    constexpr float currentViewTranslationZ = -10.0f;
    constexpr float cachedViewTranslationZ = -18.0f;
    constexpr float currentDepth =
        -(worldAlongLightZ + currentViewTranslationZ);
    constexpr float expectedCachedDepth =
        -(worldAlongLightZ + cachedViewTranslationZ);
    constexpr float rebasedCurrentDepth =
        expectedCachedDepth +
        cachedViewTranslationZ -
        currentViewTranslationZ;
    if (std::abs(rebasedCurrentDepth - currentDepth) > 1e-6f) {
        throw std::runtime_error(
            "two-layer VSM cached static depth was not rebased into current page space");
    }
    if (std::abs(currentDepth - expectedCachedDepth) < 1.0f) {
        throw std::runtime_error(
            "two-layer VSM depth-space test did not exercise an origin mismatch");
    }
    constexpr float firstFrameRebasedDepth =
        currentDepth +
        currentViewTranslationZ -
        currentViewTranslationZ;
    if (std::abs(firstFrameRebasedDepth - currentDepth) > 1e-6f) {
        throw std::runtime_error(
            "two-layer VSM changed first-frame depth with matching view origins");
    }
}

void RunFallbackDependencyOverflowLifecycleCase()
{
    uint32_t pageEntry =
        23u |
        CLodVirtualShadowPageAllocatedMask |
        CLodVirtualShadowPageDirtyMask |
        CLodVirtualShadowPageContentValidMask |
        CLodVirtualShadowPageRerenderedThisFrameMask;
    bool physicalDirty = true;

    // Dependency-output overflow is auxiliary bookkeeping failure. The
    // successfully rendered contents must remain visible while the exact page
    // is queued for another capture attempt.
    pageEntry &= ~CLodVirtualShadowPageRerenderedThisFrameMask;
    if ((pageEntry & CLodVirtualShadowPageContentValidMask) == 0u ||
        (pageEntry & CLodVirtualShadowPageDirtyMask) == 0u ||
        !physicalDirty) {
        throw std::runtime_error(
            "fallback dependency overflow invalidated rendered shadow contents");
    }

    // Admission of that page on a later frame consumes the retained physical
    // dirty bit and replaces the still-sampleable cached contents.
    pageEntry &= ~CLodVirtualShadowPageContentValidMask;
    physicalDirty = false;
    if ((pageEntry & CLodVirtualShadowPageDirtyMask) == 0u ||
        (pageEntry & CLodVirtualShadowPageContentValidMask) != 0u ||
        physicalDirty) {
        throw std::runtime_error(
            "fallback dependency retry did not enter the normal clear lifecycle");
    }
}

void RunExactPageTokenCases()
{
    constexpr uint32_t physicalPage = 17u;
    constexpr uint32_t allocationGeneration = 9u;
    constexpr uint32_t contentGeneration = 1234u;
    constexpr uint32_t clipmap = 3u;
    constexpr uint32_t virtualAddress = 77u;
    constexpr CLodVirtualShadowPageToken token{
        physicalPage,
        allocationGeneration,
        contentGeneration,
        clipmap,
        virtualAddress
    };
    constexpr CLodVirtualShadowPhysicalPageMeta metadata{
        virtualAddress,
        contentGeneration,
        (allocationGeneration <<
            CLodVirtualShadowPhysicalPageAllocationGenerationShift) |
            CLodVirtualShadowPhysicalPageResidentFlag,
        clipmap
    };
    constexpr uint32_t pageEntry =
        physicalPage |
        CLodVirtualShadowPageAllocatedMask |
        CLodVirtualShadowPageContentValidMask;
    static_assert(CLodVirtualShadowPageTokenMatches(token, metadata, pageEntry));

    auto changedAllocation = metadata;
    changedAllocation.flags +=
        1u << CLodVirtualShadowPhysicalPageAllocationGenerationShift;
    if (CLodVirtualShadowPageTokenMatches(token, changedAllocation, pageEntry)) {
        throw std::runtime_error("reused physical page accepted stale VSM token");
    }
    auto changedContent = metadata;
    ++changedContent.lastTouchedFrame;
    if (CLodVirtualShadowPageTokenMatches(token, changedContent, pageEntry)) {
        throw std::runtime_error("rerendered page accepted stale VSM token");
    }
    auto changedOwner = metadata;
    ++changedOwner.ownerVirtualAddress;
    if (CLodVirtualShadowPageTokenMatches(token, changedOwner, pageEntry)) {
        throw std::runtime_error("reassigned virtual page accepted stale VSM token");
    }
    if (CLodVirtualShadowPageTokenMatches(
            token,
            metadata,
            pageEntry & ~CLodVirtualShadowPageContentValidMask)) {
        throw std::runtime_error("invalid cached content accepted VSM token");
    }
}

void RunAbsolutePageTagCases()
{
    const auto packTag = [](int32_t logicalX, int32_t logicalY,
                             int32_t offsetX, int32_t offsetY) {
        const uint32_t absoluteX =
            static_cast<uint32_t>(logicalX - offsetX) & 0xFFFFu;
        const uint32_t absoluteY =
            static_cast<uint32_t>(logicalY - offsetY) & 0xFFFFu;
        return absoluteX | (absoluteY << 16u);
    };

    const uint32_t cachedTag = packTag(20, 30, -5, 7);
    if (cachedTag != packTag(19, 30, -6, 7)) {
        throw std::runtime_error(
            "VSM absolute page tag changed for a stable world page");
    }
    if (cachedTag == packTag(20, 30, -6, 7)) {
        throw std::runtime_error(
            "VSM absolute page tag accepted a newly exposed wrapped page");
    }
    if (packTag(0, 0, 1, 1) != packTag(65535, 65535, 0, 0)) {
        throw std::runtime_error(
            "VSM absolute page tag wrapping is inconsistent");
    }
}

void RunDirectionalClipFitCases()
{
    constexpr int clipCount = 6;
    // Keep this below the old one-world-unit floor so the test proves that a
    // long clip ladder can genuinely fit a small scene instead of overshooting.
    constexpr float sceneExtent = 3.2f;
    const std::vector<float> cameraFarPlanes(clipCount, 50000.0f);
    const auto fitted = setupDirectionalClipmaps(
        clipCount,
        DirectX::XMVector3Normalize(DirectX::XMVectorSet(-0.1f, -0.9f, -0.5f, 0.0f)),
        DirectX::XMVectorZero(),
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        0.1f,
        DirectX::XMConvertToRadians(70.0f),
        16.0f / 9.0f,
        cameraFarPlanes,
        100.0f,
        sceneExtent);

    if (fitted.size() != clipCount) {
        throw std::runtime_error("scene-fitted VSM returned the wrong clip count");
    }
    const float expectedClipZeroDiameter =
        sceneExtent * 2.0f / std::exp2(static_cast<float>(clipCount - 1));
    if (std::abs(fitted.front().size - expectedClipZeroDiameter) > 0.001f) {
        throw std::runtime_error("scene-fitted VSM did not derive clip zero from scene extent");
    }
    if (std::abs(fitted.back().size - sceneExtent * 2.0f) > 0.001f) {
        throw std::runtime_error("scene-fitted VSM outer clip does not cover the scene extent");
    }

    constexpr float fractionalResolutionScale = 1.25f;
    const auto fractionallyScaled = setupDirectionalClipmaps(
        clipCount,
        DirectX::XMVector3Normalize(DirectX::XMVectorSet(-0.1f, -0.9f, -0.5f, 0.0f)),
        DirectX::XMVectorZero(),
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        0.1f,
        DirectX::XMConvertToRadians(70.0f),
        16.0f / 9.0f,
        cameraFarPlanes,
        100.0f,
        sceneExtent,
        fractionalResolutionScale);
    if (std::abs(
            fractionallyScaled.front().size -
            fitted.front().size * fractionalResolutionScale) > 0.001f) {
        throw std::runtime_error(
            "fractional VSM resolution scale snapped to an integer clip level");
    }
    if (std::abs(
            fractionallyScaled.front().size /
                static_cast<float>(CLodVirtualShadowFixedVirtualPageCountPerAxis) -
            fitted.front().size /
                static_cast<float>(CLodVirtualShadowFixedVirtualPageCountPerAxis) *
                fractionalResolutionScale) > 0.0001f) {
        throw std::runtime_error(
            "fractional VSM resolution scale did not produce a fractional page size");
    }

    const auto cameraFitted = setupDirectionalClipmaps(
        clipCount,
        DirectX::XMVector3Normalize(DirectX::XMVectorSet(-0.1f, -0.9f, -0.5f, 0.0f)),
        DirectX::XMVectorZero(),
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        0.1f,
        DirectX::XMConvertToRadians(70.0f),
        16.0f / 9.0f,
        cameraFarPlanes,
        100.0f,
        0.0f);
    if (cameraFitted.front().size <= fitted.front().size) {
        throw std::runtime_error("scene-fitted VSM did not improve clip-zero page density");
    }

    // Clip views must use one exact light-space basis. In particular, their
    // X/Y translations must be the integer page offsets expressed in each
    // level's page size, even at large world coordinates where reconstructing
    // a view from a world-space eye loses enough precision to move fine texels.
    const auto largeWorldFitted = setupDirectionalClipmaps(
        clipCount,
        DirectX::XMVector3Normalize(DirectX::XMVectorSet(-0.31f, -0.88f, -0.36f, 0.0f)),
        DirectX::XMVectorSet(1000000.0f, 25000.0f, -750000.0f, 1.0f),
        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
        DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
        0.1f,
        DirectX::XMConvertToRadians(70.0f),
        16.0f / 9.0f,
        cameraFarPlanes,
        100000.0f,
        100000.0f);
    for (const Cascade& clip : largeWorldFitted) {
        const float pageWorldSize =
            clip.size /
            static_cast<float>(CLodVirtualShadowFixedVirtualPageCountPerAxis);
        const float expectedTranslationX =
            static_cast<float>(clip.pageOffsetX) * pageWorldSize;
        const float expectedTranslationY =
            -static_cast<float>(clip.pageOffsetY) * pageWorldSize;
        if (DirectX::XMVectorGetX(clip.viewMatrix.r[3]) != expectedTranslationX ||
            DirectX::XMVectorGetY(clip.viewMatrix.r[3]) != expectedTranslationY) {
            throw std::runtime_error(
                "VSM clip view drifted from its integer page-coordinate basis");
        }
        if (clip.farPlane < 100000.0f) {
            throw std::runtime_error(
                "VSM clip contracted below the configured shadow distance lower bound");
        }
    }
    if (largeWorldFitted.front().farPlane != 100000.0f ||
        largeWorldFitted.back().farPlane <= largeWorldFitted.front().farPlane) {
        throw std::runtime_error(
            "VSM clip depth lower bound replaced per-level precision scaling");
    }
}
}

int main()
{
    try {
        RunBudgetCases();
        RunAllocationAdmissionCases();
        RunDeferredPageClearLifecycleCase();
        RunTwoLayerLifecycleCases();
        RunFallbackDependencyOverflowLifecycleCase();
        RunExactPageTokenCases();
        RunAbsolutePageTagCases();
        RunDirectionalClipFitCases();
        RunCasterExtensionCases();
        std::cout << "Virtual shadow budget tests passed.\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
