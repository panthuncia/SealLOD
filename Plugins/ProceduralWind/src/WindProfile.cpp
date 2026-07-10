#include "ProceduralWind/WindProfile.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace br::wind {

WindProfileSet MakeDefaultWindProfile()
{
    WindProfileSet result;
    result.groups = {
        { 0u, true, 1.00f, 0.035f, 0.018f, 0.20f, 0.05f, 0.50f, 0.12f, 1.15f },
        { 1u, false, 0.80f, 0.020f, 0.025f, 0.35f, 0.08f, 1.00f, 0.16f, 1.20f },
        { 2u, false, 0.55f, 0.008f, 0.035f, 0.50f, 0.12f, 2.00f, 0.22f, 1.25f },
        { 3u, false, 0.35f, 0.003f, 0.045f, 0.65f, 0.18f, 4.00f, 0.28f, 1.30f },
    };
    return result;
}

bool LoadWindProfile(const std::filesystem::path& path, WindProfileSet& profile, std::string* error)
{
    try {
        std::ifstream stream(path);
        if (!stream) {
            if (error) *error = "could not open profile";
            return false;
        }
        const auto json = nlohmann::json::parse(stream);
        if (json.value("schemaVersion", 0u) != 1u) {
            if (error) *error = "unsupported wind profile schema";
            return false;
        }
        WindProfileSet parsed = MakeDefaultWindProfile();
        if (const auto it = json.find("harmonicFrequenciesHz"); it != json.end()) it->get_to(parsed.harmonicFrequenciesHz);
        if (const auto it = json.find("harmonicWeights"); it != json.end()) it->get_to(parsed.harmonicWeights);
        if (const auto it = json.find("groups"); it != json.end()) {
            for (const auto& item : *it) {
                const auto id = item.at("id").get<std::uint32_t>();
                auto found = std::find_if(parsed.groups.begin(), parsed.groups.end(), [id](const auto& group) { return group.id == id; });
                if (found == parsed.groups.end()) {
                    parsed.groups.push_back({ .id = id });
                    found = std::prev(parsed.groups.end());
                }
                found->isTrunk = item.value("isTrunk", found->isTrunk);
                found->influence = item.value("influence", found->influence);
                found->meanBendRadians = item.value("meanBendRadians", found->meanBendRadians);
                found->parallelAmplitudeRadians = item.value("parallelAmplitudeRadians", found->parallelAmplitudeRadians);
                found->perpendicularRatio = item.value("perpendicularRatio", found->perpendicularRatio);
                found->torsionRatio = item.value("torsionRatio", found->torsionRatio);
                found->frequencyScale = item.value("frequencyScale", found->frequencyScale);
                found->maximumAngleRadians = item.value("maximumAngleRadians", found->maximumAngleRadians);
                found->boundsScale = item.value("boundsScale", found->boundsScale);
            }
        }
        profile = std::move(parsed);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace br::wind
