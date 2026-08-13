#pragma once

#include <memory>
#include <string>
#include <unordered_map>

class Resource;
class CLodStreamingSystem;
class VirtualShadowCasterRegistry;

// GPU-persistent producer ownership separated from RenderGraph topology and
// Renderer instances. A future SARP DLL can retain this object while swapping
// hosts or recipes; standalone BasicRenderer creates one by default.
struct ProducerPersistentState {
    std::shared_ptr<CLodStreamingSystem> clodStreaming;
    std::shared_ptr<VirtualShadowCasterRegistry> virtualShadowCasters;
    std::unordered_map<std::string, std::shared_ptr<Resource>> terrainRvtResources;

    void InvalidateTerrainRvt() { terrainRvtResources.clear(); }
};
