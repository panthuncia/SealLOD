#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BuiltinRenderPasses.h"
#include "Resources/PixelBuffer.h"

struct CLodVariantTraits {
    enum class ScheduleMode : uint8_t {
        TwoPassVisibility,
        SinglePassCullOnly,
        SinglePassDeepVisibility
    };

    CLodExtensionType type;
    flecs::entity (*ensureTypeEntity)(flecs::world&);
    std::string_view passPrefix;
    std::string_view resourcePrefix;
    std::string_view renderPhaseName;
    CLodRasterOutputKind rasterOutputKind;
    ScheduleMode scheduleMode = ScheduleMode::TwoPassVisibility;
    bool ownsStreaming = false;
    bool usesPhase2OcclusionReplay = false;
    bool schedulesPerViewDepthCopy = false;
    bool schedulesStructuralPasses = false;
};

flecs::entity EnsureVisibilityBufferTag(flecs::world& ecsWorld);
flecs::entity EnsureAlphaBlendTag(flecs::world& ecsWorld);
flecs::entity EnsureShadowTag(flecs::world& ecsWorld);

const CLodVariantTraits& GetVariantTraits(CLodExtensionType type);
std::string MakeVariantPassName(const CLodVariantTraits& traits, std::string_view suffix);
std::string MakeVariantResourceName(const CLodVariantTraits& traits, std::string_view suffix);
std::string GetVariantTechniquePath(const CLodVariantTraits& traits, std::string_view passName);

org::TextureDescription CreateVirtualShadowPageTableDescription();
org::TextureDescription CreateVirtualShadowPhysicalPagesDescription(uint32_t backingResolution, uint32_t maxPhysicalPages);
org::TextureDescription CreateVirtualShadowDirtyHierarchyDescription();