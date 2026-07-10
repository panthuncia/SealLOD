#pragma once

#include "ProceduralWind/WindTypes.h"

#include <array>
#include <filesystem>

namespace br::wind {

struct WindSimulationGroupProfile {
    std::uint32_t id = 0u;
    bool isTrunk = false;
    float influence = 1.0f;
    float meanBendRadians = 0.0f;
    float parallelAmplitudeRadians = 0.0f;
    float perpendicularRatio = 0.25f;
    float torsionRatio = 0.05f;
    float frequencyScale = 1.0f;
    float maximumAngleRadians = 0.2f;
    float boundsScale = 1.2f;
};

struct WindProfileSet {
    std::uint32_t schemaVersion = 1u;
    std::array<float, 3> harmonicFrequenciesHz{ 0.10f, 0.37f, 1.31f };
    std::array<float, 3> harmonicWeights{ 1.0f, 0.45f, 0.20f };
    std::vector<WindSimulationGroupProfile> groups;
};

WindProfileSet MakeDefaultWindProfile();
bool LoadWindProfile(const std::filesystem::path& path, WindProfileSet& profile, std::string* error = nullptr);

} // namespace br::wind
