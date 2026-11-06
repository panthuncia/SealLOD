#pragma once

namespace org { class PixelBuffer; }

enum class UpscalingMode {
    None,
    FSR3,
    DLSS,
    DLSSRayReconstruction
};

static constexpr const char* UpscalingModeNames[] = {
    "None",
    "FSR3",
    "DLSS",
    "DLSS Ray Reconstruction",
};
// Modes the upscaling combo always offers. Ray Reconstruction is excluded: the
// menu offers it separately, and only when the adapter supports it.
static constexpr int UpscalingModeCount = static_cast<int>(UpscalingMode::DLSSRayReconstruction);

// The guides are read only by DLSSRayReconstruction and are null in other modes.
struct UpscalingInputs {
    org::PixelBuffer* color = nullptr;
    org::PixelBuffer* output = nullptr;
    org::PixelBuffer* depth = nullptr;
    org::PixelBuffer* motionVectors = nullptr;
    org::PixelBuffer* diffuseAlbedo = nullptr;
    org::PixelBuffer* specularAlbedo = nullptr;
    org::PixelBuffer* normalRoughness = nullptr;
    org::PixelBuffer* specularHitDistance = nullptr;
};

enum class UpscaleQualityMode {
    DLAA,
	//UltraQuality, // DLSS UltraQuality returns a resolution of 0? What is this?
    Quality,
    Balanced,
    Performance,
    UltraPerformance
};

static constexpr const char* UpscaleQualityModeNames[] = {
    "DLAA",
    //"UltraQuality",
    "Quality",
    "Balanced",
    "Performance",
    "UltraPerformance"
};
static constexpr int UpscaleQualityModeCount = sizeof(UpscaleQualityModeNames) / sizeof(UpscaleQualityModeNames[0]);

