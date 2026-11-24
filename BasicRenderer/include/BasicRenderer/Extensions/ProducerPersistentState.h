#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace org { class Resource; }
class CLodStreamingSystem;
class VirtualShadowInvalidationQueue;

// GPU-persistent producer ownership separated from RenderGraph topology and
// Renderer instances. A future SARP DLL can retain this object while swapping
// hosts or recipes; standalone BasicRenderer creates one by default.
struct ProducerPersistentState {
    struct DirectionalVsmResources {
        std::shared_ptr<org::Resource> contract;
        std::shared_ptr<org::Resource> clipmapInfo;
        std::shared_ptr<org::Resource> mainCamera;
        std::shared_ptr<org::Resource> shadowCameras;
        std::shared_ptr<org::Resource> pageViewInfo;
        std::shared_ptr<org::Resource> pageMetadata;
        // The directional virtual shadow map's atlas: the page table and the dynamic and static physical pages. Every
        // graph's shadow CLod variant binds these, so a rebuild neither dematerializes them nor makes a second atlas.
        // They are not aliased. Made again (new objects) only when the backing resolution or the page count changes; the
        // old ones go with the graphs that hold them. A new graph resets their contents on its first frame.
        std::shared_ptr<org::Resource> pageTable;
        std::shared_ptr<org::Resource> physicalPages;
        std::shared_ptr<org::Resource> staticPhysicalPages;
        uint32_t backingResolution = 0;
        uint32_t physicalPageCount = 0;
        // Upload-heap slots CLod streaming maps from its worker for virtual shadow upgrade invalidations. Persistent, so
        // the slots the service holds stay the same across graphs.
        std::vector<std::shared_ptr<org::Resource>> upgradeUploadSlots;
        uint64_t generation = 0;

        void InvalidateGpuState() {
            contract.reset(); clipmapInfo.reset(); mainCamera.reset(); shadowCameras.reset();
            pageViewInfo.reset(); pageMetadata.reset(); pageTable.reset(); physicalPages.reset();
            staticPhysicalPages.reset(); upgradeUploadSlots.clear();
            backingResolution = 0; physicalPageCount = 0;
            ++generation;
        }
    };

    std::shared_ptr<CLodStreamingSystem> clodStreaming;
    // Where virtual shadow casters (SARP grass) queue invalidations. Each graph build has its own caster set over it.
    std::shared_ptr<VirtualShadowInvalidationQueue> virtualShadowInvalidations;
    std::unordered_map<std::string, std::shared_ptr<org::Resource>> terrainRvtResources;
    DirectionalVsmResources directionalVsm;

    // Counts InvalidateTerrainRvt calls, so a graph build that copied the resources before one does not put them back.
    uint64_t terrainRvtGeneration = 0;

    void InvalidateTerrainRvt() { terrainRvtResources.clear(); ++terrainRvtGeneration; }
};
