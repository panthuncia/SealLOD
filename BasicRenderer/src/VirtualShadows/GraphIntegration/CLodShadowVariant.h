#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Interfaces/IResourceProvider.h"
#include "Render/RenderGraph/RenderGraphExtension.h"
namespace org { class RenderGraph; }

class CLodExtension;
struct CLodVariantTraits;
struct ProducerPersistentState;
namespace org { class ResourceGroup; }
struct VirtualShadowCasterBuildContext;

class CLodShadowVariant {
public:
    // Render thread, before a build that makes the shadow variant: the renderer's directional atlas (page table, physical
    // pages, upgrade upload slots) for the build's settings. Made once, and made again only when the backing resolution or
    // the page count changes (ProducerPersistentState::DirectionalVsmResources).
    static void EnsureDirectionalVsmAtlas(ProducerPersistentState& persistent);
    // The variant's sizes, from the build's settings. Once, when the instance is made.
    static void ConfigureSettings(CLodExtension& extension);
    static uint32_t GetVisibleClusterCapacity(const CLodExtension& extension);
    static std::string AppendStructuralPrelude(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<org::ExternalPassDesc>& outPasses);
    static void AppendStructuralTail(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<org::ExternalPassDesc>& outPasses,
        const std::string& shadowClearDirtyBitsAfterPassName,
        // The variant's Reyes queue reset runs each frame, so its overflow counters are this frame's.
        bool reyesQueuesResetEachFrame);
    static std::string AppendCasterRasterPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<org::ExternalPassDesc>& outPasses,
        const std::string& afterPassName);
    static std::string AppendPhase1FineRasterPass(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<org::ResourceGroup>& slabGroup,
        std::vector<org::ExternalPassDesc>& outPasses);
    static std::string AppendPhase2FineRasterPass(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<org::ResourceGroup>& slabGroup,
        std::vector<org::ExternalPassDesc>& outPasses);
    static void InitializeResources(CLodExtension& extension);
    static void TagResourceUsages(CLodExtension& extension);
    static std::shared_ptr<org::Resource> ProvideResource(CLodExtension& extension, org::ResourceIdentifier const& key);
    static std::vector<org::ResourceIdentifier> GetSupportedKeys(const CLodExtension& extension);

private:
    static uint32_t UpgradeUploadSlotCount();
    static VirtualShadowCasterBuildContext MakeCasterContext(CLodExtension& extension);
    static std::string AppendFineRasterPassForPhase(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<org::ResourceGroup>& slabGroup,
        std::vector<org::ExternalPassDesc>& outPasses,
        uint32_t phaseIndex);
};
