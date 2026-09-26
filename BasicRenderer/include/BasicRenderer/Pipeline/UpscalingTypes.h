#pragma once

enum class UpscalingMode {
    None,
    FSR3,
    DLSS
};

static constexpr const char* UpscalingModeNames[] = {
    "None",
    "FSR3",
    "DLSS",
};
static constexpr int UpscalingModeCount = sizeof(UpscalingModeNames) / sizeof(UpscalingModeNames[0]);

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

