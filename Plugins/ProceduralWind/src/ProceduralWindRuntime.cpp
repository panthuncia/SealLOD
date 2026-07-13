#include "ProceduralWind/ProceduralWindRuntime.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>

namespace br::wind {

float ProceduralWindRuntime::DirectionAngle(const Float3& direction)
{
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || (direction.x == 0.0f && direction.y == 0.0f)) return 0.0f;
    return std::atan2(direction.y, direction.x);
}

bool ProceduralWindRuntime::SetWorldCache(std::uint32_t worldId, std::filesystem::path cachePath, std::string* error)
{
    WindCacheMetadata metadata;
    std::lock_guard lock(m_mutex);
    // A failed world transition must never leave the previous world's field active.
    // Publish the uniform prevailing fallback until a complete pair is available.
    m_worldId = worldId;
    m_cachePath.clear();
    m_metadata = {};
    m_resident = {};
    ++m_revision;
    if (!LoadWindCacheMetadata(cachePath, metadata, error)) return false;
    m_worldId = worldId; m_cachePath = std::move(cachePath); m_metadata = std::move(metadata);
    return LoadRequiredPairLocked(error);
}

bool ProceduralWindRuntime::SetWindState(Float3 directionToWS, float strength, float gustStrength, std::string* error)
{
    std::lock_guard lock(m_mutex);
    const float length = std::hypot(directionToWS.x, directionToWS.y);
    m_state.directionToWS = length > 1.0e-6f ? Float3{ directionToWS.x / length, directionToWS.y / length, 0.0f } : Float3{ 1.0f, 0.0f, 0.0f };
    m_state.strength = std::clamp(strength, 0.0f, 1.0f); m_state.gustStrength = std::clamp(gustStrength, 0.0f, 1.0f);
    return m_metadata.directions.empty() || LoadRequiredPairLocked(error);
}

bool ProceduralWindRuntime::LoadRequiredPairLocked(std::string* error)
{
    const auto bracket = ComputeDirectionBracket(DirectionAngle(m_state.directionToWS), static_cast<std::uint32_t>(m_metadata.directions.size()));
    if (m_resident.valid && m_resident.bracket.lower == bracket.lower && m_resident.bracket.upper == bracket.upper) {
        m_resident.bracket.interpolation = bracket.interpolation; m_resident.state = m_state; m_resident.revision = ++m_revision; return true;
    }
    // Load both CPU blocks before publishing, so consumers never observe a mixed pair.
    std::array<std::vector<std::uint16_t>, 2> next;
    if (!LoadWindDirection(m_cachePath, m_metadata, bracket.lower, next[0], error) || !LoadWindDirection(m_cachePath, m_metadata, bracket.upper, next[1], error)) return false;
    m_resident.metadata = m_metadata; m_resident.bracket = bracket; m_resident.rgba16f = std::move(next); m_resident.state = m_state;
    m_resident.valid = true; m_resident.revision = ++m_revision; return true;
}

void ProceduralWindRuntime::SetProfileSearchRoots(std::vector<std::filesystem::path> roots) { std::lock_guard lock(m_mutex); m_profileRoots = std::move(roots); m_profiles.clear(); ++m_profileRevision; }
void ProceduralWindRuntime::ReloadProfiles() { std::lock_guard lock(m_mutex); m_profiles.clear(); ++m_profileRevision; }
std::uint64_t ProceduralWindRuntime::ProfileRevision() const { std::lock_guard lock(m_mutex); return m_profileRevision; }

WindProfileSet ProceduralWindRuntime::ResolveProfile(std::string_view modelIdentity) const
{
    std::lock_guard lock(m_mutex); const std::string key(modelIdentity);
    if (const auto it = m_profiles.find(key); it != m_profiles.end()) return it->second;
    WindProfileSet profile = MakeDefaultWindProfile();
    const std::filesystem::path modelPath(key); const auto sidecar = modelPath.stem().string() + ".wind.json";
    for (const auto& root : m_profileRoots) { WindProfileSet candidate; if (LoadWindProfile(root / sidecar, candidate)) { profile = std::move(candidate); break; } }
    m_profiles.emplace(key, profile); return profile;
}

ResidentWindPair ProceduralWindRuntime::SnapshotResidentPair() const { std::lock_guard lock(m_mutex); return m_resident; }
WindState ProceduralWindRuntime::SnapshotWindState() const { std::lock_guard lock(m_mutex); return m_state; }
std::uint32_t ProceduralWindRuntime::WorldId() const { std::lock_guard lock(m_mutex); return m_worldId; }

Float3 ProceduralWindRuntime::SampleWindSkyrim(float x, float y) const
{
    std::lock_guard lock(m_mutex); const Float3 fallback{ m_state.directionToWS.x * m_state.strength, m_state.directionToWS.y * m_state.strength, 0.0f };
    if (!m_resident.valid) return fallback;
    const float gx = (x - m_metadata.origin.x) / m_metadata.cellSize - 0.5f, gy = (y - m_metadata.origin.y) / m_metadata.cellSize - 0.5f;
    if (gx < 0.0f || gy < 0.0f || gx >= m_metadata.width - 1.0f || gy >= m_metadata.height - 1.0f) return fallback;
    const auto x0 = static_cast<std::uint32_t>(gx), y0 = static_cast<std::uint32_t>(gy); const float tx = gx - x0, ty = gy - y0;
    const auto sampleSlice = [&](const std::vector<std::uint16_t>& slice) {
        Float3 sum{}; float coverage = 0.0f;
        for (std::uint32_t oy = 0; oy < 2u; ++oy) for (std::uint32_t ox = 0; ox < 2u; ++ox) {
            const float weight = (ox ? tx : 1.0f - tx) * (oy ? ty : 1.0f - ty); const auto i = (static_cast<std::size_t>(y0 + oy) * m_metadata.width + x0 + ox) * 4u;
            const float c = DirectX::PackedVector::XMConvertHalfToFloat(slice[i + 3u]); coverage += weight * c;
            sum.x += weight * c * DirectX::PackedVector::XMConvertHalfToFloat(slice[i]); sum.y += weight * c * DirectX::PackedVector::XMConvertHalfToFloat(slice[i + 1u]); sum.z += weight * c * DirectX::PackedVector::XMConvertHalfToFloat(slice[i + 2u]);
        }
        return std::pair{ sum, coverage };
    };
    const auto a = sampleSlice(m_resident.rgba16f[0]), b = sampleSlice(m_resident.rgba16f[1]);
    if (std::min(a.second, b.second) < 0.5f) return fallback;
    const float t = m_resident.bracket.interpolation;
    return { ((1 - t) * a.first.x + t * b.first.x) * m_state.strength, ((1 - t) * a.first.y + t * b.first.y) * m_state.strength, ((1 - t) * a.first.z + t * b.first.z) * m_state.strength };
}

std::shared_ptr<ProceduralWindRuntime> GetProceduralWindRuntime()
{
    static auto runtime = std::make_shared<ProceduralWindRuntime>();
    return runtime;
}

} // namespace br::wind
