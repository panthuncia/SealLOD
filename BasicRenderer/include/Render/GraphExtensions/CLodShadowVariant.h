#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Interfaces/IResourceProvider.h"
#include "Render/RenderGraph/RenderGraph.h"

class CLodExtension;
struct CLodVariantTraits;
namespace org { class ResourceGroup; }
using org::ResourceGroup;
struct VirtualShadowCasterBuildContext;

class CLodShadowVariant {
public:
    static void RefreshConfiguredSettings(CLodExtension& extension);
    static uint32_t GetVisibleClusterCapacity(const CLodExtension& extension);
    static std::string AppendStructuralPrelude(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static void AppendStructuralTail(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses,
        const std::string& shadowClearDirtyBitsAfterPassName);
    static std::string AppendCasterRasterPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses,
        const std::string& afterPassName);
    static std::string AppendPhase1PageJobRasterPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static std::string AppendPhase2PageJobRasterPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static std::string AppendPhase1FineRasterPass(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static std::string AppendPhase2FineRasterPass(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static std::string AppendPhase1ReyesLargeRasterPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static std::string AppendPhase2ReyesLargeRasterPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static void InitializeResources(CLodExtension& extension);
    static void TagResourceUsages(CLodExtension& extension);
    static void ReleaseResourceBackings(CLodExtension& extension);
    static void RefreshResourcesForCurrentSettings(CLodExtension& extension);
    static std::shared_ptr<Resource> ProvideResource(CLodExtension& extension, ResourceIdentifier const& key);
    static std::vector<ResourceIdentifier> GetSupportedKeys(const CLodExtension& extension);

private:
    static VirtualShadowCasterBuildContext MakeCasterContext(CLodExtension& extension);
    static std::string AppendPageJobRasterPassesForPhase(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses,
        uint32_t phaseIndex);
    static std::string AppendFineRasterPassForPhase(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses,
        uint32_t phaseIndex);
};
