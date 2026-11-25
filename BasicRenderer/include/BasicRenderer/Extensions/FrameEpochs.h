#pragma once

#include <cstdint>
#include <string_view>

namespace br::render {

// The frame's epochs (docs/frame_graph_composition_plan.md, M4). Every pass of the frame graph belongs to one. The
// renderer's own passes take the epoch of the technique that builds them. An extension's passes take the extension's
// epoch, which the renderer gives it when it registers the extension. An extension pass that belongs to another epoch
// names it with ExternalPassDesc::Epoch and one of these values.
//
// The values are the frame graph host's epoch ids (the upload epoch is 0). Until the frame is split (M4 step 4) all four
// run in one host epoch, so they are labels only: the renderer maps them to the host's epochs.
inline constexpr uint32_t kVisibilityEpoch = 1;  // streaming, culling, raster, the RT scene, grass, material evaluation
inline constexpr uint32_t kShadingEpoch = 2;     // AO, lighting, reflections, deferred shading, CLod alpha, path tracing
inline constexpr uint32_t kPostEpoch = 3;        // exposure, upscaling, lens flares, bloom, tonemapping, depth history
inline constexpr uint32_t kOverlayEpoch = 4;     // debug views, the menu, presentation (the frame's last passes)
inline constexpr uint32_t kFrameEpochCount = 4;
inline constexpr uint32_t kFrameEpochs[kFrameEpochCount] = { kVisibilityEpoch, kShadingEpoch, kPostEpoch, kOverlayEpoch };
// No epoch: ORG's value for an untagged pass (persistent::AllEpochs).
inline constexpr uint32_t kNoFrameEpoch = UINT32_MAX;

constexpr bool IsFrameEpoch(uint32_t epoch) noexcept { return epoch >= kVisibilityEpoch && epoch <= kOverlayEpoch; }
// The epoch's index in kFrameEpochs.
constexpr uint32_t FrameEpochIndex(uint32_t epoch) noexcept { return epoch - kVisibilityEpoch; }

constexpr std::string_view FrameEpochName(uint32_t epoch) noexcept {
    switch (epoch) {
    case kVisibilityEpoch: return "Visibility";
    case kShadingEpoch: return "Shading";
    case kPostEpoch: return "Post";
    case kOverlayEpoch: return "Overlay";
    default: return "untagged";
    }
}

} // namespace br::render
