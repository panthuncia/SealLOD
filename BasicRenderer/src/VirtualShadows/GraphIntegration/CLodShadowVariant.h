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
namespace org { class ResourceGroup; }
struct VirtualShadowCasterBuildContext;

class CLodShadowVariant {
public:
    static void RefreshConfiguredSettings(CLodExtension& extension);
    static uint32_t GetVisibleClusterCapacity(const CLodExtension& extension);
    static std::string AppendStructuralPrelude(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<org::ExternalPassDesc>& outPasses);
    static void AppendStructuralTail(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<org::ExternalPassDesc>& outPasses,
        const std::string& shadowClearDirtyBitsAfterPassName);
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
    static void ReleaseResourceBackings(CLodExtension& extension);
    static void RefreshResourcesForCurrentSettings(CLodExtension& extension);
    static std::shared_ptr<org::Resource> ProvideResource(CLodExtension& extension, org::ResourceIdentifier const& key);
    static std::vector<org::ResourceIdentifier> GetSupportedKeys(const CLodExtension& extension);

private:
    static VirtualShadowCasterBuildContext MakeCasterContext(CLodExtension& extension);
    static std::string AppendFineRasterPassForPhase(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<org::ResourceGroup>& slabGroup,
        std::vector<org::ExternalPassDesc>& outPasses,
        uint32_t phaseIndex);
};
