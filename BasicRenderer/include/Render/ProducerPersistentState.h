#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace org { class Resource; }
using org::Resource;
class CLodStreamingSystem;
class VirtualShadowCasterRegistry;

// GPU-persistent producer ownership separated from RenderGraph topology and
// Renderer instances. A future SARP DLL can retain this object while swapping
// hosts or recipes; standalone BasicRenderer creates one by default.
struct ProducerPersistentState {
    struct DirectionalVsmResources {
        std::shared_ptr<Resource> contract;
        std::shared_ptr<Resource> clipmapInfo;
        std::shared_ptr<Resource> mainCamera;
        std::shared_ptr<Resource> shadowCameras;
        std::shared_ptr<Resource> pageViewInfo;
        std::shared_ptr<Resource> pageMetadata;
        std::shared_ptr<Resource> pageTable;
        std::shared_ptr<Resource> physicalPages;
        uint64_t generation = 0;

        void InvalidateGpuState() {
            contract.reset(); clipmapInfo.reset(); mainCamera.reset(); shadowCameras.reset();
            pageViewInfo.reset(); pageMetadata.reset(); pageTable.reset(); physicalPages.reset();
            ++generation;
        }
    };

    std::shared_ptr<CLodStreamingSystem> clodStreaming;
    std::shared_ptr<VirtualShadowCasterRegistry> virtualShadowCasters;
    std::unordered_map<std::string, std::shared_ptr<Resource>> terrainRvtResources;
    DirectionalVsmResources directionalVsm;

    void InvalidateTerrainRvt() { terrainRvtResources.clear(); }
};
