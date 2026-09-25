#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Render/AsyncStateGraph.h"
#include "Materials/TechniqueDescriptor.h"
#include "Render/RasterBucketFlags.h"
#include "ShaderBuffers.h"

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

struct MaterialTextureBindingDependencyDTO {
    std::uint32_t streamingTextureID = 0;
    std::uint64_t bindingRevision = 0;
    std::uint32_t imageDescriptorIndex = 0;
    std::uint32_t samplerDescriptorIndex = 0;
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

struct MaterialUsageBatchEntry {
    std::uint32_t materialID = 0;
    std::uint32_t count = 0;
    PerMaterialCB base{};
    PerMaterialEvalCB evaluation{};
    PerMaterialOpenPBRCB openPbr{};
    MaterialCompileFlags compileFlags{};
    std::vector<std::shared_ptr<org::Resource>> retainedTextureResources;
    std::vector<MaterialTextureBindingDependencyDTO> textureBindings;
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

struct PublishedMaterialUsageBatch;

class MaterialUsageReservation {
public:
    using ResolveFn = std::function<bool(bool commit)>;
    MaterialUsageReservation(
        std::shared_ptr<const PublishedMaterialUsageBatch> result,
        ResolveFn resolve)
        : m_result(std::move(result)), m_resolve(std::move(resolve)) {}
    ~MaterialUsageReservation() { (void)Resolve(false); }
    MaterialUsageReservation(const MaterialUsageReservation&) = delete;
    MaterialUsageReservation& operator=(const MaterialUsageReservation&) = delete;
    [[nodiscard]] bool Commit() const { return Resolve(true); }
    [[nodiscard]] const std::shared_ptr<const PublishedMaterialUsageBatch>& Result() const noexcept {
        return m_result;
    }

private:
    bool Resolve(bool commit) const {
        std::scoped_lock lock(m_resolveMutex);
        if (m_resolved) return m_committed;
        m_resolved = true;
        m_committed = m_resolve ? m_resolve(commit) : !commit;
        return m_committed;
    }
    std::shared_ptr<const PublishedMaterialUsageBatch> m_result;
    ResolveFn m_resolve;
    mutable std::mutex m_resolveMutex;
    mutable bool m_resolved = false;
    mutable bool m_committed = false;
};

struct MaterialUsageBatchBuildInput {
    std::uint64_t sourceFingerprint = 0;
    // Requests a refresh through the material storage's configured texture
    // service. Producer payloads never borrow the host's TextureFactory.
    bool refreshTextureBindings = false;
    std::vector<MaterialUsageBatchEntry> entries;
    std::shared_ptr<const MaterialUsageReservation> reservation;
};

struct PublishedMaterialUsageBatch {
    std::uint64_t sourceFingerprint = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> materialSlots;
};

void RegisterMaterialStateProducer(AsyncStateGraph& graph);
// Reserved for the generation-safe Latest-successor cutover. Not registered by
// the renderer until internal rebuild generations stop consuming source revisions.
void RegisterMaterialRowProducer(AsyncStateGraph& graph);
void RegisterMaterialUsageBatchProducer(AsyncStateGraph& graph);

} // namespace br::render
