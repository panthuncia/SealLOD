#pragma once

#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include "Render/RenderGraph/PersistentGraph.h"
#include <unordered_map>
#include <vector>

namespace br::render {

// Selected atomically with semantic renderer contents. Producer catalog keys
// name logical collections; ordered members retain their stable slots across
// compatible backing and descriptor replacements.
struct PersistentRendererPublication {
    std::shared_ptr<const org::persistent::SelectedPublication> graph;
    std::unordered_map<PublishedResourceKey,std::vector<org::persistent::ResourceSlotId>,
        PublishedResourceKey::Hasher> slots;
};

// Worker-only preparation over exact producer snapshots. This first integration
// covers material and geometry bindings; pass authors attach to these slots next.
void PreparePersistentRendererPublication(
    const std::shared_ptr<const PublishedRendererState>& base, PublishedRendererState& successor);

} // namespace br::render
