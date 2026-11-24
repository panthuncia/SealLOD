#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/ResourceComponent.h"
#include "Runtime/GraphIntegration/FrameBuild.h"
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
flecs::entity EnsureRayTracingTag(flecs::world& ecsWorld);

const CLodVariantTraits& GetVariantTraits(CLodExtensionType type);

// Publishes a CLod resource in the ECS directory that diagnostics query: Components::Resource, the variant's type tag
// and Tags. `extra` writes more components to the same entity. Inside a frame build this is recorded and made when the
// build is published (br::render::WriteResourceEntity); a resource published earlier under the same name leaves the
// directory then.
template<typename... Tags>
void PublishCLodResource(const CLodVariantTraits& traits, const std::shared_ptr<org::Resource>& resource,
    std::function<void(flecs::entity)> extra = {})
{
    if (!resource) {
        return;
    }
    std::weak_ptr<org::Resource> weak = resource;
    br::render::WriteResourceEntity(
        resource,
        [weak, ensureTypeEntity = traits.ensureTypeEntity, extra = std::move(extra)](flecs::world& world, flecs::entity entity) {
            entity.set<Components::Resource>({ weak });
            if (extra) {
                extra(entity);
            }
            (entity.add<Tags>(), ...);
            entity.add<CLodExtensionTypeTag>(ensureTypeEntity(world));
        },
        resource->GetName());
}

// Removes a CLod resource from the ECS directory (recorded like PublishCLodResource).
inline void UnpublishCLodResource(const std::shared_ptr<org::Resource>& resource)
{
    br::render::WriteResourceEntity(resource, [](flecs::world&, flecs::entity entity) {
        if (entity.has<Components::Resource>()) {
            entity.remove<Components::Resource>();
        }
    });
}
std::string MakeVariantPassName(const CLodVariantTraits& traits, std::string_view suffix);
std::string MakeVariantResourceName(const CLodVariantTraits& traits, std::string_view suffix);
std::string GetVariantTechniquePath(const CLodVariantTraits& traits, std::string_view passName);

org::TextureDescription CreateVirtualShadowPageTableDescription();
org::TextureDescription CreateVirtualShadowPhysicalPagesDescription(uint32_t backingResolution, uint32_t maxPhysicalPages);
org::TextureDescription CreateVirtualShadowDirtyHierarchyDescription();