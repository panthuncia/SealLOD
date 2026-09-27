#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <BasicRenderer/Extensions/ShaderBuffers.h>
#include <BasicRenderer/Pipeline/DrawWorkload.h>
#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"

namespace org { class Resource; }
class Material;
class TextureAsset;

namespace br::render {

struct MaterialTextureBindingDependencyDTO {
    std::uint32_t streamingTextureID = 0;
    std::uint64_t bindingRevision = 0;
    std::uint32_t imageDescriptorIndex = 0;
    std::uint32_t samplerDescriptorIndex = 0;
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

struct MaterialUsageCapture {
    MaterialUsageBatchEntry entry;
    std::vector<std::shared_ptr<TextureAsset>> textureServiceInputs;
};

void AppendTextureDisplayRequirements(
    const Material& material, std::vector<ArtifactRequirement>& requirements);
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
} // namespace br::render
