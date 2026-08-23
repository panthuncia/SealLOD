#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Render/RenderGraph/RenderGraph.h"

class CLodExtension;
struct CLodVariantTraits;
namespace org { class ResourceGroup; }
using org::ResourceGroup;

class CLodVisibilityVariant {
public:
    static void AppendPhase1ReyesRasterPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    static void AppendPhase2ReyesRasterPasses(
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

private:
    static void AppendReyesRasterPassesForPhase(
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
