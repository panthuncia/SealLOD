#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Streaming/MaterialRequests.h>
#include "Runtime/StateGraph/AsyncStateGraph.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

class MaterialManager;
namespace org { class Resource; }

namespace br::render {

struct PublishedGpuBufferVersion;
struct MaterialRowArtifact {
    std::uint32_t materialID = 0, materialSlot = 0; std::uint64_t sourceRevision = 0;
    PerMaterialCB base{}; PerMaterialEvalCB evaluation{}; PerMaterialOpenPBRCB openPbr{};
};

inline constexpr std::uint64_t kMaterialBaseTableVariant = 1;
inline constexpr std::uint64_t kMaterialEvalTableVariant = 2;
inline constexpr std::uint64_t kMaterialOpenPbrTableVariant = 3;

struct MaterialCompileFlagEntryDTO {
    MaterialCompileFlags flags{};
    std::uint32_t slot = 0;
    auto operator<=>(const MaterialCompileFlagEntryDTO&) const = default;
};


struct MaterialStateBuildInput {
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t materialRowsRevision = 0;
    std::uint64_t materialRowCount = 0;
    std::uint32_t slotsUsed = 0;
    std::vector<MaterialCompileFlagEntryDTO> activeCompileFlags;
    // Dense shader-visible raster-bucket table. Index is the stable bucket ID;
    // unused/released entries retain MaterialRasterFlagsNone until a successor
    // publication assigns them again.
    std::vector<MaterialRasterFlags> rasterBucketFlags;
    ArtifactKey baseTableKey{ ArtifactKind::BufferVersion, 0, kMaterialBaseTableVariant };
    ArtifactKey evalTableKey{ ArtifactKind::BufferVersion, 0, kMaterialEvalTableVariant };
    ArtifactKey openPbrTableKey{ ArtifactKind::BufferVersion, 0, kMaterialOpenPbrTableVariant };
};

struct PublishedMaterialState {
    std::uint64_t sourceFingerprint = 0;
    std::uint32_t compileFlagSlotsUsed = 0;
    std::vector<MaterialCompileFlags> activeCompileFlags;
    std::vector<std::uint32_t> activeCompileFlagSlots;
    std::vector<MaterialRasterFlags> rasterBucketFlags;
    std::shared_ptr<const PublishedGpuBufferVersion> baseTable;
    std::shared_ptr<const PublishedGpuBufferVersion> evalTable;
    std::shared_ptr<const PublishedGpuBufferVersion> openPbrTable;

    [[nodiscard]] bool TryGetCompileFlagsSlot(
        MaterialCompileFlags flags, std::uint32_t& slot) const noexcept {
        for (std::size_t index = 0; index < activeCompileFlags.size(); ++index) {
            if (activeCompileFlags[index] == flags) {
                if (index >= activeCompileFlagSlots.size()) return false;
                slot = activeCompileFlagSlots[index];
                return true;
            }
        }
        return false;
    }
};


class MaterialRowReservation {
public:
    using ResolveFn = std::function<bool(bool commit)>;
    MaterialRowReservation(std::shared_ptr<const MaterialRowArtifact> row, ResolveFn resolve)
        : m_row(std::move(row)), m_resolve(std::move(resolve)) {}
    ~MaterialRowReservation() { (void)Resolve(false); }
    MaterialRowReservation(const MaterialRowReservation&) = delete;
    MaterialRowReservation& operator=(const MaterialRowReservation&) = delete;
    [[nodiscard]] bool Commit() const { return Resolve(true); }
    [[nodiscard]] const std::shared_ptr<const MaterialRowArtifact>& Row() const noexcept {
        return m_row;
    }

private:
    bool Resolve(bool commit) const {
        std::scoped_lock lock(m_resolveMutex);
        if (m_resolved) return m_committed;
        m_resolved = true;
        m_committed = m_resolve ? m_resolve(commit) : !commit;
        return m_committed;
    }
    std::shared_ptr<const MaterialRowArtifact> m_row;
    ResolveFn m_resolve;
    mutable std::mutex m_resolveMutex;
    mutable bool m_resolved = false;
    mutable bool m_committed = false;
};

struct MaterialRowInput {
    std::uint32_t materialID = 0, materialSlot = 0; std::uint64_t sourceRevision = 0;
    PerMaterialCB base{}; PerMaterialEvalCB evaluation{}; PerMaterialOpenPBRCB openPbr{};
    std::shared_ptr<const MaterialRowReservation> reservation;
};


void RegisterMaterialStateProducer(AsyncStateGraph& graph);
// Reserved for the generation-safe Latest-successor cutover. Not registered by
// the renderer until internal rebuild generations stop consuming source revisions.
void RegisterMaterialRowProducer(AsyncStateGraph& graph);
void RegisterMaterialUsageBatchProducer(AsyncStateGraph& graph);

} // namespace br::render
