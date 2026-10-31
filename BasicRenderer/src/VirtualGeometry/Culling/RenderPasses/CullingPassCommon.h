#pragma once

// Pieces both hierarchical culling backends share: the settings snapshot and
// flag word, the draw-set declaration tracker, the voxel queue table, and
// per-frame resets.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include "Resources/ResourceIdentifier.h"
#include "VirtualGeometry/Culling/RenderPasses/CullingPassResources.h"

namespace org { class RenderPassBuilder; }
namespace org { class Resource; }
namespace org { struct PipelineResources; }

namespace br::render {

// Uploads go through the owning pass's upload service.
using CullingUploader = std::function<void(const void* data, size_t size, const std::shared_ptr<org::Buffer>& target)>;

// Settings that shape the culling root constants, read once per recipe.
struct CullingSettings {
    bool telemetry = false;
    bool occlusion = false;
    bool predictiveInvalidation = false;
    bool frustumCulling = true;
    uint32_t softwareRasterThreshold = 0u;
    uint32_t forcedTraversalDepth = 0u;
    uint32_t receiverSubpageMode = CLodVirtualShadowReceiverSubpageModeOff;
};

CullingSettings ReadCullingSettings(const CullingPassConfig& config, bool hasReceiverSubpageMask);
// The CLOD_WG_FLAGS word: feature toggles, the software raster threshold and the phase.
uint32_t BuildCullingFlags(const CullingPassConfig& config, const CullingSettings& settings);

// Declares the published active draw lists and draw-record visibility
// generations that object culling reads. They are resolvers, so a change in the
// draw lists rebinds them without redeclaring the pass.
void DeclareCullingWorkloadInputs(org::RenderPassBuilder& builder, const CullingPassConfig& config);

void ResetCullingCounter(const std::shared_ptr<org::Buffer>& counter, const CullingUploader& upload);
void ResetOcclusionReplayState(const std::shared_ptr<org::Buffer>& state, const CullingUploader& upload);
void ResetCullingTelemetry(const std::shared_ptr<org::Buffer>& telemetry, std::vector<uint32_t>& scratch,
    const CullingUploader& upload);

// A per-pass table that hands the voxel raster queue UAV descriptors to the
// traversal shaders by resource name. The pass provides it as a resource, and
// the shaders bake the name in as a define. Without voxel queues the table has
// a name but no buffer.
class CullingVoxelQueueTable {
public:
    CullingVoxelQueueTable(const std::string& stablePassIdentifier, const char* debugName, bool enabled);

    bool Enabled() const { return m_buffer != nullptr; }
    const std::string& Id() const { return m_id; }
    std::wstring ShaderDefine() const;
    void Upload(const CLodVoxelRasterQueueDescriptors& table, const CullingUploader& upload) const;
    const std::shared_ptr<org::Buffer>& Buffer() const { return m_buffer; }

    // Throws unless the shader reads the table exactly when expected.
    void ValidateInterface(const org::PipelineResources& resources, bool expectsTable, const char* program) const;

private:
    std::string m_id;
    std::shared_ptr<org::Buffer> m_buffer;
};

}
