#pragma once

#include <memory>
#include <vector>

#include "Render/RenderGraph/RenderGraph.h"

class CLodExtension;
namespace org { class ResourceGroup; }
using org::ResourceGroup;
struct CLodVariantTraits;
struct RenderPhase;

class CLodAlphaVariant {
public:
    static void InitializeDeepVisibilityResources(CLodExtension& extension);
    static void InitializeAVBOITResources(CLodExtension& extension);
    static void TagResourceUsages(CLodExtension& extension);
    static void ReleaseResourceBackings(CLodExtension& extension);
    static void RefreshResourcesForCurrentSettings(CLodExtension& extension);

    static void AppendSinglePassStructuralPasses(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        const RenderPhase& renderPhase,
        bool useAVBOIT,
        bool useReyesForThisVariant,
        bool disableReyesTessellation,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);

private:
    static void AppendSinglePassResolveTail(
        CLodExtension& extension,
        const CLodVariantTraits& traits,
        const std::shared_ptr<ResourceGroup>& slabGroup,
        const RenderPhase& renderPhase,
        bool useAVBOIT,
        bool disableReyesTessellation,
        std::vector<RenderGraph::ExternalPassDesc>& outPasses);
};
