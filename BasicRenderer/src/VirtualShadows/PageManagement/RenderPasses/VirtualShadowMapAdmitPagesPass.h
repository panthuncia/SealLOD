#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <mutex>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "VirtualShadows/StreamingIntegration/VirtualShadowUpgradeService.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapAdmitPagesBindings {
    org::PipelineState admitPipeline, applyUpgradesPipeline;
    org::DeclaredViewToken pageTable, dirtyPageFlags, pageMetadata, clipmapInfo, compactShadowCameras, stats;
    std::vector<org::DeclaredViewToken> upgradeInputs;
    uint32_t normalBudget = 0;
    uint32_t upgradeBudget = 0;
};

struct VirtualShadowMapAdmitPagesFrame {
    struct Upgrade {
        uint32_t inputIndex = 0; // into VirtualShadowMapAdmitPagesBindings::upgradeInputs
        uint32_t inputCount = 0;
    };
    std::vector<Upgrade> upgrades;
    uint32_t pageTableResolution = 0;
};

class VirtualShadowMapAdmitPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapAdmitPagesPass,
    VirtualShadowMapAdmitPagesFrame, VirtualShadowMapAdmitPagesBindings> {
public:
    VirtualShadowMapAdmitPagesPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
        std::vector<std::shared_ptr<org::Buffer>> upgradeInputBuffers,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> compactShadowCamerasBuffer,
        std::shared_ptr<org::Buffer> statsBuffer,
        VirtualShadowUpgradeQueue upgradeQueue);

    VirtualShadowMapAdmitPagesBindings Declare(org::PassBuilder& builder);
    VirtualShadowMapAdmitPagesFrame Prepare(const VirtualShadowMapAdmitPagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapAdmitPagesBindings&,
        const VirtualShadowMapAdmitPagesFrame&, org::PassRecordContext&);

private:
    org::PipelineState m_pso;
    org::PipelineState m_applyUpgradesPso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_dirtyPageFlagsBuffer;
    std::vector<std::shared_ptr<org::Buffer>> m_upgradeInputBuffers;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_compactShadowCamerasBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
    VirtualShadowUpgradeQueue m_upgradeQueue;
};
