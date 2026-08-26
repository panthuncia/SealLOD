#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <vector>

#include "Render/AsyncStateGraph.h"
#include "Materials/TechniqueDescriptor.h"
#include "ShaderBuffers.h"


namespace br::render {

struct PublishedGpuBufferVersion;
struct PublishedTextureBinding;

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
    std::uint32_t slotsUsed = 0;
    std::vector<MaterialCompileFlagEntryDTO> activeCompileFlags;
	std::vector<MaterialTextureBindingDependencyDTO> textureBindings;
	// Immutable, already-usable coarse bindings captured from the streaming
	// owner. These are lifetime holds, not mip-residency graph prerequisites.
	std::vector<std::shared_ptr<const PublishedTextureBinding>> preparedTextureBindings;
    ArtifactKey baseTableKey{ ArtifactKind::BufferVersion, 0, kMaterialBaseTableVariant };
    ArtifactKey evalTableKey{ ArtifactKind::BufferVersion, 0, kMaterialEvalTableVariant };
    ArtifactKey openPbrTableKey{ ArtifactKind::BufferVersion, 0, kMaterialOpenPbrTableVariant };
};

struct PublishedMaterialState {
    std::uint64_t sourceFingerprint = 0;
    std::uint32_t compileFlagSlotsUsed = 0;
    std::vector<MaterialCompileFlags> activeCompileFlags;
    std::vector<std::uint32_t> activeCompileFlagSlots;
    std::shared_ptr<const PublishedGpuBufferVersion> baseTable;
    std::shared_ptr<const PublishedGpuBufferVersion> evalTable;
    std::shared_ptr<const PublishedGpuBufferVersion> openPbrTable;
    std::vector<std::shared_ptr<const PublishedTextureBinding>> textureBindings;
};

void RegisterMaterialStateProducer(AsyncStateGraph& graph);

} // namespace br::render
