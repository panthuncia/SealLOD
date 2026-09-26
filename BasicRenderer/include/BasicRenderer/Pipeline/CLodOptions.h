#pragma once

#include <cstdint>

// ClusterLOD modes and setting identifiers shared with scene producers.

inline constexpr const char* CLodVSMRasterModeSettingName = "clodVsmRasterMode";
inline constexpr const char* CLodTransparencyModeSettingName = "clodTransparencyMode";

enum class CLodPriorityMode : uint8_t {
    Max, // Duplicate group requests keep the maximum reported priority
    Sum, // Duplicate group requests accumulate (sum) their priorities
};

enum class CLodSoftwareRasterMode : uint8_t {
    Disabled,
    Compute,
    WorkGraph,
};

enum class CLodCullingBackend : uint8_t {
    WorkGraph,
    PureCompute,
};

enum class CLodVSMRasterMode : uint8_t {
    HardwareOnly,
    Standard,
    PageJob,
    Reyes,
};

enum class CLodLodHeightMode : uint8_t {
    RenderHeight,
    OutputHeight,
};

inline constexpr const char* CLodLodHeightModeNames[] = {
    "Render Height",
    "Output Height",
};
inline constexpr int CLodLodHeightModeCount =
    static_cast<int>(sizeof(CLodLodHeightModeNames) / sizeof(CLodLodHeightModeNames[0]));

enum class CLodRasterOutputKind : uint8_t {
    VisibilityBuffer,
    VirtualShadow,
    DeepVisibility,
    AVBOITOccupancy,
    AVBOIT,
    AVBOITShading,
};

enum class CLodTransparencyMode : uint8_t {
    LinkedListDeepVisibility,
    AVBOIT,
    Disabled,
};

inline constexpr const char* CLodSoftwareRasterModeSettingName = "clodSoftwareRasterMode";
inline constexpr const char* CLodCullingBackendSettingName = "clodCullingBackend";
inline constexpr const char* CLodSoftwareRasterModeNames[] = {
    "Disabled",
    "Compute",
    "Work Graph",
};
inline constexpr int CLodSoftwareRasterModeCount = static_cast<int>(sizeof(CLodSoftwareRasterModeNames) / sizeof(CLodSoftwareRasterModeNames[0]));
inline constexpr const char* CLodCullingBackendNames[] = {
    "Work Graph",
    "Pure Compute",
};
inline constexpr int CLodCullingBackendCount = static_cast<int>(sizeof(CLodCullingBackendNames) / sizeof(CLodCullingBackendNames[0]));
inline constexpr const char* CLodVSMRasterModeNames[] = {
    "Hardware Only",
    "Standard",
    "Page-Job",
    "Reyes",
};
inline constexpr int CLodVSMRasterModeCount = static_cast<int>(sizeof(CLodVSMRasterModeNames) / sizeof(CLodVSMRasterModeNames[0]));
inline constexpr const char* CLodTransparencyModeNames[] = {
    "Linked-List Deep Visibility",
    "AVBOIT",
    "Disabled",
};
inline constexpr int CLodTransparencyModeCount = static_cast<int>(sizeof(CLodTransparencyModeNames) / sizeof(CLodTransparencyModeNames[0]));

constexpr bool CLodSoftwareRasterEnabled(CLodSoftwareRasterMode mode)
{
    return mode != CLodSoftwareRasterMode::Disabled;
}

constexpr bool CLodSoftwareRasterUsesCompute(CLodSoftwareRasterMode mode)
{
    return mode == CLodSoftwareRasterMode::Compute;
}

constexpr bool CLodSoftwareRasterUsesWorkGraph(CLodSoftwareRasterMode mode)
{
    return mode == CLodSoftwareRasterMode::WorkGraph;
}

constexpr bool CLodVSMRasterModeUsesLegacyRasterOnly(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::HardwareOnly;
}

constexpr bool CLodVSMRasterModeUsesLargeClusterPageJob(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::PageJob;
}

constexpr bool CLodVSMRasterModeUsesLargeClusterShadowRouting(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::PageJob || mode == CLodVSMRasterMode::Reyes;
}

constexpr bool CLodVSMRasterModeUsesReyes(CLodVSMRasterMode mode)
{
    return mode == CLodVSMRasterMode::Reyes;
}

