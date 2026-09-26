#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <memory>
#include <vector>

#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

namespace org { class PixelBuffer; class Resource; }

namespace br::render {
struct PublishedGpuBufferVersion;
struct PrimaryCameraFrameUpload {
    CameraInfo camera{};
    CullingCameraInfo cullingCamera{};
    std::uint64_t viewID = 0;
    std::uint64_t revision = 0;
    std::uint64_t frameNumber = 0;
    std::uint32_t cameraBufferIndex = 0;
};

struct DepthHistoryDependency {
    mutable std::atomic<std::uint64_t> submissionID{0};
    mutable std::atomic_bool cancelled{false};
};

struct DepthHistorySelection {
    std::shared_ptr<org::PixelBuffer> resource;
    std::uint64_t epoch = 0;
    std::uint64_t producerSubmissionID = 0;
    std::uint64_t producerFrameNumber = 0;
    std::shared_ptr<const DepthHistoryDependency> dependency;

    explicit operator bool() const noexcept {
        return resource != nullptr && producerFrameNumber != 0
            && (!dependency || !dependency->cancelled.load(std::memory_order_acquire));
    }
};

struct PreparedViewFrameData {
    std::uint64_t id = 0;
    std::uint32_t cameraBufferIndex = 0;
    bool primary = false;
    bool shadow = false;
    bool cascade = false;
    Components::LightType lightType = Components::LightType::Directional;
    CameraInfo cameraInfo{};
    DirectX::XMFLOAT2 jitterPixelSpace{};
    DirectX::XMFLOAT2 jitterNDC{};
    std::shared_ptr<org::PixelBuffer> visibilityBuffer;
    std::shared_ptr<org::PixelBuffer> deepVisibilityHeadPointers;
    std::shared_ptr<org::PixelBuffer> linearDepthMap;
    DepthHistorySelection depthHistory;
    std::int32_t depthBufferArrayIndex = -1;
    // No descriptor indices: a resource's slots change whenever the render
    // graph gives it a new backing, so passes resolve them from the frame's
    // bindings during preparation (see CLodViewTables.h).
};

struct ViewFamilyBuildInput {
    std::uint64_t revision = 0;
    std::uint32_t cameraBufferSize = 0;
    std::uint64_t resourceLayoutRevision = 0;
    std::vector<PreparedViewFrameData> views;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
};

// The view set captured synchronously when a logical frame is accepted.  The
// primary camera is always element zero and is never selected through renderer
// state publication.
struct PreparedViewFamilyState {
    std::uint64_t revision = 0;
    std::uint32_t cameraBufferSize = 0;
    std::uint64_t resourceLayoutRevision = 0;
    std::vector<PreparedViewFrameData> views;
    std::vector<std::shared_ptr<org::Resource>> retainedResources;
};

struct PublishedViewFamilyState : PreparedViewFamilyState {
};


} // namespace br::render
