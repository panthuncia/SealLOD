#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

#include "BasicRenderer/Scene/Components.h"
#include <DirectXMath.h>

namespace org { class Resource; }

namespace br::render {
struct PublishedGpuBufferVersion;
inline constexpr std::uint64_t LightInfoTableVariant = 1;
inline constexpr std::uint64_t LightSpotViewTableVariant = 2;
inline constexpr std::uint64_t LightPointViewTableVariant = 3;
inline constexpr std::uint64_t LightDirectionalViewTableVariant = 4;
inline constexpr std::uint64_t LightActiveIndexTableVariant = 5;

struct PublishedDirectionalShadowLight {
    DirectX::XMFLOAT3 direction{};
    std::vector<std::uint64_t> viewIDs;
    std::vector<std::int64_t> unwrappedPageOffsetX;
    std::vector<std::int64_t> unwrappedPageOffsetY;
};

struct LightTableBuildInput {
    std::uint64_t revision = 0;
    std::uint32_t lightCount = 0;
    std::uint32_t lightPagePoolSize = 0;
    std::vector<PublishedDirectionalShadowLight> directionalShadows;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
    std::vector<std::shared_ptr<const std::vector<std::byte>>> tableImages;
};

struct PublishedLightTableState {
    std::uint64_t revision = 0;
    std::uint32_t lightCount = 0;
    std::uint32_t lightPagePoolSize = 0;
    std::vector<PublishedDirectionalShadowLight> directionalShadows;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
    std::vector<std::shared_ptr<const std::vector<std::byte>>> tableImages;
    std::vector<std::shared_ptr<const PublishedGpuBufferVersion>> tableVersions;
};


} // namespace br::render
