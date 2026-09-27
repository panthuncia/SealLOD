#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Runtime/StateGraph/AsyncStateGraph.h"

class PagePool;
namespace org { class ResourceGroup; }

namespace br::render {

struct GeometryResidencyRange {
    std::uint32_t groupsBase = 0;
    std::uint32_t groupCount = 0;
    std::uint32_t maxTraversalDepth = 0;
    // Immutable and shared: every residency delta copies all active ranges into
    // a self-contained successor state, so an owned vector per range turned each
    // delta into thousands of allocations.
    std::shared_ptr<const std::vector<std::pair<std::uint32_t, std::uint32_t>>> coarsestRanges;
};

enum class GeometryResidencyDeltaKind : std::uint8_t {
    AddOrReplace,
    Remove,
    Reset,
};

struct GeometryResidencyDeltaInput {
    GeometryResidencyDeltaKind kind = GeometryResidencyDeltaKind::Reset;
    GeometryResidencyRange range;
    std::vector<GeometryResidencyRange> resetRanges;
    std::shared_ptr<PagePool> pagePool;
    std::shared_ptr<org::ResourceGroup> slabResources;
    std::uint64_t storageGeneration = 0;
};

struct PublishedGeometryResidencyState {
    std::uint64_t revision = 0;
    std::uint32_t maxTraversalDepth = 0;
    std::uint32_t maxGroupIndex = 0;
    std::vector<GeometryResidencyRange> activeRanges;
    // These retained owners make the publication self-contained. A frame that
    // selected this artifact remains valid even after a successor residency
    // generation is published or the manager facade is torn down.
    std::shared_ptr<PagePool> pagePool;
    std::shared_ptr<org::ResourceGroup> slabResources;
    std::uint64_t storageGeneration = 0;
};

void RegisterGeometryResidencyStateProducer(AsyncStateGraph& graph);

} // namespace br::render
