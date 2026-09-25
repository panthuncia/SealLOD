#pragma once

#include <cstdint>

#include "Scene/Components.h"
#include "ShaderBuffers.h"

struct ViewFlags;
struct ViewCreationParams;

namespace br::render {

// Stable-identity and storage boundary used while scene ingestion materializes
// shadow-camera descriptions.  Frame work consumes the resulting immutable
// ViewFamily publication and never calls this service.
class IShadowViewService {
public:
    virtual ~IShadowViewService() = default;

    virtual std::uint64_t CreateShadowView(
        const CameraInfo& camera,
        const ViewFlags& flags,
        const ViewCreationParams& params) = 0;
    virtual void UpdateShadowView(std::uint64_t viewID, const CameraInfo& camera) = 0;
    virtual void DestroyShadowView(std::uint64_t viewID) = 0;
    virtual std::uint32_t ShadowViewCameraBufferIndex(std::uint64_t viewID) const = 0;
};

} // namespace br::render
