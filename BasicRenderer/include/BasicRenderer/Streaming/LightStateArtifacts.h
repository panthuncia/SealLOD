#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

#include "BasicRenderer/Scene/Components.h"
#include <DirectXMath.h>

namespace org { class Resource; }

namespace org::services { struct PublishedBufferVersion; }
namespace br::render {
using PublishedGpuBufferVersion = org::services::PublishedBufferVersion;
// The published light tables, in LightManager::CaptureTableImages order. 2 and 3 were the retired spot and point light
// shadow view tables.
inline constexpr std::uint64_t LightInfoTableVariant = 1;
inline constexpr std::uint64_t LightDirectionalViewTableVariant = 4;
inline constexpr std::uint64_t LightActiveIndexTableVariant = 5;
inline constexpr std::size_t LightTableFamilyCount = 3;

struct PublishedDirectionalShadowLight {
    DirectX::XMFLOAT3 direction{};
    std::vector<std::uint64_t> viewIDs;
    // Clip page offsets travel with each view (PreparedViewFrameData), never here:
    // this table publishes asynchronously and would trail the view matrices.
};

// A spot or point light with virtual shadow map faces. viewIDs[i] renders face firstFace + i, whose slices start at
// CLodVirtualShadowFirstLocalSlice + (firstFace + i) * CLodVirtualShadowLocalSlicesPerFace.
struct PublishedLocalShadowLight {
    std::uint32_t lightIndex = 0;
    std::uint32_t firstFace = 0;
    std::vector<std::uint64_t> viewIDs;
};

struct LightTableBuildInput {
    std::uint64_t revision = 0;
    std::uint32_t lightCount = 0;
    std::uint32_t lightPagePoolSize = 0;
    std::vector<PublishedDirectionalShadowLight> directionalShadows;
    std::vector<PublishedLocalShadowLight> localShadows;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
    std::vector<std::shared_ptr<const std::vector<std::byte>>> tableImages;
};

struct PublishedLightTableState {
    std::uint64_t revision = 0;
    std::uint32_t lightCount = 0;
    std::uint32_t lightPagePoolSize = 0;
    std::vector<PublishedDirectionalShadowLight> directionalShadows;
    std::vector<PublishedLocalShadowLight> localShadows;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
    std::vector<std::shared_ptr<const std::vector<std::byte>>> tableImages;
    std::vector<std::shared_ptr<const PublishedGpuBufferVersion>> tableVersions;
};


} // namespace br::render
