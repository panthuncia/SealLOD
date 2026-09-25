#include "Render/GraphExtensions/CLodExtensionShared.h"

#include <algorithm>
#include <array>
#include <stdexcept>

flecs::entity EnsureVisibilityBufferTag(flecs::world& ecsWorld)
{
    if (!ecsWorld.component<CLodExtensionVisibilityBufferTag>().has(flecs::Exclusive)) {
        ecsWorld.component<CLodExtensionVisibilityBufferTag>().add(flecs::Exclusive);
        ecsWorld.add<CLodExtensionVisibilityBufferTag>();
    }
    return ecsWorld.entity<CLodExtensionVisibilityBufferTag>();
}

flecs::entity EnsureAlphaBlendTag(flecs::world& ecsWorld)
{
    if (!ecsWorld.component<CLodExtensionAlphaBlendTag>().has(flecs::Exclusive)) {
        ecsWorld.component<CLodExtensionAlphaBlendTag>().add(flecs::Exclusive);
        ecsWorld.add<CLodExtensionAlphaBlendTag>();
    }
    return ecsWorld.entity<CLodExtensionAlphaBlendTag>();
}

flecs::entity EnsureShadowTag(flecs::world& ecsWorld)
{
    if (!ecsWorld.component<CLodExtensionShadowTag>().has(flecs::Exclusive)) {
        ecsWorld.component<CLodExtensionShadowTag>().add(flecs::Exclusive);
        ecsWorld.add<CLodExtensionShadowTag>();
    }
    return ecsWorld.entity<CLodExtensionShadowTag>();
}

const CLodVariantTraits& GetVariantTraits(CLodExtensionType type)
{
    static const std::array<CLodVariantTraits, 3> kTraits = {{
        {
            CLodExtensionType::VisiblityBuffer,
            &EnsureVisibilityBufferTag,
            "CLodOpaque::",
            "CLod[Opaque] ",
            Engine::Primary::GBufferPass,
            CLodRasterOutputKind::VisibilityBuffer,
            CLodVariantTraits::ScheduleMode::TwoPassVisibility,
            true,
            true,
            true,
            true
        },
        {
            CLodExtensionType::AlphaBlend,
            &EnsureAlphaBlendTag,
            "CLodAlpha::",
            "CLod[Alpha] ",
            Engine::Primary::CLodTransparentPass,
            CLodRasterOutputKind::DeepVisibility,
            CLodVariantTraits::ScheduleMode::SinglePassDeepVisibility,
            false,
            false,
            false,
            true
        },
        {
            CLodExtensionType::Shadow,
            &EnsureShadowTag,
            "CLodShadow::",
            "CLod[Shadow] ",
            Engine::Primary::ShadowMapsPass,
            CLodRasterOutputKind::VirtualShadow,
            CLodVariantTraits::ScheduleMode::TwoPassVisibility,
            false,
            false,
            false,
            true
        },
    }};

    for (const auto& traits : kTraits) {
        if (traits.type == type) {
            return traits;
        }
    }

    throw std::runtime_error("Unknown CLod extension type.");
}

std::string MakeVariantPassName(const CLodVariantTraits& traits, std::string_view suffix)
{
    return std::string(traits.passPrefix) + std::string(suffix);
}

std::string MakeVariantResourceName(const CLodVariantTraits& traits, std::string_view suffix)
{
    return std::string(traits.resourcePrefix) + std::string(suffix);
}

std::string GetVariantTechniquePath(const CLodVariantTraits& traits, std::string_view passName)
{
    switch (traits.type) {
    case CLodExtensionType::VisiblityBuffer:
        if (passName.find("Reyes") != std::string_view::npos) {
            return "Primary Visibility::CLod::Reyes";
        }
        return "Primary Visibility::CLod";
    case CLodExtensionType::AlphaBlend:
        if (passName.find("Reyes") != std::string_view::npos) {
            return "Transparency::Deep Visibility::Reyes";
        }
        if (passName.find("TransparentVBOIT") != std::string_view::npos ||
            passName.find("TransparentExtinction") != std::string_view::npos) {
            return "Transparency::VBOIT";
        }
        if (passName.find("DeepVisibility") != std::string_view::npos ||
            passName.find("ClearDeepVisibility") != std::string_view::npos ||
            passName.find("RasterizeClustersPass") != std::string_view::npos) {
            return "Transparency::Deep Visibility";
        }
        return "Transparency::CLod";
    case CLodExtensionType::Shadow:
        if (passName.find("Reyes") != std::string_view::npos) {
            return "Shadows::Virtual Shadow Mapping::Reyes";
        }
        if (passName.find("PageJob") != std::string_view::npos ||
            passName.find("VirtualShadowBlock") != std::string_view::npos) {
            return "Shadows::Virtual Shadow Mapping::Page Job";
        }
        return "Shadows::Virtual Shadow Mapping";
    default:
        return {};
    }
}

org::TextureDescription CreateVirtualShadowPageTableDescription()
{
    org::TextureDescription desc;
    org::ImageDimensions dims;
    dims.width = CLodVirtualShadowMaxPageTableResolution;
    dims.height = CLodVirtualShadowMaxPageTableResolution;
    dims.rowPitch = static_cast<uint64_t>(dims.width) * sizeof(uint32_t);
    dims.slicePitch = dims.rowPitch * static_cast<uint64_t>(dims.height);
    desc.imageDimensions.push_back(dims);
    desc.channels = 1;
    desc.format = rhi::Format::R32_UInt;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R32_UInt;
    desc.hasUAV = true;
    desc.uavFormat = rhi::Format::R32_UInt;
    desc.isArray = true;
    desc.arraySize = CLodVirtualShadowMaxSupportedClipmapCount;
    return desc;
}

org::TextureDescription CreateVirtualShadowPhysicalPagesDescription(uint32_t backingResolution, uint32_t maxPhysicalPages)
{
    org::TextureDescription desc;
    org::ImageDimensions dims;
    const uint32_t sanitizedBackingResolution = CLodVirtualShadowSanitizeBackingResolution(backingResolution);
    const uint32_t atlasPagesWide = (std::max)(
        1u,
        CLodVirtualShadowPhysicalAtlasPagesWideFromPhysicalPageCount(maxPhysicalPages, sanitizedBackingResolution));
    const uint32_t atlasPagesHigh = (std::max)(
        1u,
        CLodVirtualShadowPhysicalAtlasPagesHighFromPhysicalPageCount(maxPhysicalPages, sanitizedBackingResolution));
    dims.width = atlasPagesWide * CLodVirtualShadowPhysicalPageSize;
    dims.height = atlasPagesHigh * CLodVirtualShadowPhysicalPageSize;
    dims.rowPitch = static_cast<uint64_t>(dims.width) * sizeof(uint32_t);
    dims.slicePitch = dims.rowPitch * static_cast<uint64_t>(dims.height);
    desc.imageDimensions.push_back(dims);
    desc.channels = 1;
    desc.format = rhi::Format::R32_UInt;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R32_UInt;
    desc.hasUAV = true;
    desc.uavFormat = rhi::Format::R32_UInt;
    return desc;
}

org::TextureDescription CreateVirtualShadowDirtyHierarchyDescription()
{
    org::TextureDescription desc;
    org::ImageDimensions dims;
    dims.width = CLodVirtualShadowMaxPageTableResolution;
    dims.height = CLodVirtualShadowMaxPageTableResolution;
    dims.rowPitch = static_cast<uint64_t>(dims.width) * sizeof(uint32_t);
    dims.slicePitch = dims.rowPitch * static_cast<uint64_t>(dims.height);
    desc.imageDimensions.push_back(dims);
    desc.channels = 1;
    desc.format = rhi::Format::R32_UInt;
    desc.hasSRV = true;
    desc.srvFormat = rhi::Format::R32_UInt;
    desc.hasUAV = true;
    desc.uavFormat = rhi::Format::R32_UInt;
    desc.isArray = true;
    desc.arraySize = CLodVirtualShadowMaxSupportedClipmapCount;
    desc.generateMipMaps = true;
    return desc;
}