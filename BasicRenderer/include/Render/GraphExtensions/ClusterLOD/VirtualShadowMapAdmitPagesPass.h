#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

class Buffer;
class PixelBuffer;

class VirtualShadowMapAdmitPagesPass final : public ComputePass {
public:
    using AcquireUpgradeUploadFn =
        std::function<bool(uint32_t&, uint32_t&)>;
    using ReleaseUpgradeUploadFn = std::function<void(uint32_t)>;

    VirtualShadowMapAdmitPagesPass(
        std::shared_ptr<PixelBuffer> pageTableTexture,
        std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
        std::vector<std::shared_ptr<Buffer>> upgradeInputBuffers,
        std::shared_ptr<Buffer> pageMetadataBuffer,
        std::shared_ptr<Buffer> clipmapInfoBuffer,
        std::shared_ptr<Buffer> compactShadowCamerasBuffer,
        std::shared_ptr<Buffer> statsBuffer,
        AcquireUpgradeUploadFn acquireUpgradeUpload,
        ReleaseUpgradeUploadFn releaseUpgradeUpload,
        uint32_t framesInFlight);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    PipelineState m_applyUpgradesPso;
    std::shared_ptr<PixelBuffer> m_pageTableTexture;
    std::shared_ptr<Buffer> m_dirtyPageFlagsBuffer;
    std::vector<std::shared_ptr<Buffer>> m_upgradeInputBuffers;
    std::shared_ptr<Buffer> m_pageMetadataBuffer;
    std::shared_ptr<Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<Buffer> m_compactShadowCamerasBuffer;
    std::shared_ptr<Buffer> m_statsBuffer;
    AcquireUpgradeUploadFn m_acquireUpgradeUpload;
    ReleaseUpgradeUploadFn m_releaseUpgradeUpload;
    std::vector<uint32_t> m_inFlightSlotByFrame;
    uint32_t m_pendingUpgradeSlot = UINT32_MAX;
    uint32_t m_pendingUpgradeInputCount = 0u;
};
