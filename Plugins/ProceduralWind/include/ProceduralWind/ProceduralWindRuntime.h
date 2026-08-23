#pragma once

#include "ProceduralWind/WindOffline.h"
#include "ProceduralWind/WindProfile.h"

#include <mutex>
#include <optional>
#include <unordered_map>

namespace br::wind {

struct ResidentWindPair {
    WindCacheMetadata metadata;
    WindDirectionBracket bracket;
    std::array<std::vector<std::uint16_t>, 2> rgba16f;
    WindState state;
    std::uint64_t revision = 0u;
    bool valid = false;
};

class ProceduralWindRuntime {
public:
    bool SetWorldCache(std::uint32_t worldId, std::filesystem::path cachePath, std::string* error = nullptr);
    bool SetWindState(Float3 directionToWS, float strength, float gustStrength, std::string* error = nullptr);
    void SetProfileSearchRoots(std::vector<std::filesystem::path> roots);
    void ReloadProfiles();
    WindProfileSet ResolveProfile(std::string_view modelIdentity) const;
    std::uint64_t ProfileRevision() const;
    ResidentWindPair SnapshotResidentPair() const;
    WindState SnapshotWindState() const;
    Float3 SampleWindSkyrim(float x, float y) const;
    std::uint32_t WorldId() const;

private:
    bool LoadRequiredPairLocked(std::string* error);
    static float DirectionAngle(const Float3& direction);

    mutable std::mutex m_mutex;
    std::uint32_t m_worldId = 0u;
    std::filesystem::path m_cachePath;
    WindCacheMetadata m_metadata;
    ResidentWindPair m_resident;
    WindState m_state;
    std::vector<std::filesystem::path> m_profileRoots;
    mutable std::unordered_map<std::string, WindProfileSet> m_profiles;
    std::uint64_t m_revision = 0u;
    std::uint64_t m_profileRevision = 1u;
};

std::shared_ptr<ProceduralWindRuntime> GetProceduralWindRuntime();

} // namespace br::wind
