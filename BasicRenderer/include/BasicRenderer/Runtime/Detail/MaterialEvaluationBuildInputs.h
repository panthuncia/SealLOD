#pragma once

#include <cstdint>
#include <memory>

class PSOManager;
class CommandSignatureManager;
namespace org { class GloballyIndexedResource; }
namespace org { class ResourceGroup; }

// Immutable graph-generation selection for material evaluation. Device-scoped
// program services remain explicit; every resource is retained by value.
struct MaterialEvaluationBuildInputs {
    PSOManager* pipelines = nullptr;
    CommandSignatureManager* commandSignatures = nullptr;
    std::shared_ptr<org::ResourceGroup> clodSlabResources;
    std::shared_ptr<org::GloballyIndexedResource> visibleClusters;
    std::shared_ptr<org::GloballyIndexedResource> visibleClusterCounter;
    std::shared_ptr<org::GloballyIndexedResource> visibleClusterTransformIndices;
    std::shared_ptr<org::GloballyIndexedResource> reyesDiceQueue;
    std::shared_ptr<org::GloballyIndexedResource> reyesTessTableConfigs;
    std::shared_ptr<org::GloballyIndexedResource> reyesTessTableVertices;
    std::shared_ptr<org::GloballyIndexedResource> reyesTessTableTriangles;
    std::uint32_t visibleClusterCapacity = 0;

    bool IsValid() const noexcept {
        return pipelines && commandSignatures && visibleClusters &&
            visibleClusterTransformIndices && visibleClusterCounter;
    }
};
